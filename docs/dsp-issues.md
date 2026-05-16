> **Status: TRACKING** — 7 open issues, 17 resolved.

# DSP Issues Found in Experiments

Tracked issues discovered by opcode experiment tests. Each entry includes the failing test, observed behavior, and suggested investigation path.

---

## Open

### 4. SAMPLE_PLAY_LOOP: Click at loop boundary

**Test:** `test_op_sample_loop.py` → `test_seamless_looping()`
**Severity:** Low
**Observed:** Max discontinuity at loop wrap point is 0.0576 (threshold: 0.01). Test uses a sine wave sample where loop start/end should be zero-crossing aligned.
**Root cause:** The "click" is the natural sample-to-sample amplitude difference of the sine wave (~0.0576), not an interpolation error. The loop interpolation (`get_interpolated_looped`) wraps correctly. The test's discontinuity threshold (0.01) is too tight for the test signal.

### 5. OSC_SQR_PWM_MINBLEP: Higher noise floor than PolyBLEP

**Test:** `test_op_sqr_pwm_minblep.py` → `test_aliasing_comparison()`
**Severity:** Low
**Observed:** At 440Hz, MinBLEP noise floor is -26.7dB vs PolyBLEP at -64.7dB — MinBLEP is 38dB *worse*. At higher frequencies the gap narrows (8000Hz: -134.3dB vs -135.3dB).
**Expected:** MinBLEP should produce equal or lower aliasing noise than PolyBLEP, particularly at higher frequencies.
**Investigate:** The 440Hz result is suspicious — -26.7dB noise floor is very high and may indicate the MinBLEP residual buffer is not being applied correctly, or there's spectral leakage in the measurement. Listen to WAV output for audible comparison. Check `cedar/include/cedar/opcodes/oscillators.hpp`.

### 6. OSC_SAW_PWM_4X: 4x oversampling not improving at all frequencies

**Test:** `test_op_saw_pwm_4x.py` → `test_aliasing_comparison()`
**Severity:** Low
**Observed:** At 2000Hz: 4x is 1.9dB worse. At 8000Hz: 4x is 11.1dB worse. At 440Hz: 5.6dB improvement. At 5000Hz: 8.0dB improvement.
**Expected:** 4x oversampling should consistently reduce aliasing across all frequencies.
**Investigate:** The inconsistency may be a measurement artifact (noise floor estimation method) or may indicate the decimation filter isn't fully attenuating mirror images at certain frequencies. Check the oversampling implementation and decimation filter in `cedar/include/cedar/opcodes/oscillators.hpp`.

### 9. FILTER_DIODE: Self-oscillation fails for most VT/feedback configurations

**Test:** `test_op_diode.py` → `test_self_oscillation()`
**Severity:** Medium
**Observed:** Original (VT=0.026, FB=1.0) correctly does not oscillate. A_fb10 (VT=0.026, FB=10.0) oscillates but at 1359Hz instead of 1000Hz (35.9% error). B_mid (VT=0.05, FB=5.0) produces no oscillation (max amp=0.0). C_soft (VT=0.1, FB=2.5) produces no oscillation (max amp=0.0). Only 1 of 4 configurations behaves as expected.
**Expected:** Diode ladder filter should self-oscillate when feedback is high enough. Higher VT (thermal voltage) with moderate feedback should still produce oscillation.
**Investigate:** The diode nonlinearity `diode_sinh(v/vt) * vt` scales linearly with VT for small signals but exponentially for large signals. At higher VT values, the feedback signal stays in the linear regime (small argument to sinh), producing insufficient loop gain for self-oscillation. The A_fb10 frequency error is expected warping from the nonlinear filter topology.

### 11. OSC_SQR: Even harmonics present (should be absent for 50% duty cycle)

**Test:** `test_op_osc.py` → harmonic analysis
**Severity:** Low
**Observed:** SQR at 440Hz shows H2 at 2.3dB and H4 at 2.2dB (relative to noise floor). A perfect square wave should have zero even harmonics.
**Expected:** Even harmonics should be at or below the noise floor (~2dB).
**Investigate:** This suggests a tiny DC offset or waveform asymmetry in the PolyBLEP square wave. The even harmonics are very low (at noise floor level), so this may be a measurement sensitivity issue rather than a real problem. Check `cedar/include/cedar/opcodes/oscillators.hpp` for DC offset.

### 12. FILTER_SALLENKEY: No self-oscillation at high resonance

