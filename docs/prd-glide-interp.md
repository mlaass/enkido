> **Status: SHIPPED** — Drafted 2026-05-15. Phases 1–3 landed 2026-05-15
> / 2026-05-16. See `web/static/docs/concepts/glide-and-interpolation.md`
> for the public-facing concept page and the `glide` / `interp*` sections
> in `web/static/docs/reference/builtins/utility.md`.

# PRD: Time-Based Glide / Interpolation (`glide` / `interp`)

## Executive Summary

Add a **time-based interpolation primitive** to Akkado, so that users can
write musical portamento between pattern events with a single function:

```akkado
n"c4 c5" |> saw(glide(@freq, 0.1)) |> out(@)
```

The existing `slew(target, rate)` is a rate-limited follower (units per
second). It is the wrong tool for note glide because the Hz delta between
adjacent notes varies with the interval — a "100 Hz/sec" slew makes a
small interval feel instant and an octave feel sluggish. Musicians want
**100 ms regardless of interval**, which is what `glide` provides.

The design splits cleanly across the two layers:

- **Cedar opcode `INTERP_TIME`** — one new opcode with change-detection
  and per-channel state. Curve shape is selected at compile time via
  `inst.rate` (4 values: linear, ease_in, ease_out, cosine).
- **Akkado stdlib `glide(sig, time, curve, space)`** — userspace wrapper
  in `akkado/include/akkado/stdlib.hpp` that adds value-space conversion
  (linear vs log/pitch) and routes the curve string to the right opcode
  variant via compile-time `match()`.

**Key design decisions** (resolved during the question rounds):

- **Time-based ramp on target change**, not rate-based. Constant ramp
  duration regardless of interval size.
- **Auto-detect** target change via exact float compare. Sample-and-hold
  pattern fields (`@freq` etc.) feed in cleanly; users feeding audio-rate
  noise into glide should `sah` or `slew` first.
- **Two independent curve dimensions**: time-shape curve (4 values in v1)
  applied to the 0..1 progress, and value-shape space (linear / log)
  applied as an outer log/exp wrapper in userspace.
- **Time default = `0.05` (50 ms)** — `glide(@freq)` adds a subtle
  baseline portamento without forcing users to pick a number.
- **Both `glide` and `interp` are user-facing**. `glide` is the
  recommended high-level entry point; `interp` is the documented primitive
  for users who want fewer layers (no value-space, fewer characters).
- **Stays compatible** with `slew()` — no changes to the existing opcode.
  Docs add a "when to use which" comparison table.
- **Three-phase implementation**: opcode kernel → akkado layer → docs.

---

## 1. Current State

### 1.1 What exists today

Two primitives in Cedar deal with smoothing a control signal toward a
target:

| Op            | Location                                            | Mode                         | Time param shape       |
|---------------|-----------------------------------------------------|------------------------------|------------------------|
| `SLEW`        | `cedar/include/cedar/opcodes/utility.hpp:166-209`   | Rate-limited linear follower | units / second         |
| `ENV_FOLLOWER`| `cedar/include/cedar/opcodes/envelopes.hpp:158-199` | Asymmetric AR exponential    | attack / release sec   |
| (`LP` filter) | `cedar/include/cedar/opcodes/filters.hpp:45-72`     | One-pole-like at low cutoff  | implicit via frequency |

`SLEW` is the closest existing match but is rate-based. To make
`slew(@freq, ?)` give a 100 ms glide between c4 (261.6 Hz) and c5
(523.3 Hz), the user must compute `rate = (523.3 - 261.6) / 0.1 ≈
2617 Hz/s` — and that number is wrong for every other interval. There is
no way to say "100 ms, period."

### 1.2 The gap

Pattern fields like `@freq` arrive as **sample-and-hold buffers** from
`SEQPAT_FIELD` (`cedar/include/cedar/opcodes/sequencing.hpp:769-826`).
They jump step-wise from one note's value to the next. The missing
primitive is one that:

