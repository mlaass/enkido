# DSP Quality Assurance Checklist

This document tracks the quality verification status of Cedar DSP opcodes. Each opcode should be tested for correctness, performance, and audio quality before being considered production-ready.

## Quality Criteria by Category

### Oscillators
- **Frequency accuracy**: Output frequency matches input within 0.1%
- **Aliasing**: High frequencies fold below Nyquist without audible artifacts
- **DC offset**: Output centered around zero (< 0.001 DC component)
- **Harmonic purity**: Spectrum matches theoretical waveform harmonics
- **Phase continuity**: No discontinuities during frequency modulation

### Filters
- **Frequency response**: Cutoff matches specification within 1dB
- **Resonance behavior**: Q factor produces expected peak
- **Self-oscillation**: High resonance produces clean sine (where applicable)
- **Stability**: No blowups at extreme settings

### Effects
- **Wet/dry blend**: Clean signal path when dry
- **Modulation smoothness**: No zipper noise on parameter changes
- **Impulse response**: Decay characteristics match design

### Envelopes
- **Timing accuracy**: Attack/decay/release times match within 1% (or ±5 samples)
- **Curve shape**: Exponential vs linear matches specification
- **Retrigger behavior**: Handles rapid triggers without glitches
- **Sample accuracy**: All timing parameters read from buffer inputs for per-sample precision

### Dynamics
- **Threshold accuracy**: Compression/gating activates at specified level
- **Attack/release**: Timing matches specification
- **Gain reduction**: Ratio produces expected output levels

---

## Oscillators

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `OSC_SIN` | ✅ Tested | Frequency accuracy, FM aliasing | Basic sine, good quality |
| `OSC_SIN_2X` | ✅ Tested | 2x oversampling FM test | Reduced aliasing vs 1x |
| `OSC_SIN_4X` | ✅ Tested | 4x oversampling FM test | Minimal aliasing |
| `OSC_TRI` | ✅ Tested | Waveform shape, harmonic content | PolyBLEP anti-aliasing |
| `OSC_SAW` | ✅ Tested | Waveform, harmonics, aliasing | PolyBLEP anti-aliasing |
| `OSC_SQR` | ✅ Tested | DC offset, harmonics, PolyBLEP comparison | Extensive quality tests |
| `OSC_SQR_MINBLEP` | ✅ Tested | Compared against PolyBLEP | Higher quality, more CPU |
| `OSC_SQR_PWM` | ✅ Tested | Duty cycle accuracy (10%-90%) | PWM range verified |
| `OSC_SQR_PWM_4X` | ✅ Tested | PWM + FM aliasing | 4x oversampling |
| `OSC_SAW_PWM` | ✅ Tested | Variable slope | PolyBLEP PWM variant |
| `OSC_SAW_PWM_4X` | ✅ Tested | PWM + FM, 4x oversampling | High quality |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `OSC_RAMP` | Medium | Frequency accuracy, DC offset, comparison with SAW |
| `OSC_PHASOR` | Medium | Linearity, wraparound behavior, sync compatibility |
| `OSC_SQR_PWM_MINBLEP` | Low | Compare MinBLEP vs PolyBLEP PWM quality |
| `OSC_SAW_2X` | Low | 2x oversampling quality vs 1x/4x |
| `OSC_SQR_2X` | Low | 2x oversampling quality vs 1x/4x |
| `OSC_TRI_2X` | Low | 2x oversampling quality vs 1x/4x |
| `OSC_SAW_4X` | Low | 4x oversampling, FM test |
| `OSC_SQR_4X` | Low | 4x oversampling, FM test |
| `OSC_TRI_4X` | Low | 4x oversampling, FM test |

---

## Filters

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `FILTER_SVF_LP` | ✅ Tested | Frequency response, resonance sweep | State-variable lowpass |
| `FILTER_SVF_HP` | ✅ Tested | Frequency response, resonance sweep | State-variable highpass |
| `FILTER_SVF_BP` | ✅ Tested | Frequency response, resonance sweep | State-variable bandpass |
| `FILTER_MOOG` | ✅ Tested | Resonance sweep, self-oscillation | Classic ladder character |
| `FILTER_DIODE` | ✅ Tested | Frequency response, resonance sweep, self-oscillation, Moog comparison | ZDF diode ladder (TB-303 style) |
| `FILTER_FORMANT` | ✅ Tested | Vowel accuracy (A/I/U/E/O), morph smoothness spectrogram | 3-band parallel vowel filter |
| `FILTER_SALLENKEY` | ✅ Tested | LP/HP modes, self-oscillation, diode character analysis | MS-20 style with diode feedback |

