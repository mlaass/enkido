> **Status: NOT STARTED** — Investigation, integration plan, and side-by-side alternatives for adopting MIT-licensed DaisySP DSP modules into Cedar/Akkado.

# PRD: DaisySP Integration

**Project:** Cedar (live-coding audio engine) + Akkado language
**Source:** [electro-smith/DaisySP](https://github.com/electro-smith/DaisySP) — MIT-licensed (the `Source/` tree only; the `DaisySP-LGPL/` submodule is **out of scope** and must not be vendored)
**Driver:** Add gap-filling DSP voices and effects (drum models, physical modeling, additive/FM/sync synthesis voices, useful noise and utility primitives) by vendoring DaisySP and wrapping each module as a Cedar opcode.

---

## 1. Executive Summary

DaisySP is a small, focused, embedded-grade C++ DSP library from Electrosmith. It is MIT-licensed (separated cleanly from an isolated LGPL submodule) and contains a number of voices and effects that nkido does not currently ship — most notably a complete set of analog/synth drum models, the Plaits-derived physical modeling family (modal, resonator, string, drip), additive/FM/sync synthesis oscillators (FM2, harmonic, VOSIM, ZOscillator), several useful noise variants (dust, fractal/pink, particle, grainlet, clocked), a pitch shifter, a granular player, and a few timing/control primitives (looper, smooth random, maytrig).

This PRD proposes:

1. **Vendor `DaisySP/Source/` as `cedar/third_party/daisysp/`**, MIT-only, no LGPL submodule.
2. **Wrap each chosen module as a new Cedar opcode** that owns a `daisysp::Foo` instance in `StatePool` and runs `Foo::Process(...)` in a `BLOCK_SIZE = 128` sample loop.
3. **Expose each opcode as an Akkado builtin** with idiomatic naming. Gap fillers get clean names (`modal`, `dust`, `pitch_shift`); side-by-side alternatives to existing nkido opcodes (chorus, flanger, phaser, wavefold, overdrive) get the `ds_` prefix to avoid collisions.
4. **Plug voice modules into the existing POLY block** so `bd_analog`, `modal`, `string_voice`, etc. work the same way `osc()` and `smooch()` do today.
5. **Ship in five phases by category** (infra → drums → physical models → synthesis voices → noise/utility/effects), each phase user-visible before the next begins.

### Key Design Decisions

- **MIT only.** `DaisySP-LGPL/` (which contains GPL-incompatible Mutable Instruments ports) is explicitly excluded.
- **Vendor as-is, do not port.** Drop the upstream `Source/` tree under `cedar/third_party/daisysp/` and call DaisySP classes directly from opcode wrappers. Keeps attribution intact and makes future updates a `git pull` away.
- **Per-class state via StatePool.** Each stateful module (the vast majority) gets a typed StatePool entry; stateless modules opt out and behave like existing stateless Cedar opcodes.
- **Block-loop wrapper, not control-rate.** Per-sample modulation is preserved by calling DaisySP's `Set*` methods inside the per-sample loop rather than once per block.
- **Additive integration.** No existing opcode is removed. Where DaisySP overlaps with an existing nkido opcode, both are kept side-by-side — DaisySP gets the `ds_` prefix; users can A/B them in patches.
- **DaisySP-native parameter signatures.** Builtins expose DaisySP's `Set*` parameters verbatim as audio-rate inputs (e.g., `bd_analog(trig, freq, tone, decay, attack_fm, self_fm, accent)`), so the upstream documentation maps cleanly to Akkado.
- **Special-handler `ds_drum()` macro.** A live-coding shortcut that compiles a mini-notation drum pattern (`"bd:sd:hh:hh"`) into a sequence of triggered DaisySP drum voices summed into one signal.

---

## 2. Goals and Non-Goals

### 2.1 Goals

- **Drum kit out of the box.** A live-coder can write `ds_drum("bd:sd:hh:hh")` and get a working analog-style drum line without sampling a kit.
- **Physical modeling palette.** Bring Plaits-derived modal/resonator/string/drip voices to nkido. These cover percussive, mallet, pluck, and bell timbres.
- **Wider synthesis vocabulary.** FM2, harmonic, VOSIM, ZOscillator, and an additive bank widen the range of timbres beyond Cedar's existing analog-style oscillators and Smooch wavetable.
- **Pitch shifter.** Enables shimmer reverb, harmonizer effects, and tape-style pitch tricks that Cedar can't currently do.
- **Granular playback.** A new `granular()` builtin alongside the existing `s()` / `sample()` family.
- **Hot-swap parity.** Every new opcode preserves state under code reload via Cedar's existing semantic-ID matching.
- **WASM-compatible.** Every new opcode compiles under Emscripten and the WASM bundle stays under a published growth budget.
- **Attribution-correct.** MIT obligations honored via per-source headers + repo-root NOTICE.

### 2.2 Non-Goals

- **No LGPL modules.** `DaisySP-LGPL/` (Mutable Instruments ports under GPL) is excluded. Nkido is permissively licensed and that compatibility comes first.
- **No replacement of existing nkido opcodes.** If nkido already has an equally good or better implementation, the DaisySP version is either skipped (filters, noise base, limiter, delay line, DC block) or shipped as `ds_*` alternative (chorus, flanger, phaser, wavefolder, overdrive).
- **No re-implementation in nkido style.** This PRD is explicitly *vendor-as-is*. A future PRD may port hot algorithms to block/SIMD form if profiling shows it's needed.
- **No new file formats, no UI controls, no editor integration.** Everything is opcode + builtin only. Future PRDs can add UI parameter controls if a module warrants a dedicated panel.
- **No DaisySP build-time configuration knobs.** Daisy compiles for STM32 with various sample rates; nkido is fixed at 48 kHz with `BLOCK_SIZE = 128`. We pass that to `Init()` and that's it.
- **`drum()` macro is `ds_drum()` only.** No unprefixed `drum()` — the name is too generic and we want to leave room for a future native drum sequencer.

---

## 3. Current State and Survey of DaisySP

### 3.1 Modules in DaisySP (MIT, `Source/` only)

DaisySP is organized into nine categories. The full inventory and decisions are below; see §4 for signatures and §5 for files.

| Module | DaisySP file | Decision | Akkado name |
|---|---|---|---|
| **Drums** | | | |
| AnalogBassDrum | `Drums/analogbassdrum.h` | **Add** | `bd_analog` |
| SynthBassDrum | `Drums/synthbassdrum.h` | **Add** | `bd_synth` |
| AnalogSnareDrum | `Drums/analogsnaredrum.h` | **Add** | `sd_analog` |
| SynthSnareDrum | `Drums/synthsnaredrum.h` | **Add** | `sd_synth` |
| HiHat | `Drums/hihat.h` | **Add** | `hh` |
| **Physical Modeling** | | | |
| ModalVoice | `PhysicalModeling/modalvoice.h` | **Add** | `modal` |
| Resonator | `PhysicalModeling/resonator.h` | **Add** | `resonator` |
| Drip | `PhysicalModeling/drip.h` | **Add** | `drip` |
| StringVoice | `PhysicalModeling/stringvoice.h` | **Add** | `string_voice` |
| KarplusString | `PhysicalModeling/KarplusString.h` | **Add** | `karplus` |
| **Synthesis** | | | |
| Fm2 | `Synthesis/fm2.h` | **Add** | `fm2` |
| FormantOscillator | `Synthesis/formantosc.h` | **Add** | `formant_osc` |
| HarmonicOscillator | `Synthesis/harmonic_osc.h` | **Add** | `harmonic_osc` |
| OscillatorBank | `Synthesis/oscillatorbank.h` | **Add** | `osc_bank` |
| VosimOscillator | `Synthesis/vosim.h` | **Add** | `vosim` |
| ZOscillator | `Synthesis/zoscillator.h` | **Add** | `zosc` |
| VariableShapeOscillator | `Synthesis/variableshapeosc.h` | **Add** | `var_shape` |
| Oscillator (basic) | `Synthesis/oscillator.h` | Skip — internal use only | — |
| VariableSawOscillator | `Synthesis/variablesawosc.h` | Skip — overlaps `saw_pwm` | — |
| **Noise** | | | |
| Dust | `Noise/dust.h` | **Add** | `dust` |
| FractalNoise | `Noise/fractal_noise.h` | **Add** | `pink_noise` |
| Particle | `Noise/particle.h` | **Add** | `particle` |
| GrainletOscillator | `Noise/grainlet.h` | **Add** | `grainlet` |
| ClockedNoise | `Noise/clockednoise.h` | **Add** | `clocked_noise` |
| WhiteNoise | `Noise/whitenoise.h` | Skip — Cedar `noise()` exists | — |
| **Effects** | | | |
| PitchShifter | `Effects/pitchshifter.h` | **Add** | `pitch_shift` |
| AutoWah | `Effects/autowah.h` | **Add** | `autowah` |
| Chorus | `Effects/chorus.h` | **Add side-by-side** | `ds_chorus` |
| Flanger | `Effects/flanger.h` | **Add side-by-side** | `ds_flanger` |
| Phaser | `Effects/phaser.h` | **Add side-by-side** | `ds_phaser` |
| Wavefolder | `Effects/wavefolder.h` | **Add side-by-side** | `ds_fold` |
| Overdrive | `Effects/overdrive.h` | **Add side-by-side** | `ds_overdrive` |
| Decimator | `Effects/decimator.h` | Skip — `bitcrush` covers it | — |
| SampleRateReducer | `Effects/sampleratereducer.h` | Skip — `bitcrush` covers it | — |
| Tremolo | `Effects/tremolo.h` | Skip — trivial in Akkado | — |
| **Filters** | | | |
| FIR | `Filters/fir.h` | **Add** | `fir` |
| SOAP | `Filters/soap.h` | **Add** | `soap` |
| Ladder | `Filters/ladder.h` | Skip — `FILTER_MOOG` exists | — |
| Svf | `Filters/svf.h` | Skip — Cedar SVF exists | — |
| OnePole | `Filters/onepole.h` | Skip — trivial / overlaps SVF | — |
| **Dynamics** | | | |
| Crossfade | `Dynamics/crossfade.h` | Skip — one line of Akkado | — |
| Limiter | `Dynamics/limiter.h` | Skip — `DYNAMICS_LIMITER` exists | — |
| **Sampling** | | | |
| GranularPlayer | `Sampling/granularplayer.h` | **Add** | `granular` |
| **Utility** | | | |
| Looper | `Utility/looper.h` | **Add** | `looper` |
| Metro | `Utility/metro.h` | Skip — `clock()`/`trigger()` cover | — |
| Maytrig | `Utility/maytrig.h` | **Add** | `maytrig` |
| SampleHold | `Utility/samplehold.h` | Skip — `EDGE_OP`/`SLEW` cover | — |
| SmoothRandomGenerator | `Utility/smooth_random.h` | **Add** | `smooth_rand` |
| DcBlock | `Utility/dcblock.h` | Skip — trivial | — |
| DelayLine | `Utility/delayline.h` | Skip — Cedar `DELAY` exists | — |
| Dsp | `Utility/dsp.h` | Vendored as helper (not exposed) | — |

### 3.2 Why DaisySP

| Aspect | Benefit |
|---|---|
| **License** | MIT — fully compatible with nkido. |
| **Code quality** | Embedded-grade: no allocations in process loops, simple float math, predictable cycles per sample. Already shipping in Daisy hardware. |
| **Provenance** | Many modules are ports of [pichenettes/eurorack/plaits](https://github.com/pichenettes/eurorack) (Mutable Instruments) by Emilie Gillet — recognized as some of the best small-format DSP code in existence. The MIT-licensed re-ports in DaisySP/Source/ avoid the LGPL of the original Plaits firmware. |
| **Coverage** | Fills concrete gaps in nkido's palette (drums, physical modeling, additive/FM voices, pitch shifter, granular, pink noise). |
| **Maintenance** | Active upstream, semantic versioning, clear directory layout. Vendoring is a low-friction commitment. |

### 3.3 Why side-by-side rather than replace

For modules that overlap an existing nkido opcode (chorus, flanger, phaser, wavefolder, overdrive), DaisySP and nkido implement different topologies. Rather than picking a winner from this PRD's desk:

- **Add the DaisySP version under a `ds_` prefix.**
- **Keep both running.**
- **Compare with side-by-side A/B tests** (rendered WAV diffs, see §10).
- **Decide replacements in a follow-up PRD** once we have user feedback and audio comparisons.

This PRD is explicitly **additive**. No existing opcode is deprecated.

---

## 4. Target Akkado Syntax

Each new builtin's signature is below, lifted from DaisySP's public `Set*` methods. All are mono-output unless otherwise noted; voice builtins are designed to slot into Cedar's POLY block.

### 4.1 Drum voices

```akkado
// 808-style analog bass drum (Plaits port)
bd_analog(trig, freq, tone, decay, attack_fm, self_fm, accent)
// trig:      rising-edge trigger
// freq:      Hz, root frequency
// tone:      0..1, click amount
// decay:     0..1, body decay length
// attack_fm: 0..1, FM attack amount
// self_fm:   0..1, self-FM (pitch envelope)
// accent:    0..1, soft accent

// Synthetic bass drum (Plaits port)
bd_synth(trig, freq, tone, decay, dirtiness, fm_envelope_amount, fm_envelope_decay)

// Analog snare (Plaits port)
sd_analog(trig, freq, tone, decay, snappy, accent)

// Synthetic snare (Plaits port)
sd_synth(trig, freq, tone, decay, snappy, fm_amount)

// Hi-hat (Plaits port; rate field selects 0=open / 1=closed in Akkado)
hh(trig, freq, tone, decay, noisiness)
hh_open(trig, freq, tone, decay, noisiness)   // alias setting rate=0
hh_closed(trig, freq, tone, decay, noisiness) // alias setting rate=1
```

### 4.2 Sequenced-drum sugar

```akkado
// Mini-notation pattern of drum names. Each step plays one preset voice
// at the inferred beat. Voices are bd_analog, sd_analog, hh_closed, hh_open.
// Output is mono.
ds_drum("bd:sd:hh:hh") |> out(@, @)

// Custom voice/freq overrides via record syntax (mirrors `s()` events)
ds_drum("bd hh*4 sd?", { bd: { freq: 60, decay: 0.7 } })
```

`ds_drum()` is a **special-handler builtin** (codegen-special, like `pat()`) that:
1. Parses the mini-notation pattern at compile time.
2. Emits one drum-voice opcode per active step name plus a triggered `pat()`-driven gate.
3. Sums all voices into a single buffer.

A live-coder gets a working drum kit from a single line.

### 4.3 Physical modeling

```akkado
// Mallet-struck modal resonator. Polyphonic-friendly.
modal(trig, freq, structure, brightness, damping, accent)
// structure:  0..1, stiffness/inharmonicity
// brightness: 0..1, exciter LP cutoff and noise density
// damping:    0..1, body decay time
// accent:     0..1, hit force

// Bare modal resonator (no exciter — feed your own click/pulse)
resonator(input, freq, structure, brightness, damping)

// Particle exciter for percussion bodies
drip(trig, freq, decay, density)

// Karplus-Strong + body resonator
string_voice(trig, freq, structure, brightness, damping, accent)

// Plain Karplus-Strong plucked string
karplus(trig, freq, brightness, damping, accent)
```

### 4.4 Synthesis voices

```akkado
// 2-operator FM with feedback
fm2(freq, ratio, index, feedback)

// Formant oscillator (vocal/synth-formant)
formant_osc(freq, formant_freq, phase_shift)

// Additive harmonic oscillator (12 fixed partials)
harmonic_osc(freq, magnitudes)         // magnitudes is an array of 12 floats

// Detunable bank of N oscillators (additive supersaw)
osc_bank(freq, gain, registration)     // registration: organ-stop-style mix array

// VOSIM vocal-style synth
vosim(freq, formant1, formant2, shape)

// Sync / phase-distortion oscillator (Plaits ZOSC port)
zosc(freq, formant_freq, carrier_shape, mode)

// Variable-shape morphing oscillator (saw → square → triangle → ...)
var_shape(freq, shape, pw, sync)
```

### 4.5 Noise / utility / effects

```akkado
// Random impulses (density: events/sec)
dust(density)

// Pink/brown noise (rate field: 0=pink, 1=brown)
pink_noise()
pink_noise(color: 0.5)   // 0=pink, 1=brown — interpolated

// Particle noise (rare-event LP-filtered noise; freq + density)
particle(freq, density, gain, spread)

// Grainlet oscillator (windowed grain stream; pitch + grain pitch)
grainlet(carrier_freq, formant_freq, shape, bleed)

// Sample-and-hold noise at variable rate
clocked_noise(freq)

// Pitch shifter (semitones, ±24, transposition only)
pitch_shift(input, transposition)

// Auto-wah envelope-follower bandpass
autowah(input, drive, sensitivity, level)

// Side-by-side alternatives to existing nkido FX
ds_chorus(input, lfo_depth, lfo_freq, delay, feedback)
ds_flanger(input, lfo_depth, lfo_freq, feedback)
ds_phaser(input, lfo_depth, lfo_freq, feedback)
ds_fold(input, gain, offset, iterations)
ds_overdrive(input, drive)
```

### 4.6 Filters

```akkado
// Generic FIR filter (compile-time taps array)
fir(input, taps)         // taps: array of coefficients, max length TBD per CLAUDE.md ExtendedParams pattern

// Pirkle SOAP variable-character filter
soap(input, freq, q, character)
```

### 4.7 Sampling, looping, control

```akkado
// Granular sample player
granular(uri, density, grain_size, position, pitch, spread)

// Looper (record/play/overdub controlled by triggers)
looper(input, record, play, overdub)
// record/play/overdub: rising-edge trigger inputs

// Probabilistic trigger (Maytrig)
maytrig(trig, probability)

// Slewed random LFO (smooth random walk)
smooth_rand(freq)
```

---

## 5. Architecture

### 5.1 Three-layer integration

```
[ Akkado source ]
       |
       v
[ Akkado builtins.hpp / codegen ]    — Maps name → opcode, signature, defaults
       |
       v
[ Cedar opcode (e.g. DAISY_BD_ANALOG) ]
       |  owns
       v
[ DaisyState<daisysp::AnalogBassDrum> ]   — Lives in StatePool, keyed by semantic ID hash
       |  calls
       v
[ daisysp::AnalogBassDrum::Process(...) ] — One sample per loop iteration
```

Every chosen DaisySP module gets:

1. A new `Opcode::DAISY_*` enum entry in `cedar/include/cedar/vm/instruction.hpp`.
2. A wrapper struct in a new `cedar/include/cedar/opcodes/daisy.hpp` containing the per-opcode block-loop body.
3. A typed `DaisyState<T>` (or named state struct) in `cedar/include/cedar/dsp_state.hpp` that holds the `daisysp::Foo` instance.
4. A builtin entry in `akkado/include/akkado/builtins.hpp` mapping `name → Opcode`.

### 5.2 Block-loop wrapper pattern

Standard pattern for a stateful single-input voice (e.g. `bd_analog`):

```cpp
// cedar/include/cedar/opcodes/daisy.hpp
case Opcode::DAISY_BD_ANALOG: {
    auto& state = ctx.states->get_or_create<DaisyAnalogBassDrumState>(inst.state_id);
    if (!state.initialized) {
        state.bd.Init(static_cast<float>(SAMPLE_RATE));
        state.initialized = true;
    }

    float* trig  = ctx.get_input(inst, 0);   // BLOCK_SIZE samples
    float* freq  = ctx.get_input(inst, 1);
    float* tone  = ctx.get_input(inst, 2);
    float* decay = ctx.get_input(inst, 3);
    float* afm   = ctx.get_input(inst, 4);
    float* sfm   = ctx.get_input_extended(state.ext, 0); // ExtendedParams<2>: self_fm
    float* acc   = ctx.get_input_extended(state.ext, 1); // ExtendedParams<2>: accent

    float* out = ctx.get_output(inst);
    float prev_trig = state.last_trig;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        state.bd.SetFreq(freq[i]);
        state.bd.SetTone(tone[i]);
        state.bd.SetDecay(decay[i]);
        state.bd.SetAttackFmAmount(afm[i]);
        state.bd.SetSelfFmAmount(sfm[i]);
        state.bd.SetAccent(acc[i]);
        bool trigger = (trig[i] > 0.5f && prev_trig <= 0.5f);
        prev_trig = trig[i];
        out[i] = state.bd.Process(trigger);
    }
    state.last_trig = prev_trig;
    break;
}
```

Notes:
- DaisySP's `Process()` is single-sample and consumes `bool trigger`. Edge detection is done in the wrapper (mirrors how `EDGE_OP` does it today).
- All inputs are read as audio-rate buffers. Per-sample modulation is preserved.
- Extended parameters (params 5+) follow the existing nkido `ExtendedParams<N>` pattern (CLAUDE.md §"Extended Parameter Patterns").

### 5.3 State preservation under hot-swap

Each DaisySP-derived opcode declares a typed state struct:

```cpp
// cedar/include/cedar/dsp_state.hpp
struct DaisyAnalogBassDrumState {
    daisysp::AnalogBassDrum bd;
    bool initialized = false;
    float last_trig = 0.0f;
};
```

Hot-swap reuses the existing semantic-ID matching: as long as the opcode in the new program has the same `state_id`, `StatePool::get_or_create<T>(id)` returns the existing instance and `initialized` remains true — phase, envelope state, and trigger-edge state survive the reload. This is the same mechanism Smooch and SoundFont voices already use.

### 5.4 Polyphony

Voice modules (`bd_analog`, `bd_synth`, `sd_*`, `hh*`, `modal`, `string_voice`, `karplus`, `drip`, `fm2`, `formant_osc`, `harmonic_osc`, `osc_bank`, `vosim`, `zosc`, `var_shape`) must be usable inside `poly { ... }` blocks. The contract is:

- The first input is `trig` (rising-edge fires the voice) or `gate` (level-driven), matching the voice's natural envelope behavior. Drums are trigger-driven; modal/string/karplus accept either depending on whether `sustain` mode is wired.
- The second input is `freq` in Hz.
- Subsequent inputs are voice-specific (tone, decay, structure, etc.).
- Output is mono. Stereo placement happens via Cedar's existing `pan()` after the voice.

The existing POLY block's per-voice trigger fan-out (gate/freq/vel/trig wiring from a pattern) maps directly onto these signatures.

### 5.5 Stateless modules

`crossfade`, `dust` (no internal state beyond an RNG seed), `whitenoise` (skipped), and a few utilities are stateless. These follow the existing nkido convention of skipping `StatePool` allocation. `dust` is a borderline case — it has a small RNG state — so it gets a state struct.

### 5.6 The `ds_drum()` macro

`ds_drum()` is a **special-handler builtin** in the codegen (mirrors `pat()`, `wt_load()`):

1. Parse the mini-notation pattern at compile time using existing `MiniNotationAst`.
2. For each unique drum symbol (`bd`, `sd`, `hh`, `hh_open`, `cp`, etc.), emit one DaisySP voice opcode with a default preset.
3. Wire each voice's `trig` input to a `pat()`-driven trigger that fires on the right step.
4. Sum all voice outputs into a single buffer with `ADD` opcodes.
5. Optional record argument override per-voice (uses the existing record-spread pattern from `prd-record-argument-spread.md`):
   ```akkado
   ds_drum("bd hh*4 sd?", { bd: { freq: 60, decay: 0.7 } })
   ```

This is purely a compile-time expansion. Runtime is identical to writing the chain by hand.

---

## 6. Vendoring and Build Integration

### 6.1 Repository layout

```
cedar/
├── third_party/
│   ├── kissfft/              (existing)
│   └── daisysp/              (NEW)
│       ├── LICENSE           (MIT, copied from upstream)
│       ├── README.md         (note: vendored from electro-smith/DaisySP commit <sha>)
│       └── Source/           (mirror of upstream Source/, MIT modules only)
```

- The `DaisySP-LGPL/` submodule is **not** mirrored. Verify before and after vendoring (`grep -r LGPL cedar/third_party/daisysp/` must return nothing).
- A `THIRDPARTY_NOTICES.md` at the repo root lists DaisySP, the upstream commit hash, and the modules used. Updated when we re-vendor.

### 6.2 CMake integration

DaisySP's upstream CMakeLists is intentionally minimal (it expects to be embedded). We add a new file `cmake/daisysp.cmake` that:

```cmake
# cmake/daisysp.cmake
add_library(daisysp STATIC
    cedar/third_party/daisysp/Source/Drums/analogbassdrum.cpp
    cedar/third_party/daisysp/Source/Drums/synthbassdrum.cpp
    # ... one .cpp per included module
)
target_include_directories(daisysp PUBLIC
    cedar/third_party/daisysp/Source
)
target_compile_features(daisysp PUBLIC cxx_std_17)
```

Cedar links `daisysp` privately. The library is built once; each opcode wrapper `#include`s the relevant DaisySP header.

### 6.3 Per-source attribution

Every Cedar opcode wrapper file gets a header comment:

```cpp
// cedar/include/cedar/opcodes/daisy.hpp
//
// Wrappers around DaisySP modules (MIT, https://github.com/electro-smith/DaisySP).
// Several modules are ports of pichenettes/eurorack/plaits by Emilie Gillet,
// re-licensed under MIT in the DaisySP/Source/ tree. See third_party/daisysp/LICENSE.
```

DaisySP source headers retain their original copyright notices unchanged — they are vendored as-is.

### 6.4 WASM build budget

WASM is the primary distribution channel for the web IDE. Each new opcode adds bytes; the budget (uncompressed):

| Category | Estimated module count | Estimated WASM growth | Notes |
|---|---|---|---|
| Drums | 5 | ~12 KB | Each drum is small (~110 lines, simple math) |
| Physical modeling | 5 | ~25 KB | ModalVoice + Resonator are the heaviest (FFT-free, but multi-band) |
| Synthesis voices | 7 | ~30 KB | HarmonicOscillator and OscillatorBank are larger (lookup tables) |
| Noise / utility / effects | ~13 | ~25 KB | PitchShifter and GranularPlayer dominate |
| Filters | 2 (FIR + SOAP) | ~5 KB | |
| **Total budget** | **~32 modules** | **~100 KB compressed** | Hard cap: **150 KB compressed gzip** added to the WASM bundle. If exceeded, drop lowest-priority modules per Phase. |

Implementation MUST measure the bundle delta after each phase and report it. If the cumulative growth exceeds 150 KB compressed, the next phase is paused for a scope review.

---

## 7. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar/include/cedar/vm/instruction.hpp` (Opcode enum) | **Modified** | Add `DAISY_*` opcodes in a new range (e.g. 210-255 — see §8.1) |
| `cedar/include/cedar/dsp_state.hpp` | **Modified** | Add per-module state structs |
| `cedar/include/cedar/opcodes/daisy.hpp` | **New** | Block-loop wrappers for every DaisySP opcode |
| `cedar/include/cedar/opcodes/opcodes.hpp` | **Modified** | Include `daisy.hpp` in dispatch switch |
| `cedar/third_party/daisysp/` | **New** | Vendored DaisySP source (MIT-only) |
| `cmake/daisysp.cmake` | **New** | Build DaisySP as a static library |
| `CMakeLists.txt` (cedar) | **Modified** | Link `daisysp` privately |
| `akkado/include/akkado/builtins.hpp` | **Modified** | New builtin entries for each Akkado name |
| `akkado/src/codegen_patterns.cpp` | **Modified** | Special handler for `ds_drum()` |
| `akkado/src/codegen.cpp` | **Modified** | Wire DaisySP voice builtins through generic dispatch |
| `web/wasm/nkido_wasm.cpp` | **Stays** | No changes — the WASM build picks up new opcodes via the existing `daisy.hpp` include and the autogenerated opcode metadata |
| `web/scripts/build-opcodes.ts` | **Stays** | The autogen script (`bun run build:opcodes`) automatically picks up the new opcode names. Run it once after each phase. |
| `cedar/include/cedar/generated/opcode_metadata.hpp` | **Regenerated** | Via `bun run build:opcodes` |
| `web/static/patches/` | **Modified** | New demo patches per category (see §10) |
| `THIRDPARTY_NOTICES.md` | **New** | Repo-root notice file |
| `docs/dsp-issues.md` | **Modified** | Open issues for any modules that fail comparison tests |
| Existing nkido opcodes | **Stays** | No removals, no behavior changes |
| `cedar/third_party/daisysp/DaisySP-LGPL/` | **Excluded** | Must NOT be vendored. Verify with grep. |

---

## 8. File-Level Changes

### 8.1 Cedar opcode enum allocation

| Range | Category | Notes |
|---|---|---|
| 210-219 | Daisy drums | bd_analog, bd_synth, sd_analog, sd_synth, hh, (room for cp, tom) |
| 220-229 | Daisy physical modeling | modal, resonator, drip, string_voice, karplus, (room) |
| 230-239 | Daisy synthesis | fm2, formant_osc, harmonic_osc, osc_bank, vosim, zosc, var_shape |
| 240-247 | Daisy noise / particle | dust, pink_noise, particle, grainlet, clocked_noise |
| 248-251 | Daisy effects | pitch_shift, autowah, ds_chorus, ds_flanger, ds_phaser, ds_fold, ds_overdrive |
| 252-253 | Daisy filters | fir, soap |
| 254 | Daisy granular / looper | granular, looper |
| (full 8-bit) | — | Note: `INVALID = 255` is reserved. If we run out of space, allocate from a 9-bit opcode field in a follow-up PRD. Keep an opcode-allocation budget in mind. |

### 8.2 New files

| File | Purpose |
|---|---|
| `cedar/third_party/daisysp/Source/...` | Vendored upstream tree (MIT only) |
| `cedar/third_party/daisysp/LICENSE` | Upstream MIT license |
| `cedar/third_party/daisysp/VENDORED.md` | Commit hash, vendoring date, list of included modules |
| `cedar/include/cedar/opcodes/daisy.hpp` | All DaisySP opcode wrappers in one file |
| `cmake/daisysp.cmake` | Build config for DaisySP as a static library |
| `THIRDPARTY_NOTICES.md` | Repo-root third-party notices |
| `web/static/patches/daisy-drums.akk` | Demo: `ds_drum()` and individual drum voices |
| `web/static/patches/daisy-physical.akk` | Demo: modal bells, plucked strings |
| `web/static/patches/daisy-synthesis.akk` | Demo: FM2, harmonic, vosim, zosc |
| `web/static/patches/daisy-noise-fx.akk` | Demo: pink noise, dust, pitch shifter, autowah |
| `akkado/tests/test_daisy_codegen.cpp` | Akkado integration tests for every new builtin |

### 8.3 Modified files

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | Add `DAISY_*` opcodes per §8.1 |
| `cedar/include/cedar/dsp_state.hpp` | Add per-module state structs |
| `cedar/include/cedar/opcodes/opcodes.hpp` | `#include "daisy.hpp"` in dispatch |
| `cedar/CMakeLists.txt` | `include(cmake/daisysp.cmake)`; link `daisysp` private |
| `akkado/include/akkado/builtins.hpp` | One entry per new Akkado builtin |
| `akkado/src/codegen.cpp` | Generic dispatch handles the new builtins (most need no special handling) |
| `akkado/src/codegen_patterns.cpp` | Special handler for `ds_drum()` |
| `web/scripts/build-opcodes.ts` | Verify it picks up new opcodes; no script change expected |

---

## 9. Implementation Phases

Each phase ends with: code merged, demo patch live, comparison tests passing, WASM bundle delta reported.

### Phase 1 — Build infrastructure

**Goal:** vendor DaisySP, get one trivial module compiling end-to-end, prove the wrapping approach.

**Files:**
- New: `cedar/third_party/daisysp/`, `cmake/daisysp.cmake`, `THIRDPARTY_NOTICES.md`, `cedar/include/cedar/opcodes/daisy.hpp` (skeleton)
- Modified: `cedar/CMakeLists.txt`, `cedar/include/cedar/vm/instruction.hpp` (one opcode), `cedar/include/cedar/dsp_state.hpp`, `akkado/include/akkado/builtins.hpp`

**Deliverable:** `dust(density)` builtin works end-to-end. Smallest possible DaisySP module proves the pipeline.

**Acceptance:**
- `cmake --preset debug && cmake --build build` succeeds.
- `bun run build:opcodes` regenerates `opcode_metadata.hpp` cleanly.
- `bun run build` (web) succeeds; WASM bundle delta < 5 KB.
- New Akkado test: `dust(40)` parses, compiles, renders without crash.

### Phase 2 — Drum voices

**Goal:** five drum voices + `ds_drum()` macro, idiomatic for live coding.

**Modules:** `bd_analog`, `bd_synth`, `sd_analog`, `sd_synth`, `hh` (with `hh_open`/`hh_closed` aliases), plus the special-handler `ds_drum()` macro.

**Acceptance:**
- All five voices have Akkado integration tests.
- Demo patch `daisy-drums.akk` plays `ds_drum("bd:sd:hh:hh")` cleanly.
- WASM bundle delta < 20 KB cumulative.
- A/B comparison: `ds_drum("bd")` vs. an `osc("sin", 60) |> ar(...)` baseline. WAV files saved to `experiments/output/daisy_drums/`.

### Phase 3 — Physical modeling

**Goal:** modal, resonator, drip, string_voice, karplus.

**Modules:** five physical modeling voices, all polyphony-compatible.

**Acceptance:**
- All five have Akkado integration tests.
- Demo patch `daisy-physical.akk`: a modal bell pattern, a plucked-string lead, a drip percussion line. Polyphony works inside a `poly { ... }` block.
- WASM bundle delta < 50 KB cumulative.

### Phase 4 — Synthesis voices

**Goal:** fm2, formant_osc, harmonic_osc, osc_bank, vosim, zosc, var_shape.

**Acceptance:**
- All seven have Akkado integration tests.
- Demo patch `daisy-synthesis.akk` exercises FM, additive, vocal, and sync timbres.
- WASM bundle delta < 80 KB cumulative.

### Phase 5 — Noise / utility / effects / filters / granular

**Goal:** everything else (dust shipped earlier in Phase 1; rest in this phase).

**Modules:** `pink_noise`, `particle`, `grainlet`, `clocked_noise`, `pitch_shift`, `autowah`, `ds_chorus`, `ds_flanger`, `ds_phaser`, `ds_fold`, `ds_overdrive`, `fir`, `soap`, `granular`, `looper`, `maytrig`, `smooth_rand`.

**Acceptance:**
- All have Akkado integration tests.
- Demo patches `daisy-noise-fx.akk` and `daisy-granular.akk`.
- WASM bundle delta < 150 KB cumulative (hard cap).
- Side-by-side A/B for `ds_chorus` vs. `chorus`, `ds_flanger` vs. `flanger`, `ds_phaser` vs. `phaser`, `ds_fold` vs. `fold`, `ds_overdrive` vs. `tube`/`tanh`. Findings recorded in a follow-up triage PRD.

### Phase 6 (future / not in scope) — Replacement decisions

After Phase 5 ships and users have lived with both versions, a follow-up PRD evaluates whether any `ds_*` alternative should replace its nkido counterpart (or vice versa). This PRD does not pre-decide.

---

## 10. Testing and Verification

Per the user's choice in Round 3, every new module gets:

### 10.1 Akkado integration test (required)

In `akkado/tests/test_daisy_codegen.cpp`:

```cpp
TEST_CASE("bd_analog parses and emits DAISY_BD_ANALOG", "[daisy][drums]") {
    auto bytecode = compile("bd_analog(trig(4), 60, 0.5, 0.3, 0.1, 0.2, 0.5) |> out(@, @)");
    REQUIRE(bytecode_contains_opcode(bytecode, Opcode::DAISY_BD_ANALOG));
}

TEST_CASE("ds_drum mini-notation expands to drum voices", "[daisy][drums][ds_drum]") {
    auto bytecode = compile("ds_drum(\"bd:sd:hh:hh\") |> out(@, @)");
    REQUIRE(bytecode_contains_opcode(bytecode, Opcode::DAISY_BD_ANALOG));
    REQUIRE(bytecode_contains_opcode(bytecode, Opcode::DAISY_SD_ANALOG));
    REQUIRE(bytecode_contains_opcode(bytecode, Opcode::DAISY_HH));
}

TEST_CASE("modal renders without NaN/Inf for 5 seconds", "[daisy][physical]") {
    auto rendered = render_seconds("modal(trig(4), 220, 0.5, 0.5, 0.5, 0.5)", 5.0);
    REQUIRE(no_nans(rendered));
    REQUIRE(rms(rendered) > 0.01f);   // it's audibly producing sound
}
```

### 10.2 Demo patches (required)

Each phase ships at least one `web/static/patches/daisy-*.akk` patch demonstrating the new modules in a musical context. Demo patches must:
- Compile cleanly under the production build.
- Render audibly via `nkido-cli render`.
- Be linked from the web IDE's pattern picker.

### 10.3 Side-by-side A/B comparison (required for `ds_*`)

For each `ds_*` alternative, a comparison test renders both versions with identical input and saves both WAVs to `experiments/output/daisy_compare/`:

```python
# experiments/test_compare_chorus.py
from cedar_testing import CedarTestHost, output_dir
OUT = output_dir("daisy_compare/chorus")

def test_chorus_vs_ds_chorus():
    """Render same input through chorus() and ds_chorus(). Save both WAVs.
       Listen for: smoothness of LFO, brightness, stereo width."""
    # ... setup, render, save WAVs ...
```

Findings (qualitative + measured CPU usage) are written into a comparison report appended to `docs/dsp-issues.md`.

### 10.4 Edge cases

| Case | Expected behavior |
|---|---|
| `ds_drum("")` (empty pattern) | Compile error E1xx — pattern must have ≥1 step |
| `ds_drum("zz")` (unknown drum name) | Compile error E1xx — listing supported names |
| `bd_analog(0, 60, ...)` (no trigger ever) | Outputs silence; no crash; CPU floor unchanged |
| `bd_analog(trig=fast)` (trigger rate > sample rate) | Voice retriggers per sample; engine handles gracefully (DaisySP's internal envelope is robust) |
| `pitch_shift(input, 100)` (extreme transposition) | Clamped to ±24 semitones (DaisySP's documented range); test asserts no NaN |
| `granular(uri="bad://...", ...)` (unloadable sample) | Loader returns failure; opcode emits silence; runtime warning logged |
| `looper` recording longer than buffer | Wrap to oldest sample (DaisySP behavior); document limit |
| Hot-swap with same `state_id` for any voice | Phase, envelope, RNG state preserved |
| Hot-swap with changed `state_id` (renamed in source) | New state allocated; old GC'd after N blocks (existing Cedar behavior) |
| `modal` inside `poly { }` block | Voice allocation per active note works; no state leakage between voices |
| Sample rate mismatch (project at non-48 kHz) | DaisySP's `Init(sr)` is called with Cedar's actual sample rate at first dispatch; verify in Phase 1 |
| WASM build with all 32 opcodes enabled | Bundle stays within 150 KB compressed budget; otherwise scope review |
| LGPL submodule accidentally vendored | CI grep check on `cedar/third_party/daisysp/` flags any LGPL string and fails the build |

### 10.5 CI guard

Add a CI check that:
1. `grep -r "LGPL\|GPL" cedar/third_party/daisysp/` returns no matches (only MIT vendored).
2. WASM bundle size delta vs. main < 150 KB (gzipped).
3. All `daisy-*.akk` demo patches compile.

---

## 11. Open Questions

- **[OPEN]** `harmonic_osc` and `osc_bank` accept a magnitudes array. Akkado's existing array support (`ARRAY_PACK` etc.) can carry up to 128 elements but DaisySP's HarmonicOscillator is templated on `<int kNumHarmonics>` (default 16). Decide the fixed N at vendoring time. Default proposal: N=16.
- **[OPEN]** `granular()` URI scheme: reuse the existing `cedar::UriResolver` (per `prd-uri-resolver.md`) or define a new one? Proposal: reuse.
- **[OPEN]** `looper` write buffer size: DaisySP defaults to several seconds at 48 kHz. Pick a fixed, documented size — e.g. 8 seconds — since Cedar avoids dynamic allocation.
- **[OPEN]** `ds_drum()` mini-notation extension: the existing `pat()` mini-notation supports `*`, `?`, rest, etc. — does `ds_drum()` reuse `pat()` parsing or fork it? Proposal: reuse `pat()` parsing wholesale; the special handler only differs in *what each step compiles to*.
- **[OPEN]** Should drums emit stereo with a built-in pan? Proposal: no — they're mono; users pan with `pan()` after.
- **[OPEN]** Whether to expose DaisySP's internal helpers (`Source/Utility/dsp.h` softclip / fold / etc.) as Akkado primitives. Proposal: skip; we have equivalents.
