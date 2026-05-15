# MIDI Input PRD — Runtime MIDI as a Pattern-Equivalent Event Source

> **Status: PROPOSED** — Fresh PRD that inherits the `NoteEvent` contract from
> [`prd-polyphony-system.md`](prd-polyphony-system.md) and mirrors the host-fill
> architectural pattern from [`prd-audio-input.md`](prd-audio-input.md) (shipped
> 2026-04-26). Cross-references the deferred-future-work notes in
> [`prd-pattern-transport.md`](prd-pattern-transport.md) and
> [`prd-cedar-esp32.md`](prd-cedar-esp32.md).

## Executive Summary

Today, the only way to feed musical events into `poly()` is a pattern
(`pat()`, `seq()`, `note()`, `chord()`). This PRD adds a single new builtin,
`midi(...)`, that emits the same `OutputEvent` stream patterns produce — so
**`midi() |> poly(piano, 8)` is a drop-in replacement for**
**`pat(...) |> poly(...)`**. The source is selected explicitly via a record
literal: bare `midi()` opens the default live device; `midi({device: "..."})`
opens a named device; `midi({file: "..."})` plays back an SMF. Passing both
`file:` and `device:` is a compile error — the source must be unambiguous.

Live MIDI input is supported in both hosts:
- **Browser** — Web MIDI API for live devices; drag-drop for `.mid` files.
- **`nkido-cli`** — RtMidi for live devices; `cedar::UriResolver` for `.mid`.

Continuous controllers (CC, pitch-bend, channel pressure) do **not** thread
through `poly`. Instead, a sister builtin `midi_cc("paramName", {cc: 74})`
routes them into the existing `param()`/`EnvMap` interpolated path — zero
audio-thread work beyond what `param()` already does.

### Why?

The existing `tools/midi2akk/` Python tool converts `.mid` files to flattened
Akkado source at edit time, which drops velocity, CC, pitch-bend, tempo
changes, and any expressive performance data. There is no live MIDI input at
all. Several PRDs (`prd-polyphony-system.md`, `prd-pattern-transport.md`,
`prd-cedar-esp32.md`) explicitly mark "MIDI input" as deferred future work,
and the polyphony PRD's `NoteEvent` abstraction was designed source-agnostic
on the explicit promise that MIDI would plug in later. That is what this PRD
delivers.

### Key design decisions

- **One builtin, one new opcode.** `midi` builtin → `MIDI_QUERY` opcode.
  Patterns and MIDI share the same `OutputEvents` shape that `POLY_BEGIN`
  consumes, so the voice allocator is unmodified.
- **`.mid` parsing lives in Cedar, not the host.** One C++ SMF parser shipped
  with the engine; CLI and Web call it via byte-buffer load. Deterministic
  playback, no host-side scheduling drift.
- **CC routing goes through `param()`, not events.** `midi_cc("cutoff", cc:
  74)` is compile-time metadata; the host registers a callback that calls
  `vm.set_param()`. Reuses the lock-free interpolated path that already
  drives every UI slider.
- **Block-boundary timing in v1.** ≤ 2.67 ms jitter at 128/48k — acceptable
  for live keyboard use; sample-accurate scheduling is explicit later-phase
  scope.
- **Bare `midi()` opens the host's default live device.** Deterministic, no
  prompts. UI dropdown / `--midi-in <name>` sets the default; explicit
  `midi({device: "name"})` overrides per-call.
- **Tempo follow by default for `.mid` files.** The file plays at the engine
  BPM. `tempo: "file"` honors the embedded tempo map verbatim. `tempo` is
  declared in the options schema as an `OptionFieldType::Enum` with values
  `"follow,file"` so the editor can autocomplete.
- **`tools/midi2akk/` is unchanged.** Different use case: produces an
  editable `.ak` source file. Stays side-by-side with runtime MIDI.

---

## 1. Current State

### 1.1 No runtime MIDI path

| Stage | Today | With this PRD |
|---|---|---|
| Live MIDI in browser | Not supported | Web MIDI → worklet → WASM event push |
| Live MIDI in CLI | Not supported | RtMidi → audio engine event push |
| `.mid` runtime playback | Not supported (offline tool only) | `midi({file: "song.mid"})` |
| MIDI note → poly voice | Pattern only | Pattern *or* MIDI; same `OutputEvent` contract |
| MIDI CC → parameter | Not supported | `midi_cc("name", {cc: n})` → `param()` |

### 1.2 What already exists

- `OutputEvent` (`cedar/include/cedar/opcodes/sequence.hpp:122-168`) carries
  `time`, `duration`, `velocity`, `values[]`, `midi_note`, `prop_vals[]` —
  everything a MIDI source needs.
- `PolyAllocState::allocate_voice()` (`cedar/include/cedar/opcodes/dsp_state.hpp:1075`)
  and `release_voice_by_event()` (`dsp_state.hpp:1173`) are the actual voice
  primitives. The polyphony PRD §2.6 designed `NoteEvent` to be source-agnostic
  precisely so non-pattern sources could feed the same allocator; this PRD is
  the first to exercise that path. (The polyphony PRD's planned
  `process_note_events` helper was never implemented — voice allocation lives
  inline in `execute_poly_block` at `cedar/src/vm/vm.cpp:262-459`.)
- `ExecutionContext` (`cedar/include/cedar/vm/context.hpp:19`) already gained
  `input_left`/`input_right` pointers (lines 39-40) in `prd-audio-input.md`
  (shipped 2026-04-26). Same pre-block host-fill pattern works for MIDI events.
- `EnvMap` (`cedar/include/cedar/vm/env_map.hpp:43-75`) is the lock-free
  interpolated path from external signal to audio thread. CC routing reuses
  it.
- `cedar::UriResolver` (`cedar/include/cedar/io/uri_resolver.hpp`) already
  handles `.sf2` and sample assets at runtime. `.mid` is the same shape of
  asset.
- `tools/midi2akk/` does the offline conversion path that stays unchanged.

### 1.3 What is missing

- A C++ SMF parser (the Python tool uses `mido`, not linkable from C++).
- A runtime event queue type in `DSPState` — patterns bake events at compile
  time; live MIDI needs a SPSC ring per source.
- Cross-thread plumbing from MIDI callback to the audio thread.
- Web MIDI acquisition + AudioWorklet message routing.
- RtMidi as a CLI build dependency.

No MIDI libraries are currently linked anywhere.

---

## 2. Goals and Non-Goals

### Goals

- `midi(source?, channel?, loop?, tempo?)` builtin returns a pattern-shaped
  event stream usable wherever `pat()` is.
- Live device input: browser (Web MIDI API) and CLI (RtMidi). Default = first
  detected device.
- `.mid` file playback at runtime, with tempo follow (default) and tempo file
  modes.
- `.mid` files in the browser: drag-drop and explicit `midi({file: "song.mid"})`
  references resolve via a new file registry.
- `midi_cc("paramName", {cc: n})` routes a CC to a named `param()` slot,
  through `EnvMap`.
- Pitch-bend and channel aftertouch via the same `midi_cc` builtin with
  `{pb: true}` / `{at: true}` spellings.
- Hot-swap-safe: stuck notes get synthetic note-offs on program swap.
- Multiple `midi()` calls in one program work, each with its own queue and
  optional channel filter.

### Non-Goals

