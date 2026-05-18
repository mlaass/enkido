> **Status: AUDIT** — Diagnosis + remediation plan for a documented bug. 2026-05-18.

# Mini-notation cycle timing — audit

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

This `cycle_length` is then handed to the Cedar VM via `seq_init.cycle_length` (`codegen_patterns.cpp:1385`), which `cedar/include/cedar/opcodes/sequencing.hpp:325` reads as the scheduling cycle length in beats:
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

Strudel-correct behaviour would set `cycle_length = 4.0f` always, giving beats `0, 0.5, 1.0, …, 3.5` — eight half-beat notes in **1 cycle**.

---

## 4. Why two prior code-review passes declared this "correct"

Both `Explore` sub-agents that audited this stopped at `pattern_eval.cpp` and saw correctly-normalized times in `[0, 1)`. They never followed the data into `codegen_patterns.cpp` where the times get multiplied by element-count beats.

**Reviewer note for any future timing audit**: do *not* stop at `pattern_eval`. The contract is upheld in eval and broken in codegen. Trace `seq_init.cycle_length` end-to-end (eval → codegen → `cedar/include/cedar/opcodes/sequencing.hpp` → VM scheduler) before drawing conclusions.

---

## 5. Remediation — code

### Primary fix

Replace the element-count derivation with the canonical 4-beats-per-cycle constant at all three sites:

- `akkado/src/codegen_patterns.cpp:1328`
- `akkado/src/codegen_patterns.cpp:1692`
- `akkado/src/codegen_patterns.cpp:1844`

Before:
```cpp
std::uint32_t num_elements = compiler.count_top_level_elements(pattern_node);
float cycle_length = static_cast<float>(std::max(1u, num_elements));
```
After:
```cpp
float cycle_length = 4.0f;  // 1 cycle = 4 beats (Strudel/Tidal convention)
```

`count_top_level_elements` is no longer needed at these call sites (still used elsewhere — leave the method intact, drop only the local uses).

### Secondary fix — transform pipeline

In `compute_transformed_events` (`codegen_patterns.cpp:2194-2735`), every `out_cycle_length = static_cast<float>(std::max(1u, out_num_elements));` rewrite (lines `2214, 2233, 2280, 2295, 2308, 2328, 2735`) must use `4.0f` as the base. The multiplicative rewrites that follow (`*= factor`, `/= factor`, `*= 2.0f`, `*= frac`) keep composing on that constant.

Concretely: a `slow(2)` on a base pattern previously produced `cycle_length = num_elements * 2` beats; after the fix it produces `4.0f * 2 = 8` beats. A `fast(2)` on the same base previously produced `num_elements / 2`; after, it produces `2.0f` beats. The relative meaning of `.fast()/.slow()` is preserved; only the base changes.

### Do not touch

`akkado/src/pattern_eval.cpp` and `akkado/include/akkado/pattern_event.hpp` are correct. The fix is purely the eval→codegen scaling step.

### Open design question (flag for the fix PRD; do not solve here)

`cycle_length` is a `float` representing beats. After the fix the base is always `4.0f` but transforms still mutate it. Worth considering whether `cycle_length` should be expressed in **cycles** (always `1.0` base) at the codegen level, with the `× 4 beats/cycle` factor applied at the Cedar boundary. That's a cleaner model but a bigger refactor — leave for the fix PRD to decide.

---

## 6. Remediation — tests

Tests that pin the broken behaviour and will fail after the fix (rewrite to assert Strudel-correct values):

- `akkado/tests/test_codegen.cpp:1375-1390` — `cycle_length` and `expected_duration = cycle_length / 3.0f` derived from element count. Recompute against `4.0f` base.
- `akkado/tests/test_codegen.cpp:2183-2300` — `pat("c4 e4")` cluster: every `CHECK(si.cycle_length == Catch::Approx(2.0f))` and `Approx(4.0f)` and `Approx(1.0f)` (for fast/slow combinations) needs re-derivation. The new identity-pattern base is `4`, not `num_elements`.
- `akkado/tests/test_codegen.cpp:3071-3442` — palindrome / linger / zoom / segment / run pattern-transform asserts: each `Approx(N.0f)` value was derived as `num_elements × transform_factor`; new expected values are `4 × transform_factor`.

Tests that should keep passing (verify after the fix, no edit needed):

