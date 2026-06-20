> **Status: PHASE 4 IMPLEMENTED (2026-06-19).** Phase 1 (pattern model +
> resolver), Phase 2 (operators routed through the shared `OverloadTable`),
> Phase 3 (multi-form builtin families migrated; `sample` drives the first live
> multi-pattern `resolve()` in codegen), and Phase 4 (user-function overloading)
> have shipped; Phase 5 (heavy pattern/higher-order handlers) is not started.
> Spun out of
> `prd-compiler-type-system.md` ("Phase 4 / Deferred"), which shipped the
> `ValueType`/`TypedValue` foundation but explicitly deferred overload
> resolution because "no mechanism exists." This PRD specifies that mechanism:
> a single, declarative, first-match dispatch model unifying builtin overloads,
> operators, and user-defined function overloading.
>
> **Phase 2 as-built reconciliation.** Operators **reuse their existing builtin
> names** (`add`/`sub`/`mul`/`div`/`pow`, `eq`/`neq`/`lt`/`gt`/`lte`/`gte`,
> `band`/`bor`/`bnot`) as `OverloadTable` keys — there are **no `op_*` names** and
> **no analyzer desugaring pass**; the parser already desugars operators to these
> names (`parser.cpp` `parse_binary`). The §5.4 `op_*` sketch is cosmetic. Each
> operator has a single pattern: arithmetic carries a `LegacyHandler` target (the
> array/stereo broadcasting handler `handle_binary_op_call`), comparison/logical a
> `Builtin` target. Because every operator is single-pattern, codegen dispatch
> reduces to that one target and does **not** call `resolve()` by argument type
> yet — live multi-pattern `resolve()` in codegen lands in Phase 3. `op_neg`
> (unary minus) and `op_mod` (`%`) are **out of scope**: neither operator exists
> in Akkado (`%` is the hole token; `neg`/`fmod` are functions only). `pow` (`^`),
> omitted from the §5.4 list, **is** covered.
>
> **Phase 3 as-built reconciliation.** A new `lookup_builtin_overloads`
> (`overload.cpp`) registers the migrated families; names absent from it keep the
> single-pattern `make_builtin_pattern` path (no change for ~200 builtins).
> `pan`/`pingpong`/`smooch`/`wt`/`wavetable` migrate as **single LegacyHandler
> patterns** (new `LegacyHandlerId::{Pan,Pingpong,Smooch}`) — their dispatch
> dimension (channel width / arg count) is orthogonal to the type model (§3), so
> it stays inside the existing handler; this is pure plumbing like Phase 2's
> arithmetic. `delay`/`delay_ms`/`delay_smp` migrate as single Builtin patterns
> and the redundant `if (func_name=="delay")` rate ladder is retired — the time
> unit was already data-driven via `BuiltinInfo::inst_rate`. **`sample`/
> `sample_loop`** are the one genuinely multi-form family: two Builtin patterns
> keyed by the id slot type (`String` name form first, `Signal` numeric/runtime
> id form second), and codegen now calls **`resolve()`** for real to select the
> form and reject a non-String/Signal id with **E424** (the no-match diagnostic).
> **Descoped vs §4.1:** the `delay(sig, "ms", t)` literal-unit form is **not**
> shipped — `delay_ms`/`delay_smp` keep their existing call shapes. Because that
> was the only proposed consumer of the `StringLiteral`/`NumberLiteral` matchers,
> those matcher kinds remain resolver-only (implemented + unit-tested, no codegen
> consumer yet); `sample` dispatches by `Kind::Type`, not literal value. New
> coverage: 9 `[overload]` cases (table shape, `resolve()`-by-id-type, E424
> codegen). Existing `[stereo]`/`[sample]`/`[golden]` suites stay green unchanged.

# Akkado Builtin & Function Overload Resolution PRD

## 1. Executive Summary

Akkado has no overload-resolution mechanism. A builtin maps to exactly one
`BuiltinInfo` (one entry per name in `BUILTIN_FUNCTIONS`,
`builtins.hpp:1687`), and every form that "looks like an overload" is faked one
of three incompatible ways:

- **Optional args + defaults** (`optional_count` + `defaults[]`) — most "3-arg
  vs 5-arg" builtins are *not* overloads at all, just one signature with
  defaulted slots.
- **Ad-hoc handler branching** — a handful of custom handlers (`pan`,
  `balance`, `pingpong` in `codegen_stereo.cpp`; `sample`, `delay`/`delay_ms`/
  `delay_smp`) branch on `args.nodes.size()`, channel count, or a literal
  string value with hand-written `if` ladders.
