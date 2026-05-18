> **Status: PRD** — Audit + remediation plan. Treat as the implementation contract for the fix. Drafted 2026-05-18.
>
> **Phase 1 shipped 2026-05-18** in commit `5c1c0ca` with one intentional divergence from the prescribed approach: the implementation keeps `cycle_length` in **beats throughout** (set `cycle_length = 4.0f` at every site) rather than the originally-prescribed "cycles inside codegen, `× 4` at the Cedar boundary." The two forms are semantically identical, but beats-throughout avoids scattering a magic conversion constant across 16+ handoff sites. Sections 2.1, 7, and 11 below are updated to reflect what actually shipped; the prior "cycles + boundary conversion" text is preserved at the end of §7 for posterity. Phase 1 also expanded the engine fix to cover the `run`/`binary`/`binaryN` synthetic-pattern handlers, which had the same element-count bug pattern but were not in the original §3/§7 enumeration. The "cycles everywhere — drop beats from Cedar entirely" model surfaced during implementation as the principled long-term direction; it is filed as a separate future PRD and is **not** part of this remediation.

# Mini-notation cycle timing — audit + remediation PRD

## 1. Summary

Akkado's mini-notation does **not** follow the Strudel/TidalCycles cycle-fitting convention that our user-facing docs claim. Instead of fitting the entire mini-notation string into one cycle (so that more notes → shorter notes), the codegen layer assigns **one beat per top-level element**. Pattern length therefore grows linearly with element count:

| Pattern | Strudel-correct length | Akkado actual length |
|---|---|---|
| `pat("c d")` | 1 cycle (4 beats) | **2 beats** (½ cycle) |
| `pat("c d e f")` | 1 cycle (4 beats) | 4 beats (1 cycle) — correct by coincidence |
| `pat("c d e f g a b c5")` | 1 cycle (4 beats) | **8 beats** (2 cycles) |
| `pat("<a b c d>")` (alternation, 1 top-level element) | 4 cycles | **1 beat per cycle**, 1 cycle total |

The pattern evaluator (`pattern_eval.cpp`) is correct — it produces normalized `[0, 1)` event times. The bug lives one step further down the pipeline, in `codegen_patterns.cpp`, where those normalized times are rescaled by `num_top_level_elements` beats rather than by the canonical `4` beats per cycle.

**Tag: breaking-change required.** The fix changes audible timing of every existing patch whose top-level mini-notation does not have exactly 4 elements.

**Relationship to other PRDs:** `docs/prd-mini-notation-micro-timing.md` (per-note nudge, dot-padding) assumes a Strudel-correct cycle base. That PRD is blocked on this one — fix cycle-fitting first, then `nudge:` percentages and dot-padding act on the canonical cycle.

---

## 2. The convention we claim to follow

From Strudel / TidalCycles documentation (the golden rule):

> The whole mini-notation string is exactly **one cycle** by default. Writing N notes means each note gets `1/N` of the cycle — the cycle length stays fixed; notes get faster to fit. To span multiple cycles use `<…>` (alternation, one element per cycle) or `.slow(n)`. To squeeze repeats into a slot use `*n` or `.fast(n)`.

Our own docs assert the same model:

- `docs/mini-notation-reference.md:26` — quick-reference table:
  > `a b c` Sequence `c4 e4 g4` **Subdivide cycle equally**
- `docs/mini-notation-implementation.md:38-46`:
  > `[a b c d]` = 4 elements in 1 cycle — each element gets 1/4 of the cycle, events at normalized times 0, 0.25, 0.5, 0.75; with `cycle_length=4 beats` → events at beats 0, 1, 2, 3
- `web/static/docs/reference/mini-notation/basics.md:58-67`:
  > Space-separated notes play in sequence **over one cycle**.
- `docs/mini-notation-reference.md:433-450` — explicit Strudel/Tidal compatibility table claims feature parity for `~`, `_`, `*`, `/`, `@`, `<>`, `[]` semantics.
- `CLAUDE.md:102-105` — "1 cycle = 4 beats by default".