- `akkado/tests/test_mini_notation.cpp` — operates on normalized `[0, 1)` event times from `pattern_eval`. Untouched by the fix; if these fail something else is wrong.
- `akkado/tests/test_hot_swap_determinism.cpp:230`, `test_fuzz_determinism.cpp:233`, `test_fuzz_recompile_audio.cpp:59` — they only forward `init.cycle_length` to the VM; numeric values will shift but determinism properties hold.

---

## 7. Remediation — docs

### Reference docs (currently correct; verify after fix)

- `docs/mini-notation-reference.md` — examples in the quick-reference table already describe the cycle-fitted model. Cross-check examples after the fix lands; no rewrites expected.
- `docs/mini-notation-implementation.md:22` — currently says "one iteration of the pattern, *typically* 4 beats". Tighten to "exactly 4 beats by default" once the fix is in.
- `web/static/docs/reference/mini-notation/basics.md` — currently the most Strudel-faithful doc. Validate examples post-fix.

### Source-of-truth additions

- `CLAUDE.md` "Clock System" section (lines 102-105): add an explicit clause:
  > "Every mini-notation string is exactly one cycle by default, regardless of element count. More notes → shorter notes; the cycle length stays fixed."
- Add a note in the same section that `<a b c>` alternation is the way to span multiple cycles, and `.slow(n)` / `.fast(n)` are the runtime rescales.

### Cross-references

- The "Strudel/Tidal Compatibility" table in `docs/mini-notation-reference.md:433-450` needs no edit but is the contract this fix *restores*.

---

## 8. Remediation — example patches

The fix changes audible timing of every `.akk` patch whose mini-notation strings don't have exactly 4 top-level elements. The cleanup is a follow-up task — this audit only enumerates the affected files.

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

### Per-file review checklist

For each affected patch, decide:
1. Was the pattern authored to the broken "1 beat per step" model? → Likely sounds at wrong tempo after fix; consider wrapping in `.slow(num_elements/4)` to preserve the old feel, *or* leave alone if the new (faster, denser) timing is musically defensible.
2. Was an external `trigger(N)` rate chosen to match the broken pattern length? → Will desynchronise after fix; recompute `N` against the cycle-fitted length.
3. Does the patch use `pat()` with exactly 4 top-level elements? → No change needed, behaviour identical.

### Tutorials

The synthesis tutorial (`web/static/docs/tutorials/03-synthesis.md`) and the testing-progression tutorial (`web/static/docs/tutorials/05-testing-progression.md`) use `trigger(N)` decoupled from pattern length throughout. These examples were already independent of the bug, but should be reviewed for any text that implicitly references the broken model.

---

## 9. Breaking-change call-out

- This is **not** a backwards-compatible fix. Every existing patch's audible timing changes unless it happens to have exactly 4 top-level mini-notation elements per pattern.
- Strudel/Tidal users will find the new behaviour *less* surprising; existing Akkado users will need to relearn pattern timing intuition or apply `.slow()` / `.fast()` to recover the previous feel.
- **Migration recipe** to put in release notes: an old N-element pattern that "felt right" can be recovered with `pat("…").slow(N / 4.0)`. E.g. an 8-element pattern that used to take 2 cycles now takes 1, so wrap in `.slow(2)` to restore.
- Recommended rollout: bundle the fix in a minor-version release, with a `CHANGELOG.md` entry under **Breaking changes** and the migration recipe above. The release itself is user-driven via `scripts/bump-version.sh` — not run by the implementer.

---

## 10. Critical files referenced

- `akkado/src/codegen_patterns.cpp` — site of the bug (lines 1326-1328, 1690-1692, 1842-1844; transforms 2194-2735)
- `akkado/src/pattern_eval.cpp` — correct, do not touch
- `akkado/include/akkado/pattern_event.hpp` — defines the normalized eval context
- `cedar/include/cedar/opcodes/sequencing.hpp` — consumes `cycle_length` downstream (lines 325, 383-419)
- `akkado/tests/test_codegen.cpp` — pins the broken behaviour, needs rewriting
- `akkado/tests/test_mini_notation.cpp` — eval-stage tests, should remain green
- `docs/mini-notation-reference.md`, `docs/mini-notation-implementation.md`, `web/static/docs/reference/mini-notation/basics.md`, `CLAUDE.md` — the contract docs
