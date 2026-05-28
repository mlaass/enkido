> **Status: NOT STARTED** — Multi-segment envelope (`zenv`) is not implemented. This PRD specifies the design.

# PRD: Multi-Segment Envelope (`zenv`) — Zebra-Style Envelope Generator

## Executive Summary

Add a **multi-segment envelope generator** to Akkado as the builtin `zenv` (named in homage to u-he Zebra, which popularized the style). `zenv` is gate-driven, supports N control points with per-point levels, per-segment durations, and per-segment curvature, and includes a sustain loop region that holds (or loops between two indices) while the gate is open and runs through any release tail on gate-off.

This is a new Cedar opcode (`ENV_ZENV`) backed by a new state struct (`ZenvState`). The existing `adsr`, `ar`, and `env_follower` envelopes stay shipped — `zenv` is additive, not a replacement. The two structural primitives `adsr` is built from (fixed 4-stage shape, runtime-tunable a/d/s/r) and `zenv` (compile-time N-stage shape, runtime-tunable rate scale) cover different musical needs.

**Key design decisions** (resolved during the question rounds):

- **New opcode, coexists with `adsr` / `ar`.** Existing patches are untouched; `zenv` is reached for explicitly.
- **Parallel-array surface** as the v1 user surface. Array-of-records and mini-notation surfaces are deferred (see §2.2).
- **Per-segment curvature is a continuous bend (`-1..+1`) by default**, with named shape strings (e.g. `"exp"`, `"sine"`, `"cubic_in"`) as overrides. The `curves` array is mixed-type: each element is either a float or a string. Mixed-type arrays already work in Akkado's typed-value layer; a per-builtin validator enforces the allowed element types.
- **Sustain loop region** expressed as `loop: [start, end]`. While the gate is held, the envelope loops between those two breakpoints; on gate-off, it runs through any remaining points (release tail). Degenerate `loop: [n, n]` (start == end) holds a single point — classic ADSR-style sustain.
- **Continuous bend formula is exponential**: `u(t) = (exp(k*t) - 1) / (exp(k) - 1)` with `k = bend * BEND_SCALE`. Coefficients are cached per segment.
- **Named shape set is extended**: `linear`, `exp`, `log`, `sine`, `cubic`, `cubic_in`, `cubic_out`, `quartic`, `quartic_in`, `quartic_out`, `square`, `hold`.
- **Optional runtime `rate` multiplier** scales segment progression at audio/control rate, clamped to `[0, 64]`. Wired via `ExtendedParams<1>`.
- **Hot-swap policy: fresh state on structural change.** Different point count or loop layout produces a new semantic-ID hash, gets a fresh `ZenvState`. Same point structure with different runtime `rate` preserves state. Matches every other stateful opcode family.
- **Three-phase delivery**: (1) Cedar opcode kernel + Python experiment, (2) Akkado surface + docs + demo patches, (3) draggable web-UI editor.

---

## 1. Current State

### 1.1 What exists today

Cedar ships three envelope opcodes, all in `cedar/include/cedar/opcodes/envelopes.hpp`:

| Op             | File / Lines                                       | Stages           | Curvature                            | Gate model                  |
|----------------|----------------------------------------------------|------------------|--------------------------------------|-----------------------------|
| `ENV_ADSR`     | `envelopes.hpp:16-143`                             | Fixed 4 (A/D/S/R)| Hardcoded `1 - exp(-4.6/N)` (~99 % in time) | Gate edge-triggered, sustain-holding |
| `ENV_AR`       | `envelopes.hpp:205-279`                            | Fixed 2 (A/R)    | Same hardcoded exponential           | Trig-triggered one-shot     |
| `ENV_FOLLOWER` | `envelopes.hpp:145-203`                            | N/A (peak detect)| Per-channel attack/release coeffs    | Audio amplitude detector    |

Cedar also ships `TIMELINE` (`cedar/include/cedar/opcodes/sequencing.hpp:243-302`) — a 64-breakpoint automation primitive — but it is **clock-driven (beats)**, not gate-driven, and exposes only 3 hardcoded curve modes (linear, quadratic-ease-in, hold). It is the wrong abstraction for a per-voice envelope.

### 1.2 The gap

Modern soft-synths (u-he Zebra, NI Massive, Bitwig MSEG, Ableton Wavetable) expose an envelope built from N user-defined control points where each segment carries both a target level and a curvature parameter, with an optional sustain loop region. This shape:

1. **Is gate-driven** (starts on gate-on, holds in the sustain region until gate-off, then runs the release tail).
2. **Has per-segment curvature** as a continuous knob (concave → linear → convex), not a hardcoded exponential.
3. **Can hold or loop in a sustain region**, then run through release segments.
4. **Has a tunable shape per patch**, not a fixed stage count.

None of `adsr`, `ar`, `env_follower`, or `TIMELINE` matches all four properties. Building this in Akkado userspace from existing primitives is not feasible — gate edge detection, multi-stage state machines, and per-segment curvature kernels are missing from the userspace toolkit.

### 1.3 `adsr` vs `zenv` (proposed)

| Aspect                  | `adsr(gate, a, d, s, r)`                  | `zenv(gate, levels, times, curves, loop, rate)`  |
|-------------------------|-------------------------------------------|--------------------------------------------------|
| Stage count             | Fixed 4                                    | Variable, up to 32                               |
| Curvature               | Hardcoded `1 - exp(-4.6/N)`               | Per-segment continuous bend + named shapes       |
| Levels                  | Implicit 0 → 1 → sustain → 0              | Explicit per-breakpoint, unrestricted floats     |
| Sustain                 | Single-point hold (decay → sustain → release) | Loop region `[start, end]`; degenerate case = single hold |
| Time parameters         | Per-stage audio-rate buffers              | Compile-time per-segment + optional global `rate` multiplier |
| Best for                | 95 % of patches; smallest CPU             | Complex MSEG shapes, custom curves, sustain loops |
| State                   | `EnvState` (shared with `ar`)             | `ZenvState` (new)                                |
| Existing                | ✓ Ship                                    | ✗ Missing                                        |

