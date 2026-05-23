> **Status: IN PROGRESS** — corrected scope as of 2026-05-24. Standalone sibling
> of [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md).
> The original draft (2026-05-22) specced `scale` / `key` as a new
> `EVENT_QUANTIZE` opcode. **That framing has been dropped.** Under the
> parent PRD's foundational principle ("opcodes are for primitive operations
> and DSP work, not for language constructs"), `scale` and `key` are
> **stdlib akkado on top of `event_map`** — exactly like Phase 2b's
> `transpose` / `velocity` / `dur` / `bend` migration. No new opcode is
> introduced; the parent PRD's §3.1 substrate is unchanged
> (`EVENT_MAP` + `EVENT_FILTER`). This PRD owns all `scale` / `key`
> semantics and the generated `akkado/stdlib/scales.ak` catalog.
>
> **Hard dependencies:**
> - Parent PRD's `EVENT_MAP` closure substrate (Phase 2a, SHIPPED) — `scale` / `key` lower to `event_map` calls.
> - Stdlib-module loading (SHIPPED — `akkado/stdlib/event_transforms.ak` already loads). Phase 2 here additionally requires **top-level constant bindings** to load (e.g. `minor = [0,2,3,5,7,8,10]`); a precondition spike verifies this in Commit B of `/home/moritz/.claude/plans/phase-4-fully-unified-snowglobe.md`.

# PRD: Scale & Key — Note Quantization and Degree Mapping

## Executive Summary

Akkado has no way to constrain a melody to a musical scale. Strudel/Tidal
solve this with a `scale` function that does double duty — mapping integer
scale degrees onto a named scale, and snapping arbitrary notes to the nearest
scale tone. This PRD specifies the Akkado equivalent as **two distinct
builtins** with clearly separated jobs:

- **`scale("D2:minor")`** — *degree mapping*. Reinterprets each event's number
  as a zero-indexed scale degree and emits the corresponding note. Octave-aware:
  degree 0 lands on the named root in the named octave. Does **not** snap.
- **`key("D:minor")`** — *quantization*. Snaps each event's note to the nearest
  pitch in the scale. Octave-agnostic: an octave digit in the name is ignored.
  Applies to single-note events only; chord events pass through untouched.

Both lower to a single new Cedar opcode, `EVENT_QUANTIZE`, on the parent PRD's
runtime event-transform substrate.

**Key design decisions (locked — see §11):**

- **Stdlib akkado, not a new opcode.** `scale` and `key` lower to
  `event_map` closure calls on top of the parent PRD's substrate. No
  `EVENT_QUANTIZE` opcode is introduced — that was the original framing
  and was dropped 2026-05-24 in alignment with the parent PRD's
  foundational principle.
- **Two builtins, not one.** `scale` = degree mapping (octave-aware); `key` =
  quantization (octave-agnostic). This is a deliberate divergence from
  Strudel's single overloaded `scale`.
- **The scale catalog is generated, not hand-written.** A build script
  converts the tonal.js `scale-type` table into a generated Akkado stdlib file
  (`akkado/stdlib/scales.ak`). Adding scales = re-running the script.
- **User-defined scales** via an explicit root + 0-based interval list:
  `scale(pat, "D2", [0,2,3,5,7,8,10])`. Mirrors tonal.js's tonic/interval-set
  split.
- **12-TET only for v1.** Quantization is defined in 12 equal semitones;
  interaction with non-12-EDO tunings (JI, Bohlen-Pierce) is a future PRD.
- **The scale/key argument is patternable** — `key("<C:major A:minor>")`
  alternates per cycle. **Deferred** to the follow-up PRD `prd-pattern-array-transforms.md`, since per-cycle alternation is naturally an array-of-events concept.
- **Phased delivery** — see §9, each phase independently testable.

---

## 1. Current State

### 1.1 What exists today

