> **Status: NOT STARTED** — Follow-up to the ExtendedParams mechanism (shipped 2026-05-13). Migrates the remaining `inst.rate` bit-pack offenders and the `inputs[3]/[4]` halving trick to the canonical mechanism; surfaces parameters that the C++ implementations already support but the Akkado builtins hide.
>
> **Note (2026-05-19):** §4.5 (freeverb `dry`/`wet`) was incidentally accomplished by the unified dry/wet convention PRD (commits `8052c18`, `80db1dd`) — freeverb now exposes `dry`/`wet` via `ExtendedParams<2>` and the stale rate-field comment is gone. Defaults differ from this PRD's plan: shipped as `dry=1.0, wet=0.5` (Category A convention) rather than the originally-planned `dry=0.0, wet=1.0`. All other phases (comp/limiter/gate attack/release/hold, dattorro damping/mod_depth/lfo_rate, flanger feedback, comb damping, delay_sync mix + builtin, seqpat_transport cycle_length) remain untouched.

# ExtendedParams Migration — Remaining Opcodes PRD

## Executive Summary

The [ExtendedParams mechanism](extended-params-mechanism.md) shipped with phaser as the proof migration. Eight further opcodes still reach for the deprecated workarounds — `inst.rate` bit-packing (compressor, limiter, gate, dattorro, flanger feedback, comb) or `inputs[3]/[4]` 32-bit-float halving (seqpat_transport). Two more (freeverb, delay_sync) carry stale rate-field comments without active code paths but are underspecified at the builtin level — their wet/dry mix is documented in the opcode header yet never reaches the user.

This PRD closes that gap in one phased migration. Every offender adopts ExtendedParams; underspecified opcodes gain the parameters their C++ already supports. Backward compatibility is mandatory: each opcode's new `extended_defaults[]` must reproduce the current decoded behavior of `inst.rate = 0` so existing patches sound identical.

### Key Decisions

- **Bare param names + units in docs.** Builtin signatures stay terse (`attack`, `release`, `lookahead`); web docs spell out "milliseconds".
- **Defaults must reproduce current behavior.** No audible drift on hot-reload. Per-opcode equivalence test is acceptance criterion.
- **One PRD, phased by family.** Implementer picks up Phase 1 → Phase 4 incrementally; each phase is independently shippable.
- **Limiter lookahead becomes `lookahead` (ms, 0 = off)** — replaces the boolean toggle and unhardcodes the magic 1ms.
- **Freeverb gains explicit `dry` + `wet`** — honors the opcode header's documented but unimplemented mix.
- **Dattorro also exposes hardcoded `lfo_rate`** — while we're touching the opcode anyway.
- **seqpat_transport keeps its internal-only status** — migration frees inputs[3]/[4] but no new builtin is created (separate sequencing PRD).

---

## 1. Problem Statement

The audit (2026-05-14) found the following residual workarounds:

| Opcode | File | Current workaround | Decode |
|---|---|---|---|
| `comp` | `cedar/include/cedar/opcodes/dynamics.hpp:25` | `inst.rate` 4+4 bit-pack | attack_ms [0.1–100], release_ms [10–1000] |
| `limiter` | `dynamics.hpp:90` | `inst.rate` boolean | lookahead enable (1ms hardcoded) |
| `gate` | `dynamics.hpp:160` | `inst.rate` 2+2+4 bit-pack | attack_ms [0.1–10], hold_ms [0–200], release_ms [10–500] |
| `dattorro` | `reverbs.hpp:147` | `inst.rate` 4+4 bit-pack | damping [0–1], mod_depth [0–1] (lfo_rate hardcoded 0.5 Hz) |
| `flanger` | `modulation.hpp:85` | `inst.rate` 4-bit feedback | feedback [-0.99..0.99] (lfo_phase already in ExtendedParams<1>) |
| `comb` | `modulation.hpp:25` | `inst.rate` linear 0–255 | damping [0–1] |
| `freeverb` | `reverbs.hpp:35` | header comment says `rate: wet/dry mix`; no code reads it | (unused — wet/dry never reaches the user) |
| `delay_sync` | `delays.hpp:114` | `inst.rate` linear 0–255 | mix [0–1] (no Akkado builtin exposes the opcode at all) |
| `seqpat_transport` | `sequencing.hpp:312` | `inputs[3]/[4]` halve a `bit_cast<uint32_t>(float)` | cycle_length (32-bit float) |

