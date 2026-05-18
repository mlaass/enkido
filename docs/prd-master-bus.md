> **Status: NOT STARTED** — Drafted 2026-05-18.

# Master Bus PRD

## Executive Summary

Today, every `out(...)` call in an Akkado program sums its signal directly
into the audio device's stereo output (`op_output` at
`cedar/include/cedar/opcodes/utility.hpp:42-58`). There is no shared
processing stage between the per-voice/per-track outputs and the device:
no global limiter, no soft clipper, no master fader, no NaN guard beyond
the per-sample sanitize in `op_output`.

This PRD adds a **master bus**: a final stereo processing stage that
runs once per block on the summed output of all `out()` calls. It is:

- **Always present** with a default safe chain (polynomial soft clip at 0.9 + safety rail).
- **User-overridable** via a top-level `master(closure)` call that swaps the tone chain.
- **Closure-driven** — the user supplies normal Akkado code (`(s) -> ...` or `(l, r) -> ...`) so any existing opcode is fair game.
- **Compiler-rewritten** — no VM changes; `out()` retargets to a pre-master buffer, and the compiler appends an epilogue that runs the master chain and the safety stage.

Goals: make global compression / soft-clipping a one-liner, prevent
amplitude blow-ups by default, keep hot-swap and live-coding ergonomics.

---

## 1. Motivation

### 1.1 Current behavior

```cpp
// cedar/include/cedar/opcodes/utility.hpp (op_output)
ctx.output_left[i]  += sanitize(l);
ctx.output_right[i] += sanitize(r);
```

Each `out()` is an isolated sink. Two consequences:

1. **No global FX point.** Adding a master limiter today means manually
   wiring every track through a shared bus by hand, which the language
   has no first-class shortcut for.
2. **No global headroom guard.** The per-sample NaN/Inf check inside
   `op_output` rejects bad samples but does nothing to bound the
   summed amplitude — a four-voice synth easily clips the device.

### 1.2 What live-coders want

The common reach is "wrap everything in a soft clipper / glue
compressor and forget it." That should be one line:

```akkado
master((s) -> s |> comp(@, -12, 4) |> softclip(@, 0.8))
```

…and the default behavior (no `master(...)` call) should still be safe.

---

## 2. Surface API

### 2.1 The `master` builtin

```akkado
master(closure)
```

The closure runs once per output block on the **summed pre-master
stereo bus**. Two arities are accepted:

```akkado
// Stereo form: closure receives a stereo signal value
master((s) -> s |> comp(@, -12, 4) |> softclip(@, 0.85))

// Mono-pair form: closure receives left + right separately
master((l, r) -> stereo(softclip(l, 0.9), softclip(r, 0.9)))
```

Arity is inferred from the closure's parameter list. The mono-pair form
must return a stereo value (typically via `stereo(L, R)`). If a mono
result is returned from the stereo form, it is auto-broadcast L = R
with a **W-warning** (mirrors `out(mono)` behavior).

### 2.2 Where it can appear

`master(...)` may appear anywhere a top-level statement can appear:
program root, imported modules, even inside a function body that the
program reaches at compile time. **If multiple `master(...)` calls are
present, the last call in topological-emission order wins**, and a
W-warning is emitted listing the overridden site(s). This matches the
runtime semantics of "the chain that actually runs" and avoids
silent-merge surprises.

### 2.3 Disabling the tone chain

There is **no special `none` token**. To disable the default soft
clipper without supplying your own, the user writes the identity
closure:

```akkado
master((s) -> s)   // no tone processing — only the forced safety stage runs
```

This keeps the surface uniform: `master` always takes a closure.

> **[OPEN QUESTION]** Should `master()` (zero-arg) be accepted as
> syntactic sugar for the identity closure above? The original feature
> brief mentioned `master(none)`, but Round 2 of clarification chose
> "identity closure" as the canonical form. Recommendation: ship v1
> with only the identity-closure form; revisit sugar later if users
> ask. Marked as decided unless feedback says otherwise.

### 2.4 Capturing top-level bindings

The closure captures top-level scope under normal closure rules. This
is the recommended way to expose master-bus tweakables to the UI:

```akkado
master_drive = param("master_drive", 0.85, 0.1, 1.0)

master((s) -> s |> softclip(@, master_drive))
```

---

## 3. Default Chain

When no `master(...)` call appears in the program, the compiler emits a
default tone chain followed by the forced safety stage:

```text
pre_master_L, pre_master_R
   │
   ▼
[default tone]  softclip(threshold = 0.9, polynomial)
   │
   ▼
[forced safety]  NaN/Inf guard + hard rail at ±1.0
   │
   ▼
device_L, device_R
```

- **Algorithm**: `distort_softclip` (polynomial soft-clipper, existing
  opcode in `cedar/include/cedar/opcodes/distortion.hpp`).
