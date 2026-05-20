> **Status: READY FOR IMPLEMENTATION** — Mechanical cleanup PRD, split out of the former `prd-cycles-pure-clock-model.md` on 2026-05-20. Removes the now-dead `cycle_length` plumbing left behind by the 2026-05-19 "cycle = beat" parser revert. **No behaviour change, no patch sweep.** The user-facing `beats_per_cycle` / `cps` feature that the former PRD also bundled is split into the separate, deferred [`prd-beats-per-cycle.md`](prd-beats-per-cycle.md). This PRD is the hard prerequisite of [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md) §0.5.

# PRD: `cycle_length` Plumbing Cleanup — remove the dead per-pattern cycle field

## Executive Summary

The 2026-05-19 mini-notation revert (commit `9d99490`, "cycle = beat; top-level
spaces = per-cycle alternation") shipped the headline of the former cycles-pure
PRD: `ExecutionContext::samples_per_cycle()` no longer multiplies by 4, the
`cycle_length` default flipped 4.0 → 1.0, and bar phase collapsed into beat
phase. See the CHANGELOG `[Unreleased]` "cycle = beat" section.

What the revert left behind is **dead plumbing**:

- `cycle_length` is still a field on `SequenceState`, `PatternPayload`, and
  `EventSourcePayload`, still threaded through `SEQPAT_TRANSPORT`, `query_pattern()`,
  the state-pool init paths and the JSON inspector — but it is now **always
  `1.0f`** for hand-written and compiled code. It carries no information that
  isn't already encoded by the parser's phase normalization.
- `MidiQueueState` still uses the `cycle_length = 1e6f` magic-number sentinel
  to mean "no cycle wrap".
- `ExecutionContext` still carries **both** `beat_phase` and `bar_phase`, with a
  comment confirming they are now provably identical (`beat_phase == bar_phase
  == cycle phase`).

This PRD removes that dead plumbing. It is a **pure refactor**: the acceptance
bar is bit-exact audio output and an unchanged test suite. There is no new
field, no new builtin, no behaviour change, and no patch sweep — the per-patch
migration already happened in the 2026-05-18/19 work (commit `25bc560`).

This PRD deliberately does **not** introduce `beats_per_cycle` or `cps`. The
former cycles-pure PRD framed `beats_per_cycle` as "replaces the hardcoded
`* 4`" — but the `* 4` is already gone, so `beats_per_cycle` is now a *new
optional feature*, not a cleanup. It is specified separately in
[`prd-beats-per-cycle.md`](prd-beats-per-cycle.md) and depends on this PRD.

---

## 1. Current State

### 1.1 Why the field is dead

| Layer | Today | Status |
|---|---|---|
| Mini-notation parser | Produces phase-normalized `Event::time ∈ [0, 1)` (2026-05-18 fix) | Correct — keep |
| `Sequence::duration` | Always `1.0f` | Correct — keep |
| Per-pattern `cycle_length` | `1.0f` everywhere (was `4.0f` pre-revert; flipped by `9d99490`) | **Dead — remove** |
| `query_pattern(state, cycle, cycle_length)` | `cycle_length` arg always `1.0f`; `query_sequence` scales events by it | Re-scaling by 1.0 is a no-op — drop the arg |
| `MidiQueueState::cycle_length = 1e6f` | Sentinel meaning "streaming, never wrap" | **Magic number — replace with `streaming: bool`** |
| `ExecutionContext::beat_phase` / `bar_phase` | Both computed each block; comment confirms they are equal | **Redundant — collapse to one field** |
| `SEQPAT_TRANSPORT` `cycle_length` via `ExtendedParams<1>` | `ext[0]` = `cycle_length`, always `1.0f` (migrated off `inputs[3]/[4]` bit-pack by commit `37263dc`) | **Dead ExtendedParams slot — remove** |

### 1.2 File inventory

**Cedar runtime:**

- `cedar/include/cedar/opcodes/sequence.hpp:197` — `SequenceState::cycle_length = 1.0f`
- `cedar/include/cedar/opcodes/sequence.hpp:348` — `query_pattern(state, cycle, cycle_length)`, scales by `cycle_length`
- `cedar/include/cedar/opcodes/midi.hpp` — `MidiQueueState::cycle_length` (`1e6f` sentinel)
- `cedar/include/cedar/opcodes/sequencing.hpp:309-348` — `SEQPAT_TRANSPORT` reads `cycle_length` from `ExtendedParams<1>`; SEQPAT_* opcodes read `state.cycle_length`
- `cedar/include/cedar/vm/instruction.hpp:171` — `SEQPAT_TRANSPORT` enum comment still says "cycle_length packed in in[3]+in[4]" (**stale** since the `37263dc` ExtendedParams migration)
- `cedar/include/cedar/vm/context.hpp:60-69, 97` — `beat_phase`, `bar_phase`, `beat_phase_at_sample()`
- `cedar/include/cedar/vm/vm.hpp:171-174` — `SEQPAT_TRANSPORT` init signature threads `cycle_length`
- `cedar/include/cedar/vm/state_pool.hpp:116-138, 477-483, 807-934` — `resolve_output_events()` returns `cycle_length`; init paths and JSON inspector thread it

**Akkado compiler:**

- `akkado/include/akkado/typed_value.hpp:78` — `PatternPayload::cycle_length`
- `akkado/include/akkado/typed_value.hpp:146` — `EventSourcePayload::cycle_length`
- `akkado/src/codegen_patterns.cpp` — ~149 `cycle_length` references across assignments and the `lower_inner_transform()` thread; `"// 1 cycle = 4 beats"`-style comments

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Remove the `cycle_length` field from `SequenceState`, `PatternPayload`, and
   `EventSourcePayload`.
2. Drop the `cycle_length` parameter from `query_pattern()`; `query_sequence`
   is called with `time_scale = 1.0f` (events are already phase-normalized).
3. Remove the dead `ExtendedParams<1>` `cycle_length` slot from
   `SEQPAT_TRANSPORT`; fix the stale `instruction.hpp:171` comment.
4. Replace `MidiQueueState::cycle_length = 1e6f` with an explicit
   `streaming: bool` flag; add the same flag to `EventSourcePayload`.
5. Collapse `ExecutionContext::beat_phase` / `bar_phase` into a single
   `cycle_offset` field (name matches the user-facing `co` builtin).
6. Drop `cycle_length` from `resolve_output_events()`, the state-pool init
   signatures, and the JSON state inspector; the inspector emits `streaming`
   where relevant.
7. Relabel `delay_sync` and `SEQPAT_TRANSPORT::step` parameter docs/comments
   from "beats" to "cycles" — **terminology only**. Since `1 cycle == 1 beat`
   post-revert, the numeric contract is unchanged; no value is retuned.

### 2.2 Non-Goals

- **`beats_per_cycle` / `cps`** — the configurable time-signature knob. Split
  into [`prd-beats-per-cycle.md`](prd-beats-per-cycle.md). Not a dependency of
  this PRD or of `prd-runtime-event-transforms.md`.
- **`beat()` re-spec, `beats()` / `bars()` stdlib helpers** — belong to
  `prd-beats-per-cycle.md`. `beat()` is already correct under cycle = beat.
- **Any behaviour change.** This is a pure refactor. Audio output is bit-exact.
- **Any patch sweep.** The per-patch migration shipped in commit `25bc560`.
- **Web UI changes.** None.

---

## 3. Technical Design

### 3.1 `SequenceState`

```cpp
struct SequenceState {
    Sequence* sequences = nullptr;
    std::uint32_t num_sequences = 0;
    std::uint32_t seq_capacity = 0;
    // REMOVED: float cycle_length = 1.0f;
    std::uint64_t pattern_seed = 0;
    bool is_sample_pattern = false;
    bool streaming = false;          // NEW — replaces the MidiQueueState 1e6f sentinel
    OutputEvents output;
    // ... rest unchanged ...
};

inline void query_pattern(SequenceState& state, std::uint64_t cycle) {
    state.output.clear();
    std::uint64_t seed = state.pattern_seed + cycle;
    // events are already phase-normalized [0, 1) — time_scale is 1.0f
    query_sequence(state, 0, seed, 0.0f, 1.0f, state.output);
    state.output.sort_by_time();
    state.current_index = 0;
}
```

### 3.2 `MidiQueueState` / `EventSourcePayload`

`MidiQueueState::cycle_length = 1e6f` → `MidiQueueState::streaming = true`.
Every consumer that compared against the sentinel ("if `cycle_length` is huge,
don't wrap") instead branches on `streaming`. `EventSourcePayload` drops
`cycle_length` and gains `bool streaming = true`.

### 3.3 `ExecutionContext`

```cpp
// Before: beat_phase + bar_phase (provably equal under cycle = beat)
// After:  one field
float cycle_offset = 0.0f;   // 0-1 phase within current cycle

void update_timing() {
    float spc = samples_per_cycle();
    cycle_offset = std::fmod(static_cast<float>(global_sample_counter), spc) / spc;
}
```

`samples_per_cycle()` / `cycle_at_sample()` are unchanged (already correct
post-revert). `samples_per_beat()` / `beat_at_sample()` stay as derived getters
— under cycle = beat they equal the cycle helpers, and keeping them avoids
churn in `soundfont.hpp` and `delay_sync`. `beat_phase_at_sample()` is renamed
`cycle_offset_at_sample()`; callers updated. The `co` builtin reads
`cycle_offset` (it currently reads `beat_phase`).

### 3.4 `SEQPAT_TRANSPORT`

The `ExtendedParams<1>` carrying `cycle_length` is removed entirely — the
opcode no longer emits or reads a `StateInitData{ExtendedParams}` after the
instruction. Wrap math uses the constant `1.0f`. The `instruction.hpp:171`
enum comment is corrected (it still references the pre-`37263dc`
`in[3]+in[4]` bit-pack, which no longer exists).

---

## 4. Implementation

Single PR. The change is mechanical and has no behaviour delta, so it does not
need phasing. Suggested commit split inside the PR:

1. **Cedar** — remove `cycle_length` from `SequenceState`, `MidiQueueState`,
   `query_pattern`, `SEQPAT_TRANSPORT`, `state_pool.hpp`, `context.hpp`; add
   `streaming` / `cycle_offset`; fix the `instruction.hpp` comment.
2. **Akkado** — remove `cycle_length` from `PatternPayload` /
   `EventSourcePayload`; delete the ~149 `codegen_patterns.cpp` assignments and
   the `lower_inner_transform()` `out_cycle_length` thread; add `streaming`.
3. **Docs/comments** — relabel `delay_sync` / `transport` param comments
   "beats" → "cycles"; rerun `bun run build:opcodes` if opcode metadata shifts.

---

## 5. Verification

- **Bit-exactness is the acceptance bar.** `./build/cedar/tests/cedar_tests`
  and `./build/akkado/tests/akkado_tests` pass **unchanged** — no assertion
  edits. Any test that referenced `cycle_length` in a JSON-inspector string
  updates to expect `streaming` instead; that is the only permitted test diff.
- JSON state inspector output no longer contains `"cycle_length"`; contains
  `"streaming"` for MIDI / event-source states.
- `experiments/run_all.sh` green; one ≥300 s render (`test_op_seq*.py`)
  confirmed byte-identical to a pre-PR baseline WAV.
- `cd web && bun run check && bun run build`.

---

## 6. Open Questions

**OQ-1.** Confirm no code path other than the parser ever wrote a
`cycle_length` other than `1.0f` (or the MIDI `1e6f` sentinel). Grep
`codegen_patterns.cpp` during implementation; if a non-trivial assignment
exists, surface it before deleting — it would mean the field is not actually
dead. Expectation: all assignments are `1.0f` or sentinel.

**OQ-2.** `docs/audits/mini-notation-cycle-timing_audit_2026-05-18.md` references
`cycle_length`. Skim during the doc pass — expected to be historical narrative
that needs no edit, but confirm nothing it documents as live breaks.

---

## 7. Downstream

[`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md) §0.5
declares this PRD a hard prerequisite — it needs `EventStreamPayload` to land
*without* a `cycle_length` field. Once this PRD ships, update that PRD's §0.5
callout to confirm the prereq is met.