Costs are the ones the mechanism doc enumerates: every offender invents its own layout; ranges are quantized; the parameters cannot be automated by an LFO; rate-field reads silently carry stale bits across hot-swap; `inputs[3]/[4]` halving sacrifices two real signal slots.

---

## 2. Goals & Non-Goals

### Goals
- Every opcode in the table above reads its previously-packed parameters through `ExtendedParams<N>` instead of `inst.rate` bit-packing or `inputs[]` halving.
- `inst.rate == 0` post-migration for every migrated opcode (locked in by a Cedar VM test).
- Builtin signatures expose every parameter the C++ implementation honors — no more underspecification.
- Default extended-param values reproduce current decoded behavior of `inst.rate = 0` (or current hardcoded constant) within audio-equivalence tolerance.
- Hot-swap: changing an extended param's default in source overwrites the StatePool slot on the next `apply_state_inits`, no crossfade needed.

### Non-Goals
- ADSR release-time bit-packing (`codegen.cpp:2003-2023`) — flagged by the audit, kept for a future cleanup ticket. Noted in §8.
- DELAY_PINGPONG hardcoded `damp_coeff` / `smooth_coeff` — not currently a hack (no rate-field encoding); a future "expose stereo-delay constants" PRD can pick these up.
- New audio algorithms or topology changes — pure migration only.
- Removing `inst.rate` itself — it stays as the canonical channel for audio-rate-vs-control dispatch and small fixed enum modes (see mechanism doc §5).
- Adding a public Akkado builtin for `seqpat_transport` — out of scope; the migration only frees inputs[3]/[4] internally.

---

## 3. Target Signatures

Every signature below ends with the new extended params. Required-positional and optional-input slots are unchanged so existing call sites compile without edits.

### 3.1 Dynamics (Phase 1)

```akkado
// comp: attack & release surface (were hidden in inst.rate)
comp(in, thresh, ratio, attack?, release?)
// attack default = 0.1ms (current rate=0 decode), release default = 10ms
osc("saw", 220) |> comp(@, -12, 4, attack: 5, release: 100)

// limiter: explicit `lookahead` replaces the rate-field boolean
limiter(in, ceiling, release, lookahead?)
// lookahead default = 1.0 (matches current "lookahead enabled, 1ms" hardcoded path);
// pass lookahead: 0 to disable
in |> limiter(@, -0.1, 0.1, lookahead: 0)   // off
in |> limiter(@, -0.1, 0.1, lookahead: 5)   // 5ms lookahead

// gate: attack, hold, release surface
gate(in, thresh, range, hyst, close_time, attack?, hold?, release?)
// attack default = 0.1ms, hold default = 0ms, release default = 10ms (rate=0 decode)
```

### 3.2 Reverbs (Phase 2)

```akkado
// dattorro: damping/mod_depth out of rate field, lfo_rate now tunable too
dattorro(in, decay, predelay, in_diff, dec_diff, damping?, mod_depth?, lfo_rate?)
// damping=0, mod_depth=0, lfo_rate=0.5 (current hardcoded value)
sig |> dattorro(@, 0.8, 30, 0.75, 0.625, damping: 0.4, mod_depth: 0.3, lfo_rate: 0.7)

// freeverb: gains explicit dry & wet (header had it documented; code never read rate)
freeverb(in, room, damp, room_scale, room_offset, dry?, wet?)
// dry default = 0.0, wet default = 1.0 (matches current 100%-wet behavior)
sig |> freeverb(@, 0.6, 0.5, 0.28, 0.7, dry: 0.5, wet: 0.5)
```

