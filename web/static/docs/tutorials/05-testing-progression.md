---
title: Testing Progression
description: Systematic test progression from basic synthesis to complex patterns
order: 5
category: tutorials
keywords: [testing, patterns, synthesis, triggers, envelopes, samples]
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