| Capability | Status | Location |
|---|---|---|
| Note-name → MIDI parsing (`c4`, `d#3`) | Exists | `akkado/src/mini_lexer.cpp` `parse_pitch_to_midi()` |
| Numeric note atoms in `n"…"` (MIDI 0..127, no negatives) | Exists | `mini_lexer.cpp` `lex_note_atom()` |
| Value-mode patterns `v"…"` (numeric, negatives allowed) | Exists | `mini_lexer.cpp` `lex_value_atom()` |
| Chord-symbol parsing (`Am`, `Fmaj7`) | Exists | `akkado/src/chord_parser.cpp`, `music_theory.hpp` |
| Pitch-class arithmetic (`semitone % 12`) | Implicit | scattered |
| Tuning system (12-EDO / JI / Bohlen-Pierce) | Exists | `akkado/include/akkado/tuning.hpp` |
| `scale` / `key` builtins | **Specified, unimplemented** | parent PRD §5 migration table; this PRD §4 |
| `event_map` closure substrate | **Shipped** (Phase 2a, 2026-05-23) | `cedar/include/cedar/opcodes/event_transforms.hpp`; this PRD lowers `scale`/`key` to `event_map` calls |
| Named musical scales (major, minor, modes…) | **None** | — |
| Scale-quantize / degree-mapping transform | **None** | — |

The builtin name `scale` is currently **free** — the unrelated array builtin
`scale(array, lo, hi)` was removed in commit `0d4aaa2` precisely to clear it.

### 1.2 The gap

There is no way to say "keep this melody in D minor." A user must either
hand-pick every note or accept dissonance from generative/random pitch
sources (`random()`, `transpose(irand(...))`, MIDI input). Strudel users
expect `scale` to exist and reach for it immediately.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Ship two builtins, `scale` and `key`, with the semantics in §4.
2. Ship the full tonal.js scale catalog as a **generated** Akkado stdlib file.
3. Support **user-defined** scales via a root + interval-list form.
4. Make the scale/key argument **patternable** (per-cycle alternation).
5. Lower both builtins to one new Cedar opcode, `EVENT_QUANTIZE`, on the
   parent PRD's event-transform substrate.
6. Every behavior in §4 and §8 covered by a test (§10).

### 2.2 Non-Goals (deferred)

- **Tuning-aware quantization.** v1 quantizes in 12-TET; mapping scale degrees
  onto JI / Bohlen-Pierce / non-12-EDO steps is a future PRD.
- **Chord-wide quantization.** `key` deliberately ignores chord events in v1
  (§4.5). A chord-aware mode is future work, gated on
  `prd-pattern-event-arrays.md`.
- **`scale` as a quantizer.** `scale` only does degree mapping; it never snaps.
  Quantization is `key`'s sole job.
- **Scale-degree transposition** (`scaleTranspose` in Strudel — shift within
  the scale by N steps). Future.
- **Microtonal / fractional scale degrees.** Fractional degree inputs to
  `scale` round to the nearest integer degree (§8).

---

## 3. Target Syntax / User Experience

The scale/key argument is a string `"Root[octave]:type"`, colon-separated.
Multi-word scale types use additional colons (Strudel convention): segment 0
is the tonic, segments 1..n joined by spaces form the scale-type name —
`"D2:harmonic:minor"` = tonic `D2`, type `harmonic minor`.

```akkado
// key — QUANTIZE: snap a chromatic run into D minor (octave ignored)
n"c4 c#4 d4 d#4 e4 f4 f#4 g4" |> key("d:minor")
//  c#4→c4, d#4→d4, f#4→f4   (D minor = D E F G A Bb C; ties round down)

// key — tame a random melody
n"c4 e4 g4" |> transpose(irand(24)) |> key("a:minor") |> ...

// scale — DEGREE MAPPING: integers become scale tones (octave-aware)
n"0 2 4 6 4 2" |> scale("d3:minor")        // → D3 F3 A3 C4 A3 F3
v"0 -1 2 7"    |> scale("c3:major")        // → C3 B2 E3 C4  (negatives below root)

// method-call form (consistent with .slow / .fast / .transpose)
n"0 2 4".scale("c:dorian")                 // omitted octave → octave 3
melody.key("e:phrygian")

// patternable argument — alternates per cycle
n"c4 e4 g4 b4" |> key("<c:major a:minor>")

// user-defined scale: explicit root + 0-based semitone interval list
n"0 1 2 3 4" |> scale("c3", [0,3,5,7,10])  // hand-rolled minor pentatonic
melody       |> key("e",  [0,2,4,7,9])     // custom pentatonic quantizer
```

