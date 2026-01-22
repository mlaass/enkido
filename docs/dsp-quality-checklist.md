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
- **Timing accuracy**: Attack/decay/release times match within 5%
- **Curve shape**: Exponential vs linear matches specification
- **Retrigger behavior**: Handles rapid triggers without glitches

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

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `FILTER_BIQUAD_LP` | High | Standard biquad lowpass response |
| `FILTER_BIQUAD_HP` | High | Standard biquad highpass response |
| `FILTER_BIQUAD_BP` | High | Standard biquad bandpass response |
| `FILTER_BIQUAD_NOTCH` | Medium | Notch depth and width |
| `FILTER_BIQUAD_PEAK` | Medium | Peak gain and Q |
| `FILTER_BIQUAD_LSHELF` | Medium | Low shelf frequency and gain |
| `FILTER_BIQUAD_HSHELF` | Medium | High shelf frequency and gain |
| `FILTER_DIODE` | Medium | Diode ladder character, self-oscillation |
| `FILTER_COMB` | Medium | Delay time, feedback, resonance |
| `FILTER_ALLPASS` | Low | Phase response, group delay |

---

## Effects

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `DISTORT_TANH` | ✅ Tested | Transfer curve | Soft saturation |
| `DISTORT_SOFT` | ✅ Tested | Transfer curve | Polynomial soft clip |
| `DISTORT_FOLD` | ✅ Tested | Transfer curve | Wavefolding |
| `DISTORT_TUBE` | ✅ Tested | Transfer curve | Asymmetric tube simulation |
| `EFFECT_PHASER` | ✅ Tested | Modulation spectrogram | Allpass sweep |
| `REVERB_DATTORRO` | ✅ Tested | Impulse response, decay analysis | Plate-style reverb |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `DISTORT_BITCRUSH` | High | Bit depth reduction, sample rate reduction |
| `DISTORT_SMOOTH` | Medium | Transfer curve, harmonic content |
| `DISTORT_TAPE` | Medium | Saturation curve, hysteresis |
| `DISTORT_XFMR` | Low | Transformer saturation character |
| `DISTORT_EXCITE` | Low | Harmonic enhancement spectrum |
| `EFFECT_CHORUS` | High | Modulation depth, detune amount, stereo spread |
| `EFFECT_FLANGER` | High | Delay time, feedback, modulation |
| `EFFECT_COMB` | Medium | Delay time accuracy, feedback |

---

## Delays & Reverbs

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `REVERB_DATTORRO` | ✅ Tested | Impulse response, RT60 | Plate algorithm |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `DELAY` | High | Delay time accuracy, feedback, interpolation quality |
| `REVERB_FREEVERB` | High | Impulse response, room size, damping |
| `REVERB_FDN` | Medium | Feedback delay network, decay, diffusion |
| `REVERB_SPRING` | Low | Spring character, drip effect |
| `REVERB_SHIMMER` | Low | Pitch shifting quality, modulation |

---

## Samplers

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `SAMPLE_PLAY` | ✅ Tested | Pitch accuracy, interpolation, timing | One-shot playback |
| `SAMPLE_PLAY_LOOP` | ✅ Tested | Loop discontinuity, timing drift | Looping mode |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `SAMPLE_GRANULAR` | High | Grain size, overlap, pitch/time independence |
| `SAMPLE_KARPLUS` | Medium | Excitation, damping, pitch tracking |

---

## Envelopes

### Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `ENV_ADSR` | ✅ Tested | Timing accuracy, curve shape, gate behavior, retrigger | Full ADSR lifecycle |
| `ENV_AR` | ✅ Tested | Attack/release timing, curve shape | Simplified envelope |
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

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `DYNAMICS_COMP` | High | Threshold, ratio, attack/release, makeup gain |
| `DYNAMICS_LIMITER` | High | Threshold, lookahead, release |
| `DYNAMICS_GATE` | High | Threshold, attack/release, hysteresis |

---

## Utility

### Partially Tested

| Opcode | Status | Test Coverage | Notes |
|--------|--------|---------------|-------|
| `NOISE` | ⚠️ Used | Used as helper in other tests | Not directly tested for distribution |

### Untested

| Opcode | Priority | Suggested Tests |
|--------|----------|-----------------|
| `MTOF` | Medium | MIDI to frequency accuracy across range |
| `DC` | Low | Constant value accuracy |
| `SLEW` | Medium | Rise/fall time accuracy, limiter behavior |
| `SAH` | Medium | Sample timing, hold accuracy |
| `ENV_GET` | Low | Envelope value extraction |

---

## Test Coverage Summary

| Category | Tested | Untested | Coverage |
|----------|--------|----------|----------|
| Oscillators | 11 | 9 | 55% |
| Filters | 4 | 10 | 29% |
| Effects | 6 | 8 | 43% |
| Delays & Reverbs | 1 | 4 | 20% |
| Samplers | 2 | 2 | 50% |
| Envelopes | 3 | 0 | 100% |
| Sequencers & Timing | 4 | 2 | 67% |
| Dynamics | 0 | 3 | 0% |
| Utility | 1 | 5 | 17% |
| **Total** | **32** | **43** | **43%** |

---

## Priority Action Items

### High Priority (Core Musical Functionality)
1. `DELAY` - Fundamental effect
2. `REVERB_FREEVERB` - Alternative to Dattorro
3. `DYNAMICS_COMP`, `DYNAMICS_LIMITER` - Mixing essentials
4. `SEQ_STEP` - Step sequencer timing
5. `EFFECT_CHORUS`, `EFFECT_FLANGER` - Common modulation effects
6. `DISTORT_BITCRUSH` - Popular effect

### Medium Priority (Extended Functionality)
1. `OSC_RAMP`, `OSC_PHASOR` - Modulation sources
2. `FILTER_DIODE` - Alternative filter character
3. `SLEW`, `SAH` - Control signal processing
4. `TIMELINE` - Event scheduling
5. Remaining distortion types

### Low Priority (Completeness)
1. 2x oversampling oscillator variants (already have 1x and 4x)
2. Biquad filter variants (already have SVF)
3. Utility opcodes (`DC`, `ENV_GET`)

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
