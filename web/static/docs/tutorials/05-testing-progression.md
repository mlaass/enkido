---
title: Testing Progression
description: Systematic test progression from basic synthesis to complex patterns
order: 5
category: tutorials
keywords: [testing, patterns, synthesis, triggers, envelopes, samples, filters, moog, diode, sallenkey, formant, distortion, saturate, bitcrush, fold, tube, tape, xfmr, excite, chorus, flanger, phaser, comb, reverb, freeverb, dattorro, fdn, delay, comp, limiter, gate, slew, sah, lfo, env_follower]
---

# Pattern & Synth Integration Test Document

Work through each level in order. Report back which levels work, fail, or have issues.

---

## Part A: Basic Synthesis (No Patterns)

### A1: Basic Oscillator
```akkado
bpm = 120
osc("sin", 440) |> out(%, %)
```
Expected: Continuous 440Hz sine tone

### A2: AR Envelope with Trigger
```akkado
bpm = 120
osc("sin", 220) * ar(trigger(2), 0.01, 0.3) |> out(%, %)
```
Expected: Sine tone pulsing twice per beat (8th notes)

### A3: Saw with Lowpass Filter
```akkado
bpm = 120
osc("saw", 110) |> lp(%, 800) * ar(trigger(1), 0.01, 0.3) |> out(%, %)
```
Expected: Filtered saw pulsing once per beat (quarter notes)

### A4: ADSR Envelope (Positional Args)
```akkado
bpm = 120
g = trigger(1)
osc("saw", 110) * adsr(g, 0.01, 0.1, 0.5, 0.3) |> out(%, %)
```
Expected: Saw with attack-decay-sustain-release shape

### A5: ADSR with Named Args
```akkado
bpm = 120
g = trigger(1)
osc("saw", 110) * adsr(gate:g, attack:0.01, decay:0.1, sustain:0.5, release:0.3) |> out(%, %)
```
Expected: Same as A4, tests named argument syntax

### A6: Euclidean Rhythm (3 in 8)
```akkado
bpm = 120
osc("sin", 110) * ar(euclid(3, 8), 0.001, 0.1) |> out(%, %)
```
Expected: 3 hits spread evenly across 8 steps

### A7: Euclidean with Rotation
```akkado
bpm = 120
osc("sin", 110) * ar(euclid(3, 8, 2), 0.001, 0.1) |> out(%, %)
```
Expected: Same pattern as A6, rotated by 2 steps

### A8: Triangle Oscillator
```akkado
bpm = 120
osc("tri", 110) * ar(trigger(4), 0.001, 0.1) |> out(%, %)
```
Expected: Triangle wave pulsing on 16th notes

### A9: Power/Exponential Shaping
```akkado
bpm = 120
(osc("sin", 110) * ar(trigger(2), 0.01, 0.2)) ^ 2.0 |> out(%, %)
```
Expected: Squared envelope shape (sharper attack feel)

### A10: Bandpass Filter
```akkado
bpm = 120
osc("noise") |> bp(%, 1000, 4) * ar(trigger(4), 0.001, 0.05) |> out(%, %)
```
Expected: Filtered noise bursts (hi-hat-like)

---

## Part B: Sample Patterns

### B1: Single Sample
```akkado
bpm = 120
pat("bd") |> out(%, %)
```
Expected: Bass drum once per bar

### B2: Two Samples Alternating
```akkado
bpm = 120
pat("bd sd") |> out(%, %)
```
Expected: bd on beat 1-2, sd on beat 3-4 (2 sounds per bar)

### B3: Four-on-Floor Kick
```akkado
bpm = 120
pat("bd bd bd bd") |> out(%, %)
```
Expected: Kick on every beat

### B4: Pattern with Rests
```akkado
bpm = 120
pat("bd ~ sd ~") |> out(%, %)
```
Expected: bd, silence, sd, silence

### B5: Basic Drum Pattern
```akkado
bpm = 120
pat("bd hh sd hh") |> out(%, %)
```
Expected: bd, hihat, sd, hihat

### B6: More Complex Pattern
```akkado
bpm = 120
pat("bd hh hh hh sd hh hh hh") |> out(%, %)
```
Expected: 8-step pattern with kick on 1, snare on 5

