> **Status: MOSTLY SHIPPED** — All DSP primitives are implemented as Cedar opcodes. The "SquelchEngine" itself is a userspace Akkado patch (§3). Feedback saturation in SVF/Formant and ADAA-on-`tube` moved to `docs/prd-distortion-roadmap.md` (general distortion-quality work, not squelch-specific). Remaining engine work here: SIMD for the Formant bank.

# Product Requirement Document: SquelchEngine

**Project:** Cedar (audio engine) + Akkado (DSL)

**Codename:** SquelchEngine — TB-303/MS-20-style reactive distortion-and-filter character.

---

## 1. Executive Summary

The SquelchEngine is a *character*, not a new opcode. It is the interaction between an envelope follower, a resonant filter, and pre/post-filter distortion: signal dynamics modulate the cutoff, resonance produces "chirps", and a wavefolder or asymmetric clipper supplies the harmonic grit.

When this PRD was first written (v1.0, before any of it shipped) it specified a from-scratch C++ DSP system. Since then Cedar has shipped every primitive the spec described — diode ladder (Mystran ZDF), Sallen-Key with diode feedback, Chamberlin SVF, formant bank, ADAA wavefolder, asymmetric tube clipper, bitcrush, and an analog-modeled envelope follower. Audio-rate FM of filter cutoff is also already supported (any filter cutoff input accepts a buffer; the `last_freq` cache is a static-input optimization, audio-rate buffers update per sample), and the macro-knob UX is already covered by Akkado's `param("name", default, min, max)`.

What remains is (a) documenting the canonical patch that wires the primitives into the SQUELCH/BITE/MANGLER/VOWEL/SPEED/DIRT layout, and (b) closing a small set of DSP gaps that were called out in the original spec but not finished.

---

## 2. Shipped Primitives

| Spec component | Cedar opcode | Akkado builtin (and aliases) | Source |
|---|---|---|---|
| Analog envelope follower (attack/release tau) | `ENV_FOLLOWER` | `env_follower` (`envfollow`, `follower`) | `cedar/include/cedar/opcodes/envelopes.hpp:154` |
| TB-303 diode ladder (Mystran ZDF, sinh non-linearity, Newton-Raphson) | `FILTER_DIODE` | `diode` (`diodeladder`, `tb303`, `acid`) | `cedar/include/cedar/opcodes/filters.hpp:262` |
| Chamberlin SVF (LP/HP/BP, trapezoidal integration) | `FILTER_SVF_LP/HP/BP` | `lp`, `hp`, `bp` | `cedar/include/cedar/opcodes/filters.hpp:47` |
| Formant / vowel bank (3 parallel BPFs, A/E/I/O/U morph table) | `FILTER_FORMANT` | `formant` (`vowel`) | `cedar/include/cedar/opcodes/filters.hpp:372` |
| MS-20 Sallen-Key (12dB/oct, diode clipping in feedback) | `FILTER_SALLENKEY` | `sallenkey` (`sk`, `ms20`) | `cedar/include/cedar/opcodes/filters.hpp:519` |
| Wavefolder with antiderivative anti-aliasing | `DISTORT_FOLD` | `fold` (`wavefold`) | `cedar/include/cedar/opcodes/distortion.hpp:121` |
| Asymmetric tube clipper (even-harmonic bias, 2× oversampled) | `DISTORT_TUBE` | `tube` (`valve`, `triode`) | `cedar/include/cedar/opcodes/distortion.hpp:178` |
| Bitcrush / sample-rate reduction (phasor-based S&H) | `DISTORT_BITCRUSH` | `bitcrush` (`crush`) | `cedar/include/cedar/opcodes/distortion.hpp:80` |
| Audio-rate FM of filter cutoff | (no opcode needed) | Pass any audio-rate buffer to the `cut` input | All filter builtins above |
| Macro-knob UX (live sliders) | `PARAM` | `param("name", default, min, max)` | Existing parameter exposure |

All Cedar filters and distortions run on the same per-sample DSP loop, and every cutoff/drive parameter accepts a signal-rate buffer, so the spec's "audio-rate modulation" requirement is satisfied without further work.

---

## 3. Macro-Knob Composition (Recipe)

The original spec defined six macro knobs (SQUELCH, BITE, MANGLER, VOWEL, SPEED, DIRT). The same layout is expressible as a userspace Akkado patch by binding `param(...)` outputs across multiple node parameters:

```akkado
// SquelchEngine — TB-303/MS-20-style reactive distortion + filter.
// All DSP primitives ship in Cedar; this patch wires them into the
// SQUELCH / BITE / MANGLER / VOWEL / SPEED / DIRT macro layout.

bpm = 120

// Macro knobs
sq    = param("squelch", 0.7, 0.0, 1.0)   // resonance + feedback drive
bite  = param("bite",    0.6, 0.0, 1.0)   // env-mod depth on cutoff
mng   = param("mangler", 0.4, 0.0, 1.0)   // wavefold blend
vowel = param("vowel",   0.5, 0.0, 1.0)   // formant morph A->I
speed = param("speed",   0.08, 0.005, 0.5)  // env release in seconds
dirt  = param("dirt",    0.3, 0.0, 1.0)   // tube bias (even harmonics)

// Source — a saw bass with a slow note pattern
src = n"a2 a2 e3 a2 c3 a2 g2 a2"
    |> saw(@.freq) * ar(trigger(8), 0.002, 0.4)

// 1. Driver — pre-filter saturation gives the filter content to grip
driven = tube(src, 1.5 + mng * 2.0, dirt * 0.4)

// 2. Envelope follower drives the cutoff (capacitor-modeled attack/release)
env = env_follower(driven, 0.005, speed)

// 3. Cutoff: 200 Hz base + up to 5 octaves of envelope sweep
cutoff = 200.0 * pow(2.0, bite * env * 5.0)

// 4. Filter core — pick the topology that fits the character.
//    Diode ladder ("acid") is the canonical TB-303 voice.
filtered = diode(driven, cutoff, sq, 0.026, 1.0 + sq * 15.0)
//   Vocal "wow" character:  formant(driven, 0.0, 0.5, vowel, 5.0 + sq * 20.0)
//   MS-20 "scream":         sallenkey(driven, cutoff, sq * 4.0, 0.0, 0.7)

// 5. Mangler — post-filter wavefold (and optionally bitcrush)
mangled = filtered * (1.0 - mng) + fold(filtered, 0.5 - mng * 0.4) * mng

mangled |> out(@, @)
```

