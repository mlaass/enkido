# Cedar Sampler System - Usage Guide

## Overview

The Cedar sampler system provides polyphonic sample playback with pitch control, integrated with Akkado's mini-notation for pattern-based triggering.

## Architecture

### Cedar VM Layer
- **`SampleBank`** - Manages loaded audio samples
- **`SamplerState`** - Handles polyphonic voice allocation (up to 16 voices)
- **`SAMPLE_PLAY`** - One-shot sample playback opcode
- **`SAMPLE_PLAY_LOOP`** - Looping sample playback opcode

### Akkado Compiler Layer
- **`SampleRegistry`** - Compile-time name→ID resolution
- **Mini-notation integration** - Automatic sample pattern detection
- **Builtins** - `sample()` and `sample_loop()` functions

## Loading Samples

### From WAV Files

```cpp
#include <cedar/vm/vm.hpp>
#include <akkado/sample_registry.hpp>

// Create VM and registries
cedar::VM vm;
akkado::SampleRegistry registry;
registry.register_defaults();  // Register standard drum names (bd, sd, hh, etc.)

// Load individual WAV files
// Returns sample ID (>0) on success, 0 on failure
uint32_t kick_id = vm.sample_bank().load_wav_file("kick", "samples/kick.wav");
uint32_t snare_id = vm.sample_bank().load_wav_file("snare", "samples/snare.wav");
uint32_t hh_id = vm.sample_bank().load_wav_file("hh", "samples/hihat.wav");
```

### From Memory

```cpp
// Load from embedded data or downloaded buffers
const uint8_t* wav_data = /* ... */;
size_t wav_size = /* ... */;
vm.sample_bank_.load_wav_memory("sample", wav_data, wav_size);
```

## Using Samples in Akkado

### Mini-Notation (Automatic)

```akkado
// Simple drum pattern - automatically uses sampler
pat("bd sd bd sd")

// With modifiers
pat("bd sd hh sd")*2  // Double speed

// Multiple patterns
kick = pat("bd ~ bd ~")
snare = pat("~ sd ~ sd")
out(kick + snare)
```

### Direct Function Calls

```akkado
// One-shot playback
sample(trigger(4), 1.0, 1)  // trigger, pitch, sample_id

// Looping playback
sample_loop(gate, 1.0, 2)   // gate, pitch, sample_id

// With pitch modulation
sample(trig, lfo(0.5) + 1.0, 1)  // Pitch modulation
```

## Complete Example

### C++ Setup

```cpp
#include <cedar/vm/vm.hpp>
#include <akkado/akkado.hpp>
#include <akkado/sample_registry.hpp>

int main() {
    // Initialize VM
    cedar::VM vm;
    vm.set_sample_rate(48000.0f);

    // Create sample registry with default drum names
    akkado::SampleRegistry registry;
    registry.register_defaults();

    // Load WAV samples (IDs match registry: bd=1, sd=2, hh=3)
    vm.sample_bank().load_wav_file("bd", "samples/kick.wav");
    vm.sample_bank().load_wav_file("sd", "samples/snare.wav");
    vm.sample_bank().load_wav_file("hh", "samples/hihat.wav");

    // Compile Akkado code
    std::string code = R"(
        kick = pat("bd ~ bd ~")
        snare = pat("~ sd ~ sd")
        hats = pat("hh hh hh hh")*2

        drums = kick + snare + hats
        out(drums * 0.8)
    )";

    auto result = akkado::compile(code);
    if (!result.success) {
        // Handle compilation errors
        return 1;
    }

    // Load bytecode into VM
    auto* instructions = reinterpret_cast<const cedar::Instruction*>(result.bytecode.data());
    size_t inst_count = result.bytecode.size() / sizeof(cedar::Instruction);
    vm.load_program(std::span{instructions, inst_count});

    // Apply state initializations for patterns
    for (const auto& init : result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SeqStep) {
            vm.init_seq_step_state(init.state_id, init.values.data(), init.values.size());
        }
    }

    // Process audio (4 blocks = 512 samples)
    float left[128], right[128];
    for (int i = 0; i < 4; i++) {
        vm.process_block(left, right);
        // Use audio output...
    }

    return 0;
}
```

### Akkado Code Examples

```akkado
// Basic drum machine
kick = pat("bd ~ ~ ~")
snare = pat("~ ~ sd ~")
hats = pat("hh hh hh hh")
out(kick + snare + hats)

// With effects
drums = pat("bd sd bd cp")
drums_reverb = freeverb(drums, 0.8, 0.5)
out(drums_reverb)

// Pitch-shifted samples
melody = pat("bd bd bd bd")
pitched = sample(trigger(4), seq([1.0, 1.5, 2.0, 1.25]), 1)
out(pitched)

// Layered drums
kick = sample(trigger(4), 1.0, 1)
snare = sample(trigger(2), 1.0, 2) 
hats = sample(trigger(8), 1.0, 3)
out(kick + snare*0.8 + hats*0.6)
```

## Sample Registry

### Default Drum Names

The `SampleRegistry::register_defaults()` method registers these standard names:

| Name | ID | Description |
|------|----|----|
| `bd`, `kick` | 1 | Bass drum / Kick |
| `sd`, `snare` | 2 | Snare drum |
| `hh`, `hihat` | 3 | Hi-hat (closed) |
| `oh` | 4 | Open hi-hat |
| `cp`, `clap` | 5 | Clap |
| `rim` | 6 | Rimshot |
| `tom` | 7 | Tom |
| `perc` | 8 | Percussion |
| `cymbal` | 9 | Cymbal |
| `crash` | 10 | Crash cymbal |
| `cowbell` | 11 | Cowbell |
| `shaker` | 12 | Shaker |
| `tambourine` | 13 | Tambourine |
| `conga` | 14 | Conga |
| `bongo` | 15 | Bongo |

### Custom Samples

```cpp
// Register custom samples
registry.register_sample("laser", 100);
registry.register_sample("explosion", 101);

// Load the actual audio
vm.sample_bank_.load_wav_file("laser", "sfx/laser.wav");
vm.sample_bank_.load_wav_file("explosion", "sfx/explosion.wav");
```

## WAV File Support

### Supported Formats
- **Sample rates**: Any (automatically handled)
- **Bit depths**: 16-bit, 24-bit, 32-bit PCM, 32-bit IEEE float
- **Channels**: Mono and stereo
- **File format**: Standard WAV/RIFF

### Sample Rate Conversion
Samples are automatically resampled during playback to match the VM's sample rate using linear interpolation.

## Features

### Polyphonic Playback
- Up to 16 simultaneous voices per sampler
- Automatic voice allocation with round-robin
- Voice stealing when all voices are active

### Pitch Control
- Linear interpolation for smooth pitch shifting
- Real-time pitch modulation via LFOs or envelopes
- Pitch range: 0.1x to 10x original speed

### Looping
- One-shot mode: `SAMPLE_PLAY` / `sample()`
- Looping mode: `SAMPLE_PLAY_LOOP` / `sample_loop()`
- Gate-controlled looping

## Performance Notes

- Sample data is stored in RAM (consider memory usage for large sample libraries)
- Linear interpolation is fast but may alias at extreme pitch shifts
- Polyphony limit is per-sampler instance (multiple samplers can run simultaneously)

## Next Steps

1. **Create sample packs** - Organize WAV files into directories
2. **Embed samples** - For web deployment, embed samples as base64 or binary data
3. **Add more synthesis** - Combine samples with filters, effects, and envelopes
4. **Build patterns** - Experiment with mini-notation modifiers and combinations
