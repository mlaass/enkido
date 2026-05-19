# PRD: Distortion Roadmap — New Stateless Shapers, `dist` Dispatcher, ADAA Carryover, Generic Waveshaper

> **Status: DRAFT — not started.** Consolidates four distortion threads into one shippable roadmap: (1) finishing the squelch-engine DSP carryover items (ADAA upgrade for `tube`, filter-feedback saturation in SVF and Formant); (2) four new stateless shaper stdlib fns (`hardclip`, `asym`, `foldback`, `quantize`); (3) a polymorphic `dist(algo, sig, ...)` dispatcher mirroring `osc(type, freq, ...)`; (4) a generic polynomial-Chebyshev waveshaper opcode. Multiband distortion (`multiband3fx`) already exists in stdlib; this PRD documents it and adds a 2-band variant. DaisySP `ds_fold`/`ds_overdrive` and fuzz-pedal emulations are explicitly out of scope.

## 1. Overview

Nkido already ships nine distortion opcodes — `saturate`, `softclip`, `bitcrush`, `fold`, `tube`, `smooth`, `tape`, `xfmr`, `excite` — that cover the major waveshaping families (tanh, polynomial soft-clip, sine wavefold, bitcrush, asymmetric tube, ADAA tanh, tape saturation, transformer, harmonic exciter). What is missing falls into three buckets:

1. **DSP-quality carryovers from `prd-squelch-engine.md` §4 Remaining Gaps** — these were specced but never shipped, and they live with the squelch engine only because that's where they got noticed. They are general distortion-quality improvements, not squelch-specific.
2. **Common shapers users want but cannot get from the existing opcodes** — hard clipping (the current `softclip` is true polynomial soft-clip; there is no hard clipper), asymmetric clipping with bias DC-compensated, sawtooth-style linear foldback (distinct from `fold`'s sine wavefolder), and pure bit-depth quantization (distinct from `bitcrush` which couples bit-depth with sample-rate reduction).
3. **A user-supplied transfer function** for arbitrary nonlinear curves not covered by any existing opcode — the Serum / Vital / Phase Plant "Shaper" primitive.

This PRD also introduces a polymorphic `dist(algo, sig, drive, p)` dispatcher modeled on the existing `osc(type, freq, ...)` pattern at `akkado/include/akkado/stdlib.hpp:14`. The dispatcher provides one uniform entry point across all distortion algorithms — new and existing — without breaking the per-name builtins or adding any runtime cost (the `match()` dispatch resolves at compile time and only the matched arm reaches bytecode).

### Why now?

- The squelch-engine DSP carryovers have been "remaining gaps" for several months. Bundling them with new shaper work amortizes the testing and docs effort.
- A `dist(algo, sig)` dispatcher gives users a single, discoverable entry point for the distortion family — currently users have to memorize nine separate opcode names plus their aliases.
- Hardclip in particular is a real gap: `softclip` does not hard-clip, and `clamp(sig, -t, t)` is the right primitive but is not discoverable as a distortion.

### Major design decisions

- **Stdlib-first.** New stateless shapers ship as userspace `fn` definitions in `akkado/include/akkado/stdlib.hpp`, not new opcodes. Stdlib `fn`s are fully inlined at every call site (per `docs/agent-guide-userspace-functions.md:144`), so a userspace `hardclip(sig, thresh) -> clamp(sig, -thresh, thresh)` compiles to a single `CLAMP` opcode with no function-call overhead. Only stateful / ADAA / polynomial-fit shapers earn an opcode.
- **No unified `shape(in, drive, mode)` opcode.** An early design considered consolidating multiple shapers into one mode-dispatched opcode. The Plan-mode review pushed back: stdlib `fn` inlining already eliminates the runtime branch a multiplexer opcode would re-introduce. A `match(algo)` dispatcher in stdlib gives users the same named-mode UX without any per-sample switch.
- **`dist(algo, sig, ...)` mirrors `osc(type, freq, ...)`.** Same compile-time `match()` pattern. Drive is the always-present second runtime param; `n` is the optional secondary param (asym bias, bitcrush rate, tube bias). All existing distortion opcodes are exposed through `dist` for uniformity.
- **No `sine` or `cosine` stdlib shapers.** `sine` collides with `osc("sin", ...)`; `cosine` is `cos(x) * scale`, too thin to justify a named slot. Both were dropped from the initial brain-dump after user review.
- **Filter-feedback saturation is opt-in.** Adding `tanh` to SVF/Formant feedback changes their impulse response. Gate behind an `ExtendedParams<1> fb_sat` (default 0=off) so existing patches and regression tests stay bit-identical.
- **ADAA upgrade scoped to `tube` only.** `tape`, `xfmr`, `excite` already use 2× oversampling with ~24 dB aliasing headroom; the visible win is `tube` at extreme drive (per `prd-squelch-engine.md:104`). Defer the others until a user actually hits aliasing.
- **Polynomial Chebyshev, not LUT, for the generic shaper.** Lookup tables would require a bank registry analog to wavetables (`cedar/include/cedar/opcodes/oscillators.hpp:1026-1029`), which is a significant infra lift. Polynomials fit `ExtendedParams<8>` directly and have closed-form ADAA (antiderivative of degree-N is degree-N+1).
- **`multiband3fx` already ships.** This PRD documents it and adds a 2-band `multiband2fx` variant for the common bass/treble split. True Linkwitz-Riley alignment is deferred until phase coherence becomes a user complaint.

---

## 2. Problem Statement

### What exists today

| Capability | Status | Reference |
|---|---|---|
| Nine distortion opcodes (saturate, softclip, bitcrush, fold, tube, smooth, tape, xfmr, excite) | ✅ Shipped | `cedar/include/cedar/opcodes/distortion.hpp` |
| `multiband3fx(sig, f1, f2, fx_lo, fx_mid, fx_hi)` | ✅ Shipped | `akkado/include/akkado/stdlib.hpp:38` |
| Distortion docs | ✅ Shipped | `web/static/docs/reference/builtins/distortion.md` |
| Distortion experiments | ✅ Shipped | `experiments/test_op_{saturate,softclip,bitcrush,fold,tube,smooth,tape,xfmr,excite}.py` |
| Compile-time `match()` dispatch in stdlib | ✅ Shipped | `osc(type, freq, ...)` at `stdlib.hpp:14` |
| `ExtendedParams<N>` mechanism (1, 2, 3, 4, 8 registered) | ✅ Shipped | `docs/extended-params-mechanism.md` |
| `soft_clip` helper for filters | ✅ Shipped | `cedar/include/cedar/opcodes/filters.hpp:140` |
| ADAA pattern (used by `smooth`) | ✅ Shipped | `cedar/include/cedar/opcodes/distortion.hpp:234-283` |

### What's missing

| Gap | Source |
|---|---|
| Hard clipper (true `clamp` at threshold) | New — `softclip` is polynomial cubic soft-clip, not hard |
| Asymmetric clipper with DC-compensated bias | New — `tube` is asymmetric but opcode-level; no stateless stdlib version |
| Linear sawtooth foldback | New — `fold` uses sine wavefolder; foldback is the triangle-mirror variant |
| Pure bit-depth quantization without S&H rate | New — `bitcrush` couples both axes |
| User-supplied polynomial transfer function | New — no opcode exposes coefficients |
| Polymorphic `dist(algo, sig, ...)` entry point | New — users memorize nine opcode names |
| Tanh feedback in SVF and Formant filters | `prd-squelch-engine.md:103` — specced, not shipped |
| ADAA on `tube` (currently 2× oversample) | `prd-squelch-engine.md:104` — specced, not shipped |
| `multiband2fx` 2-band variant | New — only 3-band ships today |

### Out of scope (deliberately excluded)

- **`ds_fold` / `ds_overdrive`** — owned by `prd-daisysp-integration.md` Phase 5 (side-by-side A/B alternatives).
- **Fuzz pedal models** (Big Muff, Fuzz Face) — circuit-modeled, stateful, would warrant its own fuzz-pedal-emulation PRD.
- **Unified `shape(in, drive, mode)` opcode** — pushed back during plan review; stdlib inlining makes it strictly worse than per-shape fns.
- **Migrating existing `saturate`/`softclip` to userspace forwards** — they ship with documented aliases (`distort`, etc. at `akkado/include/akkado/builtins.hpp:1398-1410`), tests, and patches in the wild.
- **LUT-driven generic waveshaper** — requires a transfer-function bank registry analog to wavetables; defer until a user asks.
- **`tape`/`xfmr`/`excite` ADAA upgrades** — 2× oversampling already buys enough headroom for current use cases.
- **True Linkwitz-Riley alignment in `multiband3fx`** — defer until phase coherence becomes a user complaint.
- **`sine` / `cosine` stdlib shapers** — `sine` collides with `osc("sin", ...)`; `cosine` is too thin to justify a named slot.

---

## 3. Goals

1. **Finish the squelch-engine DSP carryover** so `prd-squelch-engine.md` §4 can mark its remaining filter-feedback and ADAA items as done.
2. **Add four new stateless shapers** (`hardclip`, `asym`, `foldback`, `quantize`) covering real gaps in the current distortion palette, without adding any new opcodes.
3. **Add a polymorphic `dist(algo, sig, ...)` dispatcher** that gives users one uniform entry point across all distortion algorithms — new and existing — with zero runtime cost (compile-time `match()` dispatch).
4. **Add a generic polynomial Chebyshev waveshaper opcode** (`shaper(in, drive, c)`) for arbitrary nonlinear transfer functions.
5. **Document and lightly extend the multiband distortion path** (`multiband3fx` already ships; add a 2-band `multiband2fx` variant and a reference docs page).

---

## 4. Phased Plan

### Phase 1 — Squelch-engine DSP carryover (no language surface change)

Three localized C++ edits. Each ships behind an opt-in `ExtendedParams<1>` flag so impulse-response regressions stay bit-identical when the flag is off (default = 0).

#### 1a. SVF feedback saturation

**File:** `cedar/include/cedar/opcodes/filters.hpp`

Wrap the state-writeback feedback term in `soft_clip(...)` (the helper at `filters.hpp:140` is a Padé-style tanh approximation, already used by Moog/Diode/Sallen-Key):

| Line | Current | After |
|---|---|---|
| 67-68 (LP) | `state.ic1eq[ch] = clamp_audio(2.0f * v1 - state.ic1eq[ch]);` and `ic2eq` parallel | wrap `2.0f * v1 - state.ic1eq[ch]` and `2.0f * v2 - state.ic2eq[ch]` in `soft_clip(...)` inside the `clamp_audio(...)` |
| 96-97 (HP) | (same shape) | (same wrap) |
| 126-127 (BP) | (same shape) | (same wrap) |

Add `fb_sat` ExtendedParams<1> slot:
- 0.0 = off (default — bit-identical to trunk)
- 1.0 = on
- Smoothly modulatable (audio-rate buffer): `result = mix(linear, soft_clip(linear), fb_sat)` per sample.

Keep the outer `clamp_audio(...)` blowup guard.

#### 1b. Formant feedback saturation

**File:** `cedar/include/cedar/opcodes/filters.hpp:477-495` (three BPF stages)

Each band has the form `hp_N = x - state.bpN_z1[ch] * q_coef - state.bpN_z2[ch]`. Wrap the `state.bpN_z1[ch] * q_coef` feedback product in `soft_clip(...)`. Do it three times (one per formant band) so each formant peak gets its own saturation character.

Same `fb_sat` ExtendedParams<1> slot, default off.

#### 1c. ADAA upgrade for `tube`

**File:** `cedar/include/cedar/opcodes/distortion.hpp:178-221`

Drop the 2× oversample delay line; replace with ADAA following the pattern in `op_distort_smooth` at `distortion.hpp:234-283`.

**Trap:** `tube` is *asymmetric*. Current core is roughly `1 - exp(-driven)` for x ≥ 0 and `tanh(driven * 1.2)` for x < 0. ADAA needs:
1. Antiderivative of `1 - exp(-x)` is `x + exp(-x)`.
2. Antiderivative of `tanh(1.2 * x)` is `log(cosh(1.2 * x)) / 1.2` — same form as `smooth`.
3. Piecewise switch at the sign-crossing must use the linearization fallback (per the `if (abs(x - x1) < eps)` branch in `smooth`'s `tanh_adaa()` at `distortion.hpp:266-272`).
4. The sign-crossing itself needs a fallback to direct evaluation when the sample crosses zero between two adjacent samples; otherwise the AD difference is across two different antiderivatives and is nonsense.

Reuse `SmoothSatState` fields (previous sample + previous antiderivative value) on `TubeState`. Drop the per-channel oversample delay buffer — ADAA alone is sufficient and saves memory.

#### 1d. Update `prd-squelch-engine.md`

Mark the SVF/Formant feedback and ADAA-on-tube items in §4 as moved to this PRD, with a back-reference.

### Phase 2 — Four stateless shapers + polymorphic `dist` dispatcher

**File:** `akkado/include/akkado/stdlib.hpp` — append after the existing distortion-adjacent helpers around line 40.

#### Per-algo fns (only where they earn their slot)

```akkado
fn hardclip(sig, thresh = 1.0) -> clamp(sig, -thresh, thresh)

fn asym(sig, drive = 1.0, bias = 0.0) -> tanh(sig * drive + bias) - tanh(bias)

fn foldback(sig, thresh = 1.0) -> {
    t  = thresh * 2
    sx = select(sig >= 0, 1, -1)         // sign carry (no MATH_SIGN opcode)
    ax = abs(sig)
    f  = abs(((ax + thresh) - floor((ax + thresh) / t) * t) - thresh) - thresh / 2
    f * 2 * sx
}

fn quantize(sig, bits = 8) -> {
    levels = pow(2, bits)
    floor(sig * levels + 0.5) / levels
}
```

#### Polymorphic dispatcher

Mirrors `osc(type, freq, ...)` at `stdlib.hpp:14`:

```akkado
fn dist(algo, sig, drive = 1.0, p = 0.0) -> match(algo) {
    "hardclip" -> hardclip(sig, drive),
    "softclip" -> softclip(sig, drive),
    "saturate" -> saturate(sig, drive),
    "tanh"     -> saturate(sig, drive),
    "asym"     -> asym(sig, drive, p),
    "foldback" -> foldback(sig, drive),
    "fold"     -> fold(sig, drive),
    "wavefold" -> fold(sig, drive),
    "quantize" -> quantize(sig, drive),
    "bitcrush" -> bitcrush(sig, drive, p),
    "tube"     -> tube(sig, drive, p),
    "tape"     -> tape(sig, drive),
    "xfmr"     -> xfmr(sig, drive),
    "smooth"   -> smooth(sig, drive),
    _          -> sig
}
```

#### Param convention for `dist`

| algo | `drive` means | `n` means |
|---|---|---|
| `"hardclip"` | thresh | — |
| `"softclip"` | thresh | — |
| `"saturate"`, `"tanh"` | drive | — |
| `"asym"` | drive | bias |
| `"foldback"` | thresh | — |
| `"fold"`, `"wavefold"` | thresh | — |
| `"quantize"` | bits | — |
| `"bitcrush"` | bits | rate (0–1) |
| `"tube"` | drive | bias |
| `"tape"` | drive | — |
| `"xfmr"` | drive | — |
| `"smooth"` | drive | — |

#### Implementation notes

- **DC compensation on `asym`** — `- tanh(bias)` removes the DC offset that bias would introduce; `asym(0, _, _) == 0` always.
- **`sign()` via `select`** — there is no `MATH_SIGN` opcode (verified). Use `select(sig >= 0, 1, -1)`.
- **Compile-time match dispatch** — only the matched arm reaches bytecode; `dist("hardclip", sig, 0.5)` compiles to exactly the same instructions as `hardclip(sig, 0.5)`.
- **Buffer pressure** — each stdlib shaper claims ~5-6 temporary buffers per call site. Cedar has `MAX_VARS=4096`. Monitor via `cedar-size-report.md` after Phase 2 ships; only fuse the highest-use shaper into an opcode if pressure becomes real.
- **`quantize` name reservation** — any future beat-quantize feature must use `quant_beat` to avoid collision.
- **`dist` name reservation** — central name; no aliases.

### Phase 3 — Multiband: document and add 2-band variant

`multiband3fx` already exists at `akkado/include/akkado/stdlib.hpp:38-40`. Actions:

1. **Add `multiband2fx`** to `stdlib.hpp` for the common bass/treble split:
   ```akkado
   fn multiband2fx(sig, freq, fx_lo, fx_hi) -> {
       lo = lp(sig, freq)
       hi = hp(sig, freq)
       fx_lo(lo) + fx_hi(hi)
   }
   ```
2. **Write reference docs** at `web/static/docs/reference/builtins/multiband.md` (new page) covering both `multiband3fx` and `multiband2fx` with worked distortion examples:
   ```akkado
   multiband3fx(sig, 250, 2500,
       fn(x) -> dist("tube", x, 5, 0.3),
       fn(x) -> dist("saturate", x, 2),
       fn(x) -> excite(x, 0.4, 3000))
   ```
3. **Defer true Linkwitz-Riley alignment** until a user files a phase-coherence complaint. The cascaded `lp/hp` approach in current `multiband3fx` is acceptable for distortion-band splitting.

### Phase 4 — Generic polynomial Chebyshev waveshaper

New opcode `DISTORT_SHAPER` exposed as `shaper(in, drive, c)`.

**Files:**
- `cedar/include/cedar/opcodes/distortion.hpp` — add opcode body.
- `cedar/include/cedar/vm/instruction.hpp` — add enum entry `DISTORT_SHAPER`.
- `akkado/include/akkado/builtins.hpp` — register builtin with `extended_param_count = 8`.
- After: run `cd web && bun run build:opcodes` to regenerate opcode metadata for `web/wasm/nkido_wasm.cpp` and `tools/nkido-cli/bytecode_dump.cpp`.

**Akkado signature:**
```akkado
shaper(sig, drive = 1.0, c = {c0: 0, c1: 1, c2: 0, c3: 0, c4: 0, c5: 0, c6: 0, c7: 0})
```

`c` is record-as-options (per `CLAUDE.md` §Record-as-Options Convention) declared via `OptionSchema`. The eight coefficients map to Chebyshev terms T₀ through T₇. Default coeffs implement an identity passthrough (`c1=1` only); a useful tanh-like preset is documented in the reference docs.

**ADAA:** generalize cleanly because the antiderivative of a polynomial is a polynomial of degree+1. Implementation follows the pattern in `op_distort_smooth` at `distortion.hpp:234-283` — keep the previous sample, evaluate the antiderivative at current and previous, divide by the difference, and fall back to direct polynomial evaluation when the samples are within `eps` of each other.

**Add `"shaper"` to `dist` dispatcher** once Phase 4 ships:
```akkado
"shaper" -> shaper(sig, drive, p)   // p here is the coeffs record
```
(This requires `dist` to accept record-typed `n`; verify Akkado supports that or accept a more limited form like `dist("shaper", sig, drive)` that uses default coeffs.)

---

## 5. Critical Files

| Phase | File | What changes |
|---|---|---|
| 1a/1b | `cedar/include/cedar/opcodes/filters.hpp` | SVF: 67-68, 96-97, 126-127. Formant: 480, 487, 494. `soft_clip` helper at 140 already available. Add `fb_sat` ExtendedParams<1>. |
| 1c | `cedar/include/cedar/opcodes/distortion.hpp` | Tube ADAA rewrite at 178-221; mirror `smooth` at 234-283. |
| 1d | `docs/prd-squelch-engine.md` | §4: mark SVF/Formant feedback and tube ADAA items as moved to this PRD. |
| 2 | `akkado/include/akkado/stdlib.hpp` | Append four shaper fns + `dist` dispatcher after line 40. |
| 3 | `akkado/include/akkado/stdlib.hpp`, `web/static/docs/reference/builtins/multiband.md` (new) | Append `multiband2fx`; write reference docs for both. |
| 4 | `cedar/include/cedar/opcodes/distortion.hpp`, `cedar/include/cedar/vm/instruction.hpp`, `akkado/include/akkado/builtins.hpp` | New `DISTORT_SHAPER` opcode + enum + builtin registration with `extended_param_count = 8`. |
| 2+4 | `web/static/docs/reference/builtins/distortion.md` | Document `hardclip`, `asym`, `foldback`, `quantize`, `dist`, `shaper`. |
| 1, 4 | `web/wasm/nkido_wasm.cpp`, `tools/nkido-cli/bytecode_dump.cpp` | Auto-regenerate via `cd web && bun run build:opcodes` after each opcode change. |

---

## 6. Verification

Per `CLAUDE.md` rules: tests verify expected behavior; do not adjust thresholds to make them pass; ≥ 300 s simulated audio for any pattern/poly/sampler-driven test; always emit WAV files for human listening.

| Phase | Tests |
|---|---|
| 1a/1b | New `experiments/test_op_svf_feedback_sat.py`; extend `experiments/test_op_formant.py`. With `fb_sat=0`, output bit-identical to current trunk (regression guard). With `fb_sat=1`, harmonic content increases at high Q and self-oscillation point shifts. ≥ 300 s simulation. |
| 1c | New `experiments/test_op_tube_adaa.py`. Aliasing energy at drive=10.0 with 6 kHz input ≥ 6 dB reduction vs current 2× oversampled `tube`. Keep `experiments/test_op_tube.py` passing for moderate drive. |
| 2 | New `experiments/test_op_shapers_userspace.py`. Drive each new shaper with a 100 Hz sine and verify harmonic series: hardclip → 1, 3, 5, 7; asym → odd + even with bias-controlled even/odd ratio; foldback → triangle-like; quantize → stair-step. All four produce WAV output. Plus `experiments/test_dist_dispatcher.py`: call `dist("hardclip", ...)` vs direct `hardclip(...)` and confirm bytecode is identical (compile-time match dispatch verification). |
| 3 | New `experiments/test_op_multiband.py` (or extend existing). Three-band split sums back to original within −60 dB; per-band fx functions apply only to their band. Two-band split: same. |
| 4 | New `experiments/test_op_shaper.py`. Identity coeffs (c1=1, others=0) → bit-identical passthrough; Chebyshev T2/T3 coefficients reproduce 2nd/3rd harmonics analytically; ADAA cleanly antialiases under chirp input. |

After each phase ships:
- Run `cd experiments && ./run_all.sh --stop-on-error`.
- Run `./build/cedar/tests/cedar_tests` and `./build/akkado/tests/akkado_tests`.
- Update `docs/dsp-quality-checklist.md` test-coverage section.

---

## 7. Documentation Deliverables

- **Update `web/static/docs/reference/builtins/distortion.md`** with: four new stdlib shapers (`hardclip`, `asym`, `foldback`, `quantize`); `dist(algo, sig, drive, p)` dispatcher with the param-convention table from §4 Phase 2; `shaper(in, drive, c)` from Phase 4.
- **Add `web/static/docs/reference/builtins/multiband.md`** covering `multiband3fx` and `multiband2fx` with worked distortion examples.
- **Rebuild docs index:** `cd web && bun run build:docs` after markdown changes.
- **Update `docs/dsp-quality-checklist.md`** test-coverage section after each phase ships.
- **Update `docs/prd-squelch-engine.md`** §4: cross-reference this PRD for the SVF/Formant feedback and `tube` ADAA items.

---

## 8. Sequencing

Phase 1 first (DSP quality, no language surface change, lowest risk). Phase 2 second (pure stdlib, no C++ touch except code review). Phase 3 third (docs + one stdlib fn). Phase 4 last (new opcode + ExtendedParams plumbing, biggest lift).

Each phase is independently shippable and independently verifiable.

---

## 9. Open Questions

- **`shaper` in `dist` dispatcher with record-typed `n`** — does Akkado's `match` arm support passing a record through? If not, `dist("shaper", ...)` uses default coeffs only and users must call `shaper(...)` directly for custom coeffs.
- **`MATH_SIGN` primitive** — `foldback` currently uses `select(sig >= 0, 1, -1)` for sign carry. If multiple stdlib fns need this pattern, consider promoting to a `MATH_SIGN` primitive opcode (single comparison). Decide after Phase 2.
- **True Linkwitz-Riley `multiband` variant** — defer until a user files a phase-coherence complaint. If one does, ship `lr_split2(sig, f) -> {lo, hi}` returning a record (verify record returns from `fn` first).
