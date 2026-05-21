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
  bar_phase == cycle phase`). In fact both fields — and the
  `beat_phase_at_sample()` helper — are **dead**: `update_timing()` writes
  `beat_phase`/`bar_phase` but nothing in `cedar/`, `akkado/`, `tools/` or
  `web/` reads either field, and `beat_phase_at_sample()` has zero callers.
  They collapse to a single `cycle_offset` field.
- Several `delay_sync` / `SEQPAT_TRANSPORT` comments still say "beats" where
  "cycles" is now the precise term; `instruction.hpp:172`'s `SEQPAT_TRANSPORT`
  enum comment is stale (references a pre-`37263dc` `in[3]+in[4]` bit-pack that
  no longer exists); and `op_clock`'s rate-mode comment (`sequencing.hpp:28-30`)
  still says "per 4 beats".

**Bit-exact audio output is the acceptance bar.** The `ExecutionContext` and
comment work is genuinely mechanical (dead fields, comment text). The
`MidiQueueState` change is *behaviour-preserving* but not purely mechanical:
the `1e6f` `cycle_length` is load-bearing *arithmetically* (see §3.1), so
replacing it with a `streaming` flag means adding explicit guards at every
cycle-wrap site and arguing bit-exactness site-by-site. No field is removed
from `SequenceState` / `PatternPayload` / `EventSourcePayload`; no audio
behaviour changes.

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
| `ExecutionContext::beat_phase` / `bar_phase` | Both written by `update_timing()` each block; **no reader anywhere** in `cedar/`/`akkado/`/`tools/`/`web/` | **Dead — collapse to one `cycle_offset` field** |
| `ExecutionContext::beat_phase_at_sample()` | Defined at `context.hpp:97`; **zero callers** | **Dead — rename to `cycle_offset_at_sample()` as the surviving helper** |
| `instruction.hpp:172` `SEQPAT_TRANSPORT` enum comment | Says "cycle_length packed in in[3]+in[4]" | **Stale since the `37263dc` ExtendedParams migration — fix** |

### 1.2 File inventory (in scope)

**Cedar runtime:**

- `cedar/include/cedar/opcodes/midi.hpp:46,108-117,247,281` — `MidiQueueState::cycle_length` (`1.0e6f` sentinel field at `:117`), the sentinel-explanation comments (`:46`, `:52`, `:108-117` — including a stale `vm.cpp:401-424` line ref), and the move ctor/assignment copies (`:247`, `:281`)
- `cedar/src/vm/vm.cpp` — the cycle-wrap math is `compute_block_timing()` at `:558` plus the on/off event loops that read `cycle_length` (`~646-741`, `~944-954`, `~1012+`). vm.cpp does **not** compare against `1e6f`; the sentinel works purely arithmetically (a huge `cycle_length` makes `fmod` an identity and `floor → 0`). Replacing it means adding explicit `streaming` guards at each of these wrap sites — see §3.1
- `cedar/include/cedar/vm/state_pool.hpp:118-141` — `resolve_output_events()` returns `ResolvedEvents{events, cycle_length}`; the MIDI return path (`:137-139`) should surface `streaming`
- `cedar/include/cedar/vm/state_pool.hpp:474-488` — JSON inspector emits `cycle_length` for `TransportState` (`:477`) / `SequenceState` (`:481`) — **both kept unchanged**. `inspect_state_json` has no `MidiQueueState` branch, so no MIDI `cycle_length` is serialized
- `cedar/include/cedar/vm/context.hpp:60-69,97-100` — `beat_phase`, `bar_phase`, `beat_phase_at_sample()` (all dead)
- `cedar/include/cedar/opcodes/sequencing.hpp:28-30` — stale `op_clock` rate-mode comment ("per 4 beats")
- `cedar/include/cedar/opcodes/sequencing.hpp:200` — `EUCLID` computes a local `bar_phase` from `samples_per_bar`
- `cedar/include/cedar/vm/instruction.hpp:172` — stale `SEQPAT_TRANSPORT` enum comment

**Akkado compiler:**

- `akkado/include/akkado/typed_value.hpp:185-188` — `EventSourcePayload` (`cycle_length` field at `:187`; keep — gains a `streaming` flag for MIDI sources)

**Out of scope (live — not touched):** every `cycle_length` reference in
`SequenceState`, `PatternPayload`, `query_pattern`, the ~149
`codegen_patterns.cpp` assignments and the `lower_inner_transform()` thread.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Replace `MidiQueueState::cycle_length = 1e6f` with an explicit
   `streaming: bool` flag. Add a `streaming` flag to `EventSourcePayload` so
   MIDI sources are distinguished from pattern sources by an honest boolean,
   not a magic number. Every cycle-wrap site that relied on the huge
   `cycle_length` *arithmetically* suppressing the wrap gains an explicit
   `streaming` guard instead.
2. Collapse the dead `ExecutionContext::beat_phase` / `bar_phase` fields into a
   single `cycle_offset` field, and rename the dead `beat_phase_at_sample()`
   helper → `cycle_offset_at_sample()`. There is **no `co` builtin** and there
   are **no callers** of either — `update_timing()` writes the fields but
   nothing reads them. The surviving `cycle_offset` field/helper is therefore
   *also* unread today; it is retained (rather than deleted outright) as the
   intended seam for [`prd-beats-per-cycle.md`](prd-beats-per-cycle.md), which
   will give it a reader. See §3.2.
3. Fix the stale `instruction.hpp:172` `SEQPAT_TRANSPORT` enum comment and the
   `op_clock` rate-mode comment (`sequencing.hpp:28-30`, "per 4 beats");
   relabel `delay_sync` / `SEQPAT_TRANSPORT::step` parameter docs/comments from
   "beats" to "cycles" — **terminology only**. Since `1 cycle == 1 beat`
   post-revert, the numeric contract is unchanged; no value is retuned.

### 2.2 Non-Goals

- **Removing the `cycle_length` field** from `SequenceState` / `PatternPayload`
  / `EventSourcePayload` / `query_pattern` / `SEQPAT_TRANSPORT`. It is
  load-bearing: `slow`/`fast`/`palindrome`/`linger` compute it and the runtime
  scales event times by it. Explicitly out of scope.
- **`beats_per_cycle` / `cps`** — the configurable time-signature knob. Split
  into [`prd-beats-per-cycle.md`](prd-beats-per-cycle.md).
- **`beat()` re-spec, `beats()` / `bars()` stdlib helpers** — belong to
  `prd-beats-per-cycle.md`.
- **Any audio behaviour change.** Behaviour-preserving cleanup; audio output
  is bit-exact (the vm.cpp `streaming` rewrite is verified site-by-site, not
  assumed — see §3.1, §5).
- **Web UI changes.** None.

---

## 3. Technical Design

### 3.1 `MidiQueueState` / `EventSourcePayload` — `streaming` flag

`MidiQueueState::cycle_length = 1e6f` → `MidiQueueState::streaming = true`.

**The sentinel is load-bearing arithmetically, not via a branch.** No code in
`vm.cpp` ever compares `cycle_length` against `1e6f`. The value `1e6f` was
chosen so that, for any realistic session, `fmod(beat_start, cycle_length) ==
beat_start`, `floor(beat_start / cycle_length) == 0`, and `block_end_pos` never
exceeds `cycle_length` — so `compute_block_timing()` reports `current_cycle ==
0` and the cross-cycle on/off wrap conditions are never true. Removing the
field therefore can't be a search-and-replace: each cycle-wrap site must gain
an explicit `streaming` guard that reproduces that arithmetic exactly.

- `resolve_output_events()` (`state_pool.hpp:118-141`) currently returns
  `ResolvedEvents{events, cycle_length}`. It gains a `bool streaming` field;
  the MIDI return path (`:137-139`) sets `streaming = true`, the sequence path
  (`:135`) sets `streaming = false` and keeps returning the real `cycle_length`
  (still needed by POLY for `slow`-wrapped patterns).
- The POLY/FOREACH event loops in `vm.cpp` consume `events_src.cycle_length`
  (call sites ~`:651`, ~`:954`, ~`:1012+`). Each gains a `streaming` guard.
  The wrap arithmetic lives in `compute_block_timing()` (`:558`) and the on/off
  offset loops (~`:646-741`). The implementer must enumerate every wrap site
  reached from these call sites and, when `streaming` is set, take the
  no-wrap path (`current_cycle == 0`, cycle-relative == absolute coordinates)
  that the `1e6f` arithmetic produces today. Bit-exactness is argued
  per-site against a pre-PR baseline (see §5), not assumed.

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
`cycle_offset_at_sample()`.

**Note — `cycle_offset` has no reader today.** `beat_phase` / `bar_phase` are
written by `update_timing()` but read by nothing, and `beat_phase_at_sample()`
has zero callers (the `co` token is a *pattern-event field* alias for
`PatternPayload::PHASE`, unrelated to `ExecutionContext`; the `clock` / `CLOCK`
opcode computes phase locally from `samples_per_beat()` / `samples_per_bar()`).
The collapse therefore leaves a single field that is still written-but-unread.
It is retained — rather than deleted outright — as the deliberate seam for
[`prd-beats-per-cycle.md`](prd-beats-per-cycle.md), which adds the first
consumer. This is a conscious choice, not overlooked dead plumbing; the §1.1
table and Goal 2 flag it as such so a future cleanup pass does not re-file it.

`EUCLID`'s locally-computed `bar_phase` (`sequencing.hpp:200`) is updated to
read `samples_per_cycle()` (currently `samples_per_bar()` via `spb`) — a
no-op rename, see OQ-3.

### 3.3 Comment / terminology pass

The `instruction.hpp:172` enum comment is corrected (it still references the
pre-`37263dc` `in[3]+in[4]` bit-pack, which no longer exists). The `op_clock`
rate-mode comment (`sequencing.hpp:28-30`) is corrected — it still says
"`1 = bar_phase (0-1 per 4 beats)`" and "`2 = cycle_offset (same as
bar_phase)`"; post-revert all three rate modes (0/1/2) are numerically
identical. The CLOCK opcode's `inst.rate` modes are a stable ABI and are
**not** changed — only the comment. The `midi.hpp` sentinel-explanation
comments (`:46`, `:52`, `:108-117`, including a stale `vm.cpp:401-424` line
ref) are rewritten to describe the `streaming` flag. `delay_sync` and
`SEQPAT_TRANSPORT::step` parameter comments are relabelled "beats" → "cycles".
No numeric value changes.

---

## 4. Implementation

Single PR. Behaviour-preserving (bit-exact audio), but the vm.cpp portion is
not a pure search-and-replace — see §3.1. Suggested commit split:

1. **Cedar** — replace `MidiQueueState` `1e6f` with `streaming`; thread
   `streaming` through `resolve_output_events()` and add an explicit
   `streaming` guard at every cycle-wrap site reached from the POLY/FOREACH
   event loops in `vm.cpp` (see §3.1); collapse `beat_phase`/`bar_phase` →
   `cycle_offset` and rename `beat_phase_at_sample()` in `context.hpp`; update
   `EUCLID`; fix the `instruction.hpp` comment.
2. **Akkado** — add `streaming` to `EventSourcePayload`; set it for MIDI
   sources.
3. **Docs/comments** — fix the `op_clock` rate-mode comment and the `midi.hpp`
   sentinel-explanation comments; relabel `delay_sync` / `transport` param
   comments "beats" → "cycles"; rerun `bun run build:opcodes` if opcode
   metadata shifts.

---

## 5. Verification

- **Bit-exactness is the acceptance bar.** `./build/cedar/tests/cedar_tests`
  and `./build/akkado/tests/akkado_tests` pass **unchanged** — no assertion
  edits expected. Tests asserting `SequenceState` / `StateInitData`
  `cycle_length` (`slow`/`fast`/`palindrome`/`linger`) are untouched. Note:
  `inspect_state_json` has **no `MidiQueueState` branch**, so the MIDI
  `cycle_length` is never serialized — there is no JSON-inspector test to
  update.
- JSON state inspector output for `TransportState` / `SequenceState` still
  emits their real `cycle_length`, unchanged.
- `experiments/run_all.sh` green; one ≥300 s render (`test_op_seq*.py`)
  confirmed byte-identical to a pre-PR baseline WAV. **Add a MIDI-driven
  ≥300 s render** (a `poly()` fed by streaming MIDI events) confirmed
  byte-identical to baseline — this is the load-bearing check for the
  `streaming`-guard rewrite in §3.1.
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

**OQ-3 (RESOLVED).** `EUCLID` computes a local `bar_phase` from
`samples_per_bar` (`sequencing.hpp:200`). Confirmed against `context.hpp`:
`samples_per_bar()` returns `samples_per_beat()`, `samples_per_cycle()` returns
`samples_per_beat()` — all three are identical, with **no stale `* 4`**.
`EUCLID`'s local `samples_per_bar` is already `ctx.samples_per_beat()`
(`sequencing.hpp:194`). The EUCLID update is a pure `samples_per_bar()` →
`samples_per_cycle()` rename and is genuinely bit-exact.

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