- **Sample-accurate event scheduling within a block.** v1 is block-boundary
  injection (≤ 2.67 ms jitter at 128/48k). Sample-accurate is an explicit
  later phase.
- **MIDI clock / start-stop tempo sync.** Tempo-sync to incoming MIDI clock
  is a transport feature; deferred.
- **MIDI output.** Sending MIDI from the engine is out of scope; this PRD is
  input-only.
- **System exclusive / NRPN / RPN.** Parsed and discarded.
- **MPE (MIDI Polyphonic Expression).** v1 treats all channels uniformly;
  MPE-aware per-voice CC would extend this.
- **14-bit CC pair auto-detection.** v1 treats every CC as 7-bit.
- **Format-2 SMF files** (independent pattern banks). Parser rejects.
- **`midi()` inside `fn` definitions.** Compile error in v1; top-level only.
- **MIDI through / monitor mode** (echoing input to a virtual output).
- **Replacing `tools/midi2akk/`.** That tool keeps its separate offline
  workflow.
- **Polyphonic aftertouch.** Parsed and discarded. (Channel aftertouch is
  supported.)

---

## 3. Target Syntax

All `midi()` configuration goes through a single options record. There are
**no positional arguments** — the source kind (`file:` vs `device:`) and any
filtering live in the record. Passing both `file:` and `device:` is a compile
error (E411 — exact code assigned at implementation time); omitting both
opens the host's default live device.

### 3.1 Live MIDI

```akkado
fn lead(freq, gate, vel) =
    osc("saw", freq) |> lp(@, 2000 * adsr(gate)) |> @ * vel

// Default live device (set via UI dropdown or --midi-in)
midi() |> poly(lead, 8) |> out

// Named device (substring match against host's port list)
midi({device: "Launchkey"}) |> poly(lead, 8) |> out

// Only channel 1
midi({channel: 1}) |> poly(lead, 8) |> out
```

### 3.2 `.mid` file playback

```akkado
// Tempo follow (rescales to current engine BPM) — default
midi({file: "song.mid"}) |> poly(piano, 16) |> out

// Honor embedded tempo map verbatim
midi({file: "song.mid", tempo: "file"}) |> poly(piano, 16) |> out

// Loop
midi({file: "song.mid", loop: true}) |> poly(piano, 16) |> out

// Filter to one channel
midi({file: "song.mid", channel: 1, loop: true}) |> poly(piano, 16) |> out
```

### 3.3 Continuous controllers

```akkado
cutoff = param("cutoff", 1000, 50, 5000)
midi_cc("cutoff", {cc: 74, min: 50, max: 5000})

fn synth(freq, gate, vel) =
    osc("saw", freq) |> lp(@, cutoff * adsr(gate)) |> @ * vel

midi() |> poly(synth, 8) |> out
// Turn CC74 on your keyboard → cutoff sweeps live
```

Pitch-bend and aftertouch use the same builtin:

```akkado
midi_cc("bend",     {pb: true, min: -1, max: 1})
midi_cc("pressure", {at: true})
```

`min`/`max` are flat `Number` fields in the options schema. The host
callback computes `(value / 127) * (max - min) + min` (or the 14-bit
equivalent for pitch-bend) and calls `vm.set_param(name, value, slew_ms)`.

### 3.4 Multiple sources

```akkado
midi({channel: 1}) |> poly(lead, 4) |> out               // controller A on ch 1
midi({channel: 2}) |> poly(bass, 1) |> out               // pads on ch 2
midi({file: "drums.mid", loop: true}) |> poly(kit, 8) |> out
```

Each `midi()` call gets its own `MidiQueueState` and SPSC event ring. The
host's MIDI callback walks a per-device route table and pushes each event
into every queue whose channel filter matches — see §4.13 for the fan-out
contract.

---

## 4. Architecture

### 4.1 End-to-end flow

```
┌─────────────────────────────────────────────────────────┐
│ HOST                                                    │
│ ┌──────────────┐   ┌──────────────┐   ┌──────────────┐  │
│ │ Live device  │   │ .mid bytes   │   │ Live device  │  │
│ │ (Web MIDI)   │   │ (file)       │   │ (RtMidi)     │  │
│ └──────┬───────┘   └──────┬───────┘   └──────┬───────┘  │
│        │                  │                  │          │
└────────┼──────────────────┼──────────────────┼──────────┘
         │                  │                  │
         ▼                  ▼                  ▼
   cedar_push_midi_event  parse_smf      audio_engine
         │                  │           (callback push)
         ▼                  ▼                  │
   MidiQueueState ring   MidiSequence*         │
         │                  │                  │
         └────────┬─────────┘                  │
                  ▼                            │
        ┌──────────────────┐                   │
        │   MIDI_QUERY     │ ◄─────────────────┘
        │   (per block)    │
        └────────┬─────────┘
                 ▼
        MidiQueueState.output (OutputEvents)
                 │
                 ▼ (linked via seq_state_id)
        ┌──────────────────┐
        │   POLY_BEGIN     │   unchanged from prd-polyphony-system
        │   voice loop     │
        └────────┬─────────┘
                 ▼
              mix → OUTPUT
```

### 4.2 `MIDI_QUERY` opcode

New opcode in `cedar/include/cedar/vm/instruction.hpp`. Placement adjacent to
`SEQPAT_QUERY` in the sequencing range (concrete value chosen at
implementation time — next available slot).

```cpp
// cedar/include/cedar/opcodes/midi.hpp (new)
inline void op_midi_query(ExecutionContext& ctx, const Instruction& inst) {
    auto& s = ctx.state_pool->get_or_create<MidiQueueState>(inst.state_id);
    s.output.clear();
    drain_live_events_into_output(s, ctx);    // SPSC ring → OutputEvents
    advance_file_seq_into_output(s, ctx);     // .mid play-head → OutputEvents
}
```

Stateless w.r.t. the audio graph; the only side effect is mutating
`MidiQueueState.output`. The opcode has no `out_buffer` — downstream
`POLY_BEGIN` reads via `seq_state_id` linkage.

### 4.3 `MidiQueueState`

```cpp
// cedar/include/cedar/opcodes/midi.hpp (new)
struct MidiRawEvent {
    uint64_t sample_ts;     // global sample at which this event fires
    uint8_t status;         // 0x80..0xEF channel msg
    uint8_t d1, d2;         // note/cc, velocity/value
    uint8_t channel;        // 0..15
};

struct MidiQueueState {
    static constexpr size_t RING_CAPACITY = 1024;
    MidiRawEvent ring[RING_CAPACITY];
    std::atomic<uint64_t> write_pos{0};
    std::atomic<uint64_t> read_pos{0};

    uint8_t channel_filter = 0;          // 0 = any
    MidiSequence* file_seq = nullptr;    // arena-owned (see §4.4)
    bool loop = false;
    enum TempoMode : uint8_t { Follow = 0, File = 1 } tempo_mode = Follow;
    double file_play_head_beats = 0.0;

    // note → event_index of the OutputEvent currently emitting it.
    // -1 means not held. Used to patch duration on note-off.
    int32_t held_note_to_event[128];

    // Output buffer in the exact shape POLY reads from SequenceState.
    OutputEvents output;
    float cycle_length = 4.0f;
    uint64_t midi_overflow_count = 0;    // ring overflows; surfaced in UI
};
```

Added to the `DSPState` variant in `cedar/include/cedar/opcodes/dsp_state.hpp`
alongside `SequenceState`, `PolyAllocState`, etc.

