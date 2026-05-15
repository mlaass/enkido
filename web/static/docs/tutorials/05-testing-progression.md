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

## Part A: basic synthesis (no patterns)

### A1: basic oscillator
```akkado
bpm = 120
osc("sin", 440) |> out(@)
```
Expected: Continuous 440Hz sine tone

### A2: AR envelope with trigger
```akkado
bpm = 120
osc("sin", 220) * ar(trigger(2), 0.01, 0.3) |> out(@)
```
Expected: Sine tone pulsing twice per beat (8th notes)

### A3: saw with lowpass filter
```akkado
bpm = 120
osc("saw", 110)
    |> lp(@, 800)
    * ar(trigger(1), 0.01, 0.3)
    |> out(@)
```
Expected: Filtered saw pulsing once per beat (quarter notes)

### A4: ADSR envelope (positional args)
```akkado
bpm = 120
g = trigger(1)
osc("saw", 110) * adsr(g, 0.01, 0.1, 0.5, 0.3) |> out(@)
```
Expected: Saw with attack-decay-sustain-release shape

### A5: ADSR with named args
```akkado
bpm = 120
g = trigger(1)
osc("saw", 110)
    * adsr(gate:g, attack:0.01, decay:0.1, sustain:0.5, release:0.3)
    |> out(@)
```
Expected: Same as A4, tests named argument syntax

### A6: euclidean rhythm (3 in 8)
```akkado
bpm = 120
osc("sin", 110) * ar(euclid(3, 8), 0.001, 0.1) |> out(@)
```
Expected: 3 hits spread evenly across 8 steps

### A7: euclidean with rotation
```akkado
bpm = 120
osc("sin", 110) * ar(euclid(3, 8, 2), 0.001, 0.1) |> out(@)
```
Expected: Same pattern as A6, rotated by 2 steps

### A8: triangle oscillator
```akkado
bpm = 120
osc("tri", 110) * ar(trigger(4), 0.001, 0.1) |> out(@)
```
Expected: Triangle wave pulsing on 16th notes

### A9: power/exponential shaping
```akkado
bpm = 120
(osc("sin", 110) * ar(trigger(2), 0.01, 0.2)) ^ 2.0 |> out(@)
```
Expected: Squared envelope shape (sharper attack feel)

### A10: bandpass filter
```akkado
bpm = 120
osc("noise")
    |> bp(@, 1000, 4)
    * ar(trigger(4), 0.001, 0.05)
    |> out(@)
```
Expected: Filtered noise bursts (hi-hat-like)

---

## Part B: sample patterns

### B1: single sample
```akkado
bpm = 120
pat("bd") |> out(@)
```
Expected: Bass drum once per bar

### B2: two samples alternating
```akkado
bpm = 120
pat("bd sd") |> out(@)
```
Expected: bd on beat 1-2, sd on beat 3-4 (2 sounds per bar)

### B3: four-on-floor kick
```akkado
bpm = 120
pat("bd bd bd bd") |> out(@)
```
Expected: Kick on every beat

### B4: pattern with rests
```akkado
bpm = 120
pat("bd ~ sd ~") |> out(@)
```
Expected: bd, silence, sd, silence

### B5: basic drum pattern
```akkado
bpm = 120
pat("bd hh sd hh") |> out(@)
```
Expected: bd, hihat, sd, hihat

### B6: more complex pattern
```akkado
bpm = 120
pat("bd hh hh hh sd hh hh hh") |> out(@)
```
Expected: 8-step pattern with kick on 1, snare on 5

### B7: pattern with grouping
```akkado
bpm = 120
pat("[bd bd] sd") |> out(@)
```
Expected: Two quick kicks in first half, snare in second half

### B8: speed modifier
```akkado
bpm = 120
pat("bd*4") |> out(@)
```
Expected: Four kicks (speed up pattern)

### B9: slow modifier
```akkado
bpm = 120
pat("bd/2") |> out(@)
```
Expected: Kick every 2 bars

---

## Part C: pitch patterns (no closure)