These contracts are what the implementation breaks.

### 2.1 What stays vs what changes (as shipped in 5c1c0ca)

| Layer | Pre-fix | Post-fix | Notes |
|---|---|---|---|
| Mini-notation parser (`pattern_*.cpp`) | normalized `[0, 1)` | normalized `[0, 1)` | unchanged |
| Pattern evaluator (`pattern_eval.cpp`) | events in `[0, 1)` | events in `[0, 1)` | unchanged — correct already |
| Codegen `cycle_length` value | beats, derived from `num_top_level_elements` | **beats**, canonical `4.0f` at base, independent of element count | element-count derivation removed (§7) |
| Transform machinery (`compute_transformed_events`) | mutates beats base | mutates beats base | semantic-preserving multipliers (`.slow(2)` still → 2× duration) |
| Synthetic-pattern handlers (`handle_run_call`, `handle_binary_call`, `handle_binary_n_call`) | beats, derived from N / bits | canonical `4.0f` beats | same bug, fixed in 5c1c0ca (not in original §3 enumeration) |
| Cedar handoff (`seq_init.cycle_length`) | takes beats | takes beats; **no boundary conversion** — bare `seq_init.cycle_length = cycle_length;` | beats-throughout means no unit translation step |
| Cedar VM (`sequencing.hpp`) | schedules in beats | schedules in beats | unchanged |
| Existing `.akk` patches | implicitly tuned to broken model | audibly different unless wrapped in `.slow()` | §10 patch review |
| Existing `test_codegen.cpp` assertions | encode broken behavior | rewritten to canonical base (`4 × transform_factor`) | §8 |
| Existing `test_mini_notation.cpp` assertions | eval-only | eval-only | unchanged — should stay green |

### 2.5 Edge cases (post-fix expected behavior)

| Pattern | Element count | Expected duration | Expected event times |
|---|---|---|---|
| `pat("c4")` | 1 | 1 cycle (4 beats) | beat 0 only |
| `pat("c4 e4")` | 2 | 1 cycle (4 beats) | beats 0, 2 |
| `pat("c4 e4 g4")` | 3 | 1 cycle (4 beats) | beats 0, ~1.333, ~2.667 |
| `pat("c4 e4 g4 b4")` | 4 | 1 cycle (4 beats) | beats 0, 1, 2, 3 |
| `pat("c d e f g a b c5")` | 8 | 1 cycle (4 beats) | beats 0, 0.5, 1, …, 3.5 |
| `pat("a [b c] d")` | 3 (nested group counts as 1 slot) | 1 cycle (4 beats) | beat 0 (a), beat ~1.333 (b), beat ~2.0 (c), beat ~2.667 (d). Inner `[b c]` subdivides *within its 1/3 slot* — eval handles this; codegen is unchanged. |
| `pat("a@2 b")` (weighted) | 2 elements, total weight 3 | 1 cycle (4 beats) | beat 0 (a, holds for 2/3 cycle = ~2.667 beats), beat ~2.667 (b) |
| `pat("~ ~ ~")` | 3 | 1 cycle (4 beats), zero events emitted | (silence — rests produce no `PatternEvent`) |
| `pat("")` | 0 (or 1, depending on parse) | 1 cycle (4 beats), zero events | safe fallback; sequencer schedules nothing |
| `pat("<a b c d>")` (alternation) | 1 top-level | 1 cycle (4 beats); alternates a→b→c→d across 4 successive cycles | inner alternation is an eval-stage concern; codegen sees 1 slot |
| `pat("a b").slow(2)` | 2, then ×2 | 2 cycles (8 beats) | beats 0, 4 |
| `pat("a b c d").fast(2)` | 4, then ÷2 | ½ cycle (2 beats) | beats 0, 0.5, 1, 1.5 |