1. Detects the jump,
2. Captures the old value as a ramp start,
3. Ramps to the new value over a configurable **time**, and
4. Holds the target value once the ramp finishes.

### 1.3 `slew` vs `glide` (proposed)

| Aspect                  | `slew(target, rate)`                | `glide(sig, time, curve, space)` (new)        |
|-------------------------|-------------------------------------|-----------------------------------------------|
| Time meaning            | Rate of change (units per second)   | Total ramp duration (seconds)                 |
| Interval-independence   | No — Hz/s constant across intervals | Yes — same time for every interval            |
| Shape                   | Linear only                         | Linear / ease_in / ease_out / cosine          |
| Value-space             | Linear-only                         | Linear or log (musical pitch)                 |
| Best for                | CV smoothing, knob lag, audio slew  | Note glide, target value transitions          |
| State                   | `SlewState`                         | `InterpTimeState` (new)                       |
| Existing                | ✓ Ship                              | ✗ Missing                                     |

Both stay shipped after this PRD. Docs cross-reference them.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Add one Cedar opcode `INTERP_TIME` with stereo-native per-channel
   state and 4 curve modes selected via `inst.rate`.
2. Expose four Akkado builtins (`interp`, `interp_ease_in`,
   `interp_ease_out`, `interp_cos`) that share the opcode with different
   `inst_rate` values.
3. Ship a userspace stdlib function
   `glide(sig, time=0.05, curve="linear", space="linear")` that adds
   value-space conversion on top of `interp`.