---

## 4. Design

### 4.1 Two builtins

| | `scale` | `key` |
|---|---|---|
| Job | Degree → note mapping | Quantize note → nearest scale tone |
| Octave in name | **Used** — sets where degree 0 lands | **Ignored** — only the root pitch class matters |
| Input read as | Scale degree (integer index) | Note to be snapped |
| Snaps? | No | Yes |
| Chord events | Maps the primary value (§8) | **Passes through untouched** |
| Default octave when name omits one | 3 (e.g. `scale("d:minor")` → degree 0 = D3) | n/a |

Both builtins accept either a named-scale string or a `(root, interval-list)`
pair (§4.6), and both accept a patternable argument (§4.7).

### 4.2 Scale catalog — generated from tonal.js

tonal.js encodes each scale type as a **root-relative interval set, with no
octave** — e.g. `["1P 2M 3m 4P 5P 6m 7m", "minor", "aeolian"]`. The tonic is
always applied separately. nkido adopts the same model.

A build script — `web/scripts/generate-scales.ts` (run with `bun`, alongside the
existing `web/scripts/generate-*.ts` generators) — fetches/reads the tonal.js
`scale-type/data.ts` table and emits a **generated Akkado stdlib file**,
`akkado/stdlib/scales.ak`, containing one named constant per scale type whose
value is a 0-based semitone interval list:

```akkado
// GENERATED by web/scripts/generate-scales.ts — do not edit.
// Source: tonaljs/tonal scale-type data.
minor           = [0, 2, 3, 5, 7, 8, 10]
major           = [0, 2, 4, 5, 7, 9, 11]
dorian          = [0, 2, 3, 5, 7, 9, 10]
major_pentatonic = [0, 2, 4, 7, 9]
harmonic_minor  = [0, 2, 3, 5, 7, 8, 11]
// …full tonal.js set, aliases included…
```

The interval-quality notation (`1P 2M 3m…`) is converted to semitone offsets
by the generator (`1P`→0, `2M`→2, `3m`→3, `3M`→4, `4P`→5, `4A`/`5d`→6,
`5P`→7, `6m`→8, `6M`→9, `7m`→10, `7M`→11).

At compile time, `scale`/`key` resolve the type name (`"minor"`,
`"harmonic minor"`) against this catalog to obtain the interval list. An
unknown name is **E184** (§7).

**Hard dependency — stdlib-module infrastructure.** `akkado/stdlib/` does not
exist yet (the parent PRD also introduces it, for `event_transforms.ak`). The
catalog ships **only** as `akkado/stdlib/scales.ak`; `scale`/`key` resolve a
type name by loading that stdlib module and indexing its exported constants by
name. No generated C++ header, no interim path. Phase 2 of this PRD therefore
**blocks on stdlib-module loading landing first** (§9, §11.9). A generated C++
header was considered and rejected — the catalog must be userspace-visible and
extensible.

### 4.3 The scale-name string

`parse_scale_name(str)` → `{ root_pitch_class: 0..11, root_octave: int?,
type_name: string }`:

1. Split on `:`. Segment 0 = tonic; segments 1..n = type words.
2. Tonic = note letter + optional accidental(s) + optional octave digits,
   parsed by the existing note-name logic (`parse_pitch_to_midi` family).
3. `type_name` = segments 1..n joined by a single space; looked up in the
   §4.2 catalog.
4. A malformed tonic or empty type → **E185**. Unknown type → **E184**.

For `key`, `root_octave` is parsed but discarded. For `scale`, a missing
`root_octave` defaults to **3**.