### B7: Pattern with Grouping
```akkado
bpm = 120
pat("[bd bd] sd") |> out(%, %)
```
Expected: Two quick kicks in first half, snare in second half

### B8: Speed Modifier
```akkado
bpm = 120
pat("bd*4") |> out(%, %)
```
Expected: Four kicks (speed up pattern)

### B9: Slow Modifier
```akkado
bpm = 120
pat("bd/2") |> out(%, %)
```
Expected: Kick every 2 bars

---

## Part C: Pitch Patterns (No Closure)

### C1: Single Pitch
```akkado
bpm = 120
pat("c4") |> out(%, %)
```
Expected: Outputs frequency ~261Hz (unclear what sound source)

### C2: Four Pitches
```akkado
bpm = 120
pat("c4 e4 g4 c5") |> out(%, %)
```
Expected: Sequence of 4 frequencies

---

## Part D: Pitch Patterns with Closure

### D1: Closure with 1 Param (Trigger Only)
```akkado
bpm = 120
pat("c4 e4 g4 c5", (t) ->
    osc("saw", 440) * ar(t, 0.01, 0.2)
) |> out(%, %)
```
Expected: Saw at fixed 440Hz, triggered 4 times per bar

### D2: Closure with 3 Params
```akkado
bpm = 120
pat("c4 e4 g4 c5", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.2)
) |> out(%, %)
```
Expected: Saw using pitch from pattern (c4, e4, g4, c5)

### D3: Pattern + Filter in Closure
```akkado
bpm = 120
pat("c4 e4 g4 c5", (t, v, p) ->
    osc("saw", p) |> lp(%, 1500) * ar(t, 0.01, 0.2)
) |> out(%, %)
```
Expected: Filtered saw, 4 notes

### D4: Pattern with Rests
```akkado
bpm = 120
pat("c4 ~ e4 ~", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.2)
) |> out(%, %)
```
Expected: c4, silence, e4, silence (rests produce freq 0)

### D5: ADSR in Closure
```akkado
bpm = 120
pat("c4 e4 g4 c5", (t, v, p) ->
    osc("saw", p) * adsr(t, 0.01, 0.1, 0.5, 0.3)
) |> out(%, %)
```
Expected: Notes with ADSR shape

### D6: Closure Piped to Filter
```akkado
bpm = 120
pat("c3 e3 g3 c4", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.3)
) |> lp(%, 800) |> out(%, %)
```
Expected: Closure output piped through lowpass filter

---

## Part E: Pattern Modifiers

### E1: Grouping (Subdivision)
```akkado
bpm = 120
pat("[c4 e4] g4", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.1)
) |> out(%, %)
```
Expected: c4+e4 in first half (faster), g4 in second half

### E2: Speed Modifier on Group
```akkado
bpm = 120
pat("[c4 e4 g4]*2", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.1)
) |> out(%, %)
```
Expected: Pattern plays twice as fast (6 notes per bar)

### E3: Slow Modifier
```akkado
bpm = 120
pat("c4 e4 g4 c5/2", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.2)
) |> out(%, %)
```
Expected: Pattern spread over 2 bars

---

## Part F: Complex Examples

### F1: Synthesized Kick Drum
```akkado
bpm = 120
kick = osc("sin", 55 * (1 + ar(trigger(1), 0.001, 0.02) * 2)) * ar(trigger(1), 0.005, 0.2)
kick |> out(%, %)
```
Expected: 808-style synthesized kick

### F2: Synthesized Snare
```akkado
bpm = 120
snare = osc("noise") |> bp(%, 1000, 2) * ar(trigger(2), 0.001, 0.1) * 0.5
snare |> out(%, %)
```
Expected: Filtered noise snare on beats 2 and 4

### F3: Full Drum Kit
```akkado
bpm = 120
kick = osc("sin", 55 * (1 + ar(trigger(1), 0.001, 0.02) * 2)) * ar(trigger(1), 0.005, 0.2)
snare = osc("noise") |> bp(%, 1000, 2) * ar(euclid(2, 8, 4), 0.001, 0.1) * 0.5
hat = osc("noise") |> hp(%, 8000) * ar(trigger(4), 0.001, 0.03) * 0.2
kick + snare + hat |> out(%, %)
```
Expected: Layered drum kit