*All implemented filter opcodes are now tested.*

---

## Effects

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `DISTORT_TANH` | ✅ Tested | Transfer curve | Soft clipping saturation |
| `DISTORT_SOFT` | ✅ Tested | Transfer curve | Gentle saturation |
| `DISTORT_FOLD` | ✅ Tested | Transfer curve, ADAA aliasing analysis, symmetry effect, continuity | Sine wavefolder with ADAA antialiasing |
| `DISTORT_TUBE` | ✅ Tested | Transfer curve | Tube-style harmonics |
| `DISTORT_BITCRUSH` | ✅ Tested | Bit depth levels, sample rate reduction | Quantization + aliasing |
| `EFFECT_PHASER` | ✅ Tested | Spectrogram sweep | 6-stage phaser |
| `EFFECT_CHORUS` | ✅ Tested | Spectral spread, sideband analysis | 0.71 spread ratio |
| `EFFECT_FLANGER` | ✅ Tested | Spectrogram sweep pattern | Comb filter notches |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `DISTORT_SMOOTH` | Medium | Transfer curve, harmonic content |
| `DISTORT_TAPE` | Medium | Saturation curve, hysteresis |
| `DISTORT_XFMR` | Low | Transformer saturation character |
| `DISTORT_EXCITE` | Low | Harmonic enhancement spectrum |
| `EFFECT_COMB` | Medium | Delay time accuracy, feedback |

---

## Delays & Reverbs

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `DELAY` | ✅ Tested | Delay time accuracy (0.00% error), feedback decay (-6dB/echo) | 4800 samples @ 100ms |
| `REVERB_DATTORRO` | ✅ Tested | Impulse response, decay analysis | Long tail reverb |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `REVERB_FREEVERB` | High | Impulse response, room size, damping |
| `REVERB_FDN` | Medium | Feedback delay network, decay, diffusion |

---

## Samplers

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `SAMPLE_PLAY` | ✅ Tested | Pitch accuracy, interpolation, timing | One-shot playback |
| `SAMPLE_PLAY_LOOP` | ✅ Tested | Loop discontinuity, timing drift | Looping mode |

*All implemented sampler opcodes have been tested.*

---

## Envelopes

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `ENV_ADSR` | ✅ Tested | Timing accuracy (<1% error), curve shape, gate behavior, retrigger, sample-accurate release (10ms-500ms) | Full ADSR lifecycle, 5-input format with buffer-based release |
| `ENV_AR` | ✅ Tested | Attack/release timing (<1% error), curve shape, sample accuracy | Simplified envelope, ±5 samples precision |
| `ENV_FOLLOWER` | ✅ Tested | Attack/release response, signal tracking | Amplitude following |

---

## Sequencers & Timing

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `CLOCK` | ✅ Tested | Phase accuracy, BPM sync, long-term drift | 0 samples drift over 100 beats |
| `LFO` | ✅ Tested | All shapes, frequency sync, PWM duty, zero-crossing precision | Direct phase calculation, 0 drift |
| `EUCLID` | ✅ Tested | Pattern accuracy, step timing precision | 0 samples timing error |
| `TRIGGER` | ✅ Tested | Division accuracy, long-term precision, cross-opcode alignment | ≤1 sample error over 1000 beats |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `SEQ_STEP` | High | Step timing, value interpolation |
| `TIMELINE` | Medium | Event scheduling accuracy |

---

## Dynamics

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `DYNAMICS_COMP` | ✅ Tested | Transfer curve, threshold -20dB, ratio 4:1, max error 1.7dB | Compressor working |
| `DYNAMICS_LIMITER` | ✅ Tested | Ceiling enforcement, transient handling, 0dB overshoot | Limiter working |
| `DYNAMICS_GATE` | ✅ Tested | Passes loud signals, hysteresis OK, attenuation speed verified | Fast 5ms gate close, full attenuation within 200ms |

---

## Utility

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `NOISE` | ✅ Tested | Distribution (uniform), mean/std 0.577, spectral flatness 0.02dB variance | White noise generator |
| `MTOF` | ✅ Tested | MIDI to frequency accuracy (0-127 range), <0.00002% error | Note to Hz conversion |
| `SAH` | ✅ Tested | Trigger timing, hold value stability | Sample-and-hold working |
| `SLEW` | ✅ Tested | Rise/fall timing, rate limiting | <0.1% timing error, sample-accurate |
| `DC` | ✅ Tested | Constant offset accuracy | Fixed memcpy bug (was reading 32-bit from 16-bit field) |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `ENV_GET` | Low | Envelope value extraction |