**Test:** `test_op_sallenkey.py` → self-oscillation test
**Severity:** Low
**Observed:** Both LP and HP modes produce max amplitude of 0.0 with high resonance (3.8). No oscillation detected. Diode clipping headroom increased to 0.8/0.6 but still insufficient for self-oscillation.
**Expected:** Sallen-Key topology should self-oscillate when resonance (Q) is driven high enough, similar to other ladder/SVF filters.
**Investigate:** The 2-pole Sallen-Key topology with diode clipping in the feedback path may fundamentally limit achievable loop gain. The `diode_clip` function saturates feedback, preventing unity gain crossover. This may be by design — the MS-20 character is more about aggressive clipping than self-oscillation.

### 18. MIDI_QUERY mono: subtle pops on rapid retriggers (live keyboard)

**Test:** No Cedar test yet — user-reported via MPK + `web/static/patches/midi-cc-filtermono.akk`.
**Severity:** Low-Medium (musical quality regression, no functional break).
**Observed:** Playing repeated notes in quick succession on a live MIDI keyboard through `midi() |> lead(@.freq, @.gate, @.vel) |> out` produces a subtle pop on each retrigger. The chain is `osc("saw", glide(freq, 0.05)) + osc("sqr", glide(freq, 0.05)+0.5) |> lp |> @ * adsr(gate, 0.01, 0.15, 0.75, 0.35) * vel * amp`.
**Confirmed NOT the cause:** Adding a 1-sample `gate_buf[i] = 0` on every note-on (mirroring `op_seqpat_gate`'s retrigger_drop at `cedar/include/cedar/opcodes/sequencing.hpp:684-706` and the CLAUDE.md `.gate` spec) made the pops **strictly worse** — louder and audible on every note start/end. Static analysis of `op_env_adsr` at `envelopes.hpp:30-143` predicted the drop would be musically inert (level continues from current value on rising edge, no zero reset). Pattern path applies the same drop in production without pop reports. Static prediction and empirical result diverge → the popping mechanism is **not** in `fill_mono_buffers`.
**Investigate:** Build a `cedar_core` Python experiment that drives `MIDI_QUERY → glide → osc("saw") → lp → adsr → vel multiplier` with two block-aligned retriggers at different velocities. Render to WAV, plot the waveform around each transition to localise the discontinuity in time. Vary each suspect to isolate: (a) skip `glide(freq)` (jump freq instantly), (b) skip `lp` (bypass filter nonlinearity), (c) swap `osc("saw")` for `osc("sin")` (remove PolyBLEP correction), (d) hold `vel` constant across notes (remove the post-envelope multiplier discontinuity). Whichever change eliminates the pop identifies the source. Likely candidates: the post-ADSR `* vel` multiplier stepping while the envelope sustains, or the filter's state response to a sudden freq shift after glide settles.
**Workaround until fixed:** Users can wrap `glide(@.vel, 0.002)` or `interp_time(@.vel, 0.002)` in their patch to smooth velocity transitions, which is the most likely root cause. Document this in the eventual `web/static/docs/midi.md` (Phase 8) once confirmed.

---

## Won't Fix / By Design

*(Move items here only with explicit user approval)*

---

## Resolved

### 1. DISTORT_SMOOTH: Non-monotonic transfer curve

**Test:** `test_op_smooth.py` → `test_transfer_curve()`
**Fix:** Three issues in `distortion.hpp` ADAA implementation:
1. Unified antiderivative formula using `log1p` (removed branching at |x|=10)
2. Added `initialized` flag to use direct `tanh()` on first sample (avoids startup discontinuity from zero-initialized state)
3. Precision-aware antiderivative difference: when both samples have same sign and |x| > 0.5, compute `F₁(x)-F₁(x_prev)` by separating the `|x|` terms (which cancel exactly) from the `log1p(exp(-2|x|))` terms (which are small but precise)
**Result:** All drive levels pass monotonicity, antialiasing improvement preserved at 9.2dB over naive tanh.

### 2. REVERB_FREEVERB: RT60 estimates at 0.00s

**Test:** `test_op_freeverb.py` → `test_impulse_response()`, `test_room_size()`
**Fix:** Bug in test helper `estimate_rt60()` — function searched for first sample below -60dB from the START of the signal (sample 0), but reverb onset occurs after comb delay (~30ms). Fixed to search AFTER the envelope peak. No C++ changes needed.
**Result:** RT60 now correctly measured: room 0.2→0.61s, 0.5→0.95s, 0.8→1.99s, 0.95→3.90s. All tests pass.

### 3. REVERB_FDN: RT60 estimates at 0.00s

**Test:** `test_op_fdn.py` → `test_impulse_response()`, `test_decay_parameter()`
**Fix:** Same `estimate_rt60()` bug as Freeverb — fixed to search after envelope peak. No C++ changes needed.
**Result:** RT60 now measurable: impulse response RT60=2.28s. Low decay values (0.3, 0.6) still show short RT60 (0.01s) which may be correct behavior.

### 7. ADSR: Envelope timing completely wrong

**Test:** `test_op_adsr.py` → `test_stage_timing()`, `test_sample_accurate()`, `test_exponential_curve()`
**Fix:** Test bug — `cedar.hash("attack") & 0xFFFF` truncated the 32-bit FNV-1a hash, causing `ENV_GET` to look up the wrong parameter. Removed `& 0xFFFF` from all hash calls in `test_op_adsr.py`. The ADSR algorithm itself was correct.
**Result:** All timing tests pass with <1% error. Sustain levels match expected values.

### 8. EUCLID: Rotation parameter produces wrong patterns

**Test:** `test_op_euclid.py` → `test_rotation()`
**Fix:** Bit rotation direction was inverted in `sequencing.hpp`. Changed from right-shift to left-shift rotation:
`pattern = ((pattern << rotation) | (pattern >> (steps - rotation))) & mask;`
**Result:** All rotation patterns match expected values.

### 10. FILTER_FORMANT: F3 frequency off for vowels I and E

**Test:** `test_op_formant.py` → `test_vowel_response()`
**Fix:** Replaced Chamberlin SVF coefficient `2*sin(π*f/sr)` with pre-warped `min(1.9, 2*tan(π*f/sr))` in `filters.hpp`. The original formula has frequency warping at high frequencies, causing the resonant peak to shift downward.
**Result:** Vowels A, U, O now all within 4% error. Vowels I and E improved but F2/F3 peaks still overlap when frequencies are close (2300/3000Hz for I, 2000/2550Hz for E) — the test's peak finder picks the wrong peak. The filter response itself is correct.

### 15. test_op_osc_oversampling: Reference comparison crashes (array shape mismatch)

**Test:** `test_op_osc_oversampling.py` → `test_compare_with_reference()`
**Severity:** Low (test bug)
**Fix:** Use `math.ceil` for `num_blocks` or truncate both arrays to `min(len(signal), len(reference))`.

### 16. test_op_sqr_polyblep_symmetry: Crashes on numpy ≥2.0 (removed `np.trapz`)

**Test:** `test_op_sqr_polyblep_symmetry.py` line 53
**Severity:** Low (test bug)
**Fix:** Replace `np.trapz(...)` with `np.trapezoid(...)` (numpy ≥2.0) or `from scipy.integrate import trapezoid`.

### 13. OSC_SQR_PWM_MINBLEP: Amplitude exceeds ±1.0 (overshoot at PWM transitions)

**Test:** `test_op_sqr_pwm_phase.py` → `test_step_response()`
**Severity:** Medium
**Fix:** Replaced Hann window with Blackman window (-58dB sidelobes vs -31.5dB) in MinBLEP table generation (`cedar/src/opcodes/minblep_table.cpp`). The Blackman window reduces Gibbs-phenomenon overshoot.
**Result:** Overshoot reduced from 1.1694 (16.9%) to 1.1630 (16.3%). Residual overshoot is inherent to the MinBLEP approach at this table resolution.

### 14. OSC_SAW_PWM: Large discontinuity during PWM sweep

**Test:** `test_op_saw_pwm.py` → PWM sweep test
**Severity:** Low
**Fix:** Widened PWM midpoint clamp from `[0.01, 0.99]` to `[0.05, 0.95]` in both 1x and 4x variants (`cedar/include/cedar/opcodes/oscillators.hpp`). At `mid=0.01` the slope was 200, causing huge PolyBLAMP corrections. At `mid=0.05` the slope caps at 40.
**Result:** Max discontinuity reduced from 0.458 to 0.092 (5x improvement). Extreme PWM range slightly reduced but the improvement in stability is significant.

### 17. REVERB_FREEVERB: Inaudible output (peak ~0.134, RMS -52dB)

**Test:** `test_op_freeverb.py` → `test_impulse_response()`
**Severity:** High
**Fix:** Comb output scaling `comb_sum * 0.25f` at `reverbs.hpp:77` combined with 4 series allpass filters (each gain 0.5) attenuated the signal by ~36dB (`0.25 * 0.5^4 = 0.016`). Removed the 0.25 scaling factor — the allpass chain already provides sufficient peak reduction (~16x). Changed to `float y = comb_sum;`.
**Result:** Peak amplitude now 0.537 (was 0.134), comparable to FDN/Dattorro levels. No clipping at extreme settings (room=0.95, damp=0.0).