### 4.4 `scale` — degree-mapping algorithm

Inputs: interval list `I = [0, i₁, …, i_{k-1}]` (k = scale length), base MIDI
note `M = pitch_class(root) + 12·(octave + 1)`.

For each event, read its primary numeric value `d` (note field for `n"…"`
sources, value field for `v"…"` sources — §11.3), round to nearest integer:

```
oct  = floor(d / k)           // floor division — handles negative d
step = d - oct·k              // 0 ≤ step < k
out_midi = M + 12·oct + I[step]
```

Examples (`scale("d3:minor")`, `minor=[0,2,3,5,7,8,10]`, k=7, M=50):
`d=0`→50 (D3); `d=2`→53 (F3); `d=7`→62 (D4); `d=-1`→48 (C3).

The event's note is replaced with `out_midi`; Hz is re-derived through the
active `TuningContext`. Velocity, duration, and timing are untouched. `scale`
**never** snaps — a non-integer `d` is rounded to the nearest integer degree.

### 4.5 `key` — quantization algorithm

Inputs: pitch-class set `P = { (pc_root + i) mod 12 : i ∈ I }`.

For each **single-note** event with note `n` (real-valued MIDI, possibly
microtonal):

```
best = argmin over integer semitones s with (s mod 12) ∈ P  of  |s − n|
       ties (|s−n| equal for two candidates) → choose the lower s
```

In practice: examine `floor(n)` and `ceil(n)` outward until an in-`P`
semitone is found on each side, pick the nearer; equal distance → lower.
The event's note becomes `best`; Hz re-derived through the tuning.

`key` is **octave-agnostic** — only `pc_root` matters; the result naturally
lands in whichever octave is closest to the input.

**Chord events** (`num_values > 1`) pass through `key` entirely unmodified —
no voice is snapped. (Per §11.2; chord-aware quantization is a Non-Goal.)

### 4.6 User-defined scales

Both builtins accept, instead of a name string, a `(root, intervals)` pair:

```akkado
scale(pat, "D2", [0,2,3,5,7,8,10])   // root note string, then 0-based intervals
key(pat,   "D",  [0,2,3,5,7,8,10])   // key ignores the octave as usual
```

The interval list must be a compile-time-constant array, ascending, first
element `0`, all elements in `0..11`. Violations → **E186**. The root string
is parsed by §4.3 rules (octave used by `scale`, ignored by `key`).

Named-scale strings (`"D2:minor"`) are sugar: the parser splits the string,
resolves the type to its interval list via §4.2, and proceeds identically.

### 4.7 Patternable scale/key argument

The scale/key argument may be a mini-notation pattern of name strings:

```akkado
n"c4 e4 g4 b4" |> key("<c:major a:minor>")     // cycle 0: C major, cycle 1: A minor
```

The argument pattern compiles to a per-cycle sequence of resolved scale IDs.
`EVENT_QUANTIZE` selects the active `(root, intervals)` by the **cycle index
of each event's onset** — consistent with how other transforms read per-cycle
state. A non-patterned string is the degenerate single-element case.

Implementation is staged: Phases 2–4 ship the constant-argument form; Phase 5
adds the patternable form (§9).

### 4.8 Lowering: stdlib `event_map` (no new opcode)

`scale` and `key` are stdlib akkado functions, defined in
`akkado/stdlib/event_transforms.ak` (or a new
`akkado/stdlib/scale_quantize.ak`):

```akkado
fn scale(events, name) = event_map(events, (e) -> {
    note: degree_to_note(e.note, parse_scale_root(name), parse_scale_intervals(name))
})

fn key(events, name) = event_map(events, (e) -> {
    note: snap_to_scale(e.note, parse_scale_root(name), parse_scale_intervals(name))
})

fn snap_to_scale(note, root_pc, intervals) = ...  // §4.5 algorithm
fn degree_to_note(d, root_midi, intervals)  = ...  // §4.4 algorithm
fn parse_scale_root(name)     = ...                // "d:minor"  → 50 (D3 default); "d2:minor" → 38
fn parse_scale_intervals(name) = ...               // "d:minor" → minor from scales.ak
```