### 3.3 Modulation & Delays (Phase 3)

```akkado
// flanger: feedback joins lfo_phase in extended params (lfo_phase already shipped)
flanger(in, rate, depth, min_delay, max_delay, feedback?, lfo_phase?)
// feedback default = current inst.rate=0 decode (compute exact in test);
// extended_param_names now { "feedback", "lfo_phase" }, count 2
sig |> flanger(@, 0.4, 0.7, 1.0, 5.0, feedback: 0.6, lfo_phase: 0.5)

// comb: damping surfaces (was packed in rate field 0-255)
comb(in, time, fb, damping?)
// damping default = 0.0 (current rate=0)
noise() |> comb(@, 0.005, 0.95, damping: 0.4)

// delay_sync: gains a builtin entry + tunable mix parameter
delay_sync(in, beats, fb, mix?)
// mix default = 1.0 (current rate=0 → 100% wet)
// NEW: the opcode existed but no Akkado builtin exposed it; this PRD adds one.
sig |> delay_sync(@, 0.25, 0.5, mix: 0.6)
```

### 3.4 Sequencing — Internal Cleanup (Phase 4)

```
seqpat_transport: cycle_length moves from inputs[3]/[4] halving to ExtendedParams<1>[0].
No public builtin change (opcode remains codegen-internal).
inputs[3] and inputs[4] become available for future signal slots.
```

---

## 4. Per-Opcode Migration Specs

For each opcode, the implementer:
1. Adds `extended_param_count`, `extended_param_names`, `extended_defaults` to the `BuiltinInfo` entry in `akkado/include/akkado/builtins.hpp`. Use designated-init syntax (positional init breaks past `consumes_polyphonic_pattern`).
2. In the opcode body (`cedar/include/cedar/opcodes/<file>.hpp`), replaces the `inst.rate` decode with the `ext_params_state_id` + `ExtendedParams<N>` resolve-slot pattern from the mechanism doc §3b.
3. Deletes any codegen special-case bit-pack block in `akkado/src/codegen.cpp` (none currently exist for these opcodes; the generic `emit_extended_params_init` path is sufficient).

### 4.1 Compressor — `ExtendedParams<2>`

| Field | Source | Default | Range |
|---|---|---|---|
| `attack` | `inst.rate[7:4]` decode | 0.1 (current rate=0) | 0.1–100 ms |
| `release` | `inst.rate[3:0]` decode | 10.0 | 10–1000 ms |

```cpp
// builtins.hpp
{"comp", {.opcode = cedar::Opcode::DYNAMICS_COMP,
          .input_count = 1, .optional_count = 2, .requires_state = true,
          .param_names = {"in", "thresh", "ratio", "", "", ""},
          .defaults = {-12.0f, 4.0f, NAN},
          .description = "Dynamic-range compressor. attack/release in ms.",
          .extended_param_count = 2,
          .output_channels = ChannelCount::Mono,
          .extended_param_names = {"attack", "release"},
          .extended_defaults = {0.1f, 10.0f}}},
```

### 4.2 Limiter — `ExtendedParams<1>`

| Field | Default | Range |
|---|---|---|
| `lookahead` | 1.0 (matches current rate≠0 path) | 0 ms = off, >0 = on at that time |

The opcode body branches on `lookahead > 0` instead of `inst.rate != 0`. The internal lookahead buffer size still derives from a compile-time `MAX_LOOKAHEAD_MS` constant (no per-opcode allocation change needed).

### 4.3 Gate — `ExtendedParams<3>`

