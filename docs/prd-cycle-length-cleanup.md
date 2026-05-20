> **Status: DRAFT — rescoped 2026-05-20.** The original version of this PRD
> (split out of `prd-cycles-pure-clock-model.md` and marked READY FOR
> IMPLEMENTATION) was found to rest on a **false premise**: it claimed
> `cycle_length` was dead per-pattern plumbing that could be removed as a pure
> refactor. During implementation planning, the PRD's own **OQ-1** check failed
> — `cycle_length` is **live**: the `slow`/`fast`/`palindrome`/`linger` pattern
> transforms compute it, tests verify the computed values, and the runtime uses
> it to scale event times and drive cycle-wrap math. Removing the field would
> break those transforms. This PRD has been rescoped to the genuinely-dead
> cleanup that *does* exist. The `beats_per_cycle` / `cps` feature remains split
> into the separate, deferred [`prd-beats-per-cycle.md`](prd-beats-per-cycle.md).

# PRD: Clock-Phase & MIDI-Streaming Cleanup

## Executive Summary

The 2026-05-19 mini-notation revert (commit `9d99490`, "cycle = beat; top-level
spaces = per-cycle alternation") removed the hardcoded `* 4` bar length:
`ExecutionContext::samples_per_cycle()` no longer multiplies by 4, the
*default* `cycle_length` flipped 4.0 → 1.0, and bar phase collapsed into beat
phase. See the CHANGELOG `[Unreleased]` "cycle = beat" section.

The original draft of this PRD then claimed `cycle_length` itself was now dead
plumbing — "always `1.0f` for hand-written and compiled code" — and proposed
removing the field. **That is wrong.** The revert changed the *default* of
`cycle_length` to `1.0`; it did not remove the pattern transforms that compute
it:

- `slow(n)` does `cycle_length *= n`, `fast(n)` does `cycle_length /= n`,
  `palindrome()` does `cycle_length *= 2`, `linger(frac)` does
  `cycle_length *= frac` — see `akkado/src/codegen_patterns.cpp`. For
  `slow`/`fast` this is the *entire* timing implementation.
- The runtime uses the computed value load-bearingly: `query_pattern()` scales
  every event's time by `cycle_length`, and `SEQPAT_*` wrap math divides
  `beat_start` by `state.cycle_length`. Forcing it to `1.0` makes `slow(2)`
  play at normal speed.
- Tests verify the computed values (`test_codegen.cpp`,
  `test_codegen_cycle_timing.cpp` check `2.0f`, `0.5f`, `1/6f`, …).

So `cycle_length` **stays**. What *is* genuinely dead — and what this rescoped
PRD removes — is two narrower pieces of plumbing plus a comment/terminology
pass:

- `MidiQueueState` uses the `cycle_length = 1e6f` magic-number sentinel to mean
  "streaming MIDI input, never wrap". MIDI input genuinely has no pattern cycle;
  the sentinel is real dead weight and should be an explicit `streaming: bool`.
- `ExecutionContext` carries **both** `beat_phase` and `bar_phase`, with a
  comment confirming they are provably identical post-revert (`beat_phase ==
  bar_phase == cycle phase`). One of them is redundant.
- Several `delay_sync` / `SEQPAT_TRANSPORT` comments still say "beats" where
  "cycles" is now the precise term, and `instruction.hpp:171`'s
  `SEQPAT_TRANSPORT` enum comment is stale (references a pre-`37263dc`
  `in[3]+in[4]` bit-pack that no longer exists).

This is a **pure refactor**: bit-exact audio output, an unchanged test suite.
No field is removed from `SequenceState` / `PatternPayload` /
`EventSourcePayload`; no behaviour changes.

This PRD deliberately does **not** introduce `beats_per_cycle` or `cps` — that
configurable time-signature knob is specified separately in
[`prd-beats-per-cycle.md`](prd-beats-per-cycle.md).

---

## 1. Current State

### 1.1 What is dead vs. what is live

| Layer | Today | Status |
|---|---|---|
| Mini-notation parser | Produces phase-normalized `Event::time ∈ [0, 1)` (2026-05-18 fix) | Correct — keep |
| `Sequence::duration` | Always `1.0f` | Correct — keep |
| Per-pattern `cycle_length` on `SequenceState` / `PatternPayload` / `EventSourcePayload` | *Default* `1.0f`, but `slow`/`fast`/`palindrome`/`linger` compute non-1.0 values (`codegen_patterns.cpp`) | **LIVE — keep** |
| `query_pattern(state, cycle, cycle_length)` | `cycle_length` scales every event time in `query_sequence`; non-1.0 under `slow`/`fast` | **LIVE — keep** |
| `SEQPAT_TRANSPORT` `cycle_length` via `ExtendedParams<1>` | `codegen_patterns.cpp:4124` writes the computed `cycle_length`; a `slow()`-wrapped `transport()` pattern carries a non-1.0 value | **LIVE — keep** |
| `resolve_output_events()` returning `cycle_length` | POLY's gate-timing wrap math (`state_pool.hpp:116`) needs it | **LIVE — keep** |
| `MidiQueueState::cycle_length = 1e6f` | Sentinel meaning "streaming, never wrap" | **Dead magic-number — replace with `streaming: bool`** |
| `ExecutionContext::beat_phase` / `bar_phase` | Both computed each block; comment confirms they are equal under cycle = beat | **Redundant — collapse to one field** |
| `instruction.hpp:171` `SEQPAT_TRANSPORT` enum comment | Says "cycle_length packed in in[3]+in[4]" | **Stale since the `37263dc` ExtendedParams migration — fix** |

### 1.2 File inventory (in scope)

**Cedar runtime:**

- `cedar/include/cedar/opcodes/midi.hpp:52,113,117,247,281` — `MidiQueueState::cycle_length` (`1.0e6f` sentinel) and its move ctor/assignment copies
- `cedar/src/vm/vm.cpp:364,438-489` — POLY wrap branch compares the resolved source `cycle_length` against the sentinel; needs to branch on `streaming` for the MIDI source
- `cedar/include/cedar/vm/state_pool.hpp:116-138` — `resolve_output_events()` returns `{events, cycle_length}`; the MIDI return path (`:138`) should surface `streaming`
- `cedar/include/cedar/vm/state_pool.hpp:477` — JSON inspector emits `cycle_length` for `TransportState`/`SequenceState` (keep) and for the MIDI state (replace with `streaming`)
- `cedar/include/cedar/vm/context.hpp:60-69,97-100` — `beat_phase`, `bar_phase`, `beat_phase_at_sample()`
- `cedar/include/cedar/opcodes/sequencing.hpp:200` — `EUCLID` computes a local `bar_phase` from `samples_per_bar`
- `cedar/include/cedar/vm/instruction.hpp:171` — stale `SEQPAT_TRANSPORT` enum comment

**Akkado compiler:**

- `akkado/include/akkado/typed_value.hpp:146` — `EventSourcePayload::cycle_length` (keep; gains a `streaming` flag for MIDI sources)

**Out of scope (live — not touched):** every `cycle_length` reference in
`SequenceState`, `PatternPayload`, `query_pattern`, the ~149
`codegen_patterns.cpp` assignments and the `lower_inner_transform()` thread.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Replace `MidiQueueState::cycle_length = 1e6f` with an explicit
   `streaming: bool` flag. Add a `streaming` flag to `EventSourcePayload` so
   MIDI sources are distinguished from pattern sources by an honest boolean,
   not a magic number. Every consumer that compared against the `1e6f` sentinel
   ("if `cycle_length` is huge, don't wrap") branches on `streaming` instead.
2. Collapse `ExecutionContext::beat_phase` / `bar_phase` into a single
   `cycle_offset` field (name matches the user-facing `co` builtin). Rename
   `beat_phase_at_sample()` → `cycle_offset_at_sample()`; update callers. The
   `co` builtin reads `cycle_offset`.
3. Fix the stale `instruction.hpp:171` `SEQPAT_TRANSPORT` enum comment; relabel
   `delay_sync` / `SEQPAT_TRANSPORT::step` parameter docs/comments from "beats"
   to "cycles" — **terminology only**. Since `1 cycle == 1 beat` post-revert,
   the numeric contract is unchanged; no value is retuned.

### 2.2 Non-Goals

- **Removing the `cycle_length` field** from `SequenceState` / `PatternPayload`
  / `EventSourcePayload` / `query_pattern` / `SEQPAT_TRANSPORT`. It is
  load-bearing: `slow`/`fast`/`palindrome`/`linger` compute it and the runtime
  scales event times by it. Explicitly out of scope.
- **`beats_per_cycle` / `cps`** — the configurable time-signature knob. Split
  into [`prd-beats-per-cycle.md`](prd-beats-per-cycle.md).
- **`beat()` re-spec, `beats()` / `bars()` stdlib helpers** — belong to
  `prd-beats-per-cycle.md`.
- **Any behaviour change.** Pure refactor. Audio output is bit-exact.
- **Web UI changes.** None.

---

## 3. Technical Design

### 3.1 `MidiQueueState` / `EventSourcePayload` — `streaming` flag

`MidiQueueState::cycle_length = 1e6f` → `MidiQueueState::streaming = true`. The
sentinel was chosen so that `fmod(beat_start, cycle_length) == beat_start` and
`current_cycle == 0` for the whole session — i.e. the POLY wrap branch never
fires for MIDI. Every consumer of that behaviour instead branches on
`streaming`:

- `resolve_output_events()` (`state_pool.hpp:116-138`) currently returns
  `{events, cycle_length}`. It gains a `streaming` field; the MIDI return path
  sets `streaming = true`, the sequence path sets `streaming = false` and keeps
  returning the real `cycle_length` (still needed by POLY).
- The POLY wrap math in `vm.cpp` (`:364,438-489`) skips the cycle-wrap branch
  when `streaming` is set, instead of detecting the `1e6f` sentinel.

`EventSourcePayload` **keeps** `cycle_length` (pattern sources can be
`slow`-wrapped) and **gains** `bool streaming = false`, set `true` only for MIDI
sources.

### 3.2 `ExecutionContext` — collapse phase fields

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

`EUCLID`'s locally-computed `bar_phase` (`sequencing.hpp:200`) is updated to
read `cycle_offset` / `samples_per_cycle()` — see OQ-3.

### 3.3 Comment / terminology pass

The `instruction.hpp:171` enum comment is corrected (it still references the
pre-`37263dc` `in[3]+in[4]` bit-pack, which no longer exists). `delay_sync` and
`SEQPAT_TRANSPORT::step` parameter comments are relabelled "beats" → "cycles".
No numeric value changes.

---

## 4. Implementation

Single PR. Mechanical, no behaviour delta. Suggested commit split:

1. **Cedar** — replace `MidiQueueState` `1e6f` with `streaming`; thread
   `streaming` through `resolve_output_events()` and the POLY wrap branch in
   `vm.cpp`; collapse `beat_phase`/`bar_phase` → `cycle_offset` in
   `context.hpp`; update `EUCLID`; fix the `instruction.hpp` comment.
2. **Akkado** — add `streaming` to `EventSourcePayload`; set it for MIDI
   sources.
3. **Docs/comments** — relabel `delay_sync` / `transport` param comments
   "beats" → "cycles"; rerun `bun run build:opcodes` if opcode metadata shifts.

---

## 5. Verification

- **Bit-exactness is the acceptance bar.** `./build/cedar/tests/cedar_tests`
  and `./build/akkado/tests/akkado_tests` pass **unchanged** — no assertion
  edits. Any test that referenced the MIDI `cycle_length` in a JSON-inspector
  string updates to expect `streaming` instead; that is the only permitted test
  diff. Tests asserting `SequenceState` / `StateInitData` `cycle_length`
  (`slow`/`fast`/`palindrome`/`linger`) are untouched.
- JSON state inspector output for MIDI / event-source states contains
  `"streaming"` instead of a `1e6f` `"cycle_length"`; `TransportState` /
  `SequenceState` still emit their real `cycle_length`.
- `experiments/run_all.sh` green; one ≥300 s render (`test_op_seq*.py`)
  confirmed byte-identical to a pre-PR baseline WAV.
- `cd web && bun run check && bun run build`.

---

## 6. Open Questions / Resolved

**OQ-1 (RESOLVED — premise was wrong).** The original PRD asked whether any
`cycle_length` assignment was non-trivial, warning that one would mean "the
field is not actually dead." Confirmed during planning: `codegen_patterns.cpp`
has non-trivial assignments — `slow` (`*= factor`), `fast` (`/= factor`),
`palindrome` (`*= 2`), `linger` (`*= frac`) — verified by tests in
`test_codegen.cpp` / `test_codegen_cycle_timing.cpp`. The field is **live**.
The PRD was rescoped accordingly: `cycle_length` removal is now a Non-Goal.

**OQ-2.** `docs/audits/mini-notation-cycle-timing_audit_2026-05-18.md` references
`cycle_length`. Skim during the doc pass — expected to be historical narrative
that needs no edit, but confirm nothing it documents as live breaks.

**OQ-3.** `EUCLID` computes a local `bar_phase` from `samples_per_bar`
(`sequencing.hpp:200`). Confirm `samples_per_bar == samples_per_cycle`
post-revert so the `beat_phase`/`bar_phase` collapse — and the EUCLID update —
is genuinely bit-exact. If `samples_per_bar` still carries a stale `* 4`,
surface it before editing.

---

## 7. Downstream

[`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md) §0.5
declares this PRD a **hard prerequisite** on the now-falsified assumption that
it removes the `cycle_length` field — it expects `EventStreamPayload` to land
*without* `cycle_length`. Since `cycle_length` is live and stays, that PRD's
§0.5, §3.4 Phase B ("must drop `cycle_length`") and §11 OQ-7 ("`fast`/`slow` ↔
`cycle_length` interaction evaporates") all need rework, and the hard-prereq
relationship should be re-evaluated — this rescoped cleanup is largely
orthogonal to runtime event transforms.
