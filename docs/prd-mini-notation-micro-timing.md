> **Status: NOT STARTED** — Drafted 2026-05-18. Two-phase delivery:
> Phase 1 wires the existing record-suffix `nudge:` key through to
> runtime; Phase 2 adds the dot-padding shorthand and group support.

# PRD: Per-Note Micro-Timing in Mini-Notation

## Executive Summary

Akkado's mini-notation has no in-string operator for shifting a single
note's start time within a cycle. To create groove, swing, or
syncopation today, users must spell out subdivision-padding by hand
(`[~ c4]`, `[~ ~ c4]`, …), drop into outer Akkado for `.early()` /
`.late()` style chains, or accept rigid grid-aligned timing. Both
existing escape hatches are clumsy at live-coding speed.

This PRD introduces **two complementary, coexisting forms** of per-note
time offset that operate directly inside mini-notation strings:

1. **Dot-padding shorthand** — a run of dots adjacent to an atom or
   group acts as a divider/padding marker.
   `c4` → on the beat. `..c4` → c4 starts at 2/3 of its slot.
   `..c4...` → c4 starts at 2/6 with duration 1/6. Composes with all
   four group brackets (`[]`, `<>`, `()`, `{}`).

2. **Record-suffix `nudge:` precision** — the existing
   `atom{key:value}` mechanism already parses; this PRD wires it up at
   runtime. `c4{nudge: 0.07}` shifts c4 later by 7% of a cycle;
   `c4{nudge: -0.1}` shifts it earlier by 10%. Floats in `[-1.0, 1.0]`
   cycles.

Both forms coexist and stack: dots contribute integer
`pad_left`/`pad_right` positions on the host; `nudge:` contributes an
additive float on top of the resulting `event.time`. Events that spill
past the cycle boundary in either direction **wrap symmetrically**
into the adjacent cycle, matching Strudel's mental model.

**Key design decisions** (resolved during the question rounds):

- **Two-phase delivery.** Phase 1 = record-suffix `nudge:` runtime
  wiring (smallest surface; parser already accepts the syntax). Phase
  2 = dot-padding shorthand with group support.
- **Sigil is `.` (dot).** Not currently used anywhere inside the
  mini-notation string; reads as a small musical "silence dot."
- **Divider/padding semantics**: `L` dots before + the host + `R`
  dots after produce `total = L + 1 + R` positions; host start = `L /
  total` of slot, host duration = `1 / total` of slot.
- **Group support in v1.** Dots attach to atoms and to all four
  group bracket types (`[]`, `<>`, `()`, `{}`).
- **Pitch, sample, and chord atoms only.** Rests (`~`) reject
  padding — `..~` is a parse error (a delayed rest is just a smaller
  rest; use a different rest length).
- **Modifier order**: existing modifiers (`*`, `/`, `@`, `!`, `?`)
  apply first to subdivide / select; pad shrinks the result. (In
  practice equivalent for all currently-shipping modifiers, but pinned
  as spec for future ones.)
- **Cycle wrap is symmetric.** Late-spill wraps into the next cycle,
  early-spill into the previous cycle. Matches Strudel and avoids
  asymmetry footguns.
- **`@.time` returns the nudged time.** "When this event actually
  fires." The slot-base is recoverable as `@.time - @.nudge` if a
  patch needs it.
- **Whitespace required between sigil-groups**: `c4....d4` is a parse
  error; write `c4... .d4` or `c4 ....d4`.
- **Bare dots are a parse error**, not a silent rest. Use `~`.
- **Pad run length capped at 16/side**; lexer rejects more with a
  clear error.

---

## 1. Current State

### 1.1 What mini-notation does for timing today

Mini-notation timing is **fully positional**: a step's time and
duration derive from its index inside its parent scope, never from a
field on the atom. The evaluator (`akkado/src/pattern_eval.cpp`) walks
the AST recursively, creating child `PatternEvalContext` scopes via
`subdivide()` and `subdivide_weighted()`, and writes
`event.time = ctx.start_time`, `event.duration = ctx.duration` for
each emitted event.

The only existing way to "shift" a note within a step is hand-written
subdivision:

| What you want                  | How you write it today |
|--------------------------------|------------------------|
| c4 on the beat                 | `c4`                   |
| c4 at the half-step            | `[~ c4]`               |
| c4 at the 3/4-step             | `[~ ~ ~ c4]`           |
| c4 at the 2/6 with short dur   | `[~ ~ c4 ~ ~ ~]`       |
| 7% swing per note              | n/a in-string          |
| nudge a whole `[a b c]` group  | n/a in-string          |