The `parse_*` helpers compile-time-evaluate when `name` is a string
literal — Akkado already constant-folds string parsing in similar paths
(see `parse_pitch_to_midi`). When `name` is itself a signal (the
patternable scale-name case), this PRD's Phase 4 (formerly Phase 5) is
deferred to the follow-up PRD `prd-pattern-array-transforms.md`, since
per-cycle alternation is an array-of-events concept.

There is **no `EVENT_QUANTIZE` opcode**. The Cedar substrate is
unchanged — the parent PRD's `EVENT_MAP` opcode handles the per-event
pitch rewrite for both `scale` and `key`. Codegen needs no new handler:
once stdlib `fn scale` / `fn key` are registered, the existing
user-fn-call path lowers them through `event_map` automatically.

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| Parent event-transform substrate (`EVENT_MAP`, `OutputEvents`, `state_id` packing) | **Stays** | Hard dependency; `scale`/`key` lower to `event_map` calls |
| `cedar/include/cedar/vm/instruction.hpp` Opcode enum | **Unchanged** | No new opcode |
| Cedar VM dispatch | **Unchanged** | No new opcode |
| `akkado/include/akkado/builtins.hpp` | **Modified** | Register `scale`, `key` as stdlib-resolved names (NOP opcode entries) |
| `akkado/src/codegen_patterns.cpp` | **Unchanged** | No new C++ handler — stdlib `fn` lowering reuses the existing user-fn-call path |
| `mini_lexer.cpp` note parsing | **Stays** | Reused for tonic parsing |
| `tuning.hpp` | **Stays** | Reused for Hz re-derivation; not extended (12-TET v1) |
| `chord_parser.cpp` | **Stays** | Unaffected |
| `akkado/stdlib/scales.ak` | **New, generated** | Scale catalog (interval lists) |
| `akkado/stdlib/event_transforms.ak` (or `scale_quantize.ak`) | **Modified** | Add `fn scale`, `fn key`, helpers (`snap_to_scale`, `degree_to_note`, `parse_scale_root`, `parse_scale_intervals`) |
| `web/scripts/generate-scales.ts` | **New** | tonal.js → catalog generator |

---

## 6. File-Level Changes

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | **Unchanged** — no new opcode |
| `cedar/src/vm/vm.cpp` | **Unchanged** — no new dispatch case |
| `akkado/include/akkado/builtins.hpp` | **New** `scale`, `key` `BuiltinInfo` entries (NOP opcode; stdlib resolution) |
| `akkado/src/codegen_patterns.cpp` | **Unchanged** — no new handlers; stdlib `fn` lowering via existing user-fn-call path |
| `akkado/stdlib/event_transforms.ak` (or `scale_quantize.ak`) | **Modified** — add `fn scale`, `fn key`, helpers (`snap_to_scale`, `degree_to_note`, `parse_scale_root`, `parse_scale_intervals`) |
| `akkado/stdlib/scales.ak` | **New, generated** — named scale interval lists |
| `web/scripts/generate-scales.ts` | **New** — converts tonal.js `scale-type` data to `scales.ak` |
| `web/static/docs/reference/` | **New** doc page for `scale` / `key` |
| `web/scripts/generate-builtins-json.ts` output | Auto — picks up the two builtins for editor autocomplete |
| `CHANGELOG.md` | `Added` entry |

---

## 7. Error Codes (proposed)

| Code | Site | Meaning |
|---|---|---|
| `E184` | `scale`/`key` codegen | Unknown scale/key type name (not in the catalog) |
| `E185` | `scale`/`key` codegen | Malformed scale/key name string (bad tonic or empty type) |
| `E186` | `scale`/`key` codegen | Invalid user-defined interval list (non-ascending, not 0-based, out of `0..11`, empty, or non-constant) |