All edge-case behaviors above derive from the rule "cycle length is 1.0 cycle (4 beats) regardless of element count; transforms compose multipliers on cycles."

---

## 3. What the code actually does

The bug is split across two stages of the pipeline. Stage A is correct; the bug is entirely in Stage B's rescaling.

### Stage A — pattern eval (correct, normalized to `[0, 1)`)

- `akkado/include/akkado/pattern_event.hpp:150-222` — `PatternEvalContext` starts with `duration = 1.0f` (one full cycle):
  ```cpp
  struct PatternEvalContext {
      float start_time = 0.0f;
      float duration = 1.0f;          // one cycle
      ...
      PatternEvalContext subdivide(std::size_t child_index, std::size_t child_count) const {
          float child_duration = duration / static_cast<float>(child_count);
          ...
      }
  };
  ```
- `akkado/src/pattern_eval.cpp:146-176` (`eval_pattern`) and `:261-290` (`eval_group`) — divide the parent `duration` by `total_weight`, giving each child its proportional slice. Correct.
- `akkado/tests/test_mini_notation.cpp:569-586` confirms `pat("c4 e4 g4")` produces events at normalized times `[0.0, 0.333, 0.666]`. These are *cycle-relative* — the eval stage is doing the right thing.

### Stage B — codegen (WRONG: rescales `[0, 1)` by element count)

The bug:

- `akkado/src/codegen_patterns.cpp:1326-1328`:
  ```cpp
  // Determine cycle length from top-level element count (each element = 1 beat)
  std::uint32_t num_elements = compiler.count_top_level_elements(pattern_node);
  float cycle_length = static_cast<float>(std::max(1u, num_elements));
  ```
  Duplicated at lines **1690-1692** and **1842-1844** (for the polyphonic and curve/timeline variants).

The literal comment is the smoking gun: **"each element = 1 beat"**. That is not the Strudel model.

This `cycle_length` is then handed to the Cedar VM via `seq_init.cycle_length` (`codegen_patterns.cpp:1385`), which `cedar/include/cedar/opcodes/sequencing.hpp:326` reads as the scheduling cycle length in beats:
```cpp
state.cycle_length = cycle_length;
```
Event times are emitted into the audio thread as `e.time * cycle_length`. With the bug:
- `e.time` is normalized in `[0, 1)` (correct, from Stage A);
- `cycle_length` = number of top-level elements in beats (wrong);
- final scheduled beat times scale linearly with element count.

The pattern-transform machinery (`compute_transformed_events` and `out_cycle_length` in `codegen_patterns.cpp:2194-2735`) inherits the wrong base — every `.fast()/.slow()/.palindrome()/.linger()/.run()/.zoom()/.segment()` chain composes its multiplicative rewrite on top of an element-count-derived base.

### Worked example end-to-end

`pat("c d e f g a b c5")` (8 notes) at default bpm:
- Stage A produces 8 events at times `0, 0.125, 0.25, …, 0.875` (in `[0, 1)`).
- Stage B sets `cycle_length = 8.0f` beats.
- Cedar schedules them at beats `0, 1, 2, …, 7` — one per beat, pattern length = 8 beats = **2 cycles**.

Strudel-correct behaviour would set the equivalent of `cycle_length = 4.0f` beats always, giving beats `0, 0.5, 1.0, …, 3.5` — eight half-beat notes in **1 cycle**.

---

## 4. Why two prior code-review passes declared this "correct"

Both `Explore` sub-agents that audited this stopped at `pattern_eval.cpp` and saw correctly-normalized times in `[0, 1)`. They never followed the data into `codegen_patterns.cpp` where the times get multiplied by element-count beats.

**Reviewer note for any future timing audit**: do *not* stop at `pattern_eval`. The contract is upheld in eval and broken in codegen. Trace `seq_init.cycle_length` end-to-end (eval → codegen → `cedar/include/cedar/opcodes/sequencing.hpp` → VM scheduler) before drawing conclusions.