### 1.2 Token inventory (no free timing operators)

Every existing mini-notation sigil is taken
(`akkado/include/akkado/mini_token.hpp:36–79`,
`akkado/src/mini_lexer.cpp:701–733`):

| Sigil(s)        | Meaning                                |
|-----------------|----------------------------------------|
| `* /`           | Speed multiplier / slow divisor        |
| `@`             | Weight (also outer Akkado hole token)  |
| `!`             | Repeat count                           |
| `?`             | Chance                                 |
| `\|`            | Choice / alternation                   |
| `[ ]`           | Grouping                               |
| `< >`           | Sequence (one element per cycle)       |
| `( )`           | Euclidean rhythm                       |
| `{ }`           | Polymeter / record-suffix              |
| `:`             | Sample variant (`bd:2`)                |
| `~`             | Rest                                   |
| `_`             | Elongate / tie                         |
| `^ v + \`       | Pitch microtonal offsets               |
| `' #`           | Chord suffix / pitch sharp             |

`.` is **not used** inside the mini-notation string. Outer Akkado
record access (`@.freq`) lives outside the string, so the dot is
reusable for in-string semantics without parser conflict.

### 1.3 Why we can't repurpose `_`

`_` already means **elongate** — it extends the previous atom's
duration. `c4 _ _` parses as `MiniAtomKind::Elongate` placeholders
that lengthen `c4` to span 3 slots. Repurposing `_` for nudge would
collide even with adjacency rules: `c4__` (no space) is irreducibly
ambiguous between "elongate twice" and "nudge by 2 padding chars."

### 1.4 Why we're not porting Strudel/Tidal's `.nudge()` chain

Strudel and Tidal both expose timing through whole-pattern method
chains (`.nudge()`, `.early()`, `.late()`, `.swing()`, `.swingBy()`,
`press`, `off`, `ply`) — none have in-string operators. Bringing
nudge into the string makes per-step shifts ergonomic at live-coding
speed and lets the pattern *itself* be the source of truth for groove,
rather than splitting it between the pattern string and a chained
modifier.

### 1.5 What's already half-wired

Two pieces of infra make this cheap to land:

- **`PatternEvent` already has `time` and `duration` floats**
  (`akkado/include/akkado/pattern_event.hpp:30–99`). A nudge handler
  just adjusts `event.time` after the positional scheduler runs.
- **Record-suffix `atom{key:value}` is parsed today** and stored on
  the atom AST in `MiniAtomData::properties`
  (`docs/mini-notation-reference.md:274–309`). `c4{nudge: 0.07}`
  already round-trips through lexer + parser; runtime exposure is the
  only missing piece. Phase 1 closes this gap.

---

## 2. Goals and Non-Goals

### 2.1 Goals

- Per-note time offsets writeable directly inside a mini-notation
  string (no outer Akkado chain required).
- Two ergonomic forms — terse symbolic for live coding (dots),
  precise numeric for groove templates (`nudge:` record key).
- First-class group support — nudge a `[a b c]` or `<a b c>` as a
  unit.
- Composes with every existing mini-notation construct without
  changing their meaning.
- Backwards compatible — every pattern that parses today continues
  to parse identically.
- Surface pad/nudge state in the debug panel and AST JSON.

### 2.2 Non-Goals

- **Whole-pattern timing functions** (`.nudge()`, `.swing()`,
  `.swingBy()`, `.early()`, `.late()`) — deferred to a future PRD
  ("pattern method chain timing"). They would live in outer Akkado
  and operate on `Pattern` objects, not in the string.