### F4: Pattern-Based Synth Lead
```akkado
bpm = 120
pat("c4 d4 e4 g4 e4 d4 c4 ~", (t, v, p) ->
    osc("tri", p) * adsr(t, 0.01, 0.05, 0.3, 0.2)
) |> lp(%, 2000) |> out(%, %)
```
Expected: Melodic sequence with triangle wave

### F5: Pattern with Power Shaping
```akkado
bpm = 120
pat("[~ ~ bd ~]*2", (t, v, p) ->
    osc("tri", 110) * adsr(t, 0.001, 0.05, 0.15, 0.5) ^ 2.2
) |> bp(%, 2000) |> out(%, %)
```
Expected: Pattern triggers synthesized drum with shaped envelope

### F6: Complex Pattern (Corrected)
```akkado
bpm = 120
snare = pat("[~ ~ bd ~ ~ bd bd ~ ~ ~ bd ~ ~ ~ bd ~]*0.5", (t, v, p) ->
    osc("tri", 110) * adsr(t, 0.0004, 0.05, 0.15, 0.5) ^ 2.2
) |> bp(%, 2000)
snare |> out(%, %)
```
Expected: Complex 16-step pattern at half speed

---

## Part G: Advanced Filters

### G1: Moog Ladder Filter
```akkado
bpm = 120
osc("saw", 55) |> moog(%, 400, 2) |> out(%, %)
```
Expected: Classic Moog bass sound with warm, creamy tone

### G2: Moog with High Resonance
```akkado
bpm = 120
osc("saw", 110) |> moog(%, 100 + osc("sin", 0.5) * 1000, 3.5) |> out(%, %)
```
Expected: Filter sweep with resonance near self-oscillation

### G3: Diode Ladder Filter (TB-303 Style)
```akkado
bpm = 120
osc("saw", 55) |> diode(%, 300, 2) * ar(trigger(2)) |> out(%, %)
```
Expected: Acid bass sound with characteristic diode resonance

### G4: Sallen-Key Filter (MS-20 Style)
```akkado
bpm = 120
osc("saw", 55) |> sallenkey(%, 600, 3) |> out(%, %)
```
Expected: Aggressive MS-20 style filtering with screaming resonance

### G5: Sallen-Key Highpass Mode
```akkado
bpm = 120
osc("saw", 110) |> sallenkey(%, 500, 2, 1.0) |> out(%, %)
```
Expected: Highpass filtering (mode=1.0)

### G6: Formant Filter (Vowel)
```akkado
bpm = 120
osc("saw", 110) |> formant(%, 0, 2, 0.5) * ar(trigger(1)) |> out(%, %)
```
Expected: Vowel-like formant sound morphing between two vowels

---

## Part H: Distortion & Saturation

### H1: Tanh Saturation
```akkado
bpm = 120
osc("saw", 110) |> saturate(%, 3) |> out(%, %)
```
Expected: Warm overdrive with added odd harmonics

### H2: Heavy Saturation
```akkado
bpm = 120
osc("saw", 55) |> saturate(%, 8) |> lp(%, 400) |> out(%, %)
```
Expected: Heavy saturation on bass, filtered

### H3: Soft Clipping
```akkado
bpm = 120
osc("saw", 220) |> softclip(%, 0.7) |> out(%, %)
```
Expected: Gentle compression-like soft clipping

### H4: Bit Crusher
```akkado
bpm = 120
osc("saw", 220) |> bitcrush(%, 8, 0.5) |> out(%, %)
```
Expected: Classic 8-bit lo-fi sound

### H5: Extreme Bit Crusher
```akkado
bpm = 120
osc("saw", 110) |> bitcrush(%, 4, 0.2) |> out(%, %)
```
Expected: Extreme lo-fi with heavy artifacts

### H6: Wavefolding
```akkado
bpm = 120
osc("sin", 110) * 2 |> fold(%, 0.5) |> out(%, %)
```
Expected: West Coast-style wavefolding harmonics

### H7: Animated Wavefolding
```akkado
bpm = 120
osc("sin", 110) * (1.5 + osc("sin", 0.2)) |> fold(%, 0.4) |> out(%, %)
```
Expected: Dynamic wavefolding with modulated amplitude