| Field | Source | Default | Range |
|---|---|---|---|
| `attack` | `inst.rate[7:6]` decode | 0.1 | 0.1–10 ms |
| `hold` | `inst.rate[5:4]` decode | 0.0 | 0–200 ms |
| `release` | `inst.rate[3:0]` decode | 10.0 | 10–500 ms |

### 4.4 Dattorro — `ExtendedParams<3>`

| Field | Source | Default | Range |
|---|---|---|---|
| `damping` | `inst.rate[3:0]` decode | 0.0 | 0–1 |
| `mod_depth` | `inst.rate[7:4]` decode | 0.0 | 0–1 |
| `lfo_rate` | `DATTORRO_LFO_RATE_DEFAULT` (currently hardcoded) | 0.5 | 0.05–5 Hz |

`lfo_rate` is added opportunistically: while we're already plumbing ExtendedParams<3> for the bit-packed pair, the third hardcoded value moves out at trivial extra cost.

### 4.5 Freeverb — `ExtendedParams<2>`

| Field | Default | Range |
|---|---|---|
| `dry` | 0.0 | 0–1 |
| `wet` | 1.0 (matches current 100%-wet) | 0–1 |

The stale `// rate: wet/dry mix (0-255 -> 0.0-1.0)` comment in `reverbs.hpp:32` is deleted. Output formula in the opcode body becomes `out = in * dry + reverb * wet`.

### 4.6 Flanger — bump `ExtendedParams<1>` → `ExtendedParams<2>`

Existing slot 0 (`lfo_phase`) stays. New slot 1 (`feedback`) replaces the `inst.rate[7:4]` decode. The "legacy phaser-style packing kept until flanger feedback is migrated" comment in `modulation.hpp:99-100` is deleted.

| Field | Source | Default | Range |
|---|---|---|---|
| `feedback` | `inst.rate[7:4]` decode (currently rate=0 → -0.99) | -0.99 — **but see test §7.3** | -0.99 to 0.99 |
| `lfo_phase` | already in ext slot 0 | 0.25 | 0–1 |

Note: the rate=0 decode yields feedback = -0.99, which is musically unusual. Audio-equivalence test §7.3 verifies this is the actual pre-migration behavior; if so, the default carries forward unchanged (this PRD does not redesign defaults).

### 4.7 Comb — `ExtendedParams<1>`

| Field | Default | Range |
|---|---|---|
| `damping` | 0.0 | 0–1 |

### 4.8 Delay_sync — NEW BUILTIN + `ExtendedParams<1>`

`delay_sync` has a C++ opcode but no Akkado builtin entry today. This PRD adds the builtin:

```cpp
{"delay_sync", {.opcode = cedar::Opcode::DELAY_SYNC,
                .input_count = 3, .optional_count = 1, .requires_state = true,
                .param_names = {"in", "beats", "fb", "", "", ""},
                .defaults = {NAN, NAN, NAN},
                .description = "Beat-synced delay. mix is wet level (0=dry, 1=wet).",
                .extended_param_count = 1,
                .extended_param_names = {"mix"},
                .extended_defaults = {1.0f}}},
```

| Field | Default | Range |
|---|---|---|
| `mix` | 1.0 (current rate=0 → 100% wet) | 0–1 |

### 4.9 Seqpat_transport — `ExtendedParams<1>`, internal only

Replace the `inputs[3]/[4]` halving:

```cpp
// BEFORE
std::uint32_t cl_bits = static_cast<std::uint32_t>(inst.inputs[3])
                      | (static_cast<std::uint32_t>(inst.inputs[4]) << 16);
float cycle_length = std::bit_cast<float>(cl_bits);

// AFTER
const auto* ext = ctx.states->get_if<ExtendedParams<1>>(
    ext_params_state_id(inst.state_id));
float cycle_length = ext ? ext->params[0].constant : DEFAULT_CYCLE_LENGTH;
```