Both stay shipped after this PRD. Docs cross-reference them.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Add one Cedar opcode `ENV_ZENV` with `ZenvState`, supporting up to 32 control points, 12 named shape codes plus continuous bend, and a sustain loop region.
2. Expose one Akkado builtin `zenv(gate, levels: …, times: …, curves: …, loop: …, rate: …)` with parallel-array arguments.
3. Compile-time validate the array shapes and emit `E150`–`E153` diagnostics for mismatched lengths, out-of-bounds loop indices, bad curve elements, and overflow beyond 32 points.
4. Add a small reusable helper `validate_array_elements()` in `akkado/include/akkado/codegen/options.hpp` for per-builtin array typing; used by `zenv` first, available to future array-consuming builtins.
5. Document `zenv` in `web/static/docs/reference/builtins/envelopes.md` with concrete examples covering ADSR-equivalent, MSEG, and looped-sustain shapes.
6. Ship two demo patches (`zenv-pluck.akk` and `zenv-mseg-pad.akk`) under `web/static/patches/`.
7. Cover the kernel with Python experiments (timing, per-shape correctness, gate edge cases, loop behavior, 300 s long-run stability) and add an `[zenv]` Akkado codegen test case.
8. Phase 3: ship a **draggable web-UI editor** that round-trips with the Akkado source — see §8.3 for scope.

### 2.2 Non-Goals (deferred to future work)

- **Array-of-records surface** (`zenv(gate, [{l:0, t:0}, {l:1, t:0.01, c:1, sustain:true}, ...])`). Designed for, not implemented in v1. Add as a sibling codegen path once the parallel-array form is shipped and tested.
- **Mini-notation surface** (`zenv(gate, "0 1:0.01:^ ...")`). Pure ergonomic alternative; design after we see how the array form feels in real patches.
- **Per-segment `rate` scale**. v1 only has a global multiplier; per-segment scaling is YAGNI until proven needed.
- **`MAX_POINTS > 32`**. 32 covers commercial-synth MSEG editors; lift only if a real patch hits the wall.
- **Audio-rate or runtime-tunable `levels` / `times`**. Structural data is compile-time fixed; modulation flows through `rate`.
- **Backwards-playable envelopes** (negative `rate`). Clamped to `[0, 64]` in v1.
- **Custom user-defined shape functions**. Named shape set is fixed; users wanting other shapes use the continuous bend or compose in userspace.
- **Per-event override via pattern fields**. `zenv` is a control-rate primitive; if you want per-note envelope variation, gate it from `@.gate` and modulate `rate` from a pattern field.
- **Replace `adsr` / `ar`**. They stay as the ergonomic primitives most patches actually use.

---

## 3. Target Syntax

### 3.1 Minimal usage

```akkado
// ADSR-equivalent via zenv: attack 10 ms, decay 100 ms, sustain 0.5, release 300 ms
sine(440) * zenv(button("g"),
    levels: [0, 1.0, 0.5, 0],
    times:  [0.01, 0.1, 0.3],
    curves: ["exp", "log", "log"],
    loop:   [2, 2]) |> out(@)
```

### 3.2 Mixed continuous and named curves

```akkado
zenv(gate,
    levels: [0, 1.0, 0.7, 0.9, 0],
    times:  [0.01, 0.2, 0.2, 0.5],
    curves: [1.0, "exp", -0.5, "sine"],
    loop:   [1, 3])
```

- `1.0` → maximally convex bend (steep at start, easy at end).
- `"exp"` → preset convex bend (`bend = +0.7` internally).
- `-0.5` → mildly concave bend.
- `"sine"` → smoothstep / half-cosine S-curve.

While gate is held, the envelope loops `levels[1] → levels[2] → levels[3] → levels[1] → ...`. On gate-off, it runs `levels[3] → levels[4]` (= 0) as the release tail.

### 3.3 Single-point sustain (ADSR-style)

```akkado
// loop: [2, 2] holds levels[2] until gate-off, then runs through points 3..end
zenv(gate, levels: [0, 1.0, 0.6, 0], times: [0.01, 0.1, 0.3], curves: ["exp", "log", "log"], loop: [2, 2])
```

### 3.4 One-shot envelope (no sustain loop)

```akkado
// loop omitted → envelope plays through all points and stops at the last level
zenv(trig, levels: [0, 1.0, 0.5, 0.3, 0], times: [0.005, 0.05, 0.1, 0.5], curves: ["exp", -0.3, -0.3, -0.7])
```

### 3.5 Procedural envelope construction

Because `levels`, `times`, and `curves` are real Akkado arrays, they compose with `linspace`, `map`, and arithmetic:

```akkado
// Smoothly stepped 8-point ramp
zenv(gate,
    levels: linspace(0, 1, 8),
    times:  [0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02],
    curves: [0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5],
    loop:   [7, 7])
```

### 3.6 Runtime rate modulation

```akkado
speed = param("envSpeed", 1.0, 0.1, 8.0)
zenv(gate,
    levels: [0, 1, 0.6, 0],
    times:  [0.01, 0.1, 0.3],
    curves: ["exp", "log", "log"],
    loop:   [2, 2],
    rate:   speed)
```

`rate` is a control-rate input (default `1.0`) clamped to `[0, 64]`. NaN / inf are treated as `1.0`. Doubling `rate` halves the perceived attack/decay/release time without recompiling.

### 3.7 Channel handling

`zenv` outputs a **mono control signal** (`ChannelCount::Mono`) — envelopes are control-domain, like `adsr` and `ar`. To apply it to a stereo signal, multiply broadcast (`stereo_sig * zenv(...)` works because `*` already broadcasts mono → stereo).

---

## 4. Architecture / Technical Design

### 4.1 Cedar opcode `ENV_ZENV`