- **Nothing** — user-defined functions cannot be overloaded; a re-`fn` of the
  same name simply replaces the prior definition.

This sprawl has a second cost flagged during the `prd-compiler-type-system`
audit: **`param_types` is only enforced in the generic `visit_call` type-check
loop** (`codegen.cpp:1529-1573`). Every builtin with a genuinely specific-type
parameter is custom-handled and returns *before* that loop, so its `param_types`
annotation is **decorative** (`transport`'s `{Pattern}`, `midi`'s `{Record}`,
the visualizers' `{Signal, String, Record}` are never checked by the generic
path — enforcement, where it exists, is a separate per-handler diagnostic like
E133, E198, E423).

This PRD replaces all of it with one model:

- Every overloadable name (builtin, operator, or user `fn`) owns an **ordered
  list of dispatch patterns**.
- A **pattern** is an ordered list of per-argument matchers (a `ValueType`, a
  compile-time literal-value guard, or `Any`), plus the existing optional/
  default slots.
- Resolution is **first match in declaration order**. Coercion is part of
  matching — a `Signal` matcher matches a `Number` argument by coercion. To
  prefer a more specific form, **declare it earlier**.
- **No match** (even allowing coercion) is a hard error that names the closest
  candidate signature.
- Operators desugar to internal builtin names (`op_mul`, `op_add`, …) and share
  the same table.
- User functions accumulate same-name definitions with distinct signatures as
  overloads; re-defining the *same* signature replaces it (live-coding
  hot-swap).

### Major design decisions

- **Arity is not a dispatch dimension.** Different forms are just patterns of
  different length; defaults fill optional slots within a chosen pattern.
