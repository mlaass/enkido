# Mini-notation polyrhythm with compound branches drops voices; `poly()` voice_fields was a red herring

**Date:** 2026-04-26
**Areas:**
- `akkado/src/codegen_patterns.cpp` — polyrhythm codegen
- `akkado/include/akkado/typed_value.hpp` — `PatternPayload` cleanup

**Symptom (user-reported):**
> `n"[bd, [hh hh hh hh]]" |> out(@)` should play the bd every cycle alongside four 16th-note hihats. Today only the hihats play; the kick is silently dropped. Arbitrary nesting of polyrhythm should be a simple recursive algorithm — the fact that this doesn't work suggests the codegen got too complex.

**Reproducer:** `s"[bd, [hh hh hh hh]]" |> out(%, %)`. Codegen test confirms the bug at `akkado/tests/test_codegen.cpp:3170` (new) — pre-fix, only 4 events were emitted (the four hihats), with the bd merged-into-nowhere because the merge fast-path couldn't accept compound children.

---

## TL;DR

The mini-notation parser and AST were fine. Two issues, only one a real bug:

| # | Bug | Root cause | Fix |
|---|---|---|---|
| A | Polyrhythm with any compound child silently drops voices at runtime | Codegen had a dual path: a "merge into single multi-voice event" fast path that only accepted bare-sample atoms, plus a fallback that emitted parallel events into the same single-stream sequence. The runtime `SEQPAT_QUERY/STEP` (and `SAMPLE_PLAY`) walks events in time order and treats them as mutually exclusive — when bd and hh both started at t=0, only the last-crossed event was "current," dropping bd. | Replace both paths with a single recursive `flatten_to_timelines` + merge in `codegen_patterns.cpp`. Each polyrhythm branch becomes its own parallel timeline; group/euclidean nodes widen when they enclose a polyrhythm; the merger emits one multi-voice DATA event per unique trigger time. The merged path now handles arbitrary nesting up to `MAX_VALUES_PER_EVENT` (4) total branches. |
| B | (Reported, but not actually a bug.) "Pitched polyrhythm into `poly()` is broken because `payload->voice_fields` is never populated." | `voice_fields` was dead code — declared in `PatternPayload` but never read or written anywhere. `poly()` does **not** consume `voice_fields`; `POLY_BEGIN` reads its source events directly from the linked `SequenceState` via `poly_seq_state_id` and allocates one voice per `(event, evt.values[vi])` pair. | Removed `voice_fields` (and the unused `num_voices`) from `PatternPayload`. Added codegen tests covering pitched polyrhythm + `poly()`, and verified end-to-end via FFT that `[c4, e4]` and `[c4, [e4 g4 b4]]` both produce the expected pitches. |

---

## Bug A — `[bd, [hh hh hh hh]]` drops the kick

### Pipeline trace

1. `mini_parser.cpp` correctly parses the pattern as a `MiniPolyrhythm` with two children: `MiniAtom` (bd) and `MiniGroup` containing four `MiniAtom`s (hh × 4). Verified via existing AST tests.
2. `codegen_patterns.cpp:474-491` (`compile_polyrhythm_events`, pre-fix):
   - **Fast path** `can_merge_sample_polyrhythm` (L497) required *every* direct child to be a bare `Sample`/`Rest` atom. The `MiniGroup` fails this check → merge bails out.
   - **Fallback** (L487-491) iterated children and called `compile_into_sequence` for each into the same sequence at the same `time_offset`/`time_span`. For the user's pattern this produced 5 events into one sequence: `bd@0/dur=1`, `hh@0/dur=0.25`, `hh@0.25/dur=0.25`, `hh@0.5/dur=0.25`, `hh@0.75/dur=0.25`.
3. At runtime, `SEQPAT_QUERY` (`cedar/include/cedar/opcodes/sequencing.hpp:366`) sorts events by `.time` and `SEQPAT_STEP` (L415) walks the stream as if events were mutually exclusive — `current_index` advances past everything whose `time <= beat_pos`. With bd and hh both at `t=0`, both get crossed in one tick and the last-crossed one wins. The single-stream assumption also shows up in `SEQPAT_TYPE` and `SEQPAT_GATE`. There is no way to represent two events sounding simultaneously except by packing them into one `Event.values[]` array.
4. The recently-added merge path (commit `b72d774`, two days before this report) was the right idea — but only for the special case of a *flat* polyrhythm of bare sample atoms. The moment a branch was anything more interesting (nested polyrhythm, group, euclidean) it fell back to the broken fallback.