**SPSC ring memory ordering** — single producer is the OS MIDI callback
thread (RtMidi or the WASM-side worklet bridge); single consumer is the
audio thread inside `op_midi_query`. Standard release/acquire pairing:

```cpp
// Producer (host callback)
size_t w = ring_write_idx;             // local, no atomic needed (SPSC)
ring[w] = event;
write_pos.store(w + 1, std::memory_order_release);

// Consumer (audio thread in op_midi_query)
size_t w = write_pos.load(std::memory_order_acquire);
size_t r = read_pos.load(std::memory_order_relaxed);
for (; r != w; ++r) {
    auto e = ring[r % RING_CAPACITY];
    // ... emit OutputEvent ...
}
read_pos.store(r, std::memory_order_release);
```

The release on `write_pos` publishes the event payload; the matching
acquire on the consumer's load establishes the happens-before. `read_pos`
needs release on the consumer side only so the producer can observe
drained slots (relevant if the producer ever stalls on a full ring; today
the policy is drop-oldest, see §7).

**Note-on emission** (live or file):
```cpp
uint32_t idx = s.output.num_events;
s.output.add(
    /*time*/      block_start_beat + on_sample_offset / samples_per_beat,
    /*duration*/  1.0e9f,   // sentinel: still held; patched on note-off
    /*vals*/      {mtof(note)},
    /*count*/     1,
    /*velocity*/  vel / 127.0f,
    /*type_id*/   0,
    /*src_off*/   0,
    /*src_len*/   0,
    /*chance*/    1.0f,
    /*midi_note*/ note);
s.held_note_to_event[note] = idx;
```

**Note-off**:
```cpp
int32_t idx = s.held_note_to_event[note];
if (idx >= 0) {
    auto& e = s.output.events[idx];
    e.duration = current_beat_pos - e.time;
    s.held_note_to_event[note] = -1;
}
```

`POLY`'s `release_voice_by_event` (already at `cedar/src/vm/vm.cpp:431`)
fires when `evt.time + evt.duration` falls within the block. No POLY change.

### 4.4 SMF parser

New `cedar/include/cedar/io/midi_sequence.hpp` and
`cedar/src/io/smf_parser.cpp`. Pure C++; arena-allocated; ~300 lines.

```cpp
struct MidiNote   { uint32_t tick_on, tick_off; uint8_t note, vel, channel; };
struct MidiTempo  { uint32_t tick; uint32_t us_per_quarter; };
struct MidiSequence {
    MidiNote*   notes;     uint32_t num_notes;
    MidiTempo*  tempos;    uint32_t num_tempos;
    uint16_t    ticks_per_quarter;
    uint32_t    total_ticks;
};

// Returns nullptr on parse failure (unsupported format, bad header, etc.).
MidiSequence* parse_smf(const uint8_t* data, size_t len, AudioArena& arena);
```

Supported:
- Format 0 and 1
- Note on (0x90) / note off (0x80), velocity-0 note-on treated as note-off
- Running status
- Meta 0x51 (set tempo)
- Meta 0x2F (end of track)

Ignored / discarded:
- Sysex (0xF0..0xF7)
- Channel pressure (0xD0)
- Polyphonic aftertouch (0xA0)
- Time signature (0x58)
- Control change in the file — these are dropped for v1 (a follow-up could
  feed file CCs through `midi_cc` routes too)
- Format 2 (multi-pattern files) → returns nullptr

### 4.5 File play-head advancement

```cpp
inline void advance_file_seq_into_output(MidiQueueState& s,
                                         ExecutionContext& ctx) {
    if (!s.file_seq) return;
    const double spb = ctx.samples_per_beat;
    const double block_beats = double(BLOCK_SIZE) / spb;
    const double start = s.file_play_head_beats;
    const double end   = start + block_beats;

    for (uint32_t i = 0; i < s.file_seq->num_notes; ++i) {
        const auto& n = s.file_seq->notes[i];
        if (s.channel_filter && (n.channel + 1) != s.channel_filter) continue;
        double on_beat  = tick_to_beat(n.tick_on,  *s.file_seq, s.tempo_mode);
        double off_beat = tick_to_beat(n.tick_off, *s.file_seq, s.tempo_mode);
        if (on_beat >= start && on_beat < end) emit_note_on(s, n, on_beat - start);
        if (off_beat >= start && off_beat < end) emit_note_off(s, n.note, off_beat - start);
    }
    s.file_play_head_beats = end;
    if (s.loop && s.file_play_head_beats >= total_beats(*s.file_seq)) {
        s.file_play_head_beats -= total_beats(*s.file_seq);
    }
}
```

`tick_to_beat` honors the tempo map when `tempo_mode == File`; otherwise
divides by `ticks_per_quarter` and lets engine BPM scale.

**Algorithm** — to avoid O(N²) walks over the tempo map per note per block,
`MidiQueueState` maintains a `current_tempo_idx` cursor (added to the
struct in §4.3). For `tempo_mode == File`:

1. Advance `current_tempo_idx` while
   `tempos[current_tempo_idx + 1].tick <= play_head_tick`. Amortized O(1)
   per block since tempos are tick-monotone and the play head only moves
   forward (looping resets the cursor to 0).
2. From the active tempo entry, beats elapsed within the segment =
   `(tick - tempos[i].tick) / ticks_per_quarter`. Sum with the cached
   beats-at-tempo-entry running total (precomputed once during
   `parse_smf`, stored as `MidiTempo::beats_before`).

For `tempo_mode == Follow`, the formula collapses to
`tick / ticks_per_quarter` and the cursor is unused. The cached
`beats_before` precomputation is a 4-byte field per tempo event — cheap.

### 4.6 POLY linkage (no POLY changes)

`PolyAllocState::seq_state_id` (`dsp_state.hpp:1046`) — set by
`init_poly_state` during codegen — semantically widens from "linked
SequenceState" to "linked event-source state id." A new helper in
`StatePool`:

```cpp
// cedar/include/cedar/vm/state_pool.hpp
OutputEvents* resolve_output_events(uint32_t state_id);
// Looks up state_id, returns &state.output for SequenceState OR
// MidiQueueState; returns nullptr for any other type.
```

`execute_poly_block` at `cedar/src/vm/vm.cpp:291-294` currently reads
`state_pool_.get_if<SequenceState>(poly_state.seq_state_id)`. It is rewritten
to call `state_pool_.resolve_output_events(poly_state.seq_state_id)`. Two
lines of code; no behavioral change for pattern users.

### 4.7 Akkado builtin

`akkado/include/akkado/builtins.hpp`:

```cpp
{"midi", {.opcode = cedar::Opcode::MIDI_QUERY,
          .input_count = 0, .optional_count = 1, .requires_state = true,
          .param_names = {"options", "", "", "", "", ""},
          .defaults = {NAN, NAN, NAN, NAN, NAN},
          .description =
              "MIDI event source. Bare midi() opens the default live device. "
              "midi({device: name}) opens a named device; midi({file: path}) "
              "plays a .mid file. Options: device, file, channel, loop, tempo.",
          .extended_param_count = 0,
          .param_types = {ParamValueType::Record, {}, {}, {}, {}, {}},
          .output_channels = ChannelCount::EventSource,  // new value, see below
          .stereo_native = false,
          .option_schemas = {OptionSchema{
              .param_index = 0,
              .fields = {
                  OptionField{"device",  OptionFieldType::String, "",     "Live device name (substring match against host port list)"},
                  OptionField{"file",    OptionFieldType::String, "",     "Path/URI of a .mid file (mutually exclusive with device:)"},
                  OptionField{"channel", OptionFieldType::Number, "0",    "Channel filter, 1-16 (0 = any)"},
                  OptionField{"loop",    OptionFieldType::Bool,   "false","Loop file playback at end-of-track"},
                  OptionField{"tempo",   OptionFieldType::Enum,   "\"follow\"",
                              "Playback tempo policy (file mode only)",
                              "follow,file"},
              },
              .field_count = 5,
              .accepts_spread = false}},
          .option_schema_count = 1}}
```