- **Threshold**: fixed at **0.9**, not exposed as a parameter. Users
  who want a tunable threshold supply their own `master(...)` closure.
- **Stereo**: applied independently per channel.

### 3.1 Forced safety stage

Regardless of whether the user overrode the tone chain, a fixed safety
stage runs **after** the user/default chain:

1. **NaN/Inf sanitize** — non-finite samples are replaced with 0
   (matches today's `op_output` guard).
2. **Hard rail at ±1.0** — `clamp(x, -1.0, 1.0)`. Not a soft clipper;
   this is the "do not damage speakers" final guarantee.

The user cannot disable this stage. It exists so that an aggressive
master closure (`(s) -> s * 100`) cannot destroy the listener's
output device.

> **[OPEN QUESTION]** Implementation detail — does the safety stage
> live as its own opcode (`op_output_master`, replacing `op_output`'s
> final stage) or as inlined IR appended after the user chain? Either
> is acceptable; the proposal below picks the inlined-IR route to
> avoid a new opcode, but a dedicated opcode would be slightly less
> code and let the disassembler label it cleanly. Recommend: inline.

---

## 4. Compilation Model

The master bus is implemented as a **compiler rewrite**. No new VM
opcode is required for the core flow, and no runtime dispatch happens.

### 4.1 Pre-master buffer

The compiler allocates a stereo buffer pair (call it `bus_master_L`,
`bus_master_R`) at program-load time. Every `OUTPUT` instruction
emitted from `out(...)` codegen is retargeted: instead of writing to
the device's `ctx.output_left/right`, it writes (sums) into
`bus_master_L/R`.

This is the **only** change required to the `out()` codegen path
(`akkado/src/codegen.cpp` around lines 1380-1500). The existing
mono/stereo branches remain unchanged; only the destination buffer
changes.

### 4.2 Epilogue

After the program's main DAG is topologically scheduled, the compiler
appends a fixed **epilogue segment**:

```text
[load]   bus_master_L, bus_master_R
[run]    user master closure body (or default softclip if none)
[guard]  NaN/Inf sanitize → clamp ±1.0
[store]  ctx.output_left, ctx.output_right   (device sinks)
```

The closure body is inlined into the epilogue at compile time, exactly
as if it were typed in source at the end of the program. State_ids
inside the closure follow the normal hot-swap rules — no reserved
`/__master/` path — so editing the closure rebinds matching opcode
state under the standard mechanism.

### 4.3 Programs with no `out()` calls

The epilogue is **always emitted**, even for programs that produce no
audio. In that case `bus_master_L/R` are simply zero buffers and the
master chain processes silence. Cost is negligible (softclip(0) = 0,
no allocation), and the uniform behavior simplifies the compiler — no
"is there at least one out()?" branch.

### 4.4 Multiple `master(...)` calls

The compiler scans for all `master(...)` invocations during AST walk.
The last one (in program order, with the standard "last bound wins"
rule applied across modules) is the one whose closure body is inlined
into the epilogue. Earlier calls are dropped; each dropped call
produces a **W-warning** listing the source location and the
overriding location.

---

## 5. Diagnostics

Two new warning codes are needed. Numbers to be allocated from the
next free `W###` slot — placeholder names below:

| Code | Trigger |
|------|---------|
| `W_MASTER_OVERRIDDEN` | More than one `master(...)` call in the program. Lists overridden sites + winning site. |
| `W_MASTER_MONO_RETURN` | Stereo-form master closure (`(s) -> ...`) returns a mono value. Auto-broadcasts L = R. |

> **[OPEN QUESTION]** Exact W-numbers and the family they belong to
> (codegen vs. type-check). Assign during implementation; suggest
> the next two consecutive slots after the most recent existing
> warning so the table stays sequential.

---

## 6. Examples

### 6.1 Default — nothing to do

```akkado
osc("saw", 220) |> out(@)
// implicit: master((s) -> softclip(s, 0.9))
// + forced NaN/clamp safety
```

### 6.2 Glue compressor + soft clip

```akkado
master((s) -> s
    |> comp(@, -12, 4)
    |> softclip(@, 0.85)
)

pat("c4 e4 g4") as e
    |> osc("saw", e.freq)
    |> @ * e.vel
    |> out(@)
```

### 6.3 Live-tweakable drive

```akkado
drive = param("master_drive", 0.85, 0.5, 1.0)
ceiling = param("master_ceiling", -1.0, -6.0, 0.0)  // dB

master((s) -> s
    |> softclip(@, drive)
    |> limiter(@, db_to_amp(ceiling))
)
```

### 6.4 Per-channel processing

```akkado
master((l, r) -> stereo(
    l |> softclip(@, 0.9),
    r |> softclip(@, 0.9)
))
```

### 6.5 Disable the default tone chain

```akkado
master((s) -> s)   // forced safety stage still runs
```

---

## 7. File-Level Changes

| File | Change |
|------|--------|
| `akkado/include/akkado/builtins.hpp` | Add `master` builtin entry (signature: closure, return: void; declared `top_level_only` if such a flag exists, otherwise documented as such). |
| `akkado/src/codegen.cpp` (~1380-1500, `out()` branch) | Retarget `OUTPUT` instructions to write into `bus_master_L/R` instead of `ctx.output_left/right`. |
| `akkado/src/codegen.cpp` (new section) | Detect `master(...)` calls during AST walk; store winning closure; emit `W_MASTER_OVERRIDDEN` for losers. Append epilogue segment (load pre-master bus → user/default chain → safety stage → store to device). |
| `cedar/include/cedar/opcodes/utility.hpp` | No change for the core flow. (`op_output`'s sanitize is preserved as the safety stage's NaN guard, applied as inlined IR in the epilogue rather than a new opcode.) |
| `cedar/include/cedar/opcodes/distortion.hpp` | No change. Default chain reuses existing `distort_softclip`. |
| `cedar/include/cedar/vm/context.hpp` | No change. `output_left/right` continue to be the device sinks; the master bus uses ordinary scratch buffers from the existing arena. |
| `docs/prd-master-bus.md` | **NEW** — this PRD. |
| `web/static/docs/concepts/master-bus.md` | Out of scope for v1; add when the feature ships. |

---

## 8. Hot-Swap Behavior

The master closure is **just code** from the hot-swap system's point of
view. Opcode state-ids inside the closure follow the standard semantic
ID path rule (e.g. an `lp(@, 1000)` inside the master closure gets a
state-id derived from its position in the closure body). On code
update:

- Edits inside the closure body rebind state under the normal hot-swap
  rules (matching state-ids preserved, structural changes
  micro-crossfaded).
- Swapping the *entire* `master(...)` call for a different one is
  treated as any other structural change — short crossfade per the
  global hot-swap policy.
- Removing `master(...)` reverts to the default tone chain. Adding it
  replaces the default. Either is structurally a change to the
  epilogue and gets the standard rebind treatment.

No reserved `/__master/` state-id path. No special handling.

---

## 9. Non-Goals (v1)

- **No per-track buses.** This PRD specifies one global master sink.
  Sub-buses / groups are a separate feature.
- **No multi-band / multi-stage routing primitives.** The user expresses
  multi-stage chains by composing existing opcodes inside the closure.
- **No bypass UI.** The closure form is the bypass mechanism; there
  is no runtime "master on/off" toggle baked into the language. Users
  who want one wire it themselves: `master((s) -> if(on, processed, s))`.
- **No metering of pre/post master amplitude.** Existing visualization
  builtins can be inserted into the closure if needed.
- **No threshold parameter on the default chain.** Fixed at 0.9. If
  the default is wrong for a project, write a `master(...)` line.

---

## 10. Acceptance Criteria

- [ ] Programs with no `master(...)` call run the default polynomial
      softclip at 0.9 + safety stage; existing test programs continue
      to produce audible output unchanged in character below 0.9
      amplitude.
- [ ] `master((s) -> s)` produces output identical to the program with
      no master tone processing (only the forced ±1.0 rail and NaN
      guard).
- [ ] `master((s) -> s |> softclip(@, 0.5))` produces measurably more
      aggressive soft-clipping than the default.
- [ ] Closures of both arities (`(s) -> ...` and `(l, r) -> ...`)
      compile and produce expected stereo output.
- [ ] Multiple `master(...)` calls: only the last takes effect;
      `W_MASTER_OVERRIDDEN` is reported with both source locations.
- [ ] Stereo-form closure returning mono: `W_MASTER_MONO_RETURN`
      reported; output is L = R.
- [ ] An aggressive closure (`(s) -> s * 100`) is clamped to ±1.0 by
      the safety stage; output device receives no |sample| > 1.0.
- [ ] Hot-swap: editing inside the master closure preserves stateful
      opcode state under the standard hot-swap rules.
- [ ] A `param(...)` captured by the closure is live-tweakable from
      the web UI and updates the master chain without recompile.

---

## 11. Resolution of Open Questions

The three `[OPEN QUESTION]` markers above are minor implementation
details that should be settled during implementation, not before. The
proposed defaults are:

1. **`master()` zero-arg sugar**: do NOT add. Only `master(closure)`.
2. **Safety stage shape**: inline IR (no new opcode).
3. **Warning code numbers**: take the next two consecutive `W###`
   slots after the most recent existing warning.

If any of these turn out to matter to a stakeholder, revise before
implementation lands.