### Why the regression test missed it

`akkado/tests/test_codegen.cpp:3145` (`[[bd, sd], hh] → outer polyrhythm falls back`) explicitly asserted the buggy codegen shape — "2 events both at time 0" — without verifying playback. Codegen tests are necessary but not sufficient: when two events sit at the same time in a single sequence, the runtime can only honor one, but the codegen-output test doesn't notice.

### Fix

Replace `compile_polyrhythm_events`, `can_merge_sample_polyrhythm`, and `emit_merged_sample_polyrhythm` with one recursive function in `akkado/src/codegen_patterns.cpp`:

- `flatten_to_timelines(NodeIndex, t_offset, t_span)` recursively expands a subtree into N≥1 parallel `BranchTimeline`s. `MiniPolyrhythm` concatenates all children's timelines; `MiniGroup` / `MiniPattern` / `MiniPolymeter` subdivide time and pad to the max child width; `MiniEuclidean` does the same per hit; `MiniAtom` returns one timeline with one event.
- `compile_polyrhythm_events` flattens its branches, then walks the union of unique trigger times across all branches and emits one merged `DATA` event per time, with `values[i]` = branch *i*'s sample_id at that time (0 means "no new trigger on this slot now"). `SAMPLE_PLAY`'s existing per-slot voice allocation already treats `values[i] == 0` as "skip this voice slot," so silence-on-empty falls out for free.
- An `is_flattenable_sample_subtree` gate restricts the new path to subtrees of pure sample/rest atoms inside group/polyrhythm/polymeter/euclidean (with optional Repeat/Weight modifiers). Pitched / chord / alternation / random branches still fall through the legacy per-child compile.
- Cap = `MAX_VALUES_PER_EVENT` (4). Excess branches truncate silently — same convention as the previous merge.

For the user's pattern, the new codegen produces:

| Event idx | time | num_values | values[0] (bd slot) | values[1] (hh slot) |
|---|---|---|---|---|
| 0 | 0.00 | 2 | bd | hh |
| 1 | 0.25 | 2 | 0 | hh |
| 2 | 0.50 | 2 | 0 | hh |
| 3 | 0.75 | 2 | 0 | hh |

Runtime: `SAMPLE_PLAY` triggers a kick voice plus a hat voice at `t=0`; only a hat voice at the three later times. The kick voice plays its sample to natural completion — no sustain marker required.

### Tests added/updated (`akkado/tests/test_codegen.cpp`)

| Section | Pattern | Asserts |
|---|---|---|
| Updated L3145 | `[[bd, sd], hh]` | One merged event with `num_values=3, values=[bd, sd, hh]` (was "2 events at t=0" — encoded the bug) |
| New L3170 | `[bd, [hh hh hh hh]]` | 4 events at 0/0.25/0.5/0.75; bd in slot 0 only at t=0; hh in slot 1 always; correct sample_mappings |
| New L3212 | `[bd, [bd, cp]]` | 1 event with `num_values=3, values=[bd, bd, cp]` (recursive flatten) |
| New L3229 | `[[bd cp], [hh hh hh hh]]` | 4 events; bd at t=0, cp at t=0.5, hh on every step (boundary alignment) |
| New L3263 | `[bd [hh, sn] cp]` | 3 events; middle is multi-voice (`hh+sn`), demonstrating polyrhythm-inside-group widening |
| New L3296 | `[bd, sn, hh, cp, oh]` | 5-voice over-cap → `num_values=4`, 5th voice dropped |

All `[codegen][samples][polyrhythm]` tests pass: 125 assertions, no regressions in the rest of the suite (487 test cases / 85,201 assertions total).

### Limitations (still apply after this fix)

- `MAX_VALUES_PER_EVENT = 4` voices per merged event. Polyrhythms with > 4 branches truncate. Raise the constant in `cedar/include/cedar/opcodes/sequence.hpp:34` if a real pattern hits this.
- Pitched polyrhythm still uses the legacy fallback (per-child compile, then runtime fans out via `POLY_BEGIN` — see Bug B). The new merge path is sample-only.
- Polyrhythm branches that contain alternation `<a b>` or random `a|b` cannot be flattened statically (they need runtime sub-sequences) and therefore also fall back to the legacy path.

---

## Bug B — `poly()` and `voice_fields` (a misdiagnosis)

### What was reported

The user, relayed from earlier exploration: "`payload->voice_fields` is never populated in `emit_pattern_with_state` — the per-voice SEQPAT_STEP buffers (built at L1649-1671) are constructed and then thrown away. `poly()` reads `voice_fields` and falls back to monophonic when empty, so multi-voice patterns into `poly()` lose voices."