Codegen for `seqpat_transport` (wherever it currently sets `inputs[3]/[4]`) instead emits a `StateInitData::Type::ExtendedParams` with `ext_constants[0] = cycle_length`. The two freed input slots are left as `BUFFER_UNUSED` for now — future PRDs may repurpose them.

No public Akkado builtin is added.

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `ExtendedParams<N>` core mechanism | **Stays** | No changes; `<1>`, `<2>`, `<3>` variants all already registered |
| `state_pool::init_extended_params_runtime` | **Stays** | Picks smallest N variant that fits |
| `codegen::emit_extended_params_init` | **Stays** | Generic helper; no special-cases needed |
| `BuiltinInfo` struct | **Stays** | Already has `extended_param_count`, `extended_param_names`, `extended_defaults` |
| `inst.rate` field semantic | **Modified** | Becomes 0 for 9 migrated opcodes; mechanism doc §5 unchanged |
| `inputs[3]/[4]` for seqpat_transport | **Modified** | Freed; become unused |
| 9 opcode bodies | **Modified** | Replace rate-decode / halve-decode with ExtendedParams resolve-slot pattern |
| 8 builtin entries + 1 new builtin | **Modified / New** | comp, limiter, gate, dattorro, flanger, comb, freeverb, seqpat_transport modified; `delay_sync` builtin is new |
| Pre-migration patches | **Compatible** | Defaults reproduce current decoded behavior; no recompile required for users; new params optional |
| Hot-swap / state preservation | **Compatible** | XOR'd sibling state_id keeps ExtendedParams in distinct StatePool slot |

---

## 6. File-Level Changes

### Cedar (VM)
| File | Change |
|---|---|
| `cedar/include/cedar/opcodes/dynamics.hpp` | Migrate `op_dynamics_comp`, `op_dynamics_limiter`, `op_dynamics_gate` (lines 25, 90, 160) — replace `inst.rate` decode with `ExtendedParams<N>` resolve-slot pattern |
| `cedar/include/cedar/opcodes/reverbs.hpp` | Migrate `op_reverb_dattorro` (line 147), `op_reverb_freeverb` (line 35; delete stale rate-field comment line 32) |
| `cedar/include/cedar/opcodes/modulation.hpp` | Migrate `op_effect_flanger` feedback (line 101), `op_effect_comb` damping (line 32) |
| `cedar/include/cedar/opcodes/delays.hpp` | Migrate `op_delay_sync` mix (line 122) |
| `cedar/include/cedar/opcodes/sequencing.hpp` | Migrate `op_seqpat_transport` cycle_length (lines 323-325) |

### Akkado (Compiler)
| File | Change |
|---|---|
| `akkado/include/akkado/builtins.hpp` | Update `comp`, `limiter`, `gate`, `dattorro`, `flanger`, `comb`, `freeverb` entries with `extended_param_count`, `extended_param_names`, `extended_defaults`. ADD new `delay_sync` entry. Switch all migrated entries to designated-init syntax. |
| `akkado/src/codegen.cpp` | Add seqpat_transport codegen path emitting `StateInitData::Type::ExtendedParams` instead of halving. No other codegen special-cases needed (generic `emit_extended_params_init` already handles the rest). |

### Tests
| File | Change |
|---|---|
| `akkado/tests/test_types.cpp` | Add 9 `[extended-params]` test cases (one per migrated opcode) asserting StateInit emission shape |
| `cedar/tests/test_vm.cpp` (or equivalent) | Add `inst.rate == 0` assertion per migrated opcode |
| `experiments/test_op_comp.py`, `test_op_limiter.py`, `test_op_gate.py`, `test_op_dattorro.py`, `test_op_freeverb.py`, `test_op_flanger.py`, `test_op_comb.py`, `test_op_delay_sync.py` (new) | Add audio-equivalence + tunability tests per §7 |