The end-to-end regression test prescribed in §8 exists specifically to defend against this failure mode — it pins the contract at the *scheduling* layer, not just at eval.

---

## 5. Non-goals

The following are explicitly out of scope for this PRD:

- **Pattern-eval rewrite.** `pattern_eval.cpp` and `pattern_event.hpp` are correct. Do not touch.
- **Parser changes.** Mini-notation grammar (tokens, group brackets, weights, alternation) is unchanged.
- **New mini-notation operators.** Per-note nudge / dot-padding belongs to `docs/prd-mini-notation-micro-timing.md` and lands *after* this fix.
- **Cedar VM scheduler changes.** The VM continues to consume `cycle_length` in beats; the codegen→VM boundary is the single conversion point.
- **`count_top_level_elements()` removal.** The helper stays (still used for metadata and diagnostics); only its use as a `cycle_length` derivation is removed.
- **Patch *re-authoring* beyond what's needed to recover musical intent.** §10 is a per-patch review pass, not a redesign.
- **Migration tooling.** No automatic `.akk` rewriter; users follow the §12 migration recipe by hand.

---

## 6. Phasing

The remediation lands across three phases, each independently shippable.

### Phase 1 — Engine fix + tests + CLAUDE.md (one PR; blocks release)

- Apply the §7 code changes at the three primary sites and seven transform sites.
- Rewrite the failing `test_codegen.cpp` assertions per §8.
- Add the new end-to-end timing regression test (§8).
- Add the canonical-cycle clause to `CLAUDE.md` per §9.

**Exit criterion:** `akkado_tests` and `cedar_tests` are green; the new regression test asserts beat-times for 1-/2-/4-/8-element patterns and `.slow(2)`/`.fast(2)` transforms.

### Phase 2 — Doc tightening + CHANGELOG (one PR; can land alongside Phase 1 or immediately after)

- Tighten `docs/mini-notation-implementation.md:24` per §9.
- Verify (no edits expected) `docs/mini-notation-reference.md`, `web/static/docs/reference/mini-notation/basics.md`.
- Add CHANGELOG entry per §12 (the migration recipe).

**Exit criterion:** doc examples render correctly against the post-fix engine; `bun run build:docs` re-indexes successfully.

### Phase 3 — Per-patch review (many small PRs; rolls out over time)

- Review every `.akk` patch listed in §10. For each patch: decide preserve-old-feel-via-`.slow()` vs. accept new musical intent vs. no change needed (already 4 top-level elements).
- Each migration PR must note its per-patch decision in the PR description (preserve / re-tune / unchanged) for the record.

**Exit criterion:** all enumerated patches reviewed; welcome-tutorial patches remain musically defensible for first-time users.

Phases 1 + 2 can ship together; Phase 3 is a slower follow-up. The release tagged with Phases 1+2 should call out the breaking change and ship the migration recipe even if Phase 3 patch updates lag.

---

## 7. Remediation — code (as shipped in 5c1c0ca)

### Primary fix — set canonical `cycle_length = 4.0f`

At the codegen layer, keep `cycle_length` in **beats** (its existing unit) and just stop deriving it from element count. The Strudel convention "1 cycle = 4 beats" becomes a literal at the source, not a runtime computation.

Replace at all three sites:

- `akkado/src/codegen_patterns.cpp:1326-1328` (monophonic)
- `akkado/src/codegen_patterns.cpp:1690-1692` (polyphonic)
- `akkado/src/codegen_patterns.cpp:1842-1844` (chord/timeline)

Before:
```cpp
// Determine cycle length from top-level element count (each element = 1 beat)
std::uint32_t num_elements = compiler.count_top_level_elements(pattern_node);
float cycle_length = static_cast<float>(std::max(1u, num_elements));
```

After:
```cpp
// 1 cycle = 4 beats by default (Strudel convention). Independent of element count.
float cycle_length = 4.0f;
```