### C1: single pitch
```akkado
bpm = 120
pat("c4") |> out(@)
```
Expected: Outputs frequency ~261Hz (unclear what sound source)

### C2: four pitches
```akkado
bpm = 120
pat("c4 e4 g4 c5") |> out(@)
```
Expected: Sequence of 4 frequencies

---

## Part D: pitch patterns with closure

### D1: closure with 1 param (trigger only)
```akkado
bpm = 120
pat("c4 e4 g4 c5", (t) ->
    osc("saw", 440) * ar(t, 0.01, 0.2)
) |> out(@)
```
Expected: Saw at fixed 440Hz, triggered 4 times per bar

### D2: closure with 3 params
```akkado
bpm = 120
pat("c4 e4 g4 c5", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.2)
) |> out(@)
```
Expected: Saw using pitch from pattern (c4, e4, g4, c5)

### D3: pattern + filter in closure
```akkado
bpm = 120
pat("c4 e4 g4 c5", (t, v, p) ->
    osc("saw", p)
        |> lp(@, 1500)
        * ar(t, 0.01, 0.2)
) |> out(@)
```
Expected: Filtered saw, 4 notes

### D4: pattern with rests
```akkado
bpm = 120
pat("c4 ~ e4 ~", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.2)
) |> out(@)
```
Expected: c4, silence, e4, silence (rests produce freq 0)

### D5: ADSR in closure
```akkado
bpm = 120
pat("c4 e4 g4 c5", (t, v, p) ->
    osc("saw", p) * adsr(t, 0.01, 0.1, 0.5, 0.3)
) |> out(@)
```
Expected: Notes with ADSR shape

### D6: closure piped to filter
```akkado
bpm = 120
pat("c3 e3 g3 c4", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.3)
)
    |> lp(@, 800)
    |> out(@)
```
Expected: Closure output piped through lowpass filter

---

## Part E: pattern modifiers

### E1: grouping (subdivision)
```akkado
bpm = 120
pat("[c4 e4] g4", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.1)
) |> out(@)
```
Expected: c4+e4 in first half (faster), g4 in second half

### E2: speed modifier on group
```akkado
bpm = 120
pat("[c4 e4 g4]*2", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.1)
) |> out(@)
```
Expected: Pattern plays twice as fast (6 notes per bar)

### E3: slow modifier
```akkado
bpm = 120
pat("c4 e4 g4 c5/2", (t, v, p) ->
    osc("saw", p) * ar(t, 0.01, 0.2)
) |> out(@)
```
Expected: Pattern spread over 2 bars

---

## Part F: complex examples

### F1: synthesized kick drum
```akkado
bpm = 120
g = trigger(1)
pitch = 55 * (1 + ar(g, 0.001, 0.02) * 2)
kick = osc("sin", pitch) * ar(g, 0.005, 0.2)
kick |> out(@)
```
Expected: 808-style synthesized kick

### F2: synthesized snare
```akkado
bpm = 120
snare = osc("noise")
    |> bp(@, 1000, 2)
    * ar(trigger(2), 0.001, 0.1) * 0.5
snare |> out(@)
```
Expected: Filtered noise snare on beats 2 and 4

### F3: full drum kit
```akkado
bpm = 120
g = trigger(1)
pitch = 55 * (1 + ar(g, 0.001, 0.02) * 2)
kick = osc("sin", pitch) * ar(g, 0.005, 0.2)
snare = osc("noise")
    |> bp(@, 1000, 2)
    * ar(euclid(2, 8, 4), 0.001, 0.1) * 0.5
hat = osc("noise") |> hp(@, 8000) * ar(trigger(4), 0.001, 0.03) * 0.2
kick + snare + hat |> out(@)
```
Expected: Layered drum kit

### F4: pattern-based synth lead
```akkado
bpm = 120
pat("c4 d4 e4 g4 e4 d4 c4 ~", (t, v, p) ->
    osc("tri", p) * adsr(t, 0.01, 0.05, 0.3, 0.2)
)
    |> lp(@, 2000)
    |> out(@)
```
Expected: Melodic sequence with triangle wave