- **Per-cycle randomized humanize** (`.humanize(amount)`) — deferred.
- **Numeric arguments inside dots** (e.g., `c4...3` meaning "shift by
  3"). The dot-count *is* the argument; use record-suffix
  `c4{nudge: 0.3}` for precise values.
- **Pad on rests**. `..~` is a parse error.
- **Editor decorations on the source line.** Could be added later;
  not in v1.
- **Microtonal-style tuning-context interactions.** Nudge is purely
  temporal; no pitch coupling.

---

## 3. Target Syntax

### 3.1 Dot-padding shorthand (Phase 2)

```akkado
pat("c4 ..d4 e4 f4")              // d4 nudged late by 2/3 of slot
pat("c4 d4 e4 f4...")             // f4 on beat, duration shrunk to 1/4
pat("c4 ..d4... e4 f4")           // d4 starts at 2/6, dur 1/6
pat("[c4 ..d4 e4 f4]")            // d4 nudged inside its 1/4-slot
pat("..[a b c]")                  // group nudged late by 2/3
pat("[a b c]...")                 // group on beat, interior compressed
pat("..<a b c>...")               // alternation group, late by 2/6
pat("..(3,8)")                    // euclidean group, late by 2/3
pat("..{a, b, c}")                // polymeter group, late by 2/3
```

Divider math:

| Notation     | L | R | Total | Start  | Dur   | Reads as           |
|--------------|---|---|-------|--------|-------|--------------------|
| `c4`         | 0 | 0 | 1     | 0      | 1     | on the beat        |
| `.c4`        | 1 | 0 | 2     | 1/2    | 1/2   | half late          |
| `..c4`       | 2 | 0 | 3     | 2/3    | 1/3   | 2/3 late           |
| `...c4`      | 3 | 0 | 4     | 3/4    | 1/4   | 3/4 late           |
| `c4.`        | 0 | 1 | 2     | 0      | 1/2   | on beat, halved    |
| `..c4...`    | 2 | 3 | 6     | 2/6    | 1/6   | 1/3 late, short    |

This is a compact spelling of `[~…~ c4 ~…~]` inside a single slot —
the parser expands it to the equivalent subdivision.

### 3.2 Record-suffix `nudge:` precision (Phase 1)

```akkado
pat("c4 d4{nudge: 0.07} e4 f4")   // d4 shifted late by 7% of cycle
pat("c4 d4 e4{nudge: -0.05} f4")  // e4 shifted early by 5%
pat("bd sd{nudge: 0.02} bd cp")   // groove on snare
pat("c4 ..d4{nudge: 0.03} e4")    // dots + nudge: stack
```

`nudge:` is a float in **cycles**, range `[-1.0, 1.0]`. Positive =
later, negative = earlier. Out-of-range values are a parse error
caught early with a clear message.

### 3.3 Combined forms

```akkado
// Atom-level: dots and nudge stack additively
pat("..c4{nudge: 0.03}")
// event.time = scope.start + (2/3)*scope.dur + 0.03

// Group-level: dots on group, nudge applies per-event inside
pat("..[c4 d4{nudge: 0.05} e4]")
// outer pad shrinks the group; d4 inside is nudged further by 0.05

// Mixed atom kinds
pat("..bd ..sd:2 ..C4'")          // pad on sample, sample-variant, chord
```

### 3.4 Composition with existing modifiers

| Pattern         | Meaning                                                |
|-----------------|--------------------------------------------------------|
| `..c4*2`        | c4 sped x2 (2 events filling its scope), then pad shrinks the result into the last 1/3 |
| `..c4/2`        | c4 spans 2 cycles, then pad shrinks each cycle's scope |
| `..c4!3`        | c4 repeated 3 times in its scope, all shrunk into last 1/3 |
| `..c4?0.5`      | c4 nudged late, 50% chance to fire                    |
| `..c4@2`        | c4 has weight 2 in its parent slot, internally nudged within that weighted scope |

Order of operations is **"modifier first, then pad shrinks the
result."** For all currently-shipping modifiers this is equivalent to
pad-first (since the inner subdivisions are uniform within the
scope), but pinning the order as spec gives a clean mental model for
future modifiers.

---

## 4. Semantics

### 4.1 Pad arithmetic

When entering an atom or group scope with pad values `L` and `R`:

```
total            = L + 1 + R
scope.start_time = original_start + (L / total) * original_duration
scope.duration   = original_duration / total
```

The host then subdivides its internal events normally inside the
shrunk window. Pads compose recursively — nesting `..[c4 ..d4 e4]`
applies the outer pad to the group, then the inner pad to `d4` inside
the group's (already shrunk) sub-slot.

### 4.2 Nudge arithmetic

After the positional + pad scheduler emits an event, the
record-suffix `nudge` value applies as an additive offset:

```
event.time += nudge_cycles                      // both in cycles
event.duration is unchanged by nudge
```

If both forms are present:

```
event.time = scope.start + (L/total)*scope.dur + nudge
event.duration = scope.dur / total
```

### 4.3 Cycle wrap (symmetric)

After all offsets apply, if `event.time` falls outside the current
cycle, the event wraps:

```
event.time = fmod(event.time + N_CYCLES, 1.0)      // wrap into [0, 1)
```

with `N_CYCLES` large enough to handle the worst case (nudge range
`[-1.0, 1.0]` + max group pad shift `< 1.0` + max slot offset `< 1.0`,
so `N_CYCLES = 4` is safe).

Late-spill events appear in the next cycle; early-spill events
appear in the previous cycle. This matches Strudel's continuous
pattern model and avoids asymmetric clamping behavior.

### 4.4 `@.time` and `@.phase` semantics

Both fields return the **nudged** time — the moment the event
actually fires. This preserves the user's intuition that
`@.time == event.time` and that `osc(@.freq) * ar(@.trig, ...)` lines
up with the audible attack of the note.

If a patch needs the un-nudged slot-base, it can compute it as
`@.time - @.nudge` (a new computed field exposing the applied offset
— see §4.5).

### 4.5 New `@.nudge` exposed field

For introspection and downstream use, a new pipe-binding field
`@.nudge` exposes the total applied offset in cycles (pad-derived
shift + record-suffix `nudge:`), so users can:

- Build groove visualizations (`@.nudge * BLOCK_SIZE` etc.).
- Conditional fanout based on swing magnitude.
- Recover the slot-base time as `@.time - @.nudge`.

Default value is `0.0` for any event with no pad/nudge applied.

---

## 5. Architecture

### 5.1 AST changes

```cpp
// akkado/include/akkado/ast.hpp

struct PadMixin {
    uint8_t pad_left  = 0;   // 0..16
    uint8_t pad_right = 0;   // 0..16
};

struct MiniAtomData : public PadMixin { ... };          // existing + mixin
struct MiniGroup      : public PadMixin { ... };
struct MiniSequence   : public PadMixin { ... };       // <a b c>
struct MiniEuclidean  : public PadMixin { ... };       // (3,8)
struct MiniPolymeter  : public PadMixin { ... };       // {a, b, c}
```

`MiniAtomData::properties["nudge"]` already exists (record-suffix);
no AST change needed for nudge in Phase 1.

### 5.2 Lexer rules (Phase 2)

```cpp
// akkado/src/mini_lexer.cpp

// New token type:
//   Dot { count: uint8_t, side: Adjacent::Left | Right }
//
// Lex a run of consecutive '.' characters. Emit a single Dot token
// with the count.
//
// Lexer-level checks:
//  1. Run length > 16 → error: "pad run too long (max 16)"
//  2. If a run is *not* immediately adjacent to a pitch / sample /
//     chord atom (left = atom follows; right = atom precedes) OR to
//     a group bracket, emit a Dot token tagged Unattached. The
//     parser converts it to a clear "stray pad" error.
//  3. If a Dot run is followed by whitespace and then another Dot
//     run with no host in between, error: "ambiguous pad — add a
//     host between sigil-groups."
```

The lexer maintains its existing whitespace-significant behavior;
adjacency is encoded into the token at lex time so the parser stays
simple.

### 5.3 Parser changes (Phase 2)

```cpp
// akkado/src/mini_parser.cpp

// When parsing an atom or group:
//   - If preceded by Dot(side=Right) on the *previous* token slot,
//     reject (right-pad must follow a host).
//   - If preceded by Dot(side=Left), consume it and set
//     host.pad_left = dot.count.
//   - After parsing the host, if the next token is Dot(side=Right)
//     and adjacent (no whitespace), consume it and set
//     host.pad_right = dot.count.
//
// Reject host kinds:
//   - Rest (~): "pad on rest is not allowed; use a different rest
//     length instead"
```

### 5.4 Pattern evaluation

```cpp
// akkado/src/pattern_eval.cpp

void enter_scope_with_pad(PatternEvalContext& ctx, uint8_t L, uint8_t R) {
    if (L == 0 && R == 0) return;
    uint32_t total = uint32_t(L) + 1u + uint32_t(R);
    float frac_L  = float(L) / float(total);
    float frac_1  = 1.0f       / float(total);
    ctx.start_time += frac_L * ctx.duration;
    ctx.duration   *= frac_1;
}

// After event is fully built (post-subdivision, post-pad), apply
// record-suffix nudge:
void apply_nudge(PatternEvent& ev, float nudge_cycles) {
    if (nudge_cycles == 0.0f) return;
    ev.time += nudge_cycles;
    ev.time -= std::floor(ev.time);   // wrap into [0, 1)
    ev.nudge_total += nudge_cycles;   // for @.nudge exposure
}
```

### 5.5 Debug serialization

`akkado::serialize_mini_ast_json()` adds `pad_left` / `pad_right`
fields to atom and group node JSON, and includes the resolved
`nudge` float from `properties`. The web pattern debug panel
(`web/src/lib/components/Panel/PatternDebugPanel.svelte`) already
walks this JSON and adds badges; pad and nudge surface there
automatically once the fields are emitted.

The active-step highlight (`pattern-highlight.svelte.ts`) tracks
`event.time` directly — no UI change needed; the highlight lands at
the nudged position by construction.

---

## 6. Impact Assessment

| Component                            | Status     | Notes                                                                 |
|--------------------------------------|------------|-----------------------------------------------------------------------|
| `mini_token.hpp` / token list        | **Modified** | Add `Dot` token (Phase 2)                                            |
| `mini_lexer.cpp`                     | **Modified** | Lex dot runs, enforce adjacency, cap at 16/side (Phase 2)            |
| `mini_parser.hpp` / parser src       | **Modified** | Attach pad to atom/group; reject pad on rest (Phase 2)               |
| `ast.hpp` group nodes                | **Modified** | Add `PadMixin` to four group node types (Phase 2)                    |
| `ast.hpp` MiniAtomData               | **Modified** | Add `PadMixin` (Phase 2)                                             |
| `pattern_event.hpp`                  | **Modified** | Add `nudge_total: float` field for `@.nudge` exposure                |
| `pattern_eval.cpp`                   | **Modified** | Pad shrink on scope entry; nudge apply on event emit; cycle wrap     |
| `codegen_patterns.cpp`               | **Modified** | Honor `properties["nudge"]` (Phase 1); read pad fields (Phase 2)     |
| `serialize_mini_ast_json`            | **Modified** | Emit pad + nudge fields                                              |
| `pattern-highlight.svelte.ts`        | **Stays**    | Uses `event.time` — picks up nudges automatically                    |
| `PatternDebugPanel.svelte`           | **Modified** | Render pad/nudge badges (small UI change)                            |
| `docs/mini-notation-reference.md`    | **Modified** | Document dot syntax, divider math, group composition, edge cases     |
| `web/static/docs/`                   | **Modified** | Rebuild F1 lookup index (`bun run build:docs`)                       |
| `cedar/` — VM, opcodes               | **Stays**    | All work happens above Cedar; runtime sees only event time/duration  |
| Existing patterns w/ `~` / `_`       | **Stays**    | No parse changes; pad on rest rejected                               |
| Outer Akkado record-suffix infra     | **Stays**    | Already parses `nudge:`; Phase 1 just reads it                       |

---

## 7. File-Level Changes

### Phase 1 — Record-suffix `nudge:` runtime wiring

| File | Change |
|------|--------|
| `akkado/include/akkado/pattern_event.hpp` | Add `float nudge_total = 0.0f;` field |
| `akkado/src/codegen_patterns.cpp` | In the per-event emit loop, read `atom.properties["nudge"]` (float); validate range `[-1.0, 1.0]`; call `apply_nudge(ev, value)` |
| `akkado/src/pattern_eval.cpp` | Add `apply_nudge()` helper with symmetric cycle wrap; call from event-emit site |
| `akkado/include/akkado/pattern_event.hpp` + `codegen.cpp` | Expose `@.nudge` pipe-binding field (extend the existing `pattern_field_aliases()` table in `akkado/src/typed_value.cpp:30–49`) |
| `akkado/src/serialize.cpp` (or wherever `serialize_mini_ast_json` lives) | Emit `nudge` in the atom JSON when set |
| `akkado/tests/test_pattern_event.cpp` (new or extend) | Unit tests for nudge stacking, wrap, range |
| `experiments/test_op_seq_nudge.py` (new) | ≥300 s render test (see §8.4) |
| `docs/mini-notation-reference.md` | New section: "Per-note timing — `nudge:`" |

### Phase 2 — Dot-padding shorthand

| File | Change |
|------|--------|
| `akkado/include/akkado/mini_token.hpp` | Add `Dot` token type |
| `akkado/src/mini_lexer.cpp` | Lex dot runs; enforce cap at 16; emit adjacency-tagged tokens; reject ambiguous configurations |
| `akkado/include/akkado/ast.hpp` | Add `PadMixin` to `MiniAtomData`, `MiniGroup`, `MiniSequence`, `MiniEuclidean`, `MiniPolymeter` |
| `akkado/include/akkado/mini_parser.hpp` + `akkado/src/mini_parser.cpp` | Consume leading/trailing pad tokens and attach to atom/group; reject pad on rest |
| `akkado/src/pattern_eval.cpp` | Add `enter_scope_with_pad()` helper; call at every scope-entry site for atom and group node visits |
| `akkado/src/serialize.cpp` | Emit `pad_left` / `pad_right` in atom and group JSON |
| `akkado/tests/test_mini_lexer.cpp` | Lexer tests: pad runs, length cap, ambiguity errors, rest rejection |
| `akkado/tests/test_mini_parser.cpp` | Parser tests: AST attachment for all atom kinds and all four group brackets |
| `akkado/tests/test_pattern_eval.cpp` | Evaluation tests: divider math correctness on atoms and groups |
| `experiments/test_op_seq_nudge.py` | Extend Phase 1 test to cover dot syntax |
| `web/src/lib/components/Panel/PatternDebugPanel.svelte` | Render pad badges on atom/group nodes |
| `docs/mini-notation-reference.md` | Extend the timing section with dot syntax |
| `web/scripts/build-docs.ts` | No code change; run `bun run build:docs` after docs edit |

---

## 8. Testing / Verification Strategy

### 8.1 Akkado unit tests (`akkado/tests/`)

| Test | Input | Expected |
|------|-------|----------|
| Lex pad run | `..c4` | One `Dot{count:2, side:Left}` + one Pitch atom |
| Cap at 16 | `.................c4` (17 dots) | Lexer error "pad run too long" |
| Reject ambiguous | `c4....d4` | Lexer error "add a host between sigil-groups" |
| Reject pad on rest | `..~` | Parser error "pad on rest is not allowed" |
| Pad on atom AST | `..c4...` | `MiniAtomData{pad_left:2, pad_right:3}` |
| Pad on group AST | `..[a b c]...` | `MiniGroup{pad_left:2, pad_right:3, children:[a,b,c]}` |
| Pad on sequence | `..<a b c>` | `MiniSequence{pad_left:2}` |
| Pad on euclidean | `..(3,8)` | `MiniEuclidean{pad_left:2, hits:3, steps:8}` |
| Pad on polymeter | `..{a, b}` | `MiniPolymeter{pad_left:2}` |
| Pad on pitch w/ microtonal | `..c^4` | `MiniAtomData{pad_left:2, midi:60, micro:+1}` |
| Pad on sample variant | `..bd:2` | `MiniAtomData{pad_left:2, sample:"bd", variant:2}` |
| Pad on chord | `..Am'...` | `MiniAtomData{pad_left:2, pad_right:3, chord:"Am"}` |

### 8.2 pattern_eval unit tests

| Pattern | Expected event(s) |
|---------|-------------------|
| `..c4...` | `{time: 2/6, duration: 1/6}` |
| `..[a b c]` | three events: `{time:2/3, dur:1/9}`, `{time:7/9, dur:1/9}`, `{time:8/9, dur:1/9}` |
| `c4{nudge: 0.1}` | `{time: 0.1, duration: 1, nudge: 0.1}` |
| `c4{nudge: -0.1}` | `{time: 0.9, duration: 1, nudge: -0.1}` (wrap) |
| `..c4{nudge: 0.05}` | `{time: 2/3 + 0.05, duration: 1/3}` |
| `c4{nudge: 1.5}` | Parse error: nudge out of range |

### 8.3 Compositions with existing modifiers

| Pattern | Verify |
|---------|--------|
| `..c4*2` | 2 events squeezed into last 1/3 of slot |
| `..c4/2` | c4 spans 2 cycles, each cycle's scope shrunk to last 1/3 |
| `..c4!3` | 3 c4 events in last 1/3 of slot |
| `..c4?0.5` | Probabilistic firing preserved post-pad |
| `..c4@2` | c4 weighted in parent, pad applies inside the weighted scope |

### 8.4 Python experiment (`experiments/test_op_seq_nudge.py`)

Per `CLAUDE.md` DSP methodology, this test must:
- Render **≥300 s of simulated audio** with a sequence that exercises
  atom pads, group pads, `nudge:` values (both signs), cycle wrap at
  both boundaries, and at least one combined `..c4{nudge: ...}` form.
- Trace JSON validates event onsets block-by-block at the expected
  sample positions.
- Save a WAV file for human listening (groove should be audible).
- Report ✓/✗ per assertion in the documented style.

Pseudo-structure:

```python
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_seq_nudge")

def test_nudge_record_suffix():
    """
    Verify c4{nudge: 0.1} fires exactly 10% of a cycle later than its
    grid position.
    """
    host = CedarTestHost()
    # ... compile pattern, run ≥300 s, inspect trace ...

def test_pad_atom():
    """..c4... fires at 2/6 with duration 1/6."""
    ...

def test_pad_group():
    """..[a b c] places three events at 2/3, 7/9, 8/9."""
    ...

def test_cycle_wrap_late():
    """Last note nudged late spills into the next cycle."""
    ...

def test_cycle_wrap_early():
    """First note nudged early spills into the previous cycle."""
    ...

def test_combined_pad_and_nudge():
    """..c4{nudge: 0.05} stacks: time = 2/3 + 0.05."""
    ...
```

### 8.5 Backwards-compatibility audit

Before merging Phase 2, grep the test corpus for any existing pattern
string containing `.` adjacent to an atom or bracket, since today
that would have been a lexer error. Confirm none of the corpus
inadvertently changes meaning.

```bash
grep -rEn '"\.+[a-zA-Z[(<{]|[a-zA-Z\])>}]\.+"' akkado/tests web docs
```

### 8.6 Web pattern debug panel (manual)

After Phase 2:
1. Open `web/` in `bun run dev`.
2. Compile pattern `..[c4 ..d4 e4]...` and open the pattern debug
   panel.
3. Confirm pad badges render on the outer group and on `d4`.
4. Press play and confirm the active-step highlight lands at the
   shrunk/shifted positions.
5. Open the AST JSON tab and confirm `pad_left`/`pad_right` are
   present on the right nodes.

---

## 9. Edge Cases

### 9.1 Both sides of the same host

`..[a b]...` — both pads apply; total = 2 + 1 + 3 = 6 positions; the
group occupies positions 2 through 2 (start = 2/6, dur = 1/6). Inside
that 1/6 slot, `a` and `b` each get 1/12.

### 9.2 Pad on a 0-duration host (rest)

Rejected at parse time with: `pad on rest (~) is not allowed; use a
longer rest instead`.

### 9.3 Nudge that exceeds `[-1.0, 1.0]`

Rejected at parse time with: `nudge out of range [-1.0, 1.0]: got
1.5`. The cycle-wrap math handles anything *up to* one cycle in
either direction; beyond that, the user is almost certainly typing a
mistake.

### 9.4 Pad inside a `?` chance branch

`c4?0.5 .d4` — d4 nudged late and is its own atom (not part of the
chance). Chance applies only to `c4`. Parsed normally.

### 9.5 Pad on a polyrhythm element

`[a, ..b, c]` — the comma is polyrhythm; each element runs in
parallel for the full slot. `..b` shrinks b into the last 1/3 of the
slot; `a` and `c` still run for the full slot. (Polyrhythm scopes are
independent.)

### 9.6 Trailing pad on the last atom of a cycle

`pat("c4 d4 e4 f4...")` — f4 occupies its 1/4-cycle slot for only
1/4 of that slot's duration. The remaining 3/4 is silence inside f4's
slot (no wrap to next cycle).

### 9.7 Late-wrap and early-wrap visualization

When a wrapped event fires in the "next" cycle visually, the pattern
debug panel should still associate it with its source AST node. The
serializer emits the source node ID alongside `time` so the
highlighter can light up the correct token even when the event lives
in an adjacent cycle.

### 9.8 Combined wrap

`..c4{nudge: 0.5}...` — pad puts c4 at 2/6 with dur 1/6, then nudge
adds 0.5 of a cycle. Result: `time = 2/6 + 0.5 = 0.833`, which is
inside the cycle, no wrap. But `c4{nudge: 0.99}` at slot 0 wraps to
`0.99`, still inside the cycle; at slot 3/4 it wraps to `3/4 + 0.99
mod 1 = 0.74`, in the *next* cycle. Both behaviors handled by the
single `floor()`-wrap formula.

### 9.9 Pad on weighted atoms (`@N` weight)

`..c4@2` — c4's parent scope gives c4 a 2-weighted slot. Pad applies
*inside* that weighted slot, shrinking it further. The parent's
weight distribution is unaffected.

### 9.10 Cap at 16/side

`...............c4` (15 dots) → start at 15/16, dur 1/16. ✓.
`................c4` (16 dots) → start at 16/17, dur 1/17. ✓.
`.................c4` (17 dots) → lexer error.

---

## 10. Implementation Phases

### Phase 1 — Record-suffix `nudge:` runtime wiring

**Goal:** Make `c4{nudge: 0.07}` and `c4{nudge: -0.1}` work end-to-end
without touching the lexer.

**Deliverables:**
- `pattern_event.hpp` extended with `nudge_total` field
- `codegen_patterns.cpp` reads `atom.properties["nudge"]`, validates,
  applies via `apply_nudge()`
- `pattern_eval.cpp` ships `apply_nudge()` with symmetric cycle wrap
- `@.nudge` exposed as a pipe-binding field
- Serializer emits `nudge` in atom JSON
- Phase-1 subset of `experiments/test_op_seq_nudge.py` (record-suffix
  forms only, ≥300 s render)
- `docs/mini-notation-reference.md` gains a "Per-note timing" section
  documenting `nudge:`

**Verification:** §8.2 nudge tests pass; §8.4 Phase-1 experiment
prints all ✓; manual web debug panel shows nudge on AST.

### Phase 2 — Dot-padding shorthand + group support

**Goal:** Add `.` as a lexer token, attach pad to atoms and all four
group kinds, ship divider-math evaluation.

**Deliverables:**
- `Dot` token in lexer with adjacency tagging, 16/side cap, ambiguity
  rejection
- `PadMixin` on all five AST node types
- Parser attaches pad to host, rejects pad-on-rest
- `enter_scope_with_pad()` in evaluator
- Serializer emits `pad_left` / `pad_right`
- Pattern debug panel renders pad badges
- `experiments/test_op_seq_nudge.py` extended with dot tests
- `docs/mini-notation-reference.md` extended with dot syntax,
  divider math table, group composition rules, edge cases
- `bun run build:docs` to refresh F1 lookup

**Verification:** §8.1, §8.2, §8.3, §8.5, §8.6 all pass; §8.4
extended Phase-2 experiment prints all ✓.

---

## 11. Open Questions / Future Work

These are explicitly **out of scope** for this PRD; called out so
future PRDs have a clear handoff:

- **Pattern method-chain timing** (`.nudge()`, `.swing()`,
  `.swingBy()`, `.early()`, `.late()`) on the outer Akkado `Pattern`
  object. Complements in-string nudging — would let you do
  `"c4 d4 e4 f4".swing()` for whole-pattern groove.
- **Humanize / random nudge** (`.humanize(amount)`,
  `c4{nudge: rand(-0.05, 0.05)}`). Needs the runtime random
  primitive PRD to land first.
- **MIDI-derived swing presets** ("MPC swing 54%", "Linn 62%"). A
  thin sugar layer over `.swingBy()`.
- **Per-cycle micro-timing patterns** — e.g., apply a different nudge
  pattern per cycle. Composable with the proposed `nudge:` field
  once it's runtime-tunable from a control source.
- **Sample-accurate nudge** below the block boundary (currently
  block-aligned at 128 samples). Sub-block scheduling is a Cedar VM
  concern; out of scope here.

---

## 12. References

- Plan file: `/home/moritz/.claude/plans/is-there-currently-in-declarative-matsumoto.md`
- Mini-notation parser: `akkado/include/akkado/mini_token.hpp:36–79`,
  `akkado/src/mini_lexer.cpp:701–733`
- AST nodes: `akkado/include/akkado/ast.hpp:56–57, 197–257`
- Pattern event: `akkado/include/akkado/pattern_event.hpp:30–99`
- Pattern eval: `akkado/src/pattern_eval.cpp`,
  `akkado/include/akkado/pattern_eval.hpp`
- Codegen: `akkado/src/codegen_patterns.cpp` (event emit site:
  ~`:1629–1637`)
- Field-alias table: `akkado/src/typed_value.cpp:30–49`
- Mini-notation reference doc: `docs/mini-notation-reference.md`
  (record-suffix section: 274–309, fields section: 338–373,
  compatibility table: 433–450)
- Related PRDs: [`prd-pattern-array-note-extensions.md`](./prd-pattern-array-note-extensions.md),
  [`prd-microtonal-extension.md`](./prd-microtonal-extension.md),
  [`prd-hole-field-shorthand.md`](./prd-hole-field-shorthand.md)
- Strudel time modifiers reference:
  https://strudel.cc/learn/time-modifiers/
- Tidal Cycles time reference: https://tidalcycles.org/docs/reference/time/