### H8: Tube Saturation
```akkado
bpm = 120
osc("saw", 110) |> tube(%, 5, 0.1) |> out(%, %)
```
Expected: Warm vintage tube drive with even harmonics

### H9: ADAA Smooth Saturation
```akkado
bpm = 120
osc("saw", 220) |> smooth(%, 3) |> out(%, %)
```
Expected: Clean master bus saturation without aliasing

### H10: Tape Saturation
```akkado
bpm = 120
osc("saw", 110) |> tape(%, 4, 0.4) |> out(%, %)
```
Expected: Tape-style glue with HF warmth

### H11: Transformer Saturation
```akkado
bpm = 120
osc("saw", 55) |> xfmr(%, 4, 8) |> out(%, %)
```
Expected: Punchy bass with heavy low-end saturation

### H12: Harmonic Exciter
```akkado
bpm = 120
osc("saw", 220) |> excite(%, 0.5, 3000) |> out(%, %)
```
Expected: Added presence and sparkle above 3kHz

---

## Part I: Modulation Effects

### I1: Chorus
```akkado
bpm = 120
osc("saw", 220) |> chorus(%, 0.5, 0.5) |> out(%, %)
```
Expected: Thicker, wider sound with pitch variations

### I2: Slow Deep Chorus
```akkado
bpm = 120
osc("tri", 110) |> chorus(%, 0.2, 0.8) |> out(%, %)
```
Expected: Deep chorus with slow modulation

### I3: Flanger
```akkado
bpm = 120
osc("saw", 110) |> flanger(%, 0.5, 0.7) |> out(%, %)
```
Expected: Classic "jet plane" sweep effect

### I4: Slow Metallic Flanger
```akkado
bpm = 120
osc("sqr", 220) |> flanger(%, 0.1, 0.9) |> out(%, %)
```
Expected: Slow metallic sweep with deep modulation

### I5: Phaser
```akkado
bpm = 120
osc("saw", 110) |> phaser(%, 0.3, 0.8) |> out(%, %)
```
Expected: Distinctive swirling notch effect

### I6: Fast Space Phaser
```akkado
bpm = 120
osc("sqr", 220) |> phaser(%, 2, 0.5) |> out(%, %)
```
Expected: Fast psychedelic phaser effect

### I7: Comb Filter as Resonator
```akkado
bpm = 120
osc("noise") |> comb(%, 1/220, 0.95) |> out(%, %)
```
Expected: Tuned resonator creating pitched noise at ~220Hz

### I8: Karplus-Strong Pluck
```akkado
bpm = 120
osc("noise") * ar(trigger(4), 0.001, 0.01) |> comb(%, 1/440, 0.99) |> out(%, %)
```
Expected: Physical modeling plucked string sound

---

## Part J: Reverbs & Delays

### J1: Freeverb Room
```akkado
bpm = 120
osc("saw", 220) * ar(trigger(2)) |> freeverb(%, 0.5, 0.5) |> out(%, %)
```
Expected: Medium room reverb with natural sound

### J2: Freeverb Large Hall
```akkado
bpm = 120
osc("saw", 110) * ar(trigger(1)) |> freeverb(%, 0.9, 0.3) |> out(%, %)
```
Expected: Large hall with long decay

### J3: Dattorro Plate
```akkado
bpm = 120
osc("saw", 220) * ar(trigger(2)) |> dattorro(%, 0.8, 30) |> out(%, %)
```
Expected: Lush plate reverb with 30ms predelay

### J4: Short Bright Plate
```akkado
bpm = 120
osc("tri", 440) * ar(trigger(4)) |> dattorro(%, 0.5, 10) |> out(%, %)
```
Expected: Short bright plate on fast notes

### J5: FDN Ambient Reverb
```akkado
bpm = 120
osc("saw", 55) * ar(trigger(0.5)) |> fdn(%, 0.9, 0.4) |> out(%, %)
```
Expected: Dense ambient reverb texture

### J6: Simple Echo
```akkado
bpm = 120
osc("saw", 220) * ar(trigger(2)) |> delay(%, 0.5, 0.4) |> out(%, %)
```
Expected: Quarter note echo at 120 BPM