### F5: pattern with power shaping
```akkado
bpm = 120
pat("[~ ~ bd ~]*2", (t, v, p) ->
    osc("tri", 110) * adsr(t, 0.001, 0.05, 0.15, 0.5) ^ 2.2
)
    |> bp(@, 2000)
    |> out(@)
```
Expected: Pattern triggers synthesized drum with shaped envelope

### F6: complex pattern (corrected)
```akkado
bpm = 120
snare = pat("[~ ~ bd ~ ~ bd bd ~ ~ ~ bd ~ ~ ~ bd ~]*0.5", (t, v, p) ->
    osc("tri", 110) * adsr(t, 0.0004, 0.05, 0.15, 0.5) ^ 2.2
) |> bp(@, 2000)
snare |> out(@)
```
Expected: Complex 16-step pattern at half speed

---

## Part G: advanced filters

### G1: Moog ladder filter
```akkado
bpm = 120
osc("saw", 55)
    |> moog(@, 400, 2)
    |> out(@)
```
Expected: Classic Moog bass sound with warm tone

### G2: Moog with high resonance
```akkado
bpm = 120
osc("saw", 110)
    |> moog(@, 100 + osc("sin", 0.5) * 1000, 3.5)
    |> out(@)
```
Expected: Filter sweep with resonance near self-oscillation

### G3: diode ladder filter (TB-303 style)
```akkado
bpm = 120
osc("saw", 55)
    |> diode(@, 300, 2)
    * ar(trigger(2))
    |> out(@)
```
Expected: Acid bass sound with characteristic diode resonance

### G4: Sallen-Key filter (MS-20 style)
```akkado
bpm = 120
osc("saw", 55)
    |> sallenkey(@, 600, 3)
    |> out(@)
```
Expected: Aggressive MS-20 style filtering with screaming resonance

### G5: Sallen-Key highpass mode
```akkado
bpm = 120
osc("saw", 110)
    |> sallenkey(@, 500, 2, 1.0)
    |> out(@)
```
Expected: Highpass filtering (mode=1.0)

### G6: formant filter (vowel)
```akkado
bpm = 120
osc("saw", 110)
    |> formant(@, 0, 2, 0.5)
    * ar(trigger(1))
    |> out(@)
```
Expected: Vowel-like formant sound morphing between two vowels

---

## Part H: distortion & saturation

### H1: tanh saturation
```akkado
bpm = 120
osc("saw", 110)
    |> saturate(@, 3)
    |> out(@)
```
Expected: Warm overdrive with added odd harmonics

### H2: heavy saturation
```akkado
bpm = 120
osc("saw", 55)
    |> saturate(@, 8)
    |> lp(@, 400)
    |> out(@)
```
Expected: Heavy saturation on bass, filtered

### H3: soft clipping
```akkado
bpm = 120
osc("saw", 220)
    |> softclip(@, 0.7)
    |> out(@)
```
Expected: Gentle compression-like soft clipping

### H4: bit crusher
```akkado
bpm = 120
osc("saw", 220)
    |> bitcrush(@, 8, 0.5)
    |> out(@)
```
Expected: Classic 8-bit lo-fi sound

### H5: extreme bit crusher
```akkado
bpm = 120
osc("saw", 110)
    |> bitcrush(@, 4, 0.2)
    |> out(@)
```
Expected: Extreme lo-fi with heavy artifacts

### H6: wavefolding
```akkado
bpm = 120
osc("sin", 110) * 2
    |> fold(@, 0.5)
    |> out(@)
```
Expected: West Coast-style wavefolding harmonics

### H7: animated wavefolding
```akkado
bpm = 120
osc("sin", 110) * (1.5 + osc("sin", 0.2))
    |> fold(@, 0.4)
    |> out(@)
```
Expected: Dynamic wavefolding with modulated amplitude

### H8: tube saturation
```akkado
bpm = 120
osc("saw", 110)
    |> tube(@, 5, 0.1)
    |> out(@)
```
Expected: Vintage tube drive with even harmonics