`count_top_level_elements` is no longer needed at these call sites (still used elsewhere — leave the method intact, drop only the local uses).

### Secondary fix — transform base becomes canonical

In `compute_transformed_events` (`codegen_patterns.cpp:2194-2735`), every `out_cycle_length` initialization at lines `2214, 2233, 2280, 2295, 2308, 2328, 2735`:

Before:
```cpp
out_cycle_length = static_cast<float>(std::max(1u, out_num_elements));   // or bare static_cast<float>(out_num_elements)
```

After:
```cpp
out_cycle_length = 4.0f;  // 1 cycle = 4 beats by default (Strudel convention)
```

The existing transform compositions (`*= factor`, `/= factor`, `*= 2.0f`, `*= frac`) keep working on beats: `slow(2)` makes it `8.0` beats, `fast(2)` makes it `2.0` beats, identity stays at `4.0` beats. Relative semantics preserved exactly.

### Tertiary fix — synthetic-pattern handlers (not in original §3 enumeration)

`run`, `binary`, `binaryN` populate the compiler with synthetic events directly and bypass `compute_transformed_events`. They had the same element-count bug pattern. Sites:

- `akkado/src/codegen_patterns.cpp:5072` (`handle_run_call`)
- `akkado/src/codegen_patterns.cpp:5105` (`handle_binary_call`)
- `akkado/src/codegen_patterns.cpp:5146` (`handle_binary_n_call`)

Before:
```cpp
float cycle_length = static_cast<float>(std::max(1, n_int));   // or bits
```

After:
```cpp
float cycle_length = 4.0f;  // 1 cycle = 4 beats by default (Strudel convention)
```

### Cedar handoff — unchanged

`seq_init.cycle_length = cycle_length;` stays bare at all 8 handoff sites (3 primary + 5 transform-emit paths). No `* 4.0f` conversion: `cycle_length` is already in beats. `cedar/include/cedar/opcodes/sequencing.hpp:326` continues to consume `cycle_length` as beats; the VM contract is unchanged.

The `payload->cycle_length = cycle_length;` writes (9 sites) likewise stay bare. The struct field's documented unit is beats (`typed_value.hpp:78`).

### Do not touch

`akkado/src/pattern_eval.cpp` and `akkado/include/akkado/pattern_event.hpp` are correct. The fix is purely the codegen-side literal-vs-derived choice.

---

### Historical: the originally-prescribed approach (cycles + boundary conversion)

For reference, the audit originally prescribed storing `cycle_length` in **cycles** (unitless, `1.0f` base) inside codegen and multiplying by `4.0f` at every Cedar handoff site. That approach is semantically identical to the as-shipped beats-throughout fix, but it scatters a magic conversion constant at 16+ handoff sites (8 `seq_init.cycle_length` writes + 9 `payload->cycle_length` writes), introduces a half-and-half model where the same name carries different units at different file locations, and arguably makes the code harder to read. The beats-throughout form was chosen during implementation; see commit `5c1c0ca` for the discussion.

The deeper "cycles-only model — drop beats from Cedar entirely" is a separate future PRD, not in scope here.

---

## 8. Remediation — tests

### Tests that pin the broken behavior and will fail after the fix (rewrite to assert Strudel-correct values)

- `akkado/tests/test_codegen.cpp:1375-1390` — `cycle_length` and `expected_duration = cycle_length / 3.0f` derived from element count. Recompute against `cycle_length = 4.0f` (post-handoff) base.
- `akkado/tests/test_codegen.cpp:2183-2300` — `pat("c4 e4")` cluster: every `CHECK(si.cycle_length == Catch::Approx(2.0f))` and `Approx(4.0f)` and `Approx(1.0f)` (for fast/slow combinations) needs re-derivation. The new identity-pattern post-handoff base is `4`, not `num_elements`.
- `akkado/tests/test_codegen.cpp:3071-3442` — palindrome / linger / zoom / segment / run pattern-transform asserts: each `Approx(N.0f)` value was derived as `num_elements × transform_factor`; new expected values are `4 × transform_factor`.