### J7: Slapback Delay
```akkado
bpm = 120
osc("saw", 110) |> delay(%, 0.08, 0.3) * 0.5 + % |> out(%, %)
```
Expected: Short slapback for thickening

---

## Part K: Dynamics Processing

### K1: Basic Compression
```akkado
bpm = 120
osc("saw", 110) * ar(trigger(2)) |> comp(%, -12, 4) |> out(%, %)
```
Expected: 4:1 compression at -12dB threshold

### K2: Heavy Compression
```akkado
bpm = 120
osc("saw", 55) * ar(trigger(4)) |> comp(%, -20, 10) |> out(%, %)
```
Expected: Heavy limiting-like compression

### K3: Master Limiter
```akkado
bpm = 120
osc("saw", 110) * 2 |> limiter(%, -0.1, 0.1) |> out(%, %)
```
Expected: Brickwall limiter preventing clipping

### K4: Aggressive Limiting
```akkado
bpm = 120
osc("saw", 55) * ar(trigger(4)) * 3 |> limiter(%, -1, 0.05) |> out(%, %)
```
Expected: Loud, pumping limited signal

### K5: Noise Gate
```akkado
bpm = 120
(osc("saw", 110) + osc("noise") * 0.1) * ar(trigger(2)) |> gate(%, -30, 6) |> out(%, %)
```
Expected: Gate removes noise between notes

### K6: Tight Gate for Percussion
```akkado
bpm = 120
osc("noise") * ar(trigger(8), 0.001, 0.05) |> gate(%, -20, 10) |> out(%, %)
```
Expected: Tight gating for punchy percussive sounds

---

## Part L: Utility & Control

### L1: Slew Rate Limiter (Portamento)
```akkado
bpm = 120
sin(slew(mtof(48 + osc("sqr", 2) * 12), 10)) |> out(%, %)
```
Expected: Smooth pitch glide between notes

### L2: Smooth Filter Sweep
```akkado
bpm = 120
osc("saw", 110) |> lp(%, slew(200 + osc("sqr", 0.5) * 2000, 5)) |> out(%, %)
```
Expected: Smoothed stepped filter sweep

### L3: Sample and Hold Random Pitches
```akkado
bpm = 120
sin(mtof(48 + sah(osc("noise") * 24, trigger(4)))) |> out(%, %)
```
Expected: Random pitches sampled on 16th notes

### L4: Sample and Hold Filter
```akkado
bpm = 120
osc("saw", 110) |> lp(%, 200 + sah(osc("noise") * 2000, trigger(2))) |> out(%, %)
```
Expected: Stepped random filter cutoff

### L5: LFO Vibrato
```akkado
bpm = 120
osc("sin", 220 + lfo(5) * 10) |> out(%, %)
```
Expected: Vibrato effect with 5Hz rate

### L6: LFO Tremolo
```akkado
bpm = 120
osc("saw", 220) * (0.5 + lfo(4) * 0.5) |> out(%, %)
```
Expected: Tremolo effect with 4Hz rate

### L7: LFO Filter Sweep
```akkado
bpm = 120
osc("saw", 110) |> lp(%, 500 + lfo(0.2) * 1500) |> out(%, %)
```
Expected: Slow automatic filter sweep

### L8: Envelope Follower
```akkado
bpm = 120
src = osc("saw", 110) * ar(trigger(2))
src |> lp(%, 200 + env_follower(src) * 2000) |> out(%, %)
```
Expected: Envelope-controlled filter that follows input dynamics

### L9: DC Offset
```akkado
bpm = 120
osc("sin", 440) * dc(0.5) |> out(%, %)
```
Expected: Sine at half amplitude (constant multiplier)

---

## Reporting Template

For each level, report:
- **Works**: Expected output
- **Partial**: What's wrong
- **Fails**: Error message or behavior

Example:
```
A1: Works
A2: Works
A3: Fails - "Unknown function 'lp'"
B1: Partial - No sound but compiles
```

---

## Known Issues from Code Review

1. **`1` is not a valid pattern token** - Use `bd`, `sd`, `c4`, etc.
2. **Single-letter params work** - `(t)`, `(t, v, p)` both valid
3. **Named args use `:`** - e.g., `attack:0.01` not `attack=0.01`
4. **Direct `sample()` needs numeric ID** - Use `pat("bd")` for sample playback by name