**ChannelCount enum addition** — today `ChannelCount::Pattern` is declared
(`builtins.hpp:32`) but unused; `pat()`/`note()`/`value()` bypass
`output_channels` entirely because they are handled as parser `MiniLiteral`
nodes. To give `midi()` a clean type to flow through `|> poly`, add a new
`ChannelCount::EventSource` value (or rename `Pattern` → `EventSource` —
small change since `Pattern` has no current consumers). The poly-linkage
typecheck in codegen learns to accept `EventSource` upstreams. Decision
between rename-vs-add deferred to the Phase 2 implementation review with
the codegen owner.

Per the **Record-as-Options Convention** (`CLAUDE.md`,
`web/static/docs/concepts/record-as-options.md`), the options record is
declared via `OptionSchema` on the builtin and extracted in codegen via
`codegen::extract_options(arena, node, schema)`.

Codegen handler `handle_midi_call` in `akkado/src/codegen.cpp` (mirror
`handle_soundfont_call`, which lives in `codegen_patterns.cpp:5549`):

1. Extract options via `extract_options(arena, node, schema)` — get
   `device`, `file`, `channel`, `loop`, `tempo`.
2. If both `device` and `file` are present → compile error
   `E411 midi: 'file' and 'device' are mutually exclusive`. If neither is
   present, treat as default live device.
3. Allocate fresh `state_id`.
4. Emit `MIDI_QUERY { state_id }`.
5. Call `vm.init_midi_queue_state(...)` with the concrete signature:

   ```cpp
   // cedar/include/cedar/vm/vm.hpp
   enum class MidiSourceKind : std::uint8_t { DefaultDevice = 0, NamedDevice, File };
   void init_midi_queue_state(std::uint32_t state_id,
                              MidiSourceKind kind,
                              const char* name_or_path,   // nullable for DefaultDevice
                              std::uint8_t channel_filter,  // 0 = any, else 1-16
                              bool loop,
                              MidiQueueState::TempoMode tempo);
   ```

6. Register the resource in `required_midi_sources_` (parallel to
   `required_soundfonts_` in `akkado/src/codegen.cpp:91`). For `kind ==
   File` this triggers host preload via `cedar_load_midi_file` /
   `UriResolver`; for `NamedDevice` / `DefaultDevice` the host opens the
   live device (CLI: RtMidi; Web: `requestMIDIAccess`).
7. Return `TypedValue` with `channels = EventSource` so downstream
   `|> poly(...)` typechecks (see ChannelCount note above).

`handle_poly_call` already wires upstream's emitted `state_id` to
`init_poly_state(seq_state_id=...)`. No change beyond accepting `MIDI_QUERY`
as a valid upstream.

### 4.8 `midi_cc` builtin

```cpp
{"midi_cc", {.opcode = cedar::Opcode::NOP,   // no bytecode; compile-time only
             .input_count = 1, .optional_count = 1, .requires_state = false,
             .param_names = {"param_name", "options", "", "", "", ""},
             .defaults = {NAN, NAN, NAN, NAN, NAN},
             .description =
                 "Route an incoming MIDI CC (or pitch-bend / aftertouch) to a "
                 "named param() slot via EnvMap. Compile-time registration; "
                 "evaluated by the host MIDI callback at runtime.",
             .extended_param_count = 0,
             .param_types = {ParamValueType::String, ParamValueType::Record,
                             {}, {}, {}, {}},
             .output_channels = ChannelCount::Mono,
             .stereo_native = false,
             .option_schemas = {OptionSchema{
                 .param_index = 1,
                 .fields = {
                     OptionField{"cc",      OptionFieldType::Number, "",     "CC number (0-127). Required unless pb: or at: is set"},
                     OptionField{"channel", OptionFieldType::Number, "0",    "Channel filter, 1-16 (0 = any)"},
                     OptionField{"pb",      OptionFieldType::Bool,   "false","Route pitch-bend (14-bit) instead of a CC"},
                     OptionField{"at",      OptionFieldType::Bool,   "false","Route channel aftertouch instead of a CC"},
                     OptionField{"min",     OptionFieldType::Number, "0",    "Output range minimum"},
                     OptionField{"max",     OptionFieldType::Number, "1",    "Output range maximum"},
                 },
                 .field_count = 6,
                 .accepts_spread = false}},
             .option_schema_count = 1}}
```

Codegen records a `RequiredMidiCcRoute { param_name, cc_num, channel,
scale, bias }` into bytecode metadata. No instruction emitted. The compiler
translates the flat `min`/`max` option fields to `scale = max - min`,
`bias = min`. Defaults: `min = 0`, `max = 1` — i.e. plain normalized
output unless the user specifies a range.

`pb: true` → `cc_num = -1` (pitch-bend, 14-bit reconstructed from d1+d2);
default `min = -1, max = 1` for pitch-bend conventions.
`at: true` → `cc_num = -2` (channel pressure).
Setting exactly one of `cc`, `pb`, `at` is required; setting multiple is a
compile error.

On program swap, the host (CLI or WASM bridge) extracts the route list and
registers it with the MIDI callback. The callback computes `(value / 127) *
scale + bias` (or the 14-bit equivalent for PB) and calls
`vm.set_param(name, value, slew_ms)`. The audio thread sees the result via
`EnvMap`'s existing interpolated path.

### 4.9 Host integration — CLI

Add to `cmake/Dependencies.cmake` (gated `if(NOT EMSCRIPTEN)`):

```cmake
FetchContent_Declare(rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi
    GIT_TAG 6.0.0)
FetchContent_MakeAvailable(rtmidi)
```

New `tools/nkido-cli/midi_input.{hpp,cpp}`:

```cpp
class MidiInput {
public:
    void list_ports(std::ostream&);
    bool open(const std::string& name_substr);  // "" = first available
    bool open_default();
    void set_route_table(const MidiRouteTable&); // (state_id, channel_filter)
    void set_cc_route_table(const MidiCcRouteTable&);
private:
    static void on_message(double dt, std::vector<unsigned char>* msg, void*);
    RtMidiIn rt_;
};
```

`audio_engine.hpp/.cpp` gains:
- `MidiInput midi_in_` member.
- `bool init_midi(const char* name)` (mirror `init_capture` at
  `audio_engine.hpp:39`).
- Route-table swap on every bytecode reload: walk new bytecode's metadata,
  resolve MIDI source state_ids to `MidiQueueState*` pointers, atomically
  swap the callback's table.

`main.cpp`:
- `--list-midi-devices` → calls `MidiInput::list_ports(cout)`.
- `--midi-in <name>` → opens named or first device.
- `--midi-file <path>` → preload helper (not strictly required since URI
  resolution covers it).