### What's actually true

- `voice_fields` is declared in `akkado/include/akkado/typed_value.hpp:44` and **never read or written** anywhere in the repo. It is dead code. Same for `num_voices` on the same struct.
- `poly()` does not read `voice_fields`. The actual mechanism, in `cedar/src/vm/vm.cpp:254` (`VM::execute_poly_block`):
  - `handle_poly_call` (`akkado/src/codegen_functions.cpp:1546-1549`) captures the pattern's `pat_tv.pattern->state_id` and stores it as `poly_init.poly_seq_state_id`.
  - `POLY_BEGIN` runtime fetches the linked `SequenceState` directly and iterates `seq_state->output.events`. For each event whose start lands in the current block, it calls `poly_state.allocate_voice(evt.values[vi], …)` once per `vi` in `[0, evt.num_values)`. Voices are keyed by event index, so two events at `t=0` (each `num_values=1`) → two voices allocated, both gate-on at sample 0.
- This means pitched polyrhythm via the legacy per-child compile already works through `poly()`. The fallback emits two events at the same time, and POLY_BEGIN fans out across events the same way it fans out across an event's voices.

### End-to-end FFT verification

```
fn lead(freq, gate, vel) -> sine(freq) * gate * 0.3
n"[c4, e4]" |> poly(%, lead, 4) |> out(%, %)
```

Rendered 2 s with `nkido-cli render`, then FFT'd:

| Frequency | Magnitude |
|---|---|
| 261.5 Hz (c4) | 6888 |
| 329.5 Hz (e4) | 6885 |

Near-equal magnitudes — both notes sounding in parallel. The same approach with `[c4, [e4 g4 b4]]` shows c4 sustained throughout the cycle while e4/g4/b4 cycle through the thirds — all four pitches present in the spectrum.

### Fix

- Removed `voice_fields` and `num_voices` from `PatternPayload`. Replaced with a comment documenting that `poly()` reads its source through `SequenceState` directly via `POLY_BEGIN`, so per-voice buffer plumbing is intentionally absent.
- Added two `[codegen][poly]` test sections at `akkado/tests/test_codegen.cpp:4665` (new, after `mono with piped pattern`) that assert the SequenceState event shape for pitched polyrhythm into `poly()` and lock in the FFT-verified behavior.

### Why this misdiagnosis was easy to make

The exploration agent saw:
1. Per-voice buffers were allocated in `emit_pattern_with_state` (L1649-1671).
2. The `voice_fields` field existed on `PatternPayload`.
3. The buffers were never copied into `voice_fields`.

Conclusion *appeared* to be: "the wiring step is missing." The piece nobody read was the runtime — `POLY_BEGIN` doesn't go through `voice_fields`. The per-voice `SEQPAT_STEP` buffers are wired up to support pitch-pattern-as-control-signal use cases (e.g., `n"c4 e4" |> sine(%)`), where each voice's frequency drives an oscillator. Removing them would break those uses; they just don't talk to `poly()`.

The lesson: when the diagnosis is "X is missing in the codegen," confirm by tracing the *consumer* in the runtime. A field declared but never read is dead code, not a wiring bug.

---

## Files changed

- `akkado/src/codegen_patterns.cpp` — replaced polyrhythm codegen with recursive flatten+merge (~190 lines net change in the same area).
- `akkado/include/akkado/typed_value.hpp` — dropped `voice_fields` and `num_voices` from `PatternPayload`; added a `state_id` comment explaining the actual `poly()` wiring.
- `akkado/tests/test_codegen.cpp` — updated the `[[bd, sd], hh]` test (its assertion encoded the bug); added 5 new sample-polyrhythm tests and 2 new pitched-polyrhythm-into-`poly()` tests.

No changes to `cedar/` runtime opcodes or to the parser. The fix is pure codegen.

## Verification

- `./build/akkado/tests/akkado_tests "[codegen][samples][polyrhythm]"` — 125 assertions, all pass.
- `./build/akkado/tests/akkado_tests "[codegen][poly]"` — 65 assertions (was 41), all pass.
- `./build/akkado/tests/akkado_tests` — full suite: 487 cases / 85,201 assertions, no regressions.
- `./build/cedar/tests/cedar_tests` — 148 cases / 331,035 assertions, no regressions.
- FFT-of-render audible check for pitched polyrhythm via `nkido-cli render` (above).
