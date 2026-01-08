# Cedar Synth Architecture Overview

This document provides a comprehensive technical reference for implementing the cedar_synth audio engine in C or C++.

## Table of Contents

1. [Core Architecture](#core-architecture)
2. [Node Reference](#node-reference)
3. [C/C++ Implementation Guidance](#cc-implementation-guidance)

---

## Core Architecture

### Graph-Based Audio Processing

Cedar uses a directed acyclic graph (DAG) for audio processing. Nodes represent audio processors (oscillators, filters, effects) and edges represent audio signal flow.

**Key characteristics:**
- Uses `petgraph::StableGraph` for O(1) node/edge insertion and removal
- Supports dynamic graph modification during playback
- Single-threaded processing with boxed dynamic dispatch

### Graph Traversal Algorithm

Audio is processed using **Depth-First Search (DFS) in post-order** with reversed edges:

```
function process(graph, destination):
    dfs_stack = [destination]
    visited = {}
    post_order = []

    while dfs_stack not empty:
        node = dfs_stack.top()
        if node in visited:
            dfs_stack.pop()
            if node not in post_order:
                post_order.append(node)
        else:
            visited.add(node)
            for input_node in graph.inputs(node):
                if input_node not in visited:
                    dfs_stack.push(input_node)

    for node in post_order:
        inputs = collect_input_buffers(node)
        node.process(inputs, node.output_buffers)
```

This ensures nodes are processed only after all their inputs are ready.

### AudioContext

The `AudioContext` is the main container for the audio graph:

```
struct AudioContext<N>:
    graph: StableGraph<NodeData<N>>
    processor: Processor<N>
    destination: NodeIndex      // Final output node (always Sum2)
    input: NodeIndex           // External input node (always Pass)
    config: AudioContextConfig

struct AudioContextConfig:
    sr: usize           // Sample rate (e.g., 44100)
    channels: usize     // Output channels (typically 2)
    max_nodes: usize    // Pre-allocated capacity
    max_edges: usize
```

**Block processing loop:**
```
function next_block() -> &[Buffer<N>]:
    processor.process(graph, destination)
    return graph[destination].buffers
```

### Buffer Type

Buffers are fixed-size arrays determined at compile time via const generics:

```
struct Buffer<N>:
    data: [f32; N]      // Stack-allocated array

const SILENT: Buffer<N> = [0.0; N]

function silence(buffer):
    buffer.data = SILENT
```

**Design rationale:**
- Zero heap allocation in hot path
- SIMD-friendly fixed arrays
- Type safety prevents buffer size mismatches

Typical block sizes: 8, 16, 32, 64, 128, 256 samples.

### Node Trait

Every audio processor implements the `Node` trait:

```
trait Node<N>:
    function process(inputs: &HashMap<usize, Input<N>>, output: &mut [Buffer<N>])
    function send_msg(msg: Message)

struct Input<N>:
    buffers: &[Buffer<N>]
    node_id: usize

struct NodeData<N>:
    node: Box<dyn Node<N> + Send>
    buffers: Vec<Buffer<N>>     // Output buffers
```

**Process method contract:**
1. `inputs` maps source node IDs to their output buffers
2. `output` is this node's output buffer(s) to write to
3. Process exactly N samples per call
4. May have 0, 1, or multiple inputs depending on node type

### Message System

Real-time parameter control via message passing:

```
enum Message:
    // Parameter setting
    SetToNumber(param_id: u8, value: f32)
    SetToNumberList(param_id: u8, values: Vec<f32>)
    SetToSymbol(param_id: u8, symbol: String)
    SetToBool(param_id: u8, value: bool)
    SetToSamples(param_id: u8, (buffer, channels, sr))

    // Pattern/sequencer control
    SetPattern(pattern: Vec<(f32, f32)>, span: f32)
    SetToSeq(param_id: u8, events: Vec<(f32, UsizeOrRef)>)

    // Graph topology
    Index(usize)                    // Append input connection
    IndexOrder(position: usize, index: usize)  // Insert at position
    ResetOrder                      // Clear input connections

    // Global
    SetBPM(f32)
    SetSampleRate(usize)
```

Messages are processed at block boundaries for real-time safety.

---

## Node Reference

### Oscillators

#### SinOsc (Sine Oscillator)

**Parameters:** `freq` (Hz, default 1.0)
**I/O:** Optional frequency modulation input, mono output

**Algorithm:**
```
phase = 0.0  // Range [0, 1)

function process(inputs, output):
    if inputs.len() == 0:
        freq = self.freq
    else:
        freq_buffer = inputs[0].buffers[0]

    for i in 0..N:
        output[0][i] = sin(phase * 2 * PI)
        if inputs.len() == 0:
            phase += freq / sr
        else:
            phase += freq_buffer[i] / sr
        if phase > 1.0:
            phase -= 1.0
```

#### SawOsc (Sawtooth Oscillator)

**Parameters:** `freq` (Hz, default 1.0)
**I/O:** Mono output

**Algorithm:**
```
output[i] = phase * 2.0 - 1.0   // Maps [0,1) to [-1,1)
phase += freq / sr
if phase > 1.0: phase -= 1.0
```

#### TriOsc (Triangle Oscillator)

**Parameters:** `freq` (Hz, default 1.0)
**I/O:** Mono output

**Algorithm:**
```
v = -1.0 + (phase * 2.0)
output[i] = 2.0 * (abs(v) - 0.5)
phase += freq / sr
if phase > 1.0: phase -= 1.0
```

#### SquOsc (Square Oscillator)

**Parameters:** `freq` (Hz, default 1.0)
**I/O:** Mono output

**Algorithm:**
```
output[i] = 1.0 if phase <= 0.5 else -1.0
phase += freq / sr
if phase > 1.0: phase -= 1.0
```

---

### Filters

#### RLPF (Resonant Low-Pass Filter)

**Parameters:** `cutoff` (Hz, default 20), `q` (resonance, default 1.0)
**I/O:** Audio input, optional cutoff modulation input, mono output

**Algorithm (Biquad):**
```
// State variables
x0, x1, x2 = 0.0  // Input history
y1, y2 = 0.0      // Output history

function update_coefficients(cutoff, q):
    theta_c = 2 * PI * cutoff / sr
    d = 1.0 / q
    beta = 0.5 * (1 - (d/2) * sin(theta_c)) / (1 + (d/2) * sin(theta_c))
    gamma = (0.5 + beta) * cos(theta_c)

    a0 = (0.5 + beta - gamma) / 2.0
    a1 = 0.5 + beta - gamma
    a2 = a0
    b1 = -2.0 * gamma
    b2 = 2.0 * beta

function process(x):
    x2, x1, x0 = x1, x0, x
    y = a0*x0 + a1*x1 + a2*x2 - b1*y1 - b2*y2
    y2, y1 = y1, y
    return y
```

#### RHPF (Resonant High-Pass Filter)

**Parameters:** `cutoff` (Hz, default 20), `q` (resonance, default 1.0)
**I/O:** Audio input, optional cutoff modulation input, mono output

**Algorithm (Biquad):**
Same coefficient calculation as RLPF, but:
```
a0 = (0.5 + beta + gamma) / 2.0
a1 = -(0.5 + beta + gamma)
a2 = a0
// b1, b2 same as RLPF
```

#### OnePole (First-Order Low-Pass)

**Parameters:** `rate` (Hz)
**I/O:** Audio input, mono output

**Algorithm:**
```
b = exp(-2 * PI * rate / sr)
a = 1 - b

y_prev = 0.0

function process(x):
    y = a * x + b * y_prev
    y_prev = y
    return y
```

#### APFMsGain (All-Pass Filter with Delay)

**Parameters:** `delay_ms`, `gain` (default 0.5)
**I/O:** Audio input, optional delay modulation, mono output

**Algorithm:**
```
// Ring buffer for delay line
delay_samples = (delay_ms / 1000.0) * sr
buffer = RingBuffer(delay_samples)

function process(x):
    x_delayed = buffer.read()
    y_delayed = output_buffer.read()
    y = -gain * x + x_delayed + gain * y_delayed
    buffer.write(x)
    output_buffer.write(y)
    return y
```

---

### Envelopes

#### ADSR

**Parameters:** `attack` (s, default 0.01), `decay` (s, default 0.1), `sustain` (0-1, default 0.3), `release` (s, default 0.1)
**I/O:** Gate input (trigger), stereo output

**State Machine:**
```
enum Phase { Attack, Decay, Sustain, Release, Idle }
phase = Idle
pos = 0
gate = 0.0
y = 0.0

function process(gate_in):
    // Trigger detection
    if gate_in > 0 and gate <= 0:
        phase = Attack
        pos = 0
    else if gate_in <= 0 and gate > 0:
        phase = Release
        pos = 0
    gate = gate_in

    switch phase:
        Attack:
            y = pos / (attack * sr)
            if y >= 1.0:
                phase = Decay
                pos = 0
        Decay:
            y = 1.0 - (1.0 - sustain) * (pos / (decay * sr))
            if y <= sustain:
                phase = Sustain
        Sustain:
            y = sustain
        Release:
            y = sustain * (1.0 - pos / (release * sr))
            if y <= 0:
                phase = Idle
                y = 0
        Idle:
            y = 0

    pos += 1
    return y
```

#### EnvPerc (Percussive Envelope)

**Parameters:** `attack` (s, default 0.01), `decay` (s, default 0.1)
**I/O:** Trigger input, mono output

**Algorithm:**
```
phase = Idle  // Attack, Decay, Idle
pos = 0

function process(trigger):
    if trigger > 0:
        phase = Attack
        pos = 0

    switch phase:
        Attack:
            y = pos / (attack * sr)
            if y >= 1.0:
                phase = Decay
                pos = 0
        Decay:
            y = 1.0 - pos / (decay * sr)
            if y <= 0:
                phase = Idle
        Idle:
            y = 0

    pos += 1
    return y
```

---

### Effects

#### Pan (Stereo Panner)

**Parameters:** `pan_pos` (-1.0 left, 0.0 center, 1.0 right)
**I/O:** Mono input, stereo output

**Algorithm (Constant Power):**
```
pan_norm = (pan_pos + 1.0) / 2.0  // Normalize to [0, 1]
left_gain = sqrt(1.0 - pan_norm)
right_gain = sqrt(pan_norm)

output[0][i] = input[i] * left_gain
output[1][i] = input[i] * right_gain
```

#### Reverb (Freeverb)

**Parameters:** `damping`, `room_size`, `width`, `wet`, `dry`
**I/O:** Stereo input, stereo output

Uses Schroeder reverberator with parallel comb filters and series all-pass filters. See `freeverb` crate for full implementation.

#### Plate (Plate Reverb)

**Parameters:** `mix` (wet/dry balance)
**I/O:** Mono input, stereo output

**Internal Structure:**
```
Components:
- Pre-filter: OnePole(0.7)
- Initial delay: 50ms
- Series all-pass filters: 4.771ms, 3.595ms, 12.72ms, 9.307ms, 100ms
- LFO modulation: SinOsc(0.1 Hz) for subtle pitch variation
- Multiple delay taps for stereo output
```

---

### Delays

#### DelayMs (Millisecond Delay)

**Parameters:** `delay_ms`, `channels` (1 or 2)
**I/O:** Audio input, optional delay modulation input, same-channel output

**Algorithm:**
```
delay_samples = (delay_ms / 1000.0) * sr
buffer = RingBuffer(max_delay_samples)

function process(x):
    if has_modulation_input:
        // Variable delay with linear interpolation
        mod_delay = modulation_input[i]
        frac_pos = delay_samples + mod_delay
        int_pos = floor(frac_pos)
        frac = frac_pos - int_pos
        y = buffer[int_pos] * (1 - frac) + buffer[int_pos + 1] * frac
    else:
        y = buffer.read_at(delay_samples)

    buffer.write(x)
    return y
```

#### DelayN (Sample Delay)

**Parameters:** `n` (samples, integer)
**I/O:** Mono input, mono output

**Algorithm:**
```
buffer = RingBuffer(n)

function process(x):
    y = buffer.push(x)  // Returns oldest value
    return y
```

---

### Sequencers

#### Seq (Sequencer)

**Parameters:** `bpm` (default 120), `events` [(time, midi_or_ref)...], `speed`
**I/O:** Optional speed input, mono output (frequency values)

**Algorithm:**
```
bar_length = 240.0 / bpm * sr / speed  // Samples per bar
step = 0

function process():
    output = 0.0
    for (event_time, value) in events:
        trigger_sample = event_time * bar_length
        if step % bar_length == trigger_sample:
            // Convert MIDI to frequency
            freq = pow(2, (midi - 60) / 12.0)  // Relative to C4
            output = freq
    step += 1
    return output
```

#### Choose (Random Selection)

**Parameters:** `note_list` [f32...], `seed`
**I/O:** No input, mono output

**Algorithm:**
```
rng = SeededRNG(seed)

function process():
    noise = rng.next()  // Range [-1, 1]
    id = floor((noise * 0.5 + 0.5) * note_list.len())
    return note_list[id]
```

#### Speed

**Parameters:** `val` (speed multiplier)
**I/O:** No input, mono output

Outputs constant speed value (used to control Seq playback rate).

---

### Sampling

#### Sampler

**Parameters:** `sample` (buffer, channels, sr)
**I/O:** Trigger/frequency input, stereo output

**Algorithm:**
```
playback_list = []  // Active playback instances

function process(trigger):
    if trigger > 0:
        // Start new playback
        playback_list.append({
            position: 0.0,
            freq: trigger,
            duration: sample_length / trigger / (sr / sample_sr)
        })

    output = [0.0, 0.0]
    for playback in playback_list:
        // Linear interpolation for pitch shifting
        int_pos = floor(playback.position)
        frac = playback.position - int_pos

        for chan in 0..channels:
            sample_val = sample[chan][int_pos] * (1 - frac)
                       + sample[chan][int_pos + 1] * frac
            output[chan] += sample_val

        playback.position += playback.freq * (sample_sr / sr)
        if playback.position >= sample_length:
            playback_list.remove(playback)

    return output
```

---

### Signals

#### Noise

**Parameters:** `seed`
**I/O:** No input, stereo output

Uses seeded pseudo-random number generator for white noise.

#### Imp (Impulse Train)

**Parameters:** `freq` (Hz)
**I/O:** No input, mono output

**Algorithm:**
```
period = sr / freq
clock = 0

function process():
    output = 1.0 if (clock % period == 0) else 0.0
    clock += 1
    return output
```

#### ConstSig (Constant Signal)

**Parameters:** `val`, optional `pattern` [(value, time)...]
**I/O:** No input, mono output

Outputs constant value, can switch values based on BPM-synced pattern.

#### Points (Breakpoint Automation)

**Parameters:** `point_list` [(time_sample, value)...], `span`, `bpm`, `is_looping`
**I/O:** No input, mono output

**Algorithm:**
```
function process():
    // Find surrounding breakpoints
    for i in 0..point_list.len()-1:
        if current_time >= point_list[i].time and
           current_time < point_list[i+1].time:
            // Linear interpolation
            t = (current_time - point_list[i].time) /
                (point_list[i+1].time - point_list[i].time)
            return point_list[i].value * (1-t) + point_list[i+1].value * t

    if is_looping:
        current_time = current_time % total_duration
```

---

### Operators

#### Add

**Parameters:** `val` (constant to add)
**I/O:** Variable inputs, same outputs

```
output[i] = input[i] + val
// Or with two inputs:
output[i] = input_a[i] + input_b[i]
```

#### Mul (Multiply)

**Parameters:** `val` (constant multiplier)
**I/O:** Variable inputs, same outputs

```
output[i] = input[i] * val
// Or with two inputs:
output[i] = input_a[i] * input_b[i]
```

---

### Compound Instruments

#### Bd (Bass Drum)

**Parameters:** `decay` (s)
**I/O:** Trigger input, stereo output

**Internal Structure:**
```
Pitch path:  Trigger -> EnvPerc(0.01, 0.1) -> Mul(50) -> Add(60) -> SinOsc
Amp path:    Trigger -> EnvPerc(0.003, decay) -> Mul(signal)
```

#### Sn (Snare)

**Parameters:** `decay` (s)
**I/O:** Trigger input, stereo output

**Internal Structure:**
```
Tone:   Same as Bd pitch/amp paths
Noise:  Noise(42) -> Mul(0.3)
Mix:    Add(tone, noise) -> RHPF(5000, 1.0)
```

#### Hh (Hi-Hat)

**Parameters:** `decay` (s)
**I/O:** Trigger input, stereo output

**Internal Structure:**
```
Noise(42) -> RHPF(15000, 1.0) -> EnvPerc(0.003, decay)
```

#### SawSynth / SquSynth / TriSynth

**Parameters:** `attack`, `decay`
**I/O:** Trigger/pitch input, stereo output

**Internal Structure:**
```
Trigger * 261.63 -> Oscillator -> EnvPerc(attack, decay) -> Output
```

---

### Synthesizers

#### MsgSynth (Message-Triggered Polyphonic)

**Parameters:** `attack` (default 0.001), `decay` (default 0.1), `events` [(step, midi)...]
**I/O:** No input, mono output

Maintains list of active oscillators, each with independent ADSR envelope.

**MIDI to Frequency:**
```
freq = pow(2, (midi - 69) / 12.0) * 440.0
```

#### PatternSynth

**Parameters:** `attack`, `decay`, `events` [(bar_position, midi)...], `period_in_cycle`, `cycle_dur`
**I/O:** No input, mono output

BPM-synchronized version of MsgSynth.

---

### Utilities

#### Pass

Copies input to output unchanged. Converts mono to stereo by duplicating.

#### Sum

Sums all input buffers channel-wise.

---

## C/C++ Implementation Guidance

### Graph Structure

Use adjacency list representation:

```cpp
struct AudioGraph {
    std::vector<NodeData*> nodes;
    std::vector<std::vector<size_t>> edges;  // edges[node_id] = [input_ids...]
    size_t destination;
};
```

### Buffer Management

Stack-allocate fixed-size buffers:

```cpp
template<size_t N>
struct Buffer {
    float data[N];
    static constexpr Buffer SILENT = {0};
};
```

Or use a pre-allocated pool for dynamic channel counts.

### Node Dispatch

**Option 1: Virtual Functions (simpler)**
```cpp
template<size_t N>
class Node {
public:
    virtual void process(InputMap<N>& inputs, Buffer<N>* output, size_t num_outputs) = 0;
    virtual void send_msg(const Message& msg) = 0;
};
```

**Option 2: Function Pointers (potentially faster)**
```cpp
typedef void (*ProcessFn)(void* state, InputMap* inputs, float** output);
typedef void (*MessageFn)(void* state, const Message* msg);

struct Node {
    void* state;
    ProcessFn process;
    MessageFn send_msg;
};
```

### Message Queue

For thread-safe parameter updates from UI thread:

```cpp
// Lock-free SPSC queue (single producer, single consumer)
template<typename T, size_t Capacity>
class SPSCQueue {
    std::atomic<size_t> read_pos{0};
    std::atomic<size_t> write_pos{0};
    T buffer[Capacity];
public:
    bool push(const T& item);
    bool pop(T& item);
};

// Process messages at block boundary
void process_messages(AudioGraph& graph) {
    Message msg;
    while (message_queue.pop(msg)) {
        graph.nodes[msg.target]->send_msg(msg);
    }
}
```

### DFS Traversal Implementation

```cpp
void process_graph(AudioGraph& graph, size_t destination) {
    std::vector<bool> visited(graph.nodes.size(), false);
    std::vector<bool> processed(graph.nodes.size(), false);
    std::stack<size_t> stack;

    stack.push(destination);

    while (!stack.empty()) {
        size_t node = stack.top();

        if (processed[node]) {
            stack.pop();
            continue;
        }

        if (visited[node]) {
            // All inputs processed, now process this node
            InputMap inputs;
            for (size_t input_id : graph.edges[node]) {
                inputs[input_id] = &graph.nodes[input_id]->buffers;
            }
            graph.nodes[node]->process(inputs);
            processed[node] = true;
            stack.pop();
        } else {
            visited[node] = true;
            // Push inputs onto stack
            for (size_t input_id : graph.edges[node]) {
                if (!visited[input_id]) {
                    stack.push(input_id);
                }
            }
        }
    }
}
```

### Ring Buffer for Delays

```cpp
template<size_t MaxSize>
class RingBuffer {
    float buffer[MaxSize];
    size_t write_pos = 0;
    size_t size;

public:
    RingBuffer(size_t delay_samples) : size(delay_samples) {}

    float push(float sample) {
        float output = buffer[write_pos];
        buffer[write_pos] = sample;
        write_pos = (write_pos + 1) % size;
        return output;
    }

    // For variable delay with interpolation
    float read_at(float delay_samples) {
        float read_pos = write_pos - delay_samples;
        if (read_pos < 0) read_pos += size;

        size_t int_pos = (size_t)read_pos;
        float frac = read_pos - int_pos;

        return buffer[int_pos] * (1 - frac)
             + buffer[(int_pos + 1) % size] * frac;
    }
};
```

### Key Constants

```cpp
constexpr float PI = 3.14159265358979323846f;
constexpr float TWO_PI = 2.0f * PI;

// MIDI to frequency
inline float midi_to_freq(float midi) {
    return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}

// Frequency to radians per sample
inline float freq_to_phase_inc(float freq, float sr) {
    return freq / sr;
}
```
