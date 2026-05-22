# PRD: Akkado Parameter Type Annotations (DRAFT / STUB)

> **Status: DRAFT — STUB.** Captured 2026-05-22 while implementing
> `prd-runtime-event-transforms.md` Phase 2. Not yet fleshed out; this file
> records the problem, the chosen direction, and the open questions so the
> work is not lost. Flesh out via the `prd-create` skill before implementation.

## Problem

Akkado patterns are compiler "magic", not first-class typed values. A pattern
passed as a user-`fn` parameter is silently coerced to a voice-0 scalar signal,
and the eager `E160` guard *rejects* polyphonic patterns passed to a user fn
outright (`akkado/src/codegen_functions.cpp` param binding, ~line 546 / ~691).

This blocks userspace (and stdlib) functions that want to **transform** an
event stream rather than sample it — e.g. the stdlib modifier one-liners that
`prd-runtime-event-transforms.md` Phase 2 depends on:

```akkado
fn transpose(events, n) = event_map(events, (e) -> {note: e.note + n})
```

Here `events` must stay a full `Pattern` / `EventSource`, not collapse to a
signal. There is no way to express that today.

## Key distinction

A pattern argument has two genuinely different uses, indistinguishable at the
call site, so the intent must be declared on the **definition** side:

- **As a signal** — sampled over time, voice-0 scalar coercion. The common
  case (`fn wobble(rate) = …` fed `n"100 200 300"`). Must not change.
- **As an event stream** — the fn transforms the events themselves
  (`event_map`, `transpose`). Rare, special, must be opt-in.

## Chosen direction (locked in discussion 2026-05-22)

Introduce **`param: type` annotation syntax** on `fn` definitions — a general
mechanism, seeded with one concrete type now and extensible later.

```akkado
fn transpose(events: stream, n) =
    event_map(events, (e) -> {note: e.note + n})
```

- Initial concrete type: **`stream`** — preserves a `Pattern` / `EventSource`
  typed value across the parameter boundary instead of coercing to voice-0.
  (Name `stream`, not `pattern`, so it also covers MIDI `EventSource` — matches
  `event_map`'s accepted set and the event-transforms PRD's Phase-B
  `EventStreamPayload` unification.)
- Un-annotated params keep **exactly** today's behavior — `E160` still applies
  to them. Annotated `stream` params bypass the eager `E160` guard. → near-zero
  regression risk for existing code.
- Designed so `signal`, `number`, `string`, `record` etc. can be added later
  without grammar churn.

## Scope

- Parser: `param: type` grammar (interaction with `= default`, destructure,
  rest params, closures).
- Analyzer: validate annotations; type-check arguments against them.
- Codegen: `handle_user_function_call` param binding preserves the
  `Pattern` / `EventSource` typed value for `stream`-annotated params (mirror
  the existing `DynArray` branch); skip the eager `E160` for them.
- Enables userspace-defined event-stream modifiers, not just the stdlib.

## Relationship to prd-runtime-event-transforms.md Phase 2

This PRD is a **hard prerequisite for the tail of Phase 2 only**:

- *Independent of this PRD* — the `event_map` / `event_filter` closure-taking
  **builtins** and the Cedar `EVENT_MAP` / `EVENT_FILTER` opcode closure rework.
  Builtins validate their own args, so no annotation is needed. (Implemented
  first, alongside this stub.)
- *Blocked on this PRD* — migrating property modifiers into
  `akkado/stdlib/event_transforms.ak` and deleting the C++ handlers; those need
  `events: stream`.

## Open questions

- How far to take the type set beyond `stream` in the first shipped version.
- Exact grammar — `name: type`, interaction with defaults (`name: type = x`).
- `E160` rework details; whether un-annotated polyphonic-pattern args should
  warn rather than error.
- Whether the annotation feeds a broader type-checking pass or stays advisory.
- Inference: should an un-annotated param used only as `event_map`'s first arg
  be inferred as `stream`? (Probably no — explicit is better.)