### H9: ADAA smooth saturation
```akkado
bpm = 120
osc("saw", 220)
    |> smooth(@, 3)
    |> out(@)
```
Expected: Clean master bus saturation without aliasing

### H10: tape saturation
```akkado
bpm = 120
osc("saw", 110)
    |> tape(@, 4, 0.4)
    |> out(@)
```
Expected: Tape-style glue with HF warmth

### H11: transformer saturation
```akkado
bpm = 120
osc("saw", 55)
    |> xfmr(@, 4, 8)
    |> out(@)
```
Expected: Punchy bass with heavy low-end saturation

### H12: harmonic exciter
```akkado
bpm = 120
osc("saw", 220)
    |> excite(@, 0.5, 3000)
    |> out(@)
```
Expected: Added presence and sparkle above 3kHz

---

## Part I: modulation effects

### I1: chorus
```akkado
bpm = 120
osc("saw", 220)
    |> chorus(@, 0.5, 0.5)
    |> out(@)
```
Expected: Thicker, wider sound with pitch variations

### I2: slow deep chorus
```akkado
bpm = 120
osc("tri", 110)
    |> chorus(@, 0.2, 0.8)
    |> out(@)
```
Expected: Deep chorus with slow modulation

### I3: flanger
```akkado
bpm = 120
osc("saw", 110)
    |> flanger(@, 0.5, 0.7)
    |> out(@)
```
Expected: Classic "jet plane" sweep effect

### I4: slow metallic flanger
```akkado
bpm = 120
osc("sqr", 220)
    |> flanger(@, 0.1, 0.9)
    |> out(@)
```
Expected: Slow metallic sweep with deep modulation

### I5: phaser
```akkado
bpm = 120
osc("saw", 110)
    |> phaser(@, 0.3, 0.8)
    |> out(@)
```
Expected: Distinctive swirling notch effect

### I6: fast space phaser
```akkado
bpm = 120
osc("sqr", 220)
    |> phaser(@, 2, 0.5)
    |> out(@)
```
Expected: Fast psychedelic phaser effect

### I7: comb filter as resonator
```akkado
bpm = 120
osc("noise")
    |> comb(@, 1/220, 0.95)
    |> out(@)
```
Expected: Tuned resonator creating pitched noise at ~220Hz

### I8: Karplus-Strong pluck
```akkado
bpm = 120
osc("noise") * ar(trigger(4), 0.001, 0.01)
    |> comb(@, 1/440, 0.99)
    |> out(@)
```
Expected: Physical modeling plucked string sound

---

## Part J: reverbs & delays

### J1: Freeverb room
```akkado
bpm = 120
osc("saw", 220) * ar(trigger(2))
    |> freeverb(@, 0.5, 0.5)
    |> out(@)
```
Expected: Medium room reverb with natural sound

### J2: Freeverb large hall
```akkado
bpm = 120
osc("saw", 110) * ar(trigger(1))
    |> freeverb(@, 0.9, 0.3)
    |> out(@)
```
Expected: Large hall with long decay

### J3: Dattorro plate
```akkado
bpm = 120
osc("saw", 220) * ar(trigger(2))
    |> dattorro(@, 0.8, 30)
    |> out(@)
```
Expected: Plate reverb with 30ms predelay

### J4: short bright plate
```akkado
bpm = 120
osc("tri", 440) * ar(trigger(4))
    |> dattorro(@, 0.5, 10)
    |> out(@)
```
Expected: Short bright plate on fast notes

### J5: FDN ambient reverb
```akkado
bpm = 120
osc("saw", 55) * ar(trigger(0.5))
    |> fdn(@, 0.9, 0.4)
    |> out(@)
```
Expected: Dense ambient reverb texture

### J6: simple echo
```akkado
bpm = 120
osc("saw", 220) * ar(trigger(2))
    |> delay(@, 0.5, 0.4)
    |> out(@)
```
Expected: Quarter note echo at 120 BPM