Add to `cedar/include/cedar/opcodes/envelopes.hpp` alongside `ENV_ADSR` / `ENV_AR` / `ENV_FOLLOWER`. New opcode-enum value in `cedar/include/cedar/vm/instruction.hpp` (next free slot in the Envelopes block).

```cpp
// Inputs:
//   inputs[0]: gate signal (>0 = on, edge-triggered)
//   inputs[1..4]: unused (structural data lives in ZenvState)
// Extended params:
//   ExtendedParams<1>: { rate }
// State:
//   ZenvState (state_id), populated by codegen via StateInitData
```

Curve dispatch happens **once per block** outside the per-sample loop (mirroring `op_interp_time`'s structure). Per-sample work: read gate, advance `time_in_seg`, compute `u`, apply shape, blend `start + u*(end-start)`.

### 4.2 Akkado builtin

Add to `akkado/include/akkado/builtins.hpp`:

```cpp
{"zenv", {.opcode = cedar::Opcode::ENV_ZENV,
          .input_count = 1, .optional_count = 0,
          .requires_state = true,
          .param_names = {"gate", "", "", "", "", ""},
          .defaults = {NAN},
          .description = "Multi-segment Zebra-style envelope with per-point curvature and sustain loop",
          .extended_param_count = 1,
          .extended_param_names = {"rate"},
          .extended_defaults = {1.0f}}}
```

Because the structural arrays (`levels`, `times`, `curves`, `loop`) are compile-time inputs that don't fit the standard 5-input-slot mechanism, `zenv` gets a **custom codegen handler**. Routed via `codegen.cpp`'s builtin dispatch table to `handle_zenv_call()` (new function in `akkado/src/codegen_envelope.cpp`).

### 4.3 State struct

Add to `cedar/include/cedar/opcodes/dsp_state.hpp` after `TimelineState`:

```cpp
struct ZenvState {
    static constexpr std::size_t MAX_POINTS = 32;

    struct Point {
        float           time      = 0.0f;   // segment duration in seconds (point i → i+1)
        float           level     = 0.0f;   // target level at this point
        float           bend      = 0.0f;   // continuous bend, in [-1, +1], used when shape == 0
        std::uint8_t    shape     = 0;      // 0=bend, 1..N=named (see §4.5)
        // Cached per-segment, recomputed when bend changes:
        float           cached_k     = 0.0f;
        float           cached_denom = 1.0f;
        float           last_bend    = 99.0f; // sentinel forces first compute
    };

    Point          points[MAX_POINTS] = {};
    std::uint32_t  num_points         = 0;

    // Sustain loop (has_loop=false → one-shot)
    bool           has_loop      = false;
    std::uint8_t   loop_start    = 0;
    std::uint8_t   loop_end      = 0;

    // Runtime state
    float          level          = 0.0f;   // current output
    std::uint32_t  current_seg    = 0;      // segment in flight (0..num_points-2)
    float          time_in_seg    = 0.0f;   // seconds elapsed in current_seg
    float          seg_start      = 0.0f;   // captured start level for current_seg
    float          prev_gate      = 0.0f;
    bool           gate_held      = false;
    bool           releasing      = false;  // true after gate-off when loop was active
};
```

Memory footprint per voice: `32 × (4+4+4+1+4+4+4 + pad) ≈ 800 bytes` + scalar runtime = ~830 bytes/voice. Comfortable for `MAX_DSP_ID = 4096`.

### 4.4 Bend formula

For each segment, `u(t)` maps normalized progress `t ∈ [0,1]` to a shaped progress `u ∈ [0,1]`, then output is `start + u * (end - start)`:

```
let k = bend * BEND_SCALE                        // BEND_SCALE = 5 (proposed; tunable)
if |bend| < 1e-4:
    u = t                                         // linear fallback (avoids divide-by-near-zero)
else:
    cached_denom = 1 / (exp(k) - 1)               // cached per segment
    u = (exp(k * t) - 1) * cached_denom
```

`BEND_SCALE` controls how aggressive the maximum bend looks; `5` is a starting proposal (matches the Zebra range subjectively). Tunable post-experiment.

Recompute `cached_k = exp(k)` and `cached_denom` only when `bend` changes (matches `ENV_ADSR`'s coefficient-cache pattern at `envelopes.hpp:16-143`).

### 4.5 Named shape table

Shape codes used in `ZenvState::Point::shape`. Names map at **codegen time**:

| `shape` | Name(s)             | u(t) formula                                          | Notes                          |
|---------|---------------------|-------------------------------------------------------|--------------------------------|
| 0       | (continuous bend)   | exponential bend (§4.4)                               | Default; uses `bend` field     |
| —       | `"linear"`          | t                                                     | Codegen collapses to `shape=0, bend=0` |
| —       | `"exp"`             | exponential bend                                      | Codegen collapses to `shape=0, bend=+0.7` |
| —       | `"log"`             | exponential bend                                      | Codegen collapses to `shape=0, bend=-0.7` |
| 1       | `"sine"`            | `0.5 * (1 - cos(π * t))`                              | Smoothstep / half-cosine S     |
| 2       | `"cubic"`           | `3t² - 2t³`                                           | Hermite smoothstep             |
| 3       | `"cubic_in"`        | `t³`                                                  | Ease-in cubic                  |
| 4       | `"cubic_out"`       | `1 - (1-t)³`                                          | Ease-out cubic                 |
| 5       | `"quartic"`         | `6t⁵ - 15t⁴ + 10t³`                                   | Perlin smootherstep            |
| 6       | `"quartic_in"`      | `t⁴`                                                  | Ease-in quartic                |
| 7       | `"quartic_out"`     | `1 - (1-t)⁴`                                          | Ease-out quartic               |
| 8       | `"square"`          | `t < 0.5 ? 0 : 1`                                     | Step at midpoint               |
| 9       | `"hold"`            | `t < 1 ? 0 : 1`                                       | Hold start until segment end   |

Total runtime dispatch: 10 cases (shape 0 + shapes 1..9). Bend-preset names (`linear`, `exp`, `log`) collapse to `shape=0` at codegen so they cost nothing at runtime.

### 4.6 Per-sample kernel (sketch)

For each sample `i` (curve switch dispatched once per block):

```
gate = inputs[0][i]
rate = clamp(ext_rate(i, default=1.0), 0, 64)
if not finite(rate): rate = 1.0

// Gate edge detection (matches ENV_ADSR pattern)
gate_on  = (prev_gate <= 0) && (gate > 0)
gate_off = (prev_gate >  0) && (gate <= 0)
prev_gate = gate

if gate_on:
    // Retrigger: attack from current level into segment 0
    seg_start    = level
    current_seg  = 0
    time_in_seg  = 0
    gate_held    = true
    releasing    = false

if gate_off && has_loop && gate_held:
    // Begin release tail: jump out of loop to first post-loop segment
    if current_seg < loop_end:
        // Inside loop, complete current segment first then continue normally
        // (handled by the normal advance path)
    else:
        // Already past loop_end (rare); continue from current segment
    gate_held = false
    releasing = true

// Advance time
dt = rate / sample_rate
time_in_seg += dt
seg_dur = points[current_seg + 1].time

while time_in_seg >= seg_dur && current_seg < num_points - 1:
    time_in_seg -= seg_dur
    seg_start    = points[current_seg + 1].level
    current_seg += 1
    // Loop logic
    if has_loop && gate_held && current_seg > loop_end:
        current_seg = loop_start
        seg_start   = points[loop_start].level
    seg_dur = (current_seg < num_points - 1) ? points[current_seg + 1].time : 0
    if seg_dur == 0:
        break  // reached the end

// Compute output
if current_seg >= num_points - 1:
    level = points[num_points - 1].level   // settled at last point
else:
    t = time_in_seg / seg_dur
    u = shape_apply(t, points[current_seg + 1].shape, points[current_seg + 1].bend, cached)
    level = seg_start + u * (points[current_seg + 1].level - seg_start)

out[i] = level
```

The release tail is just "while `gate_held` is false, the loop-wrap branch doesn't fire, so the segment counter walks past `loop_end` through any remaining points and settles at `levels[num_points - 1]`". No separate release state machine needed.

### 4.7 Compile-time codegen path

`handle_zenv_call()` in `akkado/src/codegen_envelope.cpp`:

```
1. Visit the `gate` positional arg → buffer index.
2. Extract named args (`levels`, `times`, `curves`, `loop`, `rate`).
3. Visit each array arg → ArrayPayload with TypedValue elements.
4. Validate (emit E150-E153 on failure):
   - len(times) == len(levels) - 1
   - len(curves) == len(times)
   - len(levels) <= 32
   - len(loop) == 2 (if present), indices in [0, len(levels)-1], start <= end
   - Each curves[i] is either Number or String from the allowed set
5. For each curves[i]:
   - If Number: clamp to [-1, +1], emit Point{bend: f, shape: 0}
   - If String: look up in NAMED_SHAPES table; bend-preset names collapse to
                shape=0 + appropriate bend; other names emit (bend: 0, shape: N)
6. Build StateInitData{Type::ZenvEnvelope, points[], num_points, has_loop, loop_start, loop_end}.
7. Build StateInitData{Type::ExtendedParams, ext_constants[], ext_buffer_indices[]} for `rate`
   (constant fallback = 1.0).
8. Emit Instruction{ENV_ZENV, inputs:[gate_buf, 0xFFFF, ...], state_id, out_buffer}.
9. Return TypedValue::signal(out_buffer).
```

### 4.8 `StateInitData` variant

Add `Type::ZenvEnvelope` to `akkado/include/akkado/codegen.hpp` `StateInitData` alongside `Type::Timeline`. Payload mirrors `ZenvState`'s structural fields. `program_loader.cpp:300-310` and `web/wasm/nkido_wasm.cpp` get a new case that copies the payload into the `StatePool` slot at load time.

### 4.9 Validation diagnostics

| Code   | Name                          | When                                                              |
|--------|-------------------------------|-------------------------------------------------------------------|
| `E150` | `ENV_BAD_LENGTHS`             | `len(times) != len(levels) - 1` or `len(curves) != len(times)`    |
| `E151` | `ENV_BAD_LOOP`                | `loop` is not `[a, b]` with `0 <= a <= b < len(levels)`           |
| `E152` | `ENV_BAD_CURVE_ELEMENT`       | `curves[i]` is not Number or a recognized shape name              |
| `E153` | `ENV_TOO_MANY_POINTS`         | `len(levels) > 32`                                                |

Codes `E150-E157` are unused today (existing codes start at `E158`); no conflict.

### 4.10 Reusable array validator

Add to `akkado/include/akkado/codegen/options.hpp`:

```cpp
struct ArrayElementSpec {
    bool allow_number = true;
    bool allow_string = false;
    std::span<const std::string_view> allowed_strings = {};
};

struct ArrayValidationResult {
    bool                 ok;
    std::size_t          first_bad_index = 0;
    std::string          message;
};

ArrayValidationResult validate_array_elements(
    const ArrayPayload& arr,
    const ArrayElementSpec& spec);
```

Generic helper, reused by `zenv` first; available for future array-consuming builtins (e.g. a future `harmonics(fundamental, [ratios], [amps])` etc.).

---

## 5. Edge Cases

| Case                                              | Behavior                                                                                    | Rationale                                                |
|---------------------------------------------------|---------------------------------------------------------------------------------------------|----------------------------------------------------------|
| `loop` omitted                                     | One-shot envelope; runs through all points, settles at `levels[last]`                       | Common for percussive shapes                             |
| `loop: [n, n]`                                     | Holds `levels[n]` while gate held; on gate-off, runs `levels[n+1..]`                        | ADSR-style sustain as a degenerate case of loop region   |
| `loop: [last, last]`                               | Holds the final level forever while gate held; no release tail                              | Valid; "sustain to silence" via setting `levels[last]=0` is the idiom |
| Gate held but envelope has no loop                 | Envelope plays through and settles at last level; gate-off has no audible effect             | One-shot semantics                                       |
| Gate-on mid-envelope (retrigger)                   | `seg_start = current level`, `current_seg = 0`, `time_in_seg = 0`. Smooth attack-from-here.| Matches `ENV_ADSR`; avoids clicks                        |
| Gate-off during attack (before loop region)        | `gate_held = false`. Envelope continues forward through points; loop-wrap branch never fires; reaches release tail naturally | No special "release pending" state needed                |
| `times[i] = 0`                                     | Segment completes in zero samples; while-loop advances immediately to next segment           | Acceptable; allows instant level changes within a chain  |
| `times[i] < 0`                                     | Codegen warning W160 (reserved); clamp to 0 at codegen                                       | Defensive                                                |
| `rate = 0`                                         | Envelope frozen at current level                                                             | Useful as a "pause" via UI; documented                   |
| `rate < 0`                                         | Clamped to 0 (frozen)                                                                        | Backwards playback is out of scope                       |
| `rate > 64`                                         | Clamped to 64                                                                                | Prevents finite-precision blowup                         |
| `rate = NaN` / `inf`                               | Treated as `1.0`                                                                             | Safe default                                             |
| `levels[i]` is negative or > 1                     | Allowed; envelope emits the literal value                                                    | No clamp — useful for envelope-as-modulator              |
| `bend` outside `[-1, +1]`                          | Clamped to `[-1, +1]` at codegen                                                             | Defined range; clamp keeps formula stable                |
| Unknown shape string (e.g. `"sigmoid"`)            | E152 with message listing allowed names                                                      | Fail loud at compile time                                |
| `levels = []`                                      | E150 ("zenv needs at least 2 levels")                                                        | Trivially invalid                                        |
| `levels = [single_value]`                          | E150 (need at least 2 levels for a single segment)                                           | Trivially invalid                                        |
| `loop: [3, 1]` (start > end)                       | E151                                                                                         | Reversed loops are out of scope                          |
| `loop: [n]` (single-int form)                      | E151 ("loop must be `[start, end]` — use `[n, n]` for single-point sustain")                | Disambiguates accidentally-missing second index          |
| Hot-swap: same point count, different `bend` values | State preserved; new bend recomputes `cached_k`/`cached_denom` on the next sample             | Standard hot-swap of runtime params                      |
| Hot-swap: point count changes                      | Semantic-ID hash changes → fresh `ZenvState`. Micro-crossfade applies to mask the discontinuity | Matches all other structural changes (e.g. `unison` voices) |
| First sample after program load, gate is low       | `level = 0`, `current_seg = 0`, `time_in_seg = 0`, `prev_gate = 0`. Awaits gate edge.        | Default-constructed state                                |
| Stereo gate input                                  | E186 (mixed-channel mismatch — gate must be mono)                                            | Pre-existing channel-mismatch policy                     |

---

## 6. Impact Assessment

| Component                                              | Status      | Notes                                                                                   |
|--------------------------------------------------------|-------------|-----------------------------------------------------------------------------------------|
| `ENV_ADSR` / `ENV_AR` / `ENV_FOLLOWER` opcodes         | **Stays**   | No changes; coexist                                                                     |
| `TIMELINE` opcode                                      | **Stays**   | Clock-driven; orthogonal to gate-driven `zenv`                                          |
| `EnvState` struct                                      | **Stays**   | Shared by `ENV_ADSR` / `ENV_AR`; not used by `zenv`                                     |
| Opcode-enum sequence                                   | **Modified**| `ENV_ZENV` added in the Envelopes block                                                 |
| `DSPState` variant                                     | **Modified**| New `ZenvState` added                                                                   |
| `vm.cpp` dispatch                                      | **Modified**| One new `case`                                                                          |
| `BUILTIN_FUNCTIONS` table                              | **Modified**| One new `zenv` entry                                                                    |
| `STDLIB_SOURCE`                                        | **Stays**   | `zenv` is a primitive builtin, not a userspace fn                                       |
| Akkado codegen — array literals                        | **Stays**   | Mixed-type arrays already work (`test_arrays.cpp:95-99`); no `TypedValue` changes       |
| Akkado codegen — `zenv` handler                        | **New**     | New file `akkado/src/codegen_envelope.cpp`                                              |
| Codegen options helper                                 | **Modified**| Add `validate_array_elements()` to `codegen/options.hpp`                                |
| `StateInitData` variant                                | **Modified**| Add `Type::ZenvEnvelope`                                                                |
| Program loader (CLI and WASM)                          | **Modified**| Apply `ZenvEnvelope` state init                                                         |
| `opcode_metadata.hpp` (generated)                      | **Modified**| Regenerated by `cd web && bun run build:opcodes`                                        |
| Hot-swap state preservation                            | **Stays**   | Semantic-ID path mechanism already handles fresh state on structure change              |
| Web docs (`reference/builtins/envelopes.md`)           | **Modified**| New `## zenv` section with examples                                                     |
| F1 lookup index                                        | **Modified**| Rebuild via `bun run build:docs`                                                        |
| Demo patches                                           | **New**     | `zenv-pluck.akk`, `zenv-mseg-pad.akk`                                                   |
| Web UI — `ZenvEditor.svelte`                           | **New**     | Phase 3 — draggable point editor with source round-tripping                             |
| Web UI — state inspector                               | **Modified**| Phase 3 — render `ZenvState` segments + current playhead                                |
| `experiments/test_op_zenv.py`                          | **New**     | Per-shape, per-edge-case kernel tests + 300 s long-run                                  |
| Akkado tests `test_codegen.cpp`                        | **Modified**| Add `[zenv]` section                                                                    |

No backward-incompatible changes. All additions; nothing is renamed or removed.

---

## 7. File-Level Changes

### Modify

| File                                                                | Change                                                                                       |
|---------------------------------------------------------------------|----------------------------------------------------------------------------------------------|
| `cedar/include/cedar/vm/instruction.hpp`                            | Add `ENV_ZENV` to Opcode enum (Envelopes block)                                              |
| `cedar/include/cedar/opcodes/envelopes.hpp`                         | Add `op_zenv(ctx, inst)` implementation                                                      |
| `cedar/include/cedar/opcodes/dsp_state.hpp`                         | Add `ZenvState` struct + entry in `DSPState` variant                                         |
| `cedar/src/vm/vm.cpp`                                               | Add `case Opcode::ENV_ZENV: op_zenv(ctx_, inst); break;`                                     |
| `cedar/include/cedar/generated/opcode_metadata.hpp`                 | Regenerated — do not hand-edit                                                               |
| `akkado/include/akkado/builtins.hpp`                                | Add `zenv` `BuiltinInfo` entry; flag `extended_param_count = 1` for `rate`                   |
| `akkado/include/akkado/codegen.hpp`                                 | Add `StateInitData::Type::ZenvEnvelope` payload                                              |
| `akkado/include/akkado/codegen/options.hpp`                         | Add `validate_array_elements()` helper                                                       |
| `akkado/src/codegen.cpp`                                            | Route `"zenv"` to `handle_zenv_call()` in the builtin dispatch table                         |
| `tools/nkido/program_loader.cpp`                                | Apply `ZenvEnvelope` state init                                                              |
| `web/wasm/nkido_wasm.cpp`                                           | Apply `ZenvEnvelope` state init (WASM path)                                                  |
| `web/src/lib/components/Panel/StateInspector.svelte`                | Phase 3 — render `ZenvState` segment list + playhead                                         |
| `web/static/docs/reference/builtins/envelopes.md`                   | New `## zenv` section with examples and curve-shape reference                                |
| `CLAUDE.md`                                                         | One-line addition to the envelopes paragraph noting `zenv` as the multi-segment option       |

### Create

| File                                                                | Purpose                                                                                       |
|---------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| `akkado/src/codegen_envelope.cpp`                                   | `handle_zenv_call()` — array extraction, validation, StateInitData emission                   |
| `akkado/include/akkado/codegen_envelope.hpp`                        | Declaration                                                                                   |
| `web/src/lib/components/Editor/ZenvEditor.svelte`                   | Phase 3 — draggable point + handle editor                                                     |
| `web/src/lib/zenv-source-mapper.ts`                                 | Phase 3 — parse `zenv(...)` call in source → editor model; serialize model → source patch     |
| `web/static/patches/zenv-pluck.akk`                                 | Demo: percussive pluck with cubic_out attack                                                  |
| `web/static/patches/zenv-mseg-pad.akk`                              | Demo: looped multi-segment pad                                                                |
| `experiments/test_op_zenv.py`                                       | Kernel test (shapes, edges, loop, retrigger, 300 s long-run)                                  |

### Stays

| File                                                                | Reason                                                                                        |
|---------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| `cedar/include/cedar/opcodes/envelopes.hpp` (ENV_ADSR / ENV_AR)     | Existing envelopes untouched                                                                  |
| `cedar/include/cedar/opcodes/sequencing.hpp` (TIMELINE)             | Unrelated abstraction                                                                         |
| `akkado/include/akkado/typed_value.hpp`                             | Mixed-type arrays already supported by `ArrayPayload`                                         |
| `akkado/src/codegen.cpp` array literal handling                     | No changes — `ArrayPayload::elements` already accepts mixed `TypedValue` kinds                |

---

## 8. Implementation Phases

### 8.1 Phase 1 — Cedar opcode kernel

**Goal**: ship `ENV_ZENV` opcode and validate its DSP independently of any Akkado plumbing.

Files in scope:
- `cedar/include/cedar/vm/instruction.hpp`
- `cedar/include/cedar/opcodes/dsp_state.hpp`
- `cedar/include/cedar/opcodes/envelopes.hpp`
- `cedar/src/vm/vm.cpp`
- `cedar/include/cedar/generated/opcode_metadata.hpp` (regen)
- `experiments/test_op_zenv.py`

Tasks:
1. Add `ENV_ZENV` enum value.
2. Add `ZenvState` struct + DSPState variant entry.
3. Implement `op_zenv()` with the kernel from §4.6, including the curve dispatch from §4.5 and the bend formula from §4.4.
4. Regenerate `opcode_metadata.hpp` via `cd web && bun run build:opcodes`.
5. Build Python `cedar_core` test that constructs a `ZenvState` directly (via the test host's state-pool API) and verifies per-shape correctness, gate edge handling, loop semantics, retrigger from current level, and 300 s stability.

Verification:
- `cmake --build build --target cedar` clean.
- `uv run python experiments/test_op_zenv.py` — all sub-tests pass; WAVs saved under `experiments/output/op_zenv/` for human listening.
- `uv run python experiments/test_op_adsr.py` — regression check (touched adjacent state-struct code).

### 8.2 Phase 2 — Akkado surface, docs, demos

**Goal**: surface the opcode in Akkado and ship usable docs + demo patches.

Files in scope:
- `akkado/include/akkado/builtins.hpp`
- `akkado/include/akkado/codegen.hpp`
- `akkado/include/akkado/codegen/options.hpp`
- `akkado/include/akkado/codegen_envelope.hpp` (new)
- `akkado/src/codegen.cpp` (dispatch)
- `akkado/src/codegen_envelope.cpp` (new)
- `tools/nkido/program_loader.cpp`
- `web/wasm/nkido_wasm.cpp`
- `akkado/tests/test_codegen.cpp` — `[zenv]` section
- `web/static/docs/reference/builtins/envelopes.md`
- `web/static/patches/zenv-pluck.akk` (new)
- `web/static/patches/zenv-mseg-pad.akk` (new)
- `web/static/patches/index.json` (register the new patches)
- `CLAUDE.md` (one-line update)

Tasks:
1. Add `zenv` builtin entry; route to `handle_zenv_call()`.
2. Implement `handle_zenv_call()` per §4.7. Extract arrays, validate, emit StateInitData + ExtendedParams + Instruction.
3. Implement `validate_array_elements()`.
4. Add `StateInitData::Type::ZenvEnvelope` and the load-path glue in CLI + WASM loaders.
5. Add `[zenv]` Catch2 tests in `test_codegen.cpp` covering: minimal valid call, mixed curve elements, all E150-E153 cases, loop=[n,n] degenerate case, runtime `rate` modulation, missing optional `loop`.
6. Author the two demo patches; register them in `index.json`.
7. Write the docs section with concrete examples mirroring §3.
8. Run `bun run build:docs` to refresh the F1 lookup index.

Verification:
- `cmake --build build` clean.
- `./build/akkado/tests/akkado_tests "[zenv]"` — green.
- CLI smoke: `./build/bin/akkado -e '<demo source>'` compiles; `./build/bin/nkido` plays.
- Web smoke: `bun run dev`, load each demo patch, gate it, audibly verify the envelope shape and sustain loop.

### 8.3 Phase 3 — Web UI draggable editor

**Goal**: visual `zenv` editor that round-trips with the Akkado source — drag points, handles, and loop markers; edits update the source; source changes update the editor.

Scope:
- **Editor component** `web/src/lib/components/Editor/ZenvEditor.svelte`. SVG canvas with:
  - Draggable points (modify `levels[i]` and `times[i-1]/times[i]`).
  - Per-segment curve handle (drag perpendicular to the segment line to modify `bend`, double-click to cycle through named shape presets).
  - Loop region indicator (two draggable markers).
  - Read-only playhead overlay driven by the live state inspector at 20 Hz.
- **Source mapper** `web/src/lib/zenv-source-mapper.ts`. Parses `zenv(...)` calls in source, builds an editor model; on user edits, computes a minimal source patch (preserving comments, formatting where possible) and applies it via the editor store.
- **State inspector** addition in `web/src/lib/components/Panel/StateInspector.svelte` — render `ZenvState` segments and current playhead position.
- **Trigger UX** — clicking a `zenv(...)` call in the editor opens the visual editor in a side panel.

Out of scope (deferred to a follow-up):
- Curve-shape preview palette (visual icons for each named shape).
- Copy/paste envelope shapes between calls.
- Save-as-preset library.

Verification:
- Manual: load `zenv-mseg-pad.akk`, click the `zenv(...)` call, drag points and curve handles, confirm the source updates and the audio reflects the changes immediately (hot-swap).
- Manual: edit the source directly, confirm the visual editor updates within one tick.
- No automated end-to-end test for the editor in v1; rely on manual QA.

---

## 9. Testing & Verification

### 9.1 `experiments/test_op_zenv.py` (kernel test)

Per `docs/dsp-experiment-methodology.md` and CLAUDE.md "Cedar Python Experiments". Each sub-test writes a WAV to `experiments/output/op_zenv/` and reports ✓/✗.

| # | Sub-test                            | Setup                                                                                          | Assertion                                                            |
|---|-------------------------------------|------------------------------------------------------------------------------------------------|----------------------------------------------------------------------|
| 1 | Linear segment                       | `levels=[0,1]`, `times=[0.1]`, `curves=[0]`                                                    | Output is a straight line; midpoint ≈ 0.5 ± 1e-3                     |
| 2 | Bend +1 vs bend -1                   | Same as #1 but with `curves=[+1]` vs `curves=[-1]`                                             | +1 above linear at midpoint; -1 below linear at midpoint              |
| 3 | All named shapes vs reference        | One segment per shape; compare each against its closed-form `u(t)`                              | Per-sample error < 1e-4 for non-discrete shapes; exact for square/hold |
| 4 | Bend preset names                    | `curves=["linear"]` ≡ `[0]`; `["exp"]` ≡ `[+0.7]`; `["log"]` ≡ `[-0.7]`                        | Output identical to numeric equivalents                              |
| 5 | Gate-on retrigger from current level | Gate, hold to mid-attack, release before sustain, retrigger                                    | No discontinuity at retrigger; attack continues from current level   |
| 6 | Sustain loop (degenerate [n,n])      | ADSR-equivalent shape, hold gate for 1 s, release                                              | Output holds at sustain level for 1 s, then releases per release segment |
| 7 | Sustain loop region [start, end]     | 5-point envelope with `loop=[1,3]`, gate held 2 s                                              | Loop cycles between `levels[1..3]` ≥ 4 full cycles in 2 s            |
| 8 | Release tail after loop              | Continue from #7; release gate, observe envelope through `levels[4]`                            | Release tail plays in expected duration                              |
| 9 | One-shot (no loop)                   | Gate-on with `loop` omitted                                                                    | Envelope plays through all segments and settles at `levels[last]`     |
| 10| `rate` modulation                    | `rate` sweeps 0.5 → 2.0 mid-envelope                                                           | Envelope speed visibly tracks; clamps respected (0 freezes, 64 caps) |
| 11| `rate = NaN`                         | Set rate buffer to NaN                                                                          | Output unchanged from `rate = 1`; no NaN propagates                  |
| 12| Long-run stability                   | 300 s of gated 8-point loop at 110 BPM                                                          | No NaN/inf; no drift in segment counter; bounded RMS                  |
| 13| Hot-swap structural change           | Compile with `num_points = 4`, then with `num_points = 5` mid-render                            | Fresh state on swap; no NaN; transient < 10 ms                       |

All sub-tests render at least 300 s of audio per the CLAUDE.md mandate for sequenced/long-running tests (test 12 covers the explicit long-run; shorter tests run inside that umbrella).

### 9.2 Akkado codegen tests (`akkado/tests/test_codegen.cpp`, tag `[zenv]`)

```cpp
SECTION("zenv minimal valid call") {
    auto src = R"(
        sine(440) * zenv(button("g"),
            levels: [0, 1, 0],
            times:  [0.01, 0.1],
            curves: [0, 0],
            loop:   [1, 1]) |> out(@)
    )";
    REQUIRE_NOTHROW(compile(src));
}

SECTION("zenv mixed-type curves") {
    auto src = R"(
        zenv(g,
            levels: [0, 1, 0.5, 0],
            times:  [0.01, 0.1, 0.3],
            curves: [1.0, "exp", "sine"],
            loop:   [2, 2]) |> out(@)
    )";
    REQUIRE_NOTHROW(compile(src));
}

SECTION("zenv E150 on length mismatch") {
    auto src = R"(zenv(g, levels: [0, 1, 0], times: [0.1], curves: [0, 0], loop: [1, 1]))";
    REQUIRE_THROWS_WITH(compile(src), Catch::Contains("E150"));
}

SECTION("zenv E151 on out-of-bounds loop") {
    auto src = R"(zenv(g, levels: [0, 1, 0], times: [0.01, 0.1], curves: [0, 0], loop: [5, 5]))";
    REQUIRE_THROWS_WITH(compile(src), Catch::Contains("E151"));
}

SECTION("zenv E152 on bad curve element") {
    auto src = R"(zenv(g, levels: [0, 1, 0], times: [0.01, 0.1], curves: [0, "sigmoid"], loop: [1, 1]))";
    REQUIRE_THROWS_WITH(compile(src), Catch::Contains("E152"));
}

SECTION("zenv E153 on >32 points") {
    // levels of length 33 → 32+ segments
    // (write programmatically to keep the test readable)
}

SECTION("zenv loop=[n,n] = ADSR-style sustain") {
    auto src = R"(zenv(g, levels: [0, 1, 0.5, 0], times: [0.01, 0.1, 0.3], curves: ["exp", "log", "log"], loop: [2, 2]))";
    REQUIRE_NOTHROW(compile(src));
}

SECTION("zenv runtime rate modulation") {
    auto src = R"(
        speed = param("s", 1.0, 0.1, 8.0)
        zenv(g, levels: [0, 1, 0], times: [0.01, 0.1], curves: [0, 0], loop: [1, 1], rate: speed)
    )";
    REQUIRE_NOTHROW(compile(src));
}
```

### 9.3 CLI smoke test

```bash
./build/bin/akkado -e \
  'sine(220) * zenv(button("g"), levels:[0,1,0.5,0], times:[0.01,0.2,0.3], curves:[0.5,0,-0.5], loop:[2,2]) |> out(@)' \
  > /tmp/zenv-smoke.cbc

./build/bin/nkido /tmp/zenv-smoke.cbc --seconds 5
```

### 9.4 Audible checks

- `zenv-pluck.akk` — short percussive shape with `cubic_out` attack, fast release. Should sound like a clean pluck with no clicks.
- `zenv-mseg-pad.akk` — looped 6-point shape with mixed bend + named curves. Should sound like a living, animated pad while the gate is held.

### 9.5 Phase 3 manual QA

- Click a `zenv(...)` call in the editor → editor panel opens with correct points.
- Drag a point's level → audio reflects within one block; source updates.
- Drag a curve handle → bend value updates in source; audio shifts.
- Drag a loop marker → loop region updates; audio loop behavior matches.
- Edit source directly → editor panel updates within one tick.

---

## 10. Open Questions

- **Q1 — `BEND_SCALE` value.** Proposed `5`. Worth comparing `5`, `7`, `10` audibly after Phase 1 lands. The choice is musical, not mathematical. Resolve before Phase 2 docs lock in any specific bend-preset values for `"exp"` / `"log"`.

- **Q2 — `"smoothstep"` alias for `"cubic"`?** They are the same function (`3t² - 2t³`). Adding `"smoothstep"` as an alias is one extra string-table entry. Mildly redundant; argument for: matches what graphics programmers expect. Argument against: more names to document. Default: skip.

- **Q3 — `rate` audio-rate vs control-rate.** Wired as `ExtendedParams<1>` → can be either. The kernel reads per-sample. No real downside to leaving it audio-rate. Confirm during Phase 1.

- **Q4 — Phase 3 source-mapper robustness.** Round-tripping editor edits back to the source requires preserving comments and formatting. The akkado codebase doesn't have an established pattern for this. Phase 3 spike: prototype with a simple "regenerate the call from scratch and replace the source span" approach; if user feedback says formatting matters, upgrade to a CST-preserving edit.

---

## 11. References

- Existing PRD format / structure: `docs/prd-unison.md`, `docs/prd-glide-interp.md`, `docs/prd-timeline-curve-notation.md`.
- Envelope opcode patterns: `cedar/include/cedar/opcodes/envelopes.hpp:16-279` (ADSR / AR / follower).
- Breakpoint state pattern (closest analog): `cedar/include/cedar/opcodes/dsp_state.hpp:262-276` (`TimelineState`), `cedar/include/cedar/opcodes/sequencing.hpp:243-302` (`op_timeline`).
- `inst.rate` and ExtendedParams guidelines: `CLAUDE.md` → "Extended Parameter Patterns", `docs/extended-params-mechanism.md`.
- Coefficient caching pattern: `cedar/include/cedar/opcodes/envelopes.hpp:16-143` (`ENV_ADSR`'s `last_attack` invalidation).
- Mixed-type array literal support: `akkado/include/akkado/typed_value.hpp:132-135` (`ArrayPayload::elements`), `akkado/tests/test_arrays.cpp:95-99` (existing test).
- `StateInitData::Type::Timeline` precedent for compile-time-populated breakpoint state: `akkado/src/codegen_patterns.cpp:2883-3046`.
- Stdlib `match()` compile-time evaluability limits: `akkado/src/codegen_functions.cpp:1213-1299` (`is_compile_time_match`). Not used by `zenv` (no userspace dispatch) but worth knowing if anyone tries to wrap `zenv` in a stdlib fn later.
- Demo patch convention: `web/static/patches/unison-lead.akk`, `unison-pad.akk`.
- DSP experiment methodology: `docs/dsp-experiment-methodology.md`.