4. Make the canonical example work end-to-end:
   `n"c4 c5" |> saw(mtof(glide(@.note, 0.1))) |> out(@)`. (See §3.4 on
   stereo-native composition — the bare `saw(glide(@.freq, …))` form
   collides with `saw`'s mono input slot and needs `mono(...)`.)
5. Document the distinction from `slew()` so users reach for the right
   tool.
6. Cover the kernel with experiment tests (timing accuracy, curve shapes,
   stereo independence, edge cases) and ship one akkado integration test
   that exercises the full pattern-glide pipeline.

### 2.2 Non-Goals (deferred to future work)

- **Beat-relative time units** (e.g. `glide(@freq, 0.25b)` = quarter
  beat). Requires hooking into the tempo system. v1 is seconds-only.
- **Explicit trigger input** for re-articulated identical notes. v1 uses
  value-compare auto-detection. A future `glide(sig, time, trig=…)` opt
  can be added when there is a concrete user request.
- **Per-event curve override via mini-notation syntax.** Already doable
  by attaching custom fields to events (e.g. `n"c4 c5" |> pat |> map(e
  -> { ...e, glide_time: 0.2 })`) and reading them back with
  `glide(@freq, @glide_time)`. The language already supports this.
- **Additional curve shapes** (exponential approach, smoothstep, cubic
  bezier, user-defined LUT). Out of scope; if added later, may require
  migrating curve selection from `inst.rate` to ExtendedParams.
- **Mini-notation slur / tie syntax** for marking which notes should
  glide. Future PRD.
- **Changes to `slew()`** — its rate-based mode is still useful for
  audio-rate slewing and CV smoothing.

---

## 3. Target Syntax & User Experience

### 3.1 Canonical examples

`glide` is stereo-native (see §3.4); examples lead with `mtof(glide(@.note, …))`
so the chain stays mono into `saw`. The `mono(glide(@.freq, …))` form is the
explicit alternative when only a frequency signal is available.

```akkado
// Default: 50 ms linear glide via MIDI note
n"c4 c5" |> saw(mtof(glide(@.note))) |> out(@)

// Explicit 100 ms glide
n"c4 c5" |> saw(mtof(glide(@.note, 0.1))) |> out(@)

// Cosine S-curve
n"c4 c5" |> saw(mtof(glide(@.note, 0.1, "cosine"))) |> out(@)

// Musical wide-interval portamento — feed @.freq through log-space glide
// and downmix back to mono for saw
n"c2 c6" |> saw(mono(glide(@.freq, 0.2, "ease_out", "log"))) |> out(@)

// Glide a parameter slider for smoother knob response
cutoff = param("cutoff", 1000, 100, 8000)
osc("saw", 220) |> lp(@, mono(glide(cutoff, 0.03))) |> out(@)

// Glide a velocity for smoother accents (velocity is a scalar multiplier,
// so the stereo result is fine downstream of the saw)
n"c4 c4 c4 c4" |> saw(@.freq) * glide(@.vel, 0.02) |> out(@)
```

### 3.2 Two paths to musical glide

Linear interpolation in **Hz** sounds non-uniform over wide intervals — a
c4→c5 glide rushes through low frequencies and slows at the top. There
are two ways to get pitch-perceptual (log) glide; both are documented:

**Path A — `space: "log"` opt on `glide`:**

```akkado
n"c4 c5" |> saw(glide(@freq, 0.1, "linear", "log")) |> out(@)
```

Internally pipes through `log + scale` and `pow(2, …)`. Use this when you
only have a frequency signal (e.g. from a non-pattern source).

**Path B — glide the MIDI note and convert with `mtof`:**

```akkado
n"c4 c5" |> saw(mtof(glide(@note, 0.1))) |> out(@)
```

`@note` carries the MIDI note number (linear in semitones), so a plain
linear glide is already log-pitch. More transparent; recommended when the
source is a pattern with `@note` available.

Both produce identical audio.

### 3.3 When to use `slew` vs `glide`

| Use case                           | Reach for     |
|------------------------------------|---------------|
| Note pitch / portamento            | `glide`       |
| Velocity / pattern field smoothing | `glide`       |
| Param slider response (knob lag)   | `glide`       |
| Audio-rate signal smoothing        | `slew` or LP  |
| Limiting how fast a CV can change  | `slew`        |
| Anti-click ramp on amp envelopes   | `slew` or LP  |

Rule of thumb: **if you can answer "how many seconds should the slide
take?" reach for `glide`. If you can only answer "how fast can it change
per second?" reach for `slew`.**

### 3.4 Channel-width handling

`glide`, every `interp*` builtin, `slew`, `env_follower`, and the
EDGE_OP family (`sah`/`gateup`/`gatedown`/`counter`) all declare
`output_channels = ChannelCount::Match`. The result follows the primary
signal input: mono in → mono out, stereo in → independent per-channel
processing. That means `saw(glide(@.freq, 0.1))` is a direct fit — the
mono pattern field stays mono into the mono `freq` slot. Feeding a
stereo signal (e.g. `glide(stereo(saw(218), saw(222)), …)`) still
yields per-channel ramps.

Earlier drafts of this PRD shipped these builtins as `ChannelCount::Stereo`
and recommended `mtof(glide(@.note, …))` or `mono(glide(…))` as
workarounds for an `E186` collision. Those workarounds are no longer
required — both forms still work and remain reasonable style choices,
but the bare form composes cleanly today.

---

## 4. Architecture / Technical Design

### 4.1 Cedar opcode `INTERP_TIME`

One new opcode in the Utility block:

```cpp
// cedar/include/cedar/vm/instruction.hpp
INPUT       = 58,
INTERP_TIME = 59,   // Time-based interpolator with change detection.
                    // rate: 0=linear, 1=ease_in, 2=ease_out, 3=cosine
                    // in0=target (stereo-capable), in1=time (seconds)
```

**Why one opcode with `inst.rate` carrying the curve_id**, not four
separate opcodes:

- The per-sample body is 95 % identical across curves — only the
  `t → shaped_t` transform differs. One opcode = one place to fix bugs.
- Adds 1 opcode-enum value, not 4. Smaller VM dispatch table.
- Curve selection at the call site is compile-time anyway (selected via
  `match` in the stdlib), so there is no runtime perf cost — the switch
  on `inst.rate` lives outside the per-sample loop.
- Fits CLAUDE.md's "`inst.rate` is reserved for small fixed enum modes
  ≤ 4 values" guideline. Exactly 4 curves in v1.

### 4.2 Akkado builtin layer

Four sister builtins sharing the opcode (analogous to the `edge_op`
family `sah` / `gateup` / `gatedown` / `counter`):

```cpp
// akkado/include/akkado/builtins.hpp
{"interp",          {Opcode::INTERP_TIME, 2, 0, true,
                     {"target","time"}, {NAN, NAN},
                     "Time-based interpolator (linear).",
                     0, {}, {}, ChannelCount::Stereo, true, /*inst_rate=*/0}},
{"interp_ease_in",  {Opcode::INTERP_TIME, 2, 0, true, ... /*inst_rate=*/1}},
{"interp_ease_out", {Opcode::INTERP_TIME, 2, 0, true, ... /*inst_rate=*/2}},
{"interp_cos",      {Opcode::INTERP_TIME, 2, 0, true, ... /*inst_rate=*/3}},
```

Each is independently callable (and documented; see §3 — they are
exposed as the "primitive" form).

### 4.3 Userspace stdlib wrapper

In `akkado/include/akkado/stdlib.hpp`, two new fns:

```akkado
// glide(sig, time, curve, space) — time-based interpolation with
// optional curve shape and value-space conversion.
//
// curve: "linear" (default) | "ease_in" | "ease_out" | "cosine"
// space: "linear" (default) | "log"  — use "log" for musical pitch glide
//
// Default time = 0.05 (50 ms) so bare glide(@freq) adds a subtle
// baseline portamento without forcing users to pick a number.
fn glide(sig, time = 0.05, curve = "linear", space = "linear") -> match(space) {
    "log": pow(2, _interp_dispatch(log(sig) * 1.4426950408889634, time, curve))
    _:     _interp_dispatch(sig, time, curve)
}

// Internal: route a curve string to the right interp* builtin.
// `match` resolves at compile time because `curve` is bound to a string
// literal at every call site.
fn _interp_dispatch(sig, time, curve) -> match(curve) {
    "linear":   interp(sig, time)
    "ease_in":  interp_ease_in(sig, time)
    "ease_out": interp_ease_out(sig, time)
    "cosine":   interp_cos(sig, time)
    "cos":      interp_cos(sig, time)
    _:          interp(sig, time)
}
```

Notes:

- `1.4426950408889634 = 1/ln(2)` converts natural log to log₂. No `log2`
  builtin is needed.
- Flat positional args (not a record) because `match(opts.curve)` on a
  FieldAccess scrutinee is not currently a compile-time match — only
  plain identifiers are, per `is_compile_time_match`
  (`akkado/src/codegen_functions.cpp:1213-1299`).

### 4.4 State struct

```cpp
// cedar/include/cedar/opcodes/dsp_state.hpp (near SlewState)
struct InterpTimeState {
    float start[2]    = {0.0f, 0.0f};   // per-channel ramp start
    float end[2]      = {0.0f, 0.0f};   // per-channel current target
    float progress[2] = {0.0f, 0.0f};   // samples elapsed in current ramp
    float total[2]    = {0.0f, 0.0f};   // total samples for current ramp
    bool  initialized = false;          // first-block guard
};
```

Per-channel ramps so stereo targets (rare today, possible tomorrow) work
without code change. Mono targets auto-broadcast `target_r = target_l`.

### 4.5 Per-sample kernel (sketch)

For each sample `i` and channel `ch`:

```
target = inputs[0][ch][i]
t_dur  = inputs[1][i]

if not finite(target):
    out = state.end[ch]                       // hold last good
elif t_dur <= 0:
    state.start = state.end = target          // passthrough
    state.progress = state.total = 0
    out = target
else:
    if target != state.end[ch]:
        // retarget: capture current output as new start
        current = (state.progress >= state.total)
                  ? state.end
                  : state.start + shape(state.progress/state.total)
                                  * (state.end - state.start)
        state.start[ch]    = current
        state.end[ch]      = target
        state.progress[ch] = 0
        state.total[ch]    = t_dur * sample_rate

    if state.progress >= state.total:
        out = state.end[ch]
    else:
        u = state.progress / state.total
        out = state.start + shape(u) * (state.end - state.start)
        state.progress += 1
```

`shape(t)` is selected by `inst.rate` via a switch that **wraps** the
per-sample loop (mirrors `op_edge`'s structure), so the dispatch happens
once per block, not once per sample.

### 4.6 Curve set

| `inst.rate` | Name       | Shape `t → shaped_t`             |
|-------------|------------|----------------------------------|
| 0           | linear     | `t`                              |
| 1           | ease_in    | `t * t`                          |
| 2           | ease_out   | `1 - (1-t) * (1-t)`              |
| 3           | cosine     | `0.5 * (1 - cos(π * t))`         |

Four shapes cover the 80 % case (linear baseline, paired ease-in /
ease-out, smooth cosine S). Additional shapes deferred (§2.2).

---

## 5. Edge Cases

| Case                              | Behavior                                              | Rationale                                                          |
|-----------------------------------|-------------------------------------------------------|--------------------------------------------------------------------|
| `time = 0`                        | Passthrough (`out = target`, state sync)              | "No glide"                                                         |
| `time < 0`                        | Same as `time = 0`                                    | Defensive                                                          |
| `time = NaN` / `inf`              | Same as `time = 0`                                    | Defensive                                                          |
| First sample of first block       | `out = target`, ramp marked done                      | Avoids spurious 0→target ramp at program start                     |
| Target = NaN / inf                | Hold last good `end`, no retarget                     | Preserves audio integrity                                          |
| Target change mid-ramp            | New ramp from current emitted value to new target     | Smooth re-anchor — no value jump                                   |
| Repeated identical notes          | No retarget — nothing to glide to                     | Auto-detect by value compare; documented limitation                |
| Mono target → stereo output       | Both lanes follow same target → identical L=R         | `target_r = target_l` fallback in opcode                           |
| Stereo target → stereo output     | Per-channel independent ramps                         | Free correctness from per-channel state                            |
| Audio-rate noise as target        | Constant retargeting; output ≈ heavy-LP-filtered noise | Misuse; user should `sah` or `slew` first. Documented            |
| Hot-swap (program update)         | State preserved if state-id matches                   | Same as all other stateful opcodes                                 |
| `glide(@freq, time)` mono-pipe    | Works — mono target, mono output                      | Channel inference handles this                                     |
| `glide(@freq)` (bare)             | 50 ms default, linear, linear-Hz                      | Sensible baseline                                                  |

Floating-point **exact comparison** is intentional. The primary expected
input is `SEQPAT_FIELD`'s sample-and-hold output, which emits step-wise
constant buffers — exact compare fires once per transition, zero
retarget noise. Audio-rate noise is a misuse case; documenting it is
cheaper than swallowing the cost of an epsilon compare for every user.

---

## 6. Impact Assessment

| Component                                       | Status        | Notes                                          |
|-------------------------------------------------|---------------|------------------------------------------------|
| `SLEW` opcode                                   | **Stays**     | No changes; coexists with new opcode           |
| `ENV_FOLLOWER`, `LP` filter                     | **Stays**     | Unrelated                                      |
| `SEQPAT_FIELD`, pattern compilation             | **Stays**     | Produces step-wise targets `glide` consumes    |
| `inst.rate` field semantics                     | **Stays**     | New opcode uses it within the documented guideline |
| Opcode-enum sequence                            | **Modified**  | `INTERP_TIME = 59` added in Utility block       |
| `DSPState` variant                              | **Modified**  | New `InterpTimeState` added                    |
| `vm.cpp` dispatch                               | **Modified**  | One new `case`                                 |
| `BUILTIN_FUNCTIONS` table                       | **Modified**  | 4 entries added                                |
| `STDLIB_SOURCE`                                 | **Modified**  | `glide` + `_interp_dispatch` added             |
| `opcode_metadata.hpp` (generated)               | **Modified**  | Regenerated by `bun run build:opcodes`         |
| Web docs (`builtins/utility.md`)                | **Modified**  | New `glide` and `interp*` sections             |
| `experiments/test_op_*.py`                      | **New file**  | `test_op_interp_time.py`                       |
| `experiments/test_glide_pattern.py`             | **New file**  | End-to-end akkado integration test             |

No backward-incompatible changes. All additions; nothing is removed or
renamed.

---

## 7. File-Level Changes

| File                                                       | Change                                                                       |
|------------------------------------------------------------|------------------------------------------------------------------------------|
| `cedar/include/cedar/vm/instruction.hpp`                   | Add `INTERP_TIME = 59` to the Opcode enum (Utility block), update comment    |
| `cedar/include/cedar/opcodes/dsp_state.hpp`                | Add `InterpTimeState` struct + entry in `DSPState` variant                   |
| `cedar/include/cedar/opcodes/utility.hpp`                  | Add `op_interp_time(ctx, inst)` implementation after `SLEW`                  |
| `cedar/src/vm/vm.cpp`                                      | Add `case Opcode::INTERP_TIME: op_interp_time(ctx_, inst); break;`           |
| `cedar/include/cedar/generated/opcode_metadata.hpp`        | Regenerated — **do not hand-edit** (`cd web && bun run build:opcodes`)        |
| `akkado/include/akkado/builtins.hpp`                       | Add four `interp*` `BuiltinInfo` entries, sharing `Opcode::INTERP_TIME`      |
| `akkado/include/akkado/stdlib.hpp`                         | Add `glide` and `_interp_dispatch` fns to `STDLIB_SOURCE`                    |
| `experiments/test_op_interp_time.py`                       | New — kernel test (timing, curves, stereo, edge cases)                       |
| `experiments/test_glide_pattern.py`                        | New — end-to-end integration test for `n"c4 c5" \|> saw(glide(@freq, 0.1))`  |
| `web/static/docs/reference/builtins/utility.md`            | Add `glide` and `interp*` sections; add slew-vs-glide comparison table       |
| `web/static/docs/concepts/glide-and-interpolation.md`      | New (optional) — concept page covering both paths to musical glide           |

---

## 8. Implementation Phases

### 8.1 Phase 1 — Cedar opcode kernel

Goal: ship `INTERP_TIME` opcode and validate it independently of any
akkado plumbing.

Files in scope:
- `cedar/include/cedar/vm/instruction.hpp`
- `cedar/include/cedar/opcodes/dsp_state.hpp`
- `cedar/include/cedar/opcodes/utility.hpp`
- `cedar/src/vm/vm.cpp`
- `cedar/include/cedar/generated/opcode_metadata.hpp` (regen)
- `experiments/test_op_interp_time.py`

Verification:
1. `cmake --build build` succeeds; `bun run build:opcodes` regenerates
   metadata cleanly.
2. `python experiments/test_op_interp_time.py` — all sub-cases pass
   (§9.1).
3. `python experiments/test_op_slew.py` — regression check (we touched
   adjacent state-struct code).
4. **Listen audibly**: hand-build a Python program that pipes a square
   pulse target into `INTERP_TIME` with `time=0.1`, render to WAV. Verify
   no clicks, smooth slope, correct duration.

### 8.2 Phase 2 — Akkado layer

Goal: surface the opcode in akkado and validate the stdlib wrapper.

Files in scope:
- `akkado/include/akkado/builtins.hpp`
- `akkado/include/akkado/stdlib.hpp`
- `experiments/test_glide_pattern.py`

Verification:
1. `cmake --build build --target akkado` succeeds.
2. Smallest akkado test: `glide(440, 0.1) |> sine(%) |> out(%)` compiles
   and runs (no E105/E140 codegen errors).
3. Confirm compile-time `match` resolution for the chained
   `glide → _interp_dispatch → interp_*` dispatch. If it doesn't resolve
   at compile time (string literal doesn't propagate through two user-fn
   boundaries via `param_literals_`), **inline the curve `match` into
   `glide` directly** (drop `_interp_dispatch`). See §10 risk #1.
4. Canonical example renders correctly:
   `n"c4 c5" |> saw(glide(@freq, 0.1)) |> out(@)`.
5. Log-space portamento sounds musical for wide intervals:
   `n"c2 c6" |> saw(glide(@freq, 0.5, "linear", "log")) |> out(@)` vs
   `…, "linear", "linear")` — log version should sound uniform; linear
   should sound bottom-heavy.
6. `experiments/test_glide_pattern.py` passes (§9.2).

### 8.3 Phase 3 — Docs

Goal: make the feature discoverable and the slew vs glide distinction
obvious.

Files in scope:
- `web/static/docs/reference/builtins/utility.md` — add `glide`,
  `interp`, `interp_ease_in`, `interp_ease_out`, `interp_cos` sections
  with concrete examples; add slew-vs-glide comparison table.
- `web/static/docs/concepts/glide-and-interpolation.md` (optional) —
  concept page covering the two paths to musical glide.

Verification:
1. Run `bun run build:docs` to rebuild the F1 lookup index.
2. `bun run check` — type check passes.
3. Spot-check F1 lookup for "glide", "portamento", "interp", "slew" — all
   resolve to sensible docs.

---

## 9. Testing & Verification

### 9.1 `test_op_interp_time.py` (kernel test)

Follow `test_op_slew.py` shape. Each sub-test writes a row to
`output/op_interp_time/timing.json` and a panel to `timing.png`.

| # | Sub-test                  | Setup                                                  | Assertion                                                                |
|---|---------------------------|--------------------------------------------------------|--------------------------------------------------------------------------|
| 1 | Linear ramp duration      | Step 0→1, `time=0.1s`                                  | Time-to-target = 4800 samples ± 1                                        |
| 2 | Hold at target            | Continue from #1 for 1000 samples                      | All samples == 1.0 (exact)                                               |
| 3 | Mid-ramp retarget         | Step 0→1 `time=0.2s`, then 1→0 at sample 4800          | New ramp starts at ≈ 0.5 monotonically decreasing                        |
| 4 | Linear curve shape        | `inst.rate=0`                                          | `out[i] ≈ start + (i/total)·(end-start)` to 1e-5                          |
| 5 | ease_in curve             | `inst.rate=1`                                          | matches `start + t²·(end-start)`                                          |
| 6 | ease_out curve            | `inst.rate=2`                                          | matches `start + (1-(1-t)²)·(end-start)`                                  |
| 7 | cosine curve              | `inst.rate=3`                                          | matches `start + ½(1-cos πt)·(end-start)`                                 |
| 8 | Stereo independence       | L-target changes at 100, R-target at 5000              | Lanes ramp independently                                                  |
| 9 | `time = 0` passthrough    | Square-wave target, `time=0`                           | `out == target` sample-exact                                              |
| 10| NaN target hold           | Step 0→1 (settles), then NaN for 100 samples, then 2  | Output holds at 1 during NaN, ramps to 2 after                            |

All sub-tests **simulate ≥ 300 s of audio** per the project's testing
convention (CLAUDE.md). The canonical "100 ms ramp" event is repeated in
a loop for the duration of the test.

### 9.2 `test_glide_pattern.py` (akkado integration test)

Compiles and renders `n"c4 c5" |> saw(glide(@freq, 0.1)) |> out(@)` for
≥ 300 s at 120 bpm. FFT every 50 ms; plot dominant frequency over time.

Assertions:
- During each held note, the dominant frequency stabilizes at the target
  within 100 ms ± 5 ms.
- Frequency trajectory between notes is monotonic (no overshoot).
- For `space: "log"`, the half-time of a c4→c5 transition has dominant
  frequency at ≈ 370 Hz (G4 = log midpoint), **not** ≈ 395 Hz (linear-Hz
  midpoint).

### 9.3 Audible checks (cannot be automated)

- `n"c4 c5" |> saw(glide(@freq, 0.1)) |> out(@)` — smooth 100 ms slide,
  no clicks at transitions.
- `n"c4 c5" |> saw(glide(@freq, 0.5, "cosine")) |> out(@)` — slow,
  obvious "S"-shaped pitch sweep.
- `n"c2 c6"` with `"log"` vs `"linear"` value-space — log feels uniform
  across the 4-octave glide; linear feels bottom-heavy.
- `cutoff = param("cutoff", …); … |> lp(%, glide(cutoff, 0.03))` —
  dragging the cutoff slider produces smoothed, click-free response.

---

## 10. Risks / Open Questions

1. **Compile-time `match()` resolution through a chained user-fn call.**
   The proposed wrapper is `glide → _interp_dispatch → interp_*`. The
   curve string is bound to `_interp_dispatch`'s `curve` parameter via
   `glide`'s argument list. Need to verify that
   `is_compile_time_match` (in `akkado/src/codegen_functions.cpp:1213-
   1299`) walks back through two user-fn boundaries to find the string
   literal — `unison()` doesn't exercise this path.
   **Pre-implementation check**: write a 5-line akkado program
   (`fn a(x) -> match(x) { "y": 1.0, _: 0.0 }`, `fn b(x) -> a(x)`,
   `b("y") |> out(%)`) and confirm it compiles to a constant. If not,
   inline the `match` into `glide`'s body directly and drop
   `_interp_dispatch`.

2. **Python binding `inst.rate` mutability.** The kernel test must set
   `inst.rate = N` to exercise non-linear curves. Verify that
   `experiments/test_op_edge.py` (which already does this) uses an
   exposed setter, and that the same mechanism works from
   `cedar_core.Instruction`.

3. **`STEREO_INPUT` flag on mono targets.** The opcode falls back to
   `target_r = target_l` when `STEREO_INPUT` is unset. Verify the akkado
   codegen path correctly produces a mono instruction when fed a mono
   `@freq`. Should Just Work via existing channel inference, but worth a
   trace check during Phase 2.

4. **State-pool collisions across the four sister builtins.** Because
   they share `Opcode::INTERP_TIME`, two glide calls at different source
   locations must produce different state-ids. The state-id is derived
   from the semantic path (which includes the builtin name), so this
   should be safe — verify by inspecting `compute_state_id` traces.

5. **Hot-swap behavior.** A user editing curve from `"linear"` to
   `"cosine"` mid-program changes which builtin name codegen emits,
   which changes the semantic-path hash, which means the new opcode gets
   a fresh `InterpTimeState`. The old state is GC'd. Result: the running
   ramp resets to the new target abruptly. Acceptable for v1 (matches
   how other stateful opcode-family swaps behave) — flag in docs.

6. **Curve set growth path.** If users request a 5th curve, the
   `inst.rate ≤ 4 values` guideline forces a migration to ExtendedParams
   (or a second sister opcode). Documented as a future-work item;
   should not block v1.

---

## 11. References

- Existing PRD format: `docs/prd-hole-field-shorthand.md`,
  `docs/prd-closure-pipe-operator.md`.
- `inst.rate` guideline: `CLAUDE.md` → "Extended Parameter Patterns".
- `SLEW` opcode (reference for stereo state + channel handling):
  `cedar/include/cedar/opcodes/utility.hpp:166-209`.
- `edge_op` family (reference for sharing one opcode across multiple
  builtin names via `inst_rate`): `akkado/include/akkado/builtins.hpp`
  near `sah` / `gateup` / `gatedown` / `counter`.
- Stdlib pattern (reference for userspace `match`-based dispatch):
  `osc(type, freq, …)` in `akkado/include/akkado/stdlib.hpp`.
- Pattern field materialization: `SEQPAT_FIELD` in
  `cedar/include/cedar/opcodes/sequencing.hpp:769-826`.
- Field aliases (note → MIDI etc.): `pattern_field_aliases()` in
  `akkado/src/typed_value.cpp:30-49`.