### New test — end-to-end timing regression

Add `akkado/tests/test_codegen_cycle_timing.cpp` (or extend an existing file) with explicit eval → codegen → VM-handoff assertions. The goal: catch any future regression that re-breaks codegen scaling while keeping eval-only tests green.

Suggested assertions:

| Test name | Input | Asserts |
|---|---|---|
| `cycle_timing_single_element` | `pat("c4")` | `seq_init.cycle_length == 4.0f`; 1 event at beat 0 |
| `cycle_timing_two_elements` | `pat("c4 e4")` | `seq_init.cycle_length == 4.0f`; events at beats 0, 2 |
| `cycle_timing_four_elements` | `pat("c4 d4 e4 f4")` | `seq_init.cycle_length == 4.0f`; events at beats 0, 1, 2, 3 |
| `cycle_timing_eight_elements` | `pat("c d e f g a b c5")` | `seq_init.cycle_length == 4.0f`; events at beats 0, 0.5, 1, …, 3.5 |
| `cycle_timing_slow_2` | `pat("c d").slow(2)` | `seq_init.cycle_length == 8.0f`; events at beats 0, 4 |
| `cycle_timing_fast_2` | `pat("c d e f").fast(2)` | `seq_init.cycle_length == 2.0f`; events at beats 0, 0.5, 1, 1.5 |
| `cycle_timing_alternation` | `pat("<a b c d>")` | `seq_init.cycle_length == 4.0f`; 1 event per cycle, cycling a→b→c→d |
| `cycle_timing_weighted` | `pat("a@2 b")` | `seq_init.cycle_length == 4.0f`; events at beats 0 (a, dur ~2.667) and ~2.667 (b) |

Each assertion must trace through the actual compile → `seq_init` → scheduled event times, not just the eval stage. This is the test the audit in §4 says should have existed.

### Tests that should keep passing (verify after the fix, no edit needed)

- `akkado/tests/test_mini_notation.cpp` — operates on normalized `[0, 1)` event times from `pattern_eval`. Untouched by the fix; if these fail something else is wrong.
- `akkado/tests/test_hot_swap_determinism.cpp:230`, `test_fuzz_determinism.cpp:233`, `test_fuzz_recompile_audio.cpp:59` — they only forward `init.cycle_length` to the VM; numeric values will shift but determinism properties hold.

---

## 9. Remediation — docs

### Reference docs (currently correct; verify after fix)

- `docs/mini-notation-reference.md` — examples in the quick-reference table already describe the cycle-fitted model. Cross-check examples after the fix lands; no rewrites expected.
- `docs/mini-notation-implementation.md:24` — currently says "one iteration of the pattern, *typically* 4 beats". Tighten to "exactly 4 beats by default" once the fix is in.
- `web/static/docs/reference/mini-notation/basics.md` — currently the most Strudel-faithful doc. Validate examples post-fix.

### Source-of-truth additions

- `CLAUDE.md` "Clock System" section (lines 102-105): add an explicit clause:
  > "Every mini-notation string is exactly one cycle by default, regardless of element count. More notes → shorter notes; the cycle length stays fixed."
- Add a note in the same section that `<a b c>` alternation is the way to span multiple cycles, and `.slow(n)` / `.fast(n)` are the runtime rescales.

### Cross-references

- The "Strudel/Tidal Compatibility" table in `docs/mini-notation-reference.md:433-450` needs no edit but is the contract this fix *restores*.

---

## 10. Remediation — example patches

The fix changes audible timing of every `.akk` patch whose mini-notation strings don't have exactly 4 top-level elements. The cleanup is Phase 3 — this section enumerates the affected files.

### `web/static/patches/*.akk` — main patches