> The parent PRD's `E182` reservation was tied to the now-dropped
> `EVENT_QUANTIZE` opcode and is no longer reserved. `E184`–`E186` are free
> in `akkado/src` today and confirmed as the assigned codes for this PRD.

---

## 8. Edge Cases

| Situation | Behavior | Rationale |
|---|---|---|
| `key` input note already in scale | Unchanged (snaps to itself) | Identity |
| `key` input equidistant between two scale tones | Snap to the **lower** | §11.4 — deterministic |
| `key` on a chord event (`num_values > 1`) | Whole event passes through untouched | §4.5, §11.2 |
| `scale` fractional degree (`d = 2.7`) | Rounded to nearest integer degree (3) | §2.2 — no microtonal degrees v1 |
| `scale` degree beyond scale length (`d = 7`, k=7) | Octave wraps — degree 7 = root + 12 | §4.4 floor division |
| `scale` negative degree (`d = -1`) | Maps below the root | §4.4; requires `v"…"` source (`n"…"` clamps ≥ 0) |
| `scale("d:minor")` — no octave in name | Degree 0 lands in octave 3 | §11.5 |
| `key("d2:minor")` — octave in name | Octave digit accepted and ignored | §4.1 |
| Unknown scale type (`scale("c:bogus")`) | Compile error `E184` | §7 |
| User interval list `[2,4,5]` (not 0-based) | Compile error `E186` | §4.6 |
| `key`/`scale` result outside MIDI `0..127` | Clamped to `0..127` | Matches existing note clamping |
| Empty / single-element pattern | Transform is a no-op pass-through | Substrate-consistent |
| Non-12-EDO tuning active | Quantize still computed in 12-TET semitones; integer result resolved to Hz via the tuning | §2.2 — tuning-aware deferred |
| Patternable name with an unknown entry | `E184` at compile time for that entry | §7 |

---

## 9. Phasing

Each phase is independently testable. The parent PRD's substrate (Phase 1
there — `EVENT_MAP`/`OutputEvents` scaffolding) must precede Phase 2 here.

| Phase | Deliverable | Tests |
|---|---|---|
| **1** | `web/scripts/generate-scales.ts` + generated `akkado/stdlib/scales.ak`; interval-quality → semitone conversion | Generator unit test; spot-check ~10 scales vs tonal.js |
| **2** | Stdlib `fn key` + helpers (`snap_to_scale`, `parse_scale_root`, `parse_scale_intervals`) in `akkado/stdlib/event_transforms.ak` (or `scale_quantize.ak`). Constant name string. Requires the stdlib loader to expose top-level constant bindings from `scales.ak`. | Codegen tests; quantize WAV (≥300 s) |
| **3** | Stdlib `fn scale` + `degree_to_note` helper. Constant name string. | Degree-map codegen tests; `scale` WAV (incl. negative degrees via `v"…"`) |
| **4** | User-defined `(root, intervals)` form for both builtins; `E186` validation | Custom-scale codegen + error tests |
| **5** | **Deferred** — patternable scale/key argument (per-cycle alternation) is naturally an array-of-events concept and ships once `prd-pattern-array-transforms.md` lands the array substrate. | Per-cycle alternation test; long-render WAV (§10) |

---

## 10. Testing / Verification Strategy

- **Generator** — unit-test the interval-quality → semitone conversion;
  assert a spot set of scales (`major`, `minor`, `dorian`, `harmonic minor`,
  `major pentatonic`, `whole tone`) match tonal.js exactly.
- **`key` quantization** — codegen + Cedar opcode tests with explicit
  input/expected pairs, e.g. in C major: `c#4→c4`, `d#4→e4`? No — ties round
  **down**: `c#4→c4`, `d#4→d4`, `f#4→f4`, `g#4→g4`, `a#4→a4`. In-scale notes
  unchanged. Chord event in → identical chord event out.
- **`scale` degree mapping** — explicit pairs: `scale("d3:minor")` with
  degrees `0,2,7,-1` → MIDI `50,53,62,48`. Verify octave wrap and negative
  degrees (driven from `v"…"`).