### J7: slapback delay
```akkado
bpm = 120
osc("saw", 110)
    |> delay(@, 0.08, 0.3) * 0.5 + @
    |> out(@)
```
Expected: Short slapback for thickening

---

## Part K: dynamics processing

### K1: basic compression
```akkado
bpm = 120
osc("saw", 110) * ar(trigger(2))
    |> comp(@, -12, 4)
    |> out(@)
```
Expected: 4:1 compression at -12dB threshold

### K2: heavy compression
```akkado
bpm = 120
osc("saw", 55) * ar(trigger(4))
    |> comp(@, -20, 10)
    |> out(@)
```
Expected: Heavy limiting-like compression

### K3: master limiter
```akkado
bpm = 120
osc("saw", 110) * 2
    |> limiter(@, -0.1, 0.1)
    |> out(@)
```
Expected: Brickwall limiter preventing clipping

### K4: aggressive limiting
```akkado
bpm = 120
osc("saw", 55) * ar(trigger(4)) * 3
    |> limiter(@, -1, 0.05)
    |> out(@)
```
Expected: Loud, pumping limited signal

### K5: noise gate
```akkado
bpm = 120
(osc("saw", 110) + osc("noise") * 0.1) * ar(trigger(2))
    |> gate(@, -30, 6)
    |> out(@)
```
Expected: Gate removes noise between notes

### K6: tight gate for percussion
```akkado
bpm = 120
osc("noise") * ar(trigger(8), 0.001, 0.05)
    |> gate(@, -20, 10)
    |> out(@)
```
Expected: Tight gating for punchy percussive sounds

---

## Part L: utility & control

### L1: slew rate limiter (portamento)
```akkado
bpm = 120
sin(slew(mtof(48 + osc("sqr", 2) * 12), 10)) |> out(@)
```
Expected: Smooth pitch glide between notes

### L2: smooth filter sweep
```akkado
bpm = 120
osc("saw", 110)
    |> lp(@, slew(200 + osc("sqr", 0.5) * 2000, 5))
    |> out(@)
```
Expected: Smoothed stepped filter sweep

### L3: sample and hold random pitches
```akkado
bpm = 120
sin(mtof(48 + sah(osc("noise") * 24, trigger(4)))) |> out(@)
```
Expected: Random pitches sampled on 16th notes

### L4: sample and hold filter
```akkado
bpm = 120
osc("saw", 110)
    |> lp(@, 200 + sah(osc("noise") * 2000, trigger(2)))
    |> out(@)
```
Expected: Stepped random filter cutoff

### L5: LFO vibrato
```akkado
bpm = 120
osc("sin", 220 + lfo(5) * 10) |> out(@)
```
Expected: Vibrato effect with 5Hz rate

### L6: LFO tremolo
```akkado
bpm = 120
osc("saw", 220) * (0.5 + lfo(4) * 0.5) |> out(@)
```
Expected: Tremolo effect with 4Hz rate

### L7: LFO filter sweep
```akkado
bpm = 120
osc("saw", 110)
    |> lp(@, 500 + lfo(0.2) * 1500)
    |> out(@)
```
Expected: Slow automatic filter sweep

### L8: envelope follower
```akkado
bpm = 120
src = osc("saw", 110) * ar(trigger(2))
src
    |> lp(@, 200 + env_follower(src) * 2000)
    |> out(@)
```
Expected: Envelope-controlled filter that follows input dynamics

### L9: DC offset
```akkado
bpm = 120
osc("sin", 440) * dc(0.5) |> out(@)
```
Expected: Sine at half amplitude (constant multiplier)

---

## Reporting template

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

## Known issues from code review

1. **`1` is not a valid pattern token** - Use `bd`, `sd`, `c4`, etc.
2. **Single-letter params work** - `(t)`, `(t, v, p)` both valid
3. **Named args use `:`** - e.g., `attack:0.01` not `attack=0.01`
4. **Direct `sample()` needs numeric ID** - Use `pat("bd")` for sample playback by name