All of the following use mini-notation and need a per-file review:
- `dnb-amen.akk`
- `drum-machine.akk`
- `effects-chain.akk`
- `fm-piano.akk`
- `hello-sine.akk`
- `interactive-params.akk`
- `microtonal-raga.akk`
- `midi-cc-filter.akk`
- `midi-cc-filtermono.akk`
- `midi-keys.akk`
- `midi-soundfont.akk`
- `poly-chords.akk`
- `rock-groove.akk`
- `soundfont-play.akk`
- `unison-lead.akk`
- `visualizations.akk`
- `wavetable-scan.akk`

### `web/static/patches/welcome/*.akk` — onboarding patches

These are seen by every first-time user and must continue to sound musically sensible:
- `04-am-bell.akk`, `05-fm-stab.akk`, `06-noise-wind.akk`
- `07-four-floor.akk`, `08-drum-groove.akk`, `09-euclid-bass.akk`, `10-acid-303.akk`
- `11-chord-pad.akk`, `12-pentatonic-arp.akk`
- `14-pingpong.akk`, `15-chorus-lead.akk`, `16-snare-roll.akk`
- `17-polyrhythm.akk`, `18-generative.akk`, `19-arp-echo.akk`, `20-microtonal.akk`

### `experiments/` smoke tests

- `experiments/phase2_smoke.akk`
- `experiments/phase21_smoke.akk`

### Per-file review checklist (Phase 3)

For each affected patch, decide:
1. Was the pattern authored to the broken "1 beat per step" model? → Likely sounds at wrong tempo after fix; consider wrapping in `.slow(num_elements/4)` to preserve the old feel, *or* leave alone if the new (faster, denser) timing is musically defensible.
2. Was an external `trigger(N)` rate chosen to match the broken pattern length? → Will desynchronise after fix; recompute `N` against the cycle-fitted length.
3. Does the patch use `pat()` with exactly 4 top-level elements? → No change needed, behaviour identical.

**Per-patch decision logging.** Every Phase-3 migration PR must record its choice in the PR description: `preserve` (wrapped in `.slow()` to keep old feel) / `retune` (accepted new musical intent, possibly with light rewrites) / `unchanged` (4 top-level elements; no audible change). This is especially important for `welcome/` patches — the decision shouldn't be silent. The log creates a record for future onboarding-content reviews.

### Tutorials

The synthesis tutorial (`web/static/docs/tutorials/03-synthesis.md`) and the testing-progression tutorial (`web/static/docs/tutorials/05-testing-progression.md`) use `trigger(N)` decoupled from pattern length throughout. These examples were already independent of the bug, but should be reviewed for any text that implicitly references the broken model.

---

## 11. Acceptance criteria

Phase 1 is complete when **all** of the following hold (✓ marks items satisfied by commit `5c1c0ca`):

1. ✓ The three primary `codegen_patterns.cpp` sites set `cycle_length = 4.0f` (canonical beats, Strudel "1 cycle = 4 beats" convention), not derived from `count_top_level_elements`.
2. ✓ The seven transform sites set `out_cycle_length = 4.0f`; multiplicative transform composition (`slow`, `fast`, `palindrome`, `linger`, `zoom`, `segment`, `run`) is preserved.
3. ✓ The Cedar boundary at `seq_init.cycle_length = cycle_length;` stays bare (no `* 4.0f` conversion needed — `cycle_length` is already in beats).
4. ✓ The synthetic-pattern handlers (`handle_run_call`, `handle_binary_call`, `handle_binary_n_call`) set `cycle_length = 4.0f` instead of deriving from N / bits. *(Added during implementation; not in the original §3 enumeration.)*
5. ✓ The new end-to-end timing regression test `akkado/tests/test_codegen_cycle_timing.cpp` is added and passes, asserting beat-times through eval → codegen → `seq_init` for the 8 cases in §8 (55 assertions / 8 cases, tagged `[cycle_timing]`).
6. ✓ The existing `test_codegen.cpp` cycle-length assertions are rewritten and pass with the canonical-base values (clusters around `1375-1390`, `2183-2300`, `3071-3442`, plus sample-pattern assertions near `5394`, `5555`).
7. ✓ `test_mini_notation.cpp` remains green without edits (eval invariant unchanged).
8. ✓ `test_hot_swap_determinism.cpp`, `test_fuzz_determinism.cpp`, `test_fuzz_recompile_audio.cpp` remain green (determinism preserved; values shift).
9. ✓ `CLAUDE.md` Clock System section includes the explicit canonical-cycle clause from §9.