### Docs
| File | Change |
|---|---|
| `web/static/docs/reference/builtins/dynamics.md` | Add `attack`/`release`/`hold`/`lookahead` rows + worked examples |
| `web/static/docs/reference/builtins/reverbs.md` | Add `damping`/`mod_depth`/`lfo_rate`/`dry`/`wet` rows |
| `web/static/docs/reference/builtins/modulation.md` | Add `feedback` to flanger; add `damping` to comb |
| `web/static/docs/reference/builtins/delays.md` | Add new `delay_sync` page with `mix` |
| `docs/prd-stereo-native-opcodes.md` | Update §5.5 — mark "lfo_phase deferred until ExtendedParams VM init plumbing lands" as DONE (mechanism shipped 2026-05-13) |
| Run `cd web && bun run build:docs` + `bun run build:opcodes` | After all builtin / docs changes |

### CI / Audit Guard (Phase 4 cleanup, see §9)
| File | Change |
|---|---|
| New `scripts/audit-inst-rate.sh` or similar | Greps `cedar/include/cedar/opcodes/` for new `inst.rate` reads outside the allowed list (dispatch, fixed-enum modes); fails CI on regression |

---

## 7. Testing & Verification Strategy

Each migrated opcode requires all four test types:

### 7.1 Akkado StateInit emission test (`[extended-params]` tag)
Compile a representative call, then assert:
- `state_inits` contains an `ExtendedParams` entry for the opcode's state_id
- `ext_count` matches expected count
- `ext_constants[]` matches `extended_defaults[]` when no override
- Named-arg override (e.g. `comp(in, -12, 4, release: 200)`) lands in the right slot; unspecified slots get defaults
- Gap-filling: skipping `attack` but providing `release` works (analyzer inserts `_` placeholder)

### 7.2 Cedar VM `inst.rate == 0` test
After loading a program containing each migrated opcode, walk the bytecode and assert `inst.rate == 0` for every instruction whose opcode is in the migrated set. Locks in that no stale bits are carried.

### 7.3 Python audio-equivalence test (per opcode)
Render a deterministic signal pre- and post-migration; bit-exact equality is not required, but spectrum/RMS within tolerance is:
- Generate a test signal (impulse, swept sine, or noise burst) of ≥ 1 second.
- Run through opcode at its current pre-migration defaults; render WAV (save as reference).
- Re-run post-migration with `extended_defaults` per §4; render WAV.
- Assert RMS error < -60 dBFS and spectral magnitude bins within ±0.5 dB.
- Save both WAVs under `experiments/output/op_<name>/` so a human can A/B them.

**The PRD does not specify exact default numeric values for flanger feedback** — the implementer computes `(0 >> 4 & 0xF) / 7.5f - 1.0f` empirically and confirms via this test that pre/post output matches.

### 7.4 Python tunability test (per opcode, per new param)
Drive each new extended param across its full range; assert measurable change in output:
- Compressor `attack`: at attack=0.1ms vs attack=100ms on a fast-attack transient, peak amplitude should differ measurably.
- Gate `hold`: at hold=0 vs hold=200ms, gate-close timing should differ.
- Dattorro `damping`: at damping=0 vs damping=1, high-frequency content of tail should differ ≥ 6 dB.
- Freeverb `wet`: at wet=0 vs wet=1, RMS should track expected dry/wet ratio within 5%.
- etc. — one assertion per new param.

Per-test minimum render duration is the 1 second standard unless the param controls long-time-constant behavior (reverb tails, gate hold) — those use 5 seconds.

### 7.5 Build & run
```bash
cmake --build build
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests "[extended-params]"
cd experiments && ./run_all.sh
cd ../web && bun run check && bun run build:docs && bun run build:opcodes
```

---

## 8. Edge Cases