- **Channel count (mono/stereo) is not a dispatch dimension.** Stereo
  auto-widening stays orthogonal (per the type-system non-goal "stereo is not a
  type"), applied *after* a pattern is chosen.
- **Literal-value matching is in scope.** A matcher may require a compile-time
  literal value (`String == "ms"`, `Number == 0`), folding `smooch`/`delay`
  string-dispatch into the table.
- **Coercion-or-not, first match wins.** No specificity ranking, no
  exact-then-coerce second pass — order is the author's tool.
- **The resolver subsumes `param_types`.** A single-pattern builtin's matchers
  *are* its parameter types, enforced on one code path for every builtin —
  eliminating the "decorative annotation" gap.

---

## 2. Problem Statement / Current State

| Concern | Today | Limitation |
|---|---|---|
| Builtin "overloads" | `optional_count` + `defaults[]`, or `if (args.size()==N)` in a handler | Three incompatible mechanisms; behavior-by-arity buried in C++ |
| Operator dispatch | Hardwired in codegen per operator | `*`/`+` can't be defined for Pattern/Array/Record uniformly |
| User-fn overloading | None — last `fn` wins | Can't write `fn f(x: Number)` and `fn f(x: Pattern)` |
| Type enforcement | `param_types` checked only in generic loop; inert on custom-handled builtins | `transport`/`midi`/visualizers carry decorative `param_types` |
| Literal/string dispatch | Per-handler `if (name=="ms")` ladders (`delay`, `smooch`) | Not declarative; not discoverable from the table |

Prior art that this mechanism generalizes and replaces:

- **Arity branching** — `handle_pan_call`/`handle_balance_call`
  (`codegen_stereo.cpp:165,361`) switch behavior on 1 vs 2 args; `pingpong`
  (`codegen_stereo.cpp:696`) has 3-arg and 5-arg forms.
- **Channel branching** — `pan` auto-widens a mono input to stereo.
- **String/mode dispatch** — `smooch("bank", …)` (`codegen_patterns.cpp:5973`,
  E198) and `delay`/`delay_ms`/`delay_smp` time-unit handling
  (`codegen.cpp:2201`).
- **The `: Type` annotation path** for user fns
  (`handle_user_function_call`, E184) already classifies argument
  `ValueType`s — this is the substrate user-fn overloading builds on.

---

## 3. Goals and Non-Goals

### Goals

1. One declarative dispatch model for builtins, operators, and user functions.
2. First-match, declaration-order resolution with coercion as part of matching.
3. Literal-value guards (`String == "ms"`).
4. A single enforcement path that makes `param_types` (matchers) live for
   *every* builtin, retiring the decorative-annotation gap.
5. Migrate **all** existing ad-hoc overload handlers to declarative patterns
   (channel auto-widening excepted — it stays orthogonal).
6. Clear no-match diagnostics that print the closest candidate signature.

### Non-Goals

- **Channel count as a type.** Stereo auto-widening remains a post-resolution
  step (carried over from `prd-compiler-type-system` and
  `prd-stereo-native-opcodes`).
- **Specificity ranking / SFINAE-style best-match.** Deliberately replaced by
  declaration order — simpler to reason about.
- **Runtime dispatch.** Resolution is entirely compile-time; the Cedar VM stays
  untyped (no runtime type tags).
- **Coercion redesign.** The `type_compatible` coercion matrix
  (`builtins.hpp:58`) is reused as-is; this PRD changes *dispatch*, not what
  coerces to what.

---

## 4. Target Syntax / User Experience

### 4.1 Builtin overloads (declarative)

```
// One name, an ordered pattern list. First match wins; declare the
// more specific form first so it isn't shadowed by a coercing one.
overload "delay" {
    (Signal, String == "ms",  Signal) -> DELAY_MS    // delay(sig, "ms", 250)
    (Signal, String == "smp", Signal) -> DELAY_SMP
    (Signal, Signal)                  -> DELAY        // beats (default)
}
```

### 4.2 Operators as named builtins

```
// `a * b` desugars to op_mul(a, b)
overload "op_mul" {
    (Signal, Signal)   -> MUL
    (Array,  Signal)   -> map-over-array * Signal     // [a,b,c] * g
    (Pattern, Number)  -> ... event-rate scale
}
```

### 4.3 User-function overloading

```
fn voice(f: Number)  -> sine(f)            // overload A
fn voice(p: Pattern) -> p |> poly(@, lead) // overload B  (distinct signature → accumulates)
fn voice(f: Number)  -> saw(f)             // SAME signature as A → replaces A (hot-swap)

voice(440)        // → overload A' (the saw redefinition)
n"c4 e4" |> voice(@)  // → overload B
```

### 4.4 No-match diagnostic

```
out("hello")
// E4xx: no overload of `out` matches (String).
//   candidates:
//     out(Signal)
//     out(Signal, Signal)
//   closest: out(Signal) — String is not coercible to Signal.
```

---

## 5. Architecture / Technical Design

### 5.1 The pattern model

```cpp
// A single argument matcher.
struct ArgMatcher {
    enum class Kind : uint8_t { Type, StringLiteral, NumberLiteral, Any };
    Kind        kind;
    ValueType   type;          // Kind::Type — matched via type_compatible()
    uint32_t    string_id;     // Kind::StringLiteral — interned literal
    float       number;        // Kind::NumberLiteral
};

// One overload form.
struct DispatchPattern {
    std::vector<ArgMatcher> params;   // ordered; trailing optionals allowed
    uint8_t  required_count;          // params before this index are required
    // Target: opcode + StateInit recipe, OR a NodeIndex (user-fn body),
    // OR a legacy handler id (migration bridge — see §8).
    DispatchTarget target;
};

// Registry: name → ordered pattern list.
using OverloadTable = std::unordered_map<std::string_view,
                                         std::vector<DispatchPattern>>;
```

### 5.2 Resolution algorithm (first match, coercion-aware)

```
resolve(name, arg_types):
    for pattern in table[name]:            # declaration order
        if matches(pattern, arg_types):    # coercion counts as a match
            return pattern
    error E4xx: no overload of `name` matches (arg_types)
                + closest candidate (fewest non-coercible slots)

matches(pattern, args):
    if args.count < pattern.required_count: return false
    if args.count > pattern.params.size(): return false
    for (matcher, arg) in zip(pattern.params, args):
        switch matcher.kind:
            Type:          if !type_compatible(arg.type, matcher.type): return false
            StringLiteral: if !(arg is String literal && arg.id == matcher.id): return false
            NumberLiteral: if !(arg is Number literal && arg.value == matcher.number): return false
            Any:           continue
    return true
```

`type_compatible` (`builtins.hpp:58`) already encodes coercion (Signal accepts
Number/Pattern, Record accepts Pattern, …). Because coercion is folded into
`matches`, **ordering is the only knob**: put the `Number` pattern before the
`Signal` pattern if you want a `Number` argument to bind to the former.

### 5.3 Closest-candidate selection

When no pattern matches, rank candidates by the count of slots that fail even
with coercion (then by arity distance) and print the top one in the diagnostic.

### 5.4 Operators

A desugaring pass rewrites operator nodes to calls of internal names. **All
operators are in scope for v1**: arithmetic (`op_mul`, `op_add`, `op_sub`,
`op_div`, `op_neg`, `op_mod`), comparison (`op_eq`, `op_ne`, `op_lt`, `op_le`,
`op_gt`, `op_ge`), and logical (`op_and`, `op_or`, `op_not`). These names live
in the same `OverloadTable`. Existing operator codegen becomes the body of the
corresponding default patterns.

### 5.5 User functions

- `handle_user_function_call` resolves against the user-fn overload list for the
  name using the same `matches`.
- **Signature identity = ordered param `ValueType` list *plus* literal-value
  guards.** `f(String == "ms")` and `f(String == "smp")` are distinct
  signatures. Defining a function whose full signature equals an existing one
  *replaces* it (the hot-swap / live-coding redefine path); any difference adds
  an overload.
- Inlining is unchanged: once an overload is selected, its body is inlined as
  today.

### 5.6 Relationship to `param_types` and custom handlers

- A builtin with a single pattern is exactly today's `param_types` — but now
  enforced on the *one* resolution path, so the annotation is never decorative.
- Custom handlers are migrated (§8). During migration a pattern's
  `DispatchTarget` may name a legacy handler so behavior is preserved while the
  declarative table is filled in.

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `type_compatible` coercion matrix | **Stays** | Reused verbatim as the matcher for `Kind::Type` |
| `defaults[]` / `optional_count` | **Stays** | Optional slots within a chosen pattern |
| Stereo auto-widening | **Stays** | Orthogonal post-resolution step |
| `BuiltinInfo` (single entry per name) | **Modified** | Gains a pattern list; `lookup_builtin` returns the set |
| `visit_call` type-check loop | **Modified** | Replaced by `resolve()`; one enforcement path |
| `param_types` (decorative on custom-handled builtins) | **Modified** | Becomes live matchers; no longer inert |
| `pan`/`balance`/`pingpong`/`sample`/`delay*`/`smooch` handlers | **Modified** | Migrated to patterns (channel branch excepted) |
| Operator codegen | **Modified** | Desugars to `op_*` builtins |
| `handle_user_function_call` | **Modified** | Resolves against accumulated overloads |
| Cedar VM / opcodes / bytecode | **Stays** | Pure compiler change |

---

## 7. File-Level Changes

| File | Change |
|---|---|
| `akkado/include/akkado/overload.hpp` | **New** — `ArgMatcher`, `DispatchPattern`, `OverloadTable`, `resolve()` |
| `akkado/src/overload.cpp` | **New** — resolution + closest-candidate logic |
| `akkado/include/akkado/builtins.hpp` | Builtins declare pattern lists; `lookup_builtin` exposes the set |
| `akkado/src/codegen.cpp` | `visit_call` calls `resolve()`; retire the generic `param_types` loop |
| `akkado/src/codegen_stereo.cpp` | Migrate `pan`/`balance`/`pingpong` to patterns; keep auto-widen |
| `akkado/src/codegen_patterns.cpp` | Migrate `smooch`/`delay*`/`sample` literal dispatch |
| `akkado/src/codegen_functions.cpp` | User-fn overload accumulation + resolution |
| `akkado/src/analyzer.cpp` | Operator → `op_*` desugaring pass |
| `akkado/src/diagnostics.cpp` | No-match diagnostic + closest-candidate formatting |
| `docs/prd-compiler-type-system.md` | Cross-link: deferred item now lives here |

---

## 8. Implementation Phases

### Phase 1 — Pattern model + resolver (no behavior change)
`overload.hpp/.cpp`; `resolve()`; wire `visit_call` to resolve single-pattern
builtins (each builtin gets a one-pattern list mirroring today's
`param_types`/defaults). Retire the generic `param_types` loop. **Verify:** full
`akkado_tests` green; `param_types` enforcement now uniform.

### Phase 2 — Operators as named builtins ✅ (2026-06-09)
Desugaring pass; `op_*` patterns; move operator codegen into pattern targets.
Covers **all operators** — arithmetic, comparison, and logical.
**Verify:** arithmetic/array/pattern/comparison/logical operator tests unchanged.

> **As-built:** no desugaring pass / `op_*` names were added — the parser already
> desugars operators to existing builtin names, which are reused as the
> `OverloadTable` keys (see the status block's Phase 2 reconciliation). Operator
> codegen moved into pattern targets via `DispatchTarget`: arithmetic →
> `LegacyHandler`/`BinaryOpBroadcast` (`overload.cpp` `lookup_operator_overloads`,
> consumed in `codegen.cpp` `visit_call`), comparison/logical → `Builtin` (generic
> path). Pure plumbing, zero behavior change; the full `akkado_tests` suite stays
> green with no operator test modified. New unit coverage in `test_overload.cpp`.
> Deferred to Phase 3: live `resolve()`-by-type in codegen (operators are
> single-pattern today, so dispatch reduces to the one target).

### Phase 3 — Migrate multi-form builtin handlers ✅ (2026-06-19)
`pan`/`balance`/`pingpong` (channel branch stays orthogonal), `delay*` +
`smooch`, `sample` (String vs Number id form).
**Verify:** each migrated family's existing tests pass; add a resolve()-by-type
test for the multi-form family.

> **As-built:** all six families migrated via `lookup_builtin_overloads`
> (`overload.cpp`) — see the status block's Phase 3 reconciliation.
> `pan`/`pingpong`/`smooch` route through single LegacyHandler patterns (channel/
> arity branch stays in the handler); `delay*` through single Builtin patterns
> (rate via `inst_rate`, redundant name-ladder retired); **`sample`/`sample_loop`
> drive the first live multi-pattern `resolve()` in codegen**, selecting the
> String-name vs Signal-id form by type and rejecting other id types with **E424**.
> `balance` is not a separate builtin (it is `pan`'s stereo branch). The §4.1
> `delay(sig,"ms",t)` literal-unit form was **descoped** by product decision, so
> the `StringLiteral`/`NumberLiteral` matchers stay resolver-only (no codegen
> consumer); `sample` dispatch is type-based. New `[overload]` coverage added;
> the full `akkado_tests` suite stays green with no existing test modified.

### Phase 4 — User-function overloading ✅ (2026-06-19)
Accumulate same-name defs by signature; first-match at call sites;
redefine-same-signature replaces. **Verify:** overload accumulation,
redefinition-replacement, selection, fallback, shadow, and shared-block
non-collision tests (`akkado/tests/test_overload.cpp`,
`akkado/tests/test_symbol_table.cpp`).

**Phase 4 as-built reconciliation.**
- **Storage.** `Symbol::user_function` (single) became
  `Symbol::overloads` (`std::vector<UserFunctionInfo>`), the single source of
  truth; `primary_overload()` returns the first overload for the
  single-body/fallback readers. `SymbolTable::define_function` now returns a
  `DefineFunctionResult` (`Added` / `Accumulated` / `ReplacedSameSignature`) and
  `update_function_nodes` remaps **every** overload's nodes.
- **Signature identity = arity + ordered param annotated-type list +
  per-param required-ness.** **Literal-value guards were dropped** (PRD §4.3 /
  §5.5 `f(String == "ms")`): the grammar only produces `: Type`
  (`parser.cpp`), so they are inexpressible — consistent with Phase 3 descoping
  the builtin literal-unit form. The `StringLiteral`/`NumberLiteral` matchers
  stay resolver-only.
- **No-match is a hard error, reusing `E424`** — the same "no overload
  matches" code the builtin multi-pattern path uses (`sample`/`sample_loop`).
  A type-classifiable call that matches no overload emits a single overload-aware
  `E424` naming the passed types and the candidate signatures (e.g. `tone() has
  no overload matching argument types (String); candidates: (Number),
  (Pattern)`); no fallback. This is safe to fire unconditionally on a codegen
  resolve-no-match: codegen only runs once the analyzer has accepted the call's
  arity (against ≥1 overload, else `E006`/`E007`), so anything reaching the
  resolve is a genuine type mismatch — no double diagnostic. **No new code was
  minted** and the earlier W-class `W170` was removed (it also collided with the
  reserved `W170` in `prd-patterns-as-scalar-values.md`).
- **Structurally-ambiguous calls behave like an ordinary call.** Named
  arguments, `_` partial application, spread, and the bare name used as a value
  can't be type-dispatched, so they route to the **first** overload and produce
  whatever a normal (non-overloaded) call would — no special warning, no special
  code. (Overloaded functions are not a special case here; the "use the first,
  else fail like any function" rule applies.)
- **Stdlib shadowing.** The stdlib/prelude is prepended to user source in the
  same global scope, so a user `fn osc`/`fn voice` shares a name with a stdlib
  definition. A **user-source** definition shadows the **whole** stdlib overload
  set for that name (the documented "user code can shadow these" idiom);
  same-origin definitions accumulate. Origin is tagged via the `<stdlib…>`
  source region (`UserFunctionInfo::is_stdlib`). Without this rule the existing
  "user shadows stdlib `osc`" and `unison`-passes-user-`voice` tests regress.
- **Polyphonic-pattern mirror.** `ArgDescriptor::polyphonic_scalar_incompatible`
  + a `matches_arg` guard make a polyphonic non-sample Pattern skip a
  `Type{Signal}`/`Any` overload (which binding would `E160`) so it reaches a
  `: Pattern` overload regardless of declaration order. Builtin/operator
  resolution never sets the bit, so their behavior is unchanged.
- **Hot-swap reframed.** Hot-swap is a fresh, atomic recompilation — there is no
  cross-compile symbol-table state, so "replace" is purely a within-one-source
  last-wins for same-signature defs. Determinism follows from deterministic
  first-match resolution + the existing emission-order state-id counters; the
  inline path gained **no** per-call-site path push (that would have changed
  every existing program's state IDs).
- **Shared blocks keyed by signature.** `shared_block_key()` appends a signature
  suffix only when a name is overloaded, so each overload owns its own
  `BLOCK_CALL` body; single-definition fns keep byte-identical bytecode.

### Phase 5 (Future) — Migrate heavy pattern/higher-order handlers
`poly`/`each`/`transport`/`midi` carry large bespoke codegen; their patterns
*select the handler* rather than inlining a target. Lowest priority — these are
already type-guarded by dedicated diagnostics (E133, E403, E423).

---

## 9. Edge Cases

1. **Coercing pattern shadows an exact one.** `(Signal)` declared before
   `(Number)` → a `Number` arg binds to `Signal` (coerced). Documented
   behavior: order defines priority; the resolver does not reorder. A **W-class
   shadowing warning is emitted on by default** when an earlier pattern makes a
   later one unreachable (it always matches everything the later one would),
   per the live-coding "warn, don't fail" philosophy.
2. **No pattern matches even with coercion.** Hard error + closest candidate
   (§5.3). Example: `out("x")`.
3. **Ambiguous user-fn redefinition.** Same param types but different literal
   guards → distinct overloads (both kept). Identical types *and* guards →
   replacement (hot-swap).
4. **Literal guard against a non-literal arg.** `delay(sig, unitVar, t)` where
   `unitVar` is a runtime value → the `String == "ms"` matcher fails (not a
   literal); falls through to the next pattern or errors. Literal guards never
   match non-literals.
5. **Optional/default slots within a pattern.** A pattern of length 4 with
   `required_count == 2` matches calls of arity 2–4; missing trailing slots take
   `defaults[]`.
6. **Operator on incompatible operands.** `n"c4" * (x) -> x` (Pattern * Function)
   → no `op_mul` pattern matches → no-match error.
7. **Hot-swap with literal-guarded overloads.** Each guarded overload inlines a
   distinct body → distinct semantic-id path; state preservation matches per
   overload, unaffected.

---

## 10. Testing / Verification Strategy

- **Resolver unit tests** (`test_overload.cpp`, new): `matches()` truth table
  across `Type`/`StringLiteral`/`NumberLiteral`/`Any`; first-match ordering;
  coercion-as-match; closest-candidate selection.
- **No-match diagnostics:** `out("x")` → E4xx naming `out(Signal)`; assert the
  closest-candidate line.
- **Order-sensitivity:** a builtin with `(Number)` before `(Signal)` binds a
  `Number` literal to the `Number` form; swapping the order binds it to
  `Signal`.
- **Operator parity:** every existing arithmetic/array/pattern operator test
  must pass unchanged after desugaring (regression guard for Phase 2).
- **Per-migrated-builtin:** a literal-match test (`delay(sig,"ms",250)` vs
  `delay(sig,"smp",128)` vs `delay(sig,250)`); `pan` mono vs stereo; `pingpong`
  3-arg vs 5-arg.
- **User-fn overloading:** accumulation (two signatures coexist), redefinition
  replacement (same signature), call-site selection, and a hot-swap determinism
  test with overloaded names.
- **`param_types` uniformity:** the previously-decorative annotations
  (`transport` `{Pattern}`, `midi` `{Record}`) now reject mismatches through the
  resolver — a mismatch test for each, closing the gap noted in
  `prd-compiler-type-system`.

```bash
cmake --build build --target akkado_tests
./build/akkado/tests/akkado_tests "[overload]"
./build/akkado/tests/akkado_tests            # full suite stays green
```
