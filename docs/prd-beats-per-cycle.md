> **Status: DEFERRED** — Optional musician-facing feature, split out of the former `prd-cycles-pure-clock-model.md` on 2026-05-20. Adds a configurable `beats_per_cycle` / `cps` knob so a patch can recover a classical "1 cycle = N beats" feel. Depends on [`prd-cycle-length-cleanup.md`](prd-cycle-length-cleanup.md) landing first. **Blocks nothing** — `prd-runtime-event-transforms.md` does NOT depend on this PRD. Not implementation-ready: the §6 open questions are unresolved and the PRD has not been reviewed.

# PRD: Configurable `beats_per_cycle` — recover classical time-signature feel

## Executive Summary

After the 2026-05-19 "cycle = beat" revert (commit `9d99490`), Cedar is
Strudel-pure by default: one cycle is one beat, BPM directly sets the cycle
rate. That is the right default, and it shipped. What it removed is the *option*
for a patch to say "I want one cycle to span 4 beats" — the classical
bar-oriented feel.

This PRD adds that option back as an explicit, opt-in knob. It does **not**
change the default and is **not** a breaking change — the breaking change
(`samples_per_cycle()` dropping its `* 4`) already shipped with the revert.

**Scope:**

- `ExecutionContext::beats_per_cycle` (float, default **1.0** — Strudel-pure).
- `ExecutionContext::cps` as the canonical internal cycle-rate field, derived
  from `bpm` and `beats_per_cycle`. BPM stays the canonical *user-facing* knob.
- `bpm =`, `cps =`, `beats_per_cycle =` as akkado source-level setters; setting
  any one recomputes the others per fixed rules.
- `beat()` builtin re-spec'd to read `beats_per_cycle` (no deprecation; old
  patches keep compiling and behave identically at the default `bpc = 1`).
- `beats(n)` / `bars(n)` 1-line stdlib helpers.
- A `beats_per_cycle` web *project setting* (numeric input, not in the
  transport widget).

**Prerequisite:** [`prd-cycle-length-cleanup.md`](prd-cycle-length-cleanup.md)
must land first — this PRD assumes `cycle_length` is already gone and
`ExecutionContext` already exposes a single `cycle_offset` field.

---

## 1. Why This Is Now Optional, Not a Cleanup

The former `prd-cycles-pure-clock-model.md` framed `beats_per_cycle` as the
field that "replaces the hardcoded `* 4`". That framing is stale: the `* 4` was
removed by the parser revert, and `samples_per_cycle()` is already
`sample_rate / cps`-equivalent with an implicit `bpc = 1`.

So `beats_per_cycle` is no longer cleanup — it is a **new feature**: a knob that
lets `samples_per_cycle()` honour a value other than 1. That makes this PRD:

- **Additive.** Default `bpc = 1` reproduces today's behaviour exactly.
- **Non-breaking.** No patch sweep, no migration table, no CHANGELOG breaking
  note. (Contrast the former PRD, which carried a "headline 4× speedup as the
  accepted breaking change" — that breakage already happened.)
- **Independently schedulable.** Nothing depends on it.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Add `ExecutionContext::beats_per_cycle` (default 1.0) and `cps` (derived).
2. `set_bpm()` / `set_cps()` / `set_beats_per_cycle()` keep the three values
   mutually consistent.
3. Expose `cps` and `beats_per_cycle` as akkado source-level setters alongside
   the existing `bpm =`.
4. Re-spec `beat()` in terms of `beats_per_cycle`; ship `beats(n)` / `bars(n)`.
5. Add a `beats_per_cycle` web project setting (default 1).
6. Doc sweep for the new knob.

### 2.2 Non-Goals

- **Changing the default.** `bpc = 1` (Strudel-pure) stays the default.
- **Time-signature UI** (4/4, 6/8 dropdown). Out — a raw float is the honest
  representation. See §7.
- **Transport widget redesign.** BPM slider stays primary; `beats_per_cycle`
  lives in project settings only.
- **Removing BPM.** BPM is the canonical user-facing knob.
- **`cycle_length` removal.** Owned by `prd-cycle-length-cleanup.md`.

---

## 3. Design

### 3.1 `ExecutionContext`

```cpp
float bpm = DEFAULT_BPM;                       // canonical USER-FACING knob
float cps = DEFAULT_BPM / 60.0f;               // canonical INTERNAL knob (derived)
float beats_per_cycle = 1.0f;                  // bridge; default = Strudel-pure

void set_bpm(float v)             { bpm = v; recompute_cps(); }
void set_cps(float v)             { cps = v; bpm = v * 60.0f * beats_per_cycle; }
void set_beats_per_cycle(float v) { beats_per_cycle = max(v, 1e-3f); recompute_cps(); }
//                                  ^ bpm preserved, cps recomputed
private:
void recompute_cps() { cps = bpm / (60.0f * beats_per_cycle); }

[[nodiscard]] float samples_per_cycle() const { return sample_rate / cps; }
[[nodiscard]] float samples_per_beat()  const { return samples_per_cycle() / beats_per_cycle; }
```

`samples_per_cycle()` changes from "uses an implicit `bpc = 1`" to "divides by
`cps`, which honours `beats_per_cycle`". At the default `bpc = 1` the value is
identical to post-cleanup behaviour.

### 3.2 Source-level setters

```akkado
bpm = 120                 // cps = 2.0 (bpc = 1)
beats_per_cycle = 4       // cps = 0.5; one cycle now spans 4 beats / 2 s
cps = 0.5                 // sets bpm = cps * 60 * bpc
```

Last writer wins when several appear at top level (see §6 OQ-2).

### 3.3 `beat()` re-spec

```akkado
// old: fn beat(n) -> {trigger(1/n)}
// new: fn beat(n) -> {trigger(beats_per_cycle / n)}
// At bpc = 1 the two are identical — no observable change for existing patches.
```

### 3.4 stdlib helpers

```akkado
fn beats(n) -> {n / beats_per_cycle}      // n beats expressed in cycles
fn bars(n)  -> {n * 4 / beats_per_cycle}  // 1 bar = 4 beats (see OQ-1)

osc("saw", 220) |> delay_sync(@, beats(1))   // 1-beat delay at any bpc
```

### 3.5 Web

`web/src/lib/stores/settings.svelte.ts` gains `beatsPerCycle: number`
(default 1, persisted). A numeric input in the project-settings panel — **not**
the transport widget — plumbs through `nkido_wasm.cpp` to
`set_beats_per_cycle()`. Range enforced `0.0625 .. 64`.

---

## 4. Impact

| Component | Status |
|---|---|
| `ExecutionContext` | `beats_per_cycle`, `cps` added; setters added |
| `bpm =` setter / web BPM slider | Unchanged semantics |
| `beat()` builtin | Re-spec'd; identical at `bpc = 1` |
| `beats()` / `bars()` stdlib | New |
| `delay_sync`, `SEQPAT_TRANSPORT` | No change — already in cycles per the cleanup PRD |
| `co` builtin | Unchanged (`cycle_offset` field already exists post-cleanup) |
| Web project settings | New `beats_per_cycle` numeric input |
| Docs | Sweep — document the knob |

---

## 5. Verification

- `test_context_bpm_cps_coupling.cpp`: `set_bpm(120)` at `bpc = 1` → `cps == 2.0`;
  `set_cps(0.5)` at `bpc = 4` → `bpm == 120`; `set_beats_per_cycle(4)` preserves
  `bpm`, rescales `cps`.
- `test_beat_builtin.cpp`: `beat(4)` at `bpc = 4` → 1 trigger/cycle; at
  `bpc = 1` → 1 trigger / 4 cycles.
- `test_stdlib_beats_bars.cpp`: `beats(1)` returns `0.25` at `bpc = 4`, `1.0`
  at `bpc = 1`.
- Long render: `test_op_seq_cycles.py` rendering a pattern at `bpc = 1` and
  `bpc = 4`, ≥300 s each, both clean.
- Manual: set `beats_per_cycle = 4` in project settings, confirm a `bpm = 120`
  patch feels like 2 s cycles; default `bpc = 1` confirms Strudel pacing.

---

## 6. Open Questions

**OQ-1. `bars(n)` definition.** Hardcode `1 bar = 4 beats`, or add a separate
`beats_per_bar` knob? Recommendation: hardcode the 4; a musician who wants 3/4
or 7/8 sets `beats_per_cycle` and writes plain fractions. Lock or split.

**OQ-2. Setter precedence.** `bpm =` and `cps =` both at top level — last
writer wins, both leave the context consistent. Confirm no warning is wanted.

**OQ-3. Setter timing.** If `set_beats_per_cycle()` fires mid-block via
`env_map`, defer the `cps` recompute to the block boundary (consistent with how
BPM changes apply today). Confirm against the current block-boundary path.

**OQ-4. Hot-swap with changed `beats_per_cycle`.** Patterns mid-playback
re-snap to the new cycle duration at the next cycle boundary. Document; do not
attempt to cross-fade timing changes.

---

## 7. Rejected Alternatives

**7.1 Drop BPM, expose only CPS.** Rejected — BPM is the universal
musician-facing tempo unit. CPS is canonical internally and reachable from
akkado source; the web UI keeps BPM primary.

**7.2 Time-signature UI (4/4, 6/8 dropdown).** Rejected — per user direction,
"no fake distinction between 6/8 and 3/4". A raw `beats_per_cycle` float is the
honest representation; a dropdown would imply 6/8 and 3/4 differ in cycle math
(they do not).

**7.3 Default `beats_per_cycle = 4`.** Rejected — Strudel-pure is the intended
default and already shipped via the parser revert. Shipping a non-Strudel
default would force every Strudel-style patch to set `bpc = 1`.

**7.4 Transport widget redesign (CPS slider).** Rejected — per user direction,
"the UI is already perfect." `beats_per_cycle` lives in project settings.

**7.5 A `Time` value type pretty-printing beats/bars/cycles.** Rejected as
disproportionate type-system work; plain float fractions plus `beats(n)` /
`bars(n)` suffice. Re-litigate in a separate PRD if friction surfaces.