- **Edge cases** — one test per §8 row, including each error code (`E184`,
  `E185`, `E186`) via `CHECK_FALSE(compile(...).success)`.
- **Audio render** — `nkido-cli render` of a quantized generative melody and
  a degree-mapped sequence; per the project's experiment methodology, the
  long-running pattern test (Phase 5, patternable) renders **≥ 300 s** of
  simulated audio to surface per-cycle bugs; a shorter WAV is saved for human
  listening.
- **Build / run**:
  ```bash
  cmake --build build
  ./build/akkado/tests/akkado_tests "[scale]" "[key]"
  bun web/scripts/generate-scales.ts   # regenerate catalog
  ```

---

## 11. Resolved Design Decisions

- **11.1 — Two builtins, distinct roles.** `scale` = octave-aware degree
  mapping; `key` = octave-agnostic quantization. A single overloaded `scale`
  (Strudel's design) was considered and rejected for clarity.
- **11.2 — `key` ignores chord events.** Single-note events only; chord
  events pass through. Chord-wide quantization is a Non-Goal (§2.2).
- **11.3 — Degree input source.** `scale` reads whichever numeric value the
  upstream event carries (note field or value field) as the degree.
- **11.4 — Tie-break rounds down.** Equidistant `key` snaps go to the lower
  scale tone.
- **11.5 — Default octave 3.** `scale` with no octave in the name puts degree
  0 in octave 3 (tonal.js / Strudel default).
- **11.6 — 12-TET only.** Quantization defined in 12 equal semitones for v1.
- **11.7 — Catalog is generated** from tonal.js into an Akkado stdlib file;
  not hand-maintained.
- **11.8 — Patternable argument** is **deferred** to `prd-pattern-array-transforms.md` — per-cycle alternation is an array-of-events concept that needs the array substrate.
- **11.9 — Catalog ships only as the stdlib file.** No generated C++ header,
  no interim. `scale`/`key` resolve names by loading `akkado/stdlib/scales.ak`
  through the stdlib module system, which makes the catalog userspace-visible
  and extensible. Consequence: Phase 2 has a hard dependency on stdlib-module
  loading supporting top-level constant bindings (verified as a precondition spike in Commit B of `/home/moritz/.claude/plans/phase-4-fully-unified-snowglobe.md`).
- **11.10 — No `EVENT_QUANTIZE` opcode (2026-05-24).** Original draft specced
  `scale`/`key` as a new Cedar opcode. Dropped in alignment with the parent
  PRD's principle ("opcodes are primitives, not language constructs").
  `scale`/`key` are stdlib akkado on top of `event_map`.

---

## 12. Open Questions

- **12.1 — Error-code numbers.** The parent PRD reserved `E182`, which is now
  in use. Confirm `E184`–`E186` (free in `src` today) or reassign.
- **12.2 — Method vs pipe surface.** Examples show both `pat.scale(...)` and
  `pat |> scale(...)`. Confirm both forms register identically (expected —
  consistent with `transpose`/`slow`), no separate decision needed unless the
  pattern-method dispatch needs explicit wiring.
- **12.3 — Stdlib-module loading is a hard prerequisite** (§11.9). This is
  resolved as a design decision, not an open question, but is tracked here for
  scheduling visibility: Phase 2 cannot start until stdlib-module loading
  exists. Coordinate with whoever lands `akkado/stdlib/` (parent PRD's
  `event_transforms.ak` needs the same infrastructure).

---

## 13. Related Work

- [`prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md) — the
  parent PRD; provides the `EVENT_QUANTIZE` slot and the substrate. Its §9
  Phase 5 line should be updated to cross-reference this PRD.
- [`prd-pattern-event-arrays.md`](prd-pattern-event-arrays.md) — would gate a
  future chord-aware `key` mode.
- `akkado/include/akkado/tuning.hpp` — the `TuningContext` a future
  tuning-aware version of this feature would integrate with.