### 4.10 Host integration — Web live MIDI

WASM exports (`web/wasm/nkido_wasm.cpp`):

```cpp
EMSCRIPTEN_KEEPALIVE
void cedar_push_midi_event(uint32_t state_id_hash,
                           uint8_t status, uint8_t d1, uint8_t d2,
                           double timestamp_ms);

EMSCRIPTEN_KEEPALIVE
int32_t cedar_load_midi_file(const char* name,
                             const uint8_t* data, int32_t size);

EMSCRIPTEN_KEEPALIVE
void cedar_set_default_midi_device(const char* device_name);

EMSCRIPTEN_KEEPALIVE
const char* cedar_get_required_midi_sources();   // JSON
EMSCRIPTEN_KEEPALIVE
const char* cedar_get_midi_cc_routes();          // JSON
```

**`cedar_load_midi_file` memory ownership.** The VM copies `data[0..size)`
into its arena, then calls `parse_smf` on the arena-owned bytes. Both the
raw bytes AND the parsed `MidiSequence` are retained — the raw bytes
because re-parsing on option changes (e.g. switching `tempo: "follow"` ↔
`"file"` without a recompile) stays cheap, and because hot-swap recompiles
that reuse the same `file:` name can find the bytes already resident. The
caller (JS) is free to drop its `Uint8Array` immediately after the call
returns. Returns `0` on success, negative on parse failure (with the
specific code surfaced via a sibling `cedar_get_last_error` call —
out-of-scope detail).

**`cedar_set_default_midi_device` purpose.** Bare `midi()` (no `device:`
field) emits a `MIDI_QUERY` whose state targets "the default device." The
web UI offers a device dropdown; on selection it calls
`cedar_set_default_midi_device(name)`. That name is recorded in a small
VM-side slot and is what the worklet's `'midi'` MessagePort dispatcher
uses to decide which `state_id`s receive an incoming event from a given
device. On the CLI, the same role is filled by `--midi-in <name>` (no
runtime change after startup). The call is idempotent and may happen
before or after compilation.

`web/src/lib/midi/midi-input.ts` (new): acquire `navigator.requestMIDIAccess()`,
expose a Svelte store for device list + selection, listen for
`onmidimessage`, post `{type: 'midi', state_id, status, d1, d2, ts}` to the
AudioWorklet via `MessagePort`.

`web/static/worklet/cedar-processor.js`: handle the new `'midi'` message type,
call `module._cedar_push_midi_event(...)`.

`web/src/lib/components/Panel/MidiInputPanel.svelte` (new): device dropdown,
activity LED (last-event timestamp), `.mid` drop zone.

`web/src/lib/stores/audio.svelte.ts`: own a `MidiInputStore` instance, wire
its message pump into the worklet on init.

### 4.11 Host integration — `.mid` files

**Browser**: drag-drop or compile-time `midi({file: "song.mid"})` reference.
File goes into a new `web/src/lib/audio/midi-bank.ts` registry parallel
to `bank-registry.ts`. On compile, `cedar_get_required_midi_sources()` returns
the list; bridge calls `cedar_load_midi_file(name, bytes, len)` before
bytecode swap (same gating as soundfonts).

**CLI**: `.mid` file URI resolves via `cedar::UriResolver`. The resolver's
existing bundle/file/http handlers cover the common cases; we just add a new
MIME prefix or filename match. On resolve, the engine calls `parse_smf` and
attaches the `MidiSequence*` to the relevant `MidiQueueState`.

### 4.12 Hot-swap and held notes

When the bytecode swaps, the old program's `MidiQueueState` is GC'd by the
state pool's frame-tracking. To prevent stuck notes:

- Before swap, the compiler emits a pre-swap pass that walks each old
  `MidiQueueState`'s `held_note_to_event[]` array. For every still-held
  note, it enqueues a synthetic note-off event into the same SPSC ring the
  live callback writes to — `status = 0x80, d1 = held_note, d2 = 0`. The
  next `op_midi_query` drain (which runs as part of finishing the
  outgoing program's last block before swap) picks them up and patches
  the relevant `OutputEvent.duration` via the existing note-off path.
- POLY voices observe the gate-off and run their existing 1-block release
  per `prd-polyphony-system.md` §2.7.

This means a live performer playing a sustained chord through a hot-swap
hears it cut. Acceptable for v1; §10 lists held-note **migration** (carrying
the live gate into the new program's matching `midi()` call) as future
work — that requires matching state IDs by semantic key, which is out of
scope here.

### 4.13 Live MIDI fan-out (host-side)

One physical input device may feed multiple `midi()` calls in the same
program (different channel filters, see §3.4). The fan-out is done **on
the host side**, in the MIDI callback that runs before the audio thread
sees anything:

```cpp
struct MidiRoute {
    std::uint32_t state_id;        // target MidiQueueState
    std::uint8_t  channel_filter;  // 0 = any, else 1-16
};
using MidiRouteTable = std::vector<MidiRoute>;  // typically 1-5 entries

struct MidiCcRoute {
    std::int16_t cc_num;           // 0-127 for CC, -1 for pitch-bend, -2 for aftertouch
    std::uint8_t channel_filter;   // 0 = any, else 1-16
    const char*  param_name;       // points into arena-owned string table
    float        scale;             // (max - min)
    float        bias;              // min
};
using MidiCcRouteTable = std::vector<MidiCcRoute>;
```

**Why host-side rather than VM-side filtering?** The PRD's existing data
model is one SPSC ring per `state_id`, which is single-producer (one
device callback) single-consumer (audio thread). Pushing the filter
decision into the callback preserves that property: each ring still has
exactly one writer. A VM-side filter would either need MPMC rings (each
queue scans a shared ring and tracks its own read cursor) or a shared
ring with per-consumer cursors — both more complex, and both have
audio-thread cost that scales with `(events × routes)` per block. The
host callback runs in OS thread context where the cost is invisible; the
table is typically ≤5 entries.

**Route-table swap on bytecode reload** — the audio engine maintains a
`std::atomic<MidiRouteTable*>` ptr. On bytecode swap (after the new
program's `init_midi_queue_state` calls have populated the next state
pool, before the audio thread sees the next program), the engine builds
a fresh table and CAS-swaps the pointer. The MIDI callback loads the
pointer with `memory_order_acquire` and uses whichever table it sees.
Old table is freed after one audio-thread block of grace (same
end-of-frame GC the state pool already uses).

**Per-device routing** — the MIDI callback is parameterized by which
physical device it serves. For "default device" routes (`bare midi()`),
the engine resolves them at table-build time using the
`cedar_set_default_midi_device` value (Web) or `--midi-in` flag (CLI).
If the user re-selects the default in the UI, the engine rebuilds the
table and re-routes; existing held notes get synthetic note-offs into
the old queue per §4.12.

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| Cedar VM core | **Modified** | New opcode dispatch, state-pool resolve helper, two new VM methods |
| Cedar opcode library | **New** | `op_midi_query`, drain/advance helpers |
| Cedar IO | **New** | `parse_smf`, `MidiSequence` |
| Akkado compiler | **Modified** | Two new builtins, options-schema entries, codegen handlers, required-MIDI-sources/-routes metadata |
| WASM bridge | **Modified** | Three new exports + two metadata getters |
| AudioWorklet processor | **Modified** | Handle `'midi'` MessagePort message |
| Web UI | **New** | MIDI panel, device dropdown, drop zone |
| Web audio store | **Modified** | Own `MidiInputStore`, wire to worklet |
| CLI `AudioEngine` | **Modified** | `init_midi`, route-table swap |
| CLI main | **Modified** | New flags |
| CMake | **Modified** | FetchContent rtmidi (CLI only) |
| Generated opcode metadata | **Modified** | Rebuild via `bun run build:opcodes` |
| Documentation | **New** | Entries in `web/static/docs/` for `midi` and `midi_cc` |
| `cedar::UriResolver` | **Reused** | `.mid` is another runtime asset |
| `EnvMap` / `param()` | **Reused** | CC routing path |
| `PolyAllocState` / POLY | **Unchanged** | NoteEvent contract honored |
| `OutputEvent` / `SequenceState` | **Unchanged** | Same shape consumed |
| `tools/midi2akk/` | **Unchanged** | Separate offline path |
| Python `cedar_core` bindings | **Unaffected** | Could add `push_midi_event` later for tests |
| Godot extension | **Unaffected** | Contract is host-pushes-events; Godot can opt in later |

---

## 6. File-Level Changes

### Create

| File | Purpose |
|---|---|
| `cedar/include/cedar/opcodes/midi.hpp` | `MidiQueueState`, `MidiRawEvent`, `op_midi_query` |
| `cedar/src/opcodes/midi.cpp` | drain / advance / emit helpers |
| `cedar/include/cedar/io/midi_sequence.hpp` | `MidiNote`, `MidiTempo`, `MidiSequence`, `parse_smf` decl |
| `cedar/src/io/smf_parser.cpp` | pure-C++ SMF parser, arena-allocated |
| `cedar/tests/test_midi.cpp` | `[midi]` Catch2 cases for parser and queue |
| `akkado/include/akkado/required_midi.hpp` | `RequiredMidiSource`, `RequiredMidiCcRoute` |
| `tools/nkido-cli/midi_input.hpp` | `MidiInput` RtMidi wrapper |
| `tools/nkido-cli/midi_input.cpp` | Implementation |
| `web/src/lib/midi/midi-input.ts` | Web MIDI acquisition + worklet pump |
| `web/src/lib/audio/midi-bank.ts` | `.mid` file registry |
| `web/src/lib/components/Panel/MidiInputPanel.svelte` | UI |
| `web/static/docs/midi.md` | F1 docs for `midi()` |
| `web/static/docs/midi_cc.md` | F1 docs for `midi_cc()` |

### Modify

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | Add `MIDI_QUERY` opcode enum value |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Add `MidiQueueState` to `DSPState` variant |
| `cedar/include/cedar/vm/state_pool.hpp` | Add `resolve_output_events(state_id)` helper |
| `cedar/include/cedar/vm/vm.hpp` | Add `init_midi_queue_state`, `push_midi_event`, `load_midi_file` |
| `cedar/src/vm/vm.cpp` | Dispatch case for `MIDI_QUERY`; teach POLY to use `resolve_output_events` |
| `cedar/CMakeLists.txt` | Add `src/io/smf_parser.cpp`, `src/opcodes/midi.cpp` |
| `akkado/include/akkado/builtins.hpp` | Register `midi`, `midi_cc` |
| `akkado/include/akkado/codegen.hpp` | Required-MIDI-sources / -CC-routes metadata |
| `akkado/src/codegen.cpp` | `handle_midi_call`, `handle_midi_cc_call`, accept MIDI source in POLY linkage |
| `akkado/src/analyzer.cpp` | Validate `midi()` and `midi_cc()` arg shapes |
| `web/wasm/nkido_wasm.cpp` | Add the five new exports (`cedar_push_midi_event`, `cedar_load_midi_file`, `cedar_set_default_midi_device`, `cedar_get_required_midi_sources`, `cedar_get_midi_cc_routes`) |
| `web/static/worklet/cedar-processor.js` | Handle `'midi'` MessagePort message |
| `web/src/lib/stores/audio.svelte.ts` | Own `MidiInputStore`, wire to worklet |
| `tools/nkido-cli/audio_engine.hpp` | Add `MidiInput`, `init_midi`, route-table |
| `tools/nkido-cli/audio_engine.cpp` | Implement above |
| `tools/nkido-cli/main.cpp` | `--list-midi-devices`, `--midi-in`, `--midi-file` |
| `cmake/Dependencies.cmake` | FetchContent rtmidi |
| `web/scripts/generate-opcode-metadata.ts` | Recognize `MIDI_QUERY` (stateful) |

### Modify (tests)

| File | Change |
|---|---|
| `cedar/tests/test_vm.cpp` | Add `[midi-poly]` integration: hand-built bytecode `MIDI_QUERY + POLY_BEGIN + osc + POLY_END + OUTPUT`, push events via `vm.push_midi_event`, assert voice activation |
| `akkado/tests/test_codegen.cpp` | Add `[midi]` cases for all syntax variants |

### Explicitly NOT changed

- `cedar/include/cedar/opcodes/sequence.hpp` — `OutputEvent` already carries
  everything we need.
- `cedar/include/cedar/opcodes/dsp_state.hpp` `PolyAllocState` —
  `allocate_voice` and `release_voice_by_event` consume `NoteEvent`s
  irrespective of source, per the polyphony PRD §2.6 contract. No code
  change to PolyAllocState itself.
- `tools/midi2akk/` — separate offline path, unchanged.
- Existing pattern opcodes (`SEQPAT_*`) — untouched.

### Post-change commands

- `cd web && bun run build:opcodes` — regenerates `opcode_metadata.hpp` with
  the new `MIDI_QUERY` entry.
- `cd web && bun run build:docs` — regenerates the docs lookup index.

---

## 7. Edge Cases

| Case | Expected behavior |
|---|---|
| Note-on without note-off (hang) | Synthetic note-off on hot-swap; voices fade per gate-off path |
| Note-off without note-on | `held_note_to_event[note] == -1` → silently discard |
| MIDI event arrives before VM ready | Drop at WASM/CLI boundary |
| Ring overflow | Drop oldest unread, increment `midi_overflow_count`; UI shows activity LED in warning state |
| Device unplugged | RtMidi callback stops / Web MIDI `statechange` → UI shows "Disconnected"; 1 s grace then synthetic note-offs |
| `.mid` not yet loaded at compile | `file_seq == nullptr` → silent; UI shows "Loading…" |
| `.mid` ends with `loop=false` | No more events; silent until program recompile or option change |
| Multiple `midi()` calls | Each gets own `state_id` + ring; live callback fans out to all; channel filters demultiplex |
| Channel filter rejects all events | Empty `output.events` → silent, no crash |
| `tempo: "file"` on a file with no tempo events | Default 120 BPM per SMF spec |
| Format-2 `.mid` (multi-pattern) | `parse_smf` returns nullptr → UI surfaces "unsupported MIDI format" |
| Velocity-0 note-on | Treat as note-off (MIDI spec) |
| Non-monotonic RtMidi timestamps | Clamp to `>= last_seen` before insertion |
| Two `midi({file: "song.mid"})` calls referencing same file | Dedup `MidiSequence*` (mirror soundfont dedup in `akkado/src/codegen_patterns.cpp` near line 5624 — `handle_soundfont_call` searches `required_soundfonts_` for a filename match); independent play heads |
| `midi()` inside `fn` body | Compile error: top-level only in v1 |
| Engine BPM changes mid-playback (follow mode) | Play head tracked in beats → smooth, no jump |
| Engine BPM changes mid-playback (file mode) | Play head still tracks beats but `tick_to_beat` honors the file's tempo map; engine BPM is decorative |
| `midi_cc` with no matching `param()` | Route is registered but no slot exists; `vm.set_param` is a no-op (already its current behavior) — UI shows warning |
| `midi_cc` with multiple of `cc`/`pb`/`at` set | Compile error: exactly one of `cc`, `pb`, `at` is required |
| `midi({file: ..., device: ...})` | Compile error E411: `file` and `device` are mutually exclusive |
| Sysex / unsupported message | Parsed and discarded silently |
| Polyphonic aftertouch | Parsed and discarded silently (not folded into `midi_cc`) |
| Both pattern and MIDI feeding same POLY | Compile error: a POLY block has one event source upstream |
| Hot-swap during file playback | `file_play_head_beats` resets to 0 on swap (new state); future option could preserve |
| Pitch-bend with no `midi_cc("...", {pb: true})` registered | Discarded |

---

## 8. Testing / Verification

### 8.1 Cedar unit tests (`cedar/tests/test_midi.cpp`, tag `[midi]`)

- `parse_smf` round-trips a known fixture: 4 notes at known ticks at 480 PPQN
  parses to `num_notes == 4` with correct ticks.
- Format-2 fixture returns nullptr.
- Tempo meta event populates `tempos[]`.
- `MidiQueueState` ring SPSC correctness: write 1000 events from one thread,
  read from another, no drops if under capacity.
- `op_midi_query` with synthetic ring events emits expected `OutputEvent`s
  for note-on/off pairs spanning multiple blocks.
- Note-off patches `duration` on the right event.

### 8.2 Cedar integration tests (`cedar/tests/test_vm.cpp`, tag `[midi-poly]`)

- Hand-built bytecode `MIDI_QUERY (state M) + POLY_BEGIN (seq=M) + OSC_SAW
  + ENV_ADSR + MUL + POLY_END + OUTPUT`.
- Push 3 note-on events in 1 block via `vm.push_midi_event`.
- Run 64 blocks. Assert: 3 voices allocated, gates correctly rise/fall,
  output is nonzero, releasing voices clear after gate-off.
- Stuck-note test: push note-on, hot-swap to a new program with the same
  `midi()` call, assert that the old voice fades cleanly.

### 8.3 Akkado tests (`akkado/tests/test_codegen.cpp`, tag `[midi]`)

- `midi()` compiles to `MIDI_QUERY` with `state_id` assigned and
  `kind = DefaultDevice`.
- `midi({file: "song.mid"})` compiles, adds `RequiredMidiSource` entry,
  `kind = File`.
- `midi({device: "Launchkey"})` compiles, `kind = NamedDevice`.
- `midi({channel: 1})` propagates channel filter into
  `init_midi_queue_state`.
- `midi({file: "song.mid", loop: true, tempo: "file"})` propagates all
  options.
- `midi({file: "song.mid", device: "Launchkey"})` → compile error E411.
- `midi_cc("cutoff", {cc: 74})` adds `RequiredMidiCcRoute` entry; emits
  no bytecode.
- `midi_cc("cutoff", {cc: 74, min: 50, max: 5000})` sets `scale = 4950`,
  `bias = 50` in the route table.
- `midi_cc("bend", {pb: true})` and `midi_cc("press", {at: true})` are
  accepted.
- `midi_cc("x", {cc: 1, pb: true})` → compile error (multiple of
  cc/pb/at).
- `midi() |> poly(synth, 8)` compiles: bytecode dump shows `MIDI_QUERY`
  before `POLY_BEGIN` with matching state link.
- `pat("c4") |> poly(...) ; midi() |> poly(...)` two independent POLY
  blocks work in one program.
- `fn x() = midi() |> ...` is a compile error.

### 8.4 Web manual tests

- Open MIDI panel → dropdown lists detected devices.
- Plug in a USB keyboard → device appears (within `statechange` event).
- Compile `midi() |> poly(synth, 8) |> out` → play keys → hear synth.
- Drag a `.mid` file onto the editor → compile
  `midi({file: "dropped.mid"}) |> poly(piano, 16) |> out` → file plays.
- Toggle `tempo: "follow"` vs `tempo: "file"` → playback speed responds to
  global BPM in follow mode, ignores it in file mode.
- `midi_cc("cutoff", {cc: 74})` + a `param("cutoff")`-driven filter →
  turning CC74 sweeps live.
- Pitch-bend wheel → `midi_cc("bend", {pb: true})` → param value follows
  wheel.
- Unplug device mid-playback → UI shows "Disconnected", no crash, stuck
  notes release within 1 s.

### 8.5 CLI manual tests

- `nkido-cli --list-midi-devices` prints ALSA/CoreMIDI/WinMM port list.
- `echo 'midi() |> poly(piano, 8) |> out' | nkido-cli serve` + keyboard →
  audible notes.
- `nkido-cli render --code 'midi({file: "twinkle.mid"}) |> poly(piano, 8) |> out'
  --seconds 60` produces a correct WAV.
- `--midi-in "Launchkey"` opens the named device; absent name falls back to
  first.

### 8.6 Cross-host parity

Same source `midi({file: "song.mid"}) |> poly(piano, 8) |> out` rendered
via `nkido-cli render` and recorded in the browser AudioContext should
produce identical PCM within float tolerance (modulo block-boundary phase
if any asynchronous source differs).

### 8.7 Stress

500 notes/sec for 30 s into the CLI → `midi_overflow_count` exposed via the
`inspect_state_json` API stays at 0 (ring is sized for this); no crash; CPU
stays bounded.

---

## 9. Implementation Phases

**Phase 1 — Cedar event plumbing & opcode (2 days)**

- Add `MIDI_QUERY` opcode enum value.
- `MidiQueueState`, `MidiRawEvent` in `cedar/include/cedar/opcodes/midi.hpp`.
- `op_midi_query` with live-ring drain only (file path stubbed).
- `init_midi_queue_state`, `push_midi_event` VM API.
- `StatePool::resolve_output_events` helper; POLY uses it.
- `[midi]` and `[midi-poly]` Catch2 cases.

**Verify**: `./build/cedar/tests/cedar_tests "[midi]"` and
`./build/cedar/tests/cedar_tests "[midi-poly]"` pass.

**Phase 2 — Akkado `midi()` builtin (1 day)**

- Register builtin with options schema.
- `handle_midi_call` in codegen.
- `RequiredMidiSource` metadata wiring.
- POLY linkage accepts MIDI source state.
- `[midi]` test cases for codegen.

**Verify**: `./build/akkado/tests/akkado_tests "[midi]"` passes; manual
bytecode dump of `midi() |> poly(synth)` shows expected shape.

**Phase 3 — CLI live MIDI (1.5 days)**

- rtmidi via FetchContent (gated on `NOT EMSCRIPTEN`).
- `midi_input.{hpp,cpp}`.
- `AudioEngine` integration; route-table swap on program reload.
- `--list-midi-devices`, `--midi-in <name>` flags.

**Verify**: plug a keyboard, run `nkido-cli serve` with `midi() |>
poly(synth) |> out`, audibly play notes.

**Phase 4 — Web live MIDI (1 day)**

- WASM `cedar_push_midi_event` export.
- `midi-input.ts`, `MidiInputPanel.svelte`.
- AudioWorklet `'midi'` message handling.
- `AudioStore` ownership.

**Verify**: browser MIDI keyboard plays the same patch.

**Phase 5 — `.mid` file playback (1.5 days)**

- `smf_parser.cpp` (format 0/1, note on/off, tempo meta).
- `MidiQueueState::file_seq` and `advance_file_seq_into_output`.
- `cedar::UriResolver` integration for CLI.
- `cedar_load_midi_file`, `midi-bank.ts` registry, drag-drop for browser.
- Tempo follow/file modes; loop handling.

**Verify**: `midi({file: "twinkle.mid"}) |> poly(piano, 16) |> out` plays correctly
in both hosts; tempo modes behave as specified.

**Phase 6 — `midi_cc` routing (0.5 day)**

- `midi_cc` builtin + codegen.
- `RequiredMidiCcRoute` metadata.
- Host-side CC dispatch in both CLI and web bridges.
- Pitch-bend (14-bit) and channel-pressure variants.

**Verify**: CC sweep audibly drives a `param()`; pitch-bend wheel updates
its target.

**Phase 7 — Polish & docs (0.5 day)**

- `web/static/docs/midi.md`, `midi_cc.md`.
- `bun run build:opcodes`, `bun run build:docs`.
- Edge-case test sweep (stuck-note hot-swap, ring overflow surfacing).
- Cross-host parity render comparison.

**Total**: ~8 working days for a single engineer.

### Risks and OS coverage

The MIDI integrations cross platform boundaries that aren't fully exercised
by current CI:

- **rtmidi platform quirks.** rtmidi nominally supports Linux/macOS/Windows,
  but each backend has gotchas: ALSA permission/raw-device issues; CoreMIDI
  callback threading (the callback may run on a Mach port thread that needs
  careful RT priority interaction); WinMM device-name encoding. The CI
  pipeline is Linux-only, so macOS/Windows regressions surface at user
  install time. Mitigation: explicit smoke-test checklist in Phase 3
  verification per host platform; consider adding macOS/Windows CI jobs as
  a follow-up.
- **Web MIDI gating.** `navigator.requestMIDIAccess()` requires a secure
  context (HTTPS or localhost) and an explicit user permission grant. The
  default-device dropdown must handle the "permission denied" state
  gracefully — UI shows a hint with a link to enable MIDI in browser
  settings. Verify: load the site over plain HTTP and confirm fallback
  copy is present.
- **Web MIDI cross-browser.** Firefox shipped Web MIDI later than Chrome
  and Safari has historical quirks; verify the device-list and
  `statechange` event in all three.
- **Bytes lifetime in the worklet bridge.** WASM heap is shared with JS
  but the AudioWorklet runs in a separate worker. `cedar_load_midi_file`
  is called from the main thread; the `data` pointer must reference WASM
  heap (copy from JS `Uint8Array` to a `Module.HEAPU8`-backed buffer
  before the call). Document in `web/src/lib/midi/midi-input.ts`.

### Future (separate PRDs)

- **Sample-accurate intra-block scheduling.** Promote
  `OutputEvent.gate_on_sample` to a first-class field used by POLY.
- **MIDI clock / start-stop tempo sync.** Probably under
  `prd-pattern-transport.md` as the natural home.
- **MPE support.** Per-voice CC routing in `poly()`.
- **MIDI output.** Sending events from the engine.
- **14-bit CC pair auto-detection.**
- **Held-note migration across hot-swap.** When the new program has a
  compatible `midi()` call (matching channel filter), migrate held state.
  Phase 3 (CLI Live MIDI) intentionally defers the §4.12 synthetic-note-off
  injection too: held notes survive across hot-swaps when state IDs stay
  stable (idempotent `init_midi_queue_state`), but a `midi()` call that
  disappears entirely between programs leaks its held notes until the
  voice's release timeout. Re-evaluate once a real user hits it.
- **Audible click on note-off** (and a milder one on note-on) when an
  instrument's envelope has a release tail longer than ~10 ms. Root cause
  is the polyphony engine's gate-multiplied accumulation
  (`prd-polyphony-system.md` §2.7 + §7 "Future: Configurable Voice
  Release"): each voice's output is multiplied by `gate` before mixing,
  so when gate flips 1 → 0 at note-off the voice's `* adsr(gate, ...)`
  release tail is silenced in one sample. Patterns with cycle-aligned
  note-offs paper over this; live MIDI surfaces it on every key-up.
  Fix lives in the polyphony PRD (per-instance release timeout + skip
  gate-mult during the release window, exposed as `poly(@, instr, voices,
  release: 0.5)`); track there, not here.
- **File CC playback through `midi_cc` routes.** v1 drops `.mid` CCs; a
  follow-up could feed them through registered routes.

---

## 10. Open Questions

1. **MIDI clock / start-stop** — incoming MIDI clock for tempo sync is
   attractive but a transport feature, not a poly-input concern. **Proposal:
   out of scope; a future PRD under `prd-pattern-transport.md`.**
2. **14-bit CC pairs** — controllers that send CC#n + CC#(n+32) as MSB/LSB.
   **Proposal: v1 treats as separate 7-bit; future auto-pair as needed.**
3. **Held-note migration across hot-swap** — v1 cuts notes on swap. Future
   option: migrate held state when the new program has a compatible `midi()`
   call. **Proposal: v1 cut; revisit if users complain.**
4. **`midi()` inside `fn` body** — banned in v1. **Proposal: keep banned;
   non-trivial to scope per-call state in a function body.**
5. **CLI MIDI thru / monitor** — echo received MIDI to a virtual output for
   chaining. **Proposal: out of scope for v1.**
6. **File CCs through `midi_cc`** — `.mid` files often carry CC automation;
   v1 drops them. **Proposal: defer; the architectural hook exists (the
   parser already sees CCs) so a follow-up is small.**

---

## 11. Related Work

- [`prd-polyphony-system.md`](prd-polyphony-system.md) — defines `POLY_BEGIN`,
  `PolyAllocState`, and the `NoteEvent` abstraction this PRD inherits.
  Section 1.3 previews the syntax as `midi_in() |> poly(...)`; this PRD
  shortens the builtin to `midi()` for symmetry with `pat()`/`note()`/
  `value()`. Same semantics.
- [`prd-audio-input.md`](prd-audio-input.md) — architectural template for
  "host fills VM-side buffer pre-block." Shipped 2026-04-26.
- [`prd-pattern-transport.md`](prd-pattern-transport.md) — references MIDI
  input as a possible external trigger source; this PRD's `midi()` is the
  event-source half. MIDI clock for transport is deferred there.
- [`prd-cedar-esp32.md`](prd-cedar-esp32.md) — lists USB-MIDI / BLE-MIDI /
  serial MIDI as deferred future work. This PRD's `cedar_push_midi_event`
  API is portable to any host that can deliver `MidiRawEvent`s.
- [`web/static/docs/concepts/record-as-options.md`](../web/static/docs/concepts/record-as-options.md)
  — the `{channel, loop, tempo}` record on `midi()` follows this convention.
- `tools/midi2akk/` — the offline conversion tool, unchanged. Use case:
  produce hackable `.ak` source; this PRD's runtime path doesn't replace it.