---

## Test Coverage Summary

| Category | Tested | Partial/Bug | Untested | Notes |
|----------|--------|-------------|----------|-------|
| Oscillators | 11 | - | 9 | Via test_oscillators.py, test_fm_aliasing.py |
| Filters | 7 | - | 0 | Via test_filters.py; all filters now tested |
| Effects | 8 | - | 5 | Via test_effects.py; DISTORT_FOLD ADAA tested |
| Delays & Reverbs | 2 | - | 2 | Via test_effects.py |
| Samplers | 2 | - | 0 | Via test_sampler.py |
| Envelopes | 3 | - | 0 | Via test_envelopes.py |
| Sequencers & Timing | 4 | - | 2 | Via test_sequencers.py |
| Dynamics | 3 | - | 0 | Via test_dynamics.py |
| Utility | 5 | - | 1 | Via test_utility.py |
| **Total** | **45** | **-** | **16** | 74% tested |

---

## Priority Action Items

### High Priority - Write Tests (Opcodes Available)
1. `SEQ_STEP` - Step sequencer timing (opcode exists)
2. `TIMELINE` - Event scheduling (opcode exists)
3. `REVERB_FREEVERB` - Impulse response testing

### Medium Priority (Extended Functionality)
1. `OSC_RAMP`, `OSC_PHASOR` - Modulation sources (opcodes exist, need tests)
2. `REVERB_FDN` - Feedback delay network testing
3. Remaining distortion types (`DISTORT_SMOOTH`, `DISTORT_TAPE`, `DISTORT_XFMR`, `DISTORT_EXCITE`)
4. `EFFECT_COMB` - Comb filter testing

### Low Priority (Completeness)
1. 2x oversampling oscillator variants (already have 1x and 4x)
2. 4x oversampling variants for SAW, SQR, TRI
3. Utility opcodes (`ENV_GET`)

---

## Not Implemented / Planned

These opcodes are not yet implemented but may be added in the future.

### Filters

| Opcode | Notes |
|--------|-------|
| `FILTER_BIQUAD_LP` | Standard biquad lowpass (deprecated in favor of SVF) |
| `FILTER_BIQUAD_HP` | Standard biquad highpass |
| `FILTER_BIQUAD_BP` | Standard biquad bandpass |
| `FILTER_BIQUAD_NOTCH` | Notch filter |
| `FILTER_BIQUAD_PEAK` | Peaking EQ |
| `FILTER_BIQUAD_LSHELF` | Low shelf EQ |
| `FILTER_BIQUAD_HSHELF` | High shelf EQ |
| `FILTER_COMB` | Comb filter (feedforward) |
| `FILTER_ALLPASS` | Allpass filter |

### Reverbs

| Opcode | Notes |
|--------|-------|
| `REVERB_SPRING` | Spring reverb emulation |
| `REVERB_SHIMMER` | Pitch-shifted reverb |

### Samplers

| Opcode | Notes |
|--------|-------|
| `SAMPLE_GRANULAR` | Granular synthesis |
| `SAMPLE_KARPLUS` | Karplus-Strong string synthesis |

### Synths

| Opcode | Notes |
|--------|-------|
| `SMOOCH` | Wavetable oscillator with mip-mapped anti-aliasing and Hermite interpolation. See [PRD](smooch_wavetable_synth_prd.md) |

---

## Testing Guidelines

### Creating New Tests

Tests should be added to `cedar/tests/experiments/` following the existing pattern:

```cpp
TEST_CASE("OPCODE_NAME quality test", "[opcode][category]") {
    // 1. Setup: Create VM, load program
    // 2. Generate: Run for sufficient samples (e.g., 48000 for 1 second)
    // 3. Analyze: FFT, statistics, timing measurements
    // 4. Assert: Compare against expected values with tolerances
    // 5. (Optional) Export WAV for manual inspection
}
```

### Analysis Tools Available
- FFT spectrum analysis
- DC offset measurement
- RMS level calculation
- Zero-crossing frequency detection
- WAV file export for auditory verification

### Naming Convention
- Test files: `test_<category>_<opcode>.cpp`
- Test cases: `"<OPCODE> <aspect> test"`
- Tags: `[opcode]`, `[oscillator]`, `[filter]`, `[effect]`, etc.