The original knob mapping is preserved:

| Knob | DSP parameters touched | Achieved via |
|---|---|---|
| **SQUELCH** | filter resonance + feedback gain | `sq` → `diode` `res` and `fb_gain` |
| **BITE** | envelope-to-cutoff depth | `bite` scales `env` exponent |
| **MANGLER** | drive + wavefold mix | `mng` → `tube` drive and `fold` blend |
| **VOWEL** | formant interpolation | `vowel` → `formant` `morph` |
| **SPEED** | envelope release time | `speed` → `env_follower` release |
| **DIRT** | asymmetry / even-harmonic bias | `dirt` → `tube` `bias` |

Switching the §4 filter line between `diode`, `formant`, and `sallenkey` selects between the three "voices" the original spec called for (Acid, Vowel, Scream).

---

## 4. Remaining DSP Gaps

These are the items from the original v1.0 spec that are not yet shipped. None block the recipe in §3 — the patch already sounds correct — but each closes a remaining quality dimension.

- **Feedback-path saturation in SVF and Formant.** Moog, Diode, and Sallen-Key already have non-linear feedback. The Chamberlin SVF and Formant BPFs run linear feedback and lose some character at high Q. Add a `tanh` or soft-clip to the feedback term in `op_filter_svf_*` and per-band in `op_filter_formant`. **→ Moved to `docs/prd-distortion-roadmap.md` Phase 1a/1b** (`fb_sat` ExtendedParams<1>, opt-in, default off).
- **Wider ADAA coverage.** Only `fold` and `smooth` use antiderivative anti-aliasing today (`cedar/include/cedar/opcodes/distortion.hpp:121, 235`). `tube`, `tape`, `transformer`, and `exciter` use 2× oversampling, which is fine for moderate drive but aliases at extreme settings. Convert at least `tube` to ADAA so the macro-knob recipe can push MANGLER to 1.0 cleanly. **→ Tube-only ADAA moved to `docs/prd-distortion-roadmap.md` Phase 1c**; `tape`/`xfmr`/`excite` ADAA deferred (2× oversampling buys enough headroom for current use cases).
- **SIMD for the Formant bank.** The 3 BPFs in `op_filter_formant` run serially. AVX2/NEON intrinsics processing 3 (padded to 4) BPFs in parallel would roughly halve the per-sample cost. The original spec called this out as a v1 optimization target.
- **Known self-oscillation gaps.** Two existing tracked issues in `docs/dsp-issues.md` overlap directly with the SquelchEngine character:
  - **#9 FILTER_DIODE** self-oscillates only for one of four (VT, FB) configurations tested. Higher VT keeps the sinh argument in its linear regime, producing insufficient loop gain.
  - **#12 FILTER_SALLENKEY** does not self-oscillate at all under high resonance — the diode clipping in the feedback loop saturates before unity-gain crossover. May be by design (MS-20 character is more "fuzz-wah" than self-oscillating sine), but the spec treated self-oscillation as a defining feature, so the trade-off should be made explicit.

  Resolving (or explicitly closing) these two issues is part of finishing the SquelchEngine character.

---

## 5. Non-Goals

- **No new `squelch(...)` composite opcode.** The §3 patch covers the use case with existing primitives plus `param(...)`. A bundled opcode would lock in one filter topology and one knob mapping; the patch lets users swap voices and knob curves freely.
- **No filter coefficient-cache rework.** The per-filter `last_freq` cache only short-circuits when the cutoff input is constant. Audio-rate buffers already trigger per-sample coefficient updates, which is what the original spec asked for.
- **No re-derivation of already-shipped DSP.** This PRD does not re-document the Mystran solver, the formant table, the ADAA derivation, the Chamberlin topology, or the bitcrush phasor. Those live in the source files cited in §2 and (for behavior) in the matching `experiments/test_op_*.py`.

---

## 6. Verification

For the §3 recipe:

1. `cmake --build build` succeeds.
2. Save the §3 patch as `web/static/patches/squelch-engine.akk` (or any `.akk` file) and compile: `./build/bin/akkado compile <file>` should produce bytecode without errors.
3. `./build/bin/nkido render --seconds 8 <file> out.wav` and listen — the macro knobs at their defaults should sound like a TB-303 acid line; sweeping SQUELCH and BITE should produce the characteristic "chirp" and "wah".
4. In the web IDE (`cd web && bun run dev`) load the patch and verify the six sliders appear and respond live.

For the §4 DSP gaps, the existing per-opcode tests cover regression behavior:

- `experiments/test_op_diode.py` — already documents the self-oscillation gap (#9).
- `experiments/test_op_sallenkey.py` — already documents the self-oscillation gap (#12).
- `experiments/test_op_formant.py`, `test_op_fold.py`, `test_op_tube.py`, `test_op_bitcrush.py`, `test_op_env_follower.py` — baseline behavior; any feedback-saturation or ADAA work must keep these passing (or update both test and `dsp-issues.md` if the algorithm intentionally changes, per the test methodology).