- **Pre-migration program loaded into post-migration VM**: opcode body's `get_if<ExtendedParams<N>>(...)` returns `nullptr` (no StateInit was emitted by the old codegen). Fallback to a per-opcode hardcoded default value reproducing pre-migration behavior. Mechanism doc §3b documents this pattern.
- **inst.rate carrying stale non-zero bits in pre-migration bytecode**: post-migration opcode bodies must ignore `inst.rate` entirely (no decode at all). Verified by §7.2.
- **Flanger feedback default of -0.99**: confirmed by §7.3; if the audio-equivalence test reveals this is musically wrong (rather than just unusual), file a separate "redesign flanger default" ticket — do not adjust within this PRD's scope.
- **Limiter `lookahead: 0`**: opcode body branches on `lookahead > 0`; lookahead buffer remains allocated (avoids per-block allocation churn) but is unread. Negligible memory cost.
- **Hot-swap during migration rollout**: any program compiled with the old toolchain and hot-swapped into the new VM keeps working via the §3b fallback. Conversely, new bytecode running on an old VM will be missing the StateInit handler — this is a one-way migration; users on stale builds must rebuild.
- **Dattorro `lfo_rate: 0`**: opcode body must clamp to a minimum (e.g. 0.01 Hz) to avoid divide-by-zero in the LFO phase increment. Clamp inside the opcode, not at the builtin layer.
- **seqpat_transport with no StateInit (pre-migration bytecode)**: fall back to the existing `DEFAULT_CYCLE_LENGTH` constant.

---

## 9. Implementation Phases

Each phase is independently shippable; verification gate between phases is the full test suite green.

### Phase 1 — Dynamics (comp, limiter, gate)
- Builtins: comp (ext<2>), limiter (ext<1>), gate (ext<3>)
- Opcode bodies in `dynamics.hpp`
- 3 Akkado [extended-params] tests, 3 Python equivalence + tunability tests
- Docs: `web/static/docs/reference/builtins/dynamics.md`

### Phase 2 — Reverbs (dattorro, freeverb)
- Builtins: dattorro (ext<3>), freeverb (ext<2>)
- Opcode bodies in `reverbs.hpp` (delete stale freeverb header comment)
- 2 Akkado tests, 2 Python tests
- Docs: `web/static/docs/reference/builtins/reverbs.md`

### Phase 3 — Modulation & Delays (flanger, comb, delay_sync)
- Builtins: flanger (ext<1> → ext<2>), comb (ext<1>), delay_sync (NEW builtin + ext<1>)
- Opcode bodies in `modulation.hpp` + `delays.hpp` (delete flanger "legacy" comment)
- 3 Akkado tests, 3 Python tests
- Docs: modulation.md + delays.md

### Phase 4 — Sequencing + Audit Cleanup
- Migrate `op_seqpat_transport` cycle_length (internal only, no builtin change)
- Update `docs/prd-stereo-native-opcodes.md` §5.5 to DONE
- Add `scripts/audit-inst-rate.sh` non-regression CI guard
- Note ADSR release-time `inst.rate` packing as a future cleanup candidate (do not migrate in this PRD)

---

## 10. Out of Scope / Future Work

- **ADSR release-time packing** (`akkado/src/codegen.cpp:2003-2023` packs `release_val / 0.1` into `inst.rate` 0–255). Same anti-pattern; not in the user's explicit list for this PRD. Tracked as a candidate for a future "envelope opcodes ExtendedParams cleanup" ticket.
- **DELAY_PINGPONG hardcoded `damp_coeff` / `smooth_coeff`** (`stereo.hpp:209-212`). Not an `inst.rate` hack; cleanup belongs in a stereo-delay-tunability PRD.
- **Public Akkado builtin for `seqpat_transport`** — this PRD only migrates the internal halving. Surfacing it as a user-facing function requires sequencing-PRD scope decisions.
- **Removing `inst.rate` field from `Instruction`** — out of the question; still load-bearing for dispatch and enum modes per mechanism doc §5.
- **Capacity expansion** (`ExtendedParams<N>` for N > 8) — current migration fits in the existing 1/2/3 variants. Add larger variants only when a real use case appears.