Phase 2 is complete when:

9. ✓ `docs/mini-notation-implementation.md` is tightened per §9 and re-indexed via `bun run build:docs` (57 docs / 627 lookup entries, manifest unchanged because the edit was body prose only).
10. ✓ CHANGELOG entry per §12 is in place (added under `[Unreleased]` using the existing `### ⚠ BREAKING — <summary>` style for consistency with the delay-family entry already there).

Phase 3 is complete when:

11. Every patch in §10 has been reviewed, with a logged preserve/retune/unchanged decision.

---

## 12. Breaking-change call-out

- This is **not** a backwards-compatible fix. Every existing patch's audible timing changes unless it happens to have exactly 4 top-level mini-notation elements per pattern.
- Strudel/Tidal users will find the new behaviour *less* surprising; existing Akkado users will need to relearn pattern timing intuition or apply `.slow()` / `.fast()` to recover the previous feel.
- **Migration recipe**: an old N-element pattern that "felt right" can be recovered with `pat("…").slow(N / 4.0)`. E.g. an 8-element pattern that used to take 2 cycles now takes 1, so wrap in `.slow(2)` to restore.
- Recommended rollout: bundle the fix in a **minor**-version release (pre-1.0 SemVer allows breaking changes in minor bumps), with a `CHANGELOG.md` entry under **Breaking changes**. The release itself is user-driven via `scripts/bump-version.sh` — not run by the implementer.

### CHANGELOG draft (Keep a Changelog format)

```markdown
### Breaking changes

- **Mini-notation cycle timing now matches Strudel/Tidal.** A mini-notation
  string is exactly one cycle (4 beats) by default, regardless of element
  count. Previously, each top-level element occupied one beat, so
  `pat("c d e f g a b c5")` ran for 2 cycles; it now fits in 1 cycle with
  eight half-beat notes. Only patterns with exactly 4 top-level elements
  are unaffected.

  **Migration**: to restore a pattern's old timing, wrap it in
  `.slow(N / 4.0)`, where N is the top-level element count.
  Example: `pat("c d e f g a b c5")` → `pat("c d e f g a b c5").slow(2)`.

  This restores conformance with the cycle-fitting convention documented
  in `docs/mini-notation-reference.md` and Strudel/Tidal — the prior
  behavior was a long-standing bug in the codegen layer
  (`akkado/src/codegen_patterns.cpp`). See
  `docs/audits/mini-notation-cycle-timing_audit_2026-05-18.md` for the
  full audit + rationale.
```

---

## 13. Critical files referenced

- `akkado/src/codegen_patterns.cpp` — site of the bug (lines 1326-1328, 1690-1692, 1842-1844; transforms 2194-2735)
- `akkado/src/pattern_eval.cpp` — correct, do not touch
- `akkado/include/akkado/pattern_event.hpp` — defines the normalized eval context
- `cedar/include/cedar/opcodes/sequencing.hpp` — consumes `cycle_length` downstream (lines 326, 383-419)
- `akkado/tests/test_codegen.cpp` — pins the broken behaviour, needs rewriting
- `akkado/tests/test_mini_notation.cpp` — eval-stage tests, should remain green
- `docs/mini-notation-reference.md`, `docs/mini-notation-implementation.md`, `web/static/docs/reference/mini-notation/basics.md`, `CLAUDE.md` — the contract docs
- `docs/prd-mini-notation-micro-timing.md` — downstream PRD; blocked on this fix
