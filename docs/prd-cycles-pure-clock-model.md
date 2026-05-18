> **Status: FIRST DRAFT — NOT READY FOR IMPLEMENTATION.** Drafted 2026-05-19 from the captured first-draft goal in `project_strudel_cycles_pure_model_followup.md`. This PRD is a hard prereq of `docs/prd-runtime-event-transforms.md` (see its §0.5). Resolve §11 Open Questions before locking the design.

# PRD: Cycles-Pure Clock Model — drop "beats" from Cedar/Akkado

## Executive Summary

Cedar today threads a "1 cycle = 4 beats" convention through `ExecutionContext`, every sequencing opcode, `PatternPayload`, `SequenceState`, the `SEQPAT_TRANSPORT` opcode, the `MidiQueueState` sentinel, and most user-facing time-coupled builtins (`beat()`, `delay_sync()`, `bars()` if it exists). Mini-notation patterns carry a per-pattern `cycle_length` field even though every pattern (after the 2026-05-18 cycle-timing fix) is already a single normalized cycle. The result is a half-and-half model: the parser produces cycle-phase events, but the runtime keeps re-scaling them by a `cycle_length` that is always `4.0f` in practice.

This PRD makes the **cycle the only canonical clock unit inside Cedar** and removes the per-pattern `cycle_length` field entirely. BPM remains the canonical *user-facing* knob (musicians know BPM); CPS becomes the canonical *internal* and *akkado-source* unit, derived from `bpm` and a new `beats_per_cycle` runtime field that defaults to **1** (Strudel-pure). Every sequencing opcode reads `samples_per_cycle()` directly; events are stored as phase `[0, 1)` and consumed without rescaling.

**Key design decisions** (resolved during the question rounds):

- **Strudel-pure default: 1 cycle = 1 beat.** A new `ExecutionContext::beats_per_cycle` field (float, default 1.0) replaces the hardcoded `* 4.0f` everywhere. Users (or the web UI) can override.
- **BPM stays as the canonical user-facing knob; CPS is canonical internally.** Both are addressable; setting either recomputes the other from `beats_per_cycle`. Setting `beats_per_cycle` recomputes CPS but leaves BPM alone.
- **Per-pattern `cycle_length` field removed.** From `SequenceState`, `PatternPayload`, `EventSourcePayload`, `SEQPAT_TRANSPORT`, and all call sites. Events are already phase-normalized `[0, 1)` per the 2026-05-18 parser fix.
- **`beat()` stays.** Re-spec'd to read `beats_per_cycle`. No deprecation, no error codes — old user patches keep compiling.
- **`delay_sync` and SEQPAT_TRANSPORT step port to cycles.** New unit for all beat-coupled opcode parameters is *cycles*. `delay_sync(in, 0.25)` is now a 1/4-cycle delay (was 1/4 beat).
- **`ExecutionContext` cleanup**: drop `beat_phase` / `bar_phase`, add `cycle_offset` (matches the user-facing `co` builtin name). `samples_per_beat()` / `beat_at_sample()` stay as derived getters that read `bpm` and `beats_per_cycle`; `samples_per_cycle()` / `cycle_at_sample()` are the new canonical helpers.
- **MIDI sentinel hack replaced with an explicit `streaming: bool` flag** on the event-stream state.
- **No time-signature framing in the UI.** Just expose `beats_per_cycle` as a raw float (per user direction — "no fake distinction between 6/8 and 3/4"). Time-signature dropdown deferred indefinitely.
- **Web transport widget unchanged.** BPM slider stays primary; CPS is *not* surfaced in the UI; `beats_per_cycle` is configurable via a project setting only.
- **Hard cutover** for internal Cedar/Akkado plumbing; sweep all in-repo patches/demos/tests in one PR. No legacy flag.

---

## 0. Dependencies and Coupling

- **Down-stream consumer**: [`docs/prd-runtime-event-transforms.md`](prd-runtime-event-transforms.md). That PRD's §0.5 declares this one a hard prereq. Once cycles-pure lands, `EVENT_RATE_SCALE`, the closure-based `event_map` stdlib, and the unified `EventStreamPayload` are written in cycles-pure terms.
- **Mini-notation parser changes**: none required. The 2026-05-18 cycle-timing fix (PR `2fbb230`, `25bc560`, `9146ce2`, `5c1c0ca`) already lands events in phase-normalized form `[0, 1)`. This PRD only removes the *downstream* `* cycle_length` rescaling.

---

## 1. Current State

### 1.1 The two-layer beat/cycle model today

| Layer | Today | Problem |
|---|---|---|
| Mini-notation parser | Already produces phase-normalized `Event::time ∈ [0, 1)` (per 2026-05-18 fix) | Fine |
| `Sequence::duration` | Always `1.0f` (normalized) | Fine — never anything else |
| Per-pattern `cycle_length` | `4.0f` default; baked into `SequenceState`, `PatternPayload`, `EventSourcePayload`, `SEQPAT_TRANSPORT` opcode | Dead field — always `4.0f` for hand-written code; re-scaling step at query time is pure rot |
| `ExecutionContext::beat_phase` / `bar_phase` | Computed each block from `bpm` and a hardcoded `* 4.0f` for bar | Two derived fields for one underlying thing (cycle phase) |
| `ctx.samples_per_beat()` / `samples_per_cycle()` | Returns `(60/bpm)*sr` and `spb*4` | Hardcoded 4 |
| `beat()` builtin | `fn beat(n) = trigger(1/n)`; `trigger()` is in cycle-Hz | Defined in terms of `trigger`, which assumes 4 beats per cycle implicitly |
| `delay_sync` opcode | `delay_ms = (beats / bpm) * 60000` — input arg in beats | Coupled to BPM directly |
| `SEQPAT_TRANSPORT` opcode | `step` arg in beats, bit-packs `cycle_length` into `inputs[3..4]` | Forces user to think in beats; pollutes input slots |
| `MidiQueueState::cycle_length = 1e6f` sentinel | Means "no cycle wrap" | Magic-number hack |
| Web UI BPM slider | Default 120 BPM | Stays |
| `bpm = 120` akkado top-level setting | Sets `ctx.bpm` | Stays |

### 1.2 File inventory (cycle_length / spb / beat references)

**Cedar runtime:**

- `cedar/include/cedar/opcodes/sequence.hpp:197` — `SequenceState::cycle_length = 4.0f`
- `cedar/include/cedar/opcodes/sequence.hpp:302, 348-356` — `query_pattern(state, cycle, cycle_length)`, scales events by `cycle_length`
- `cedar/include/cedar/opcodes/midi.hpp:113, 117, 247, 281` — `MidiQueueState::cycle_length` (sentinel 1e6f)
- `cedar/include/cedar/opcodes/sequencing.hpp:309-326, 344-348, 383-419, 467, 473, 474, 560, 632, 742, 816` — every SEQPAT_* opcode reads `state.cycle_length` and `ctx.samples_per_beat()`
- `cedar/include/cedar/opcodes/delays.hpp:108-145` — `DELAY_SYNC` reads `samples_per_beat`
- `cedar/include/cedar/opcodes/soundfont.hpp:303` — soundfont reads `spb_d = 60/bpm`
- `cedar/include/cedar/vm/context.hpp:53, 60-103` — `ExecutionContext` with `bpm`, `beat_phase`, `bar_phase`, `samples_per_beat()`, `samples_per_bar()`, `samples_per_cycle()` (returns `* 4.0f`), `beat_at_sample()`, `beat_phase_at_sample()`
- `cedar/include/cedar/vm/vm.hpp:119, 171, 174` — `set_bpm()`, SEQPAT_TRANSPORT init signature
- `cedar/include/cedar/vm/state_pool.hpp:116-138, 226, 477-483, 807-920` — `resolve_output_events()` returns `cycle_length`, init paths thread `cycle_length`, JSON inspector emits `cycle_length`

**Akkado compiler:**

- `akkado/include/akkado/typed_value.hpp:78` — `PatternPayload::cycle_length = 4.0f`
- `akkado/include/akkado/typed_value.hpp:146` — `EventSourcePayload::cycle_length = 4.0f`
- `akkado/src/codegen_patterns.cpp:102, 228, 1327, 1384, 1459, 1705, 1760, 1794, 1856, 1908, 1918, 2138-2320` — 20+ sites that read or assign `cycle_length`, plus `"// 1 cycle = 4 beats by default (Strudel convention)"` comments
- `akkado/include/akkado/stdlib.hpp:42` — `fn beat(n) -> {trigger(1/n)}`

**Docs:**

- `CLAUDE.md:108-115` — "1 cycle = 4 beats by default"
- `docs/mini-notation-*.md` — multiple references
- `web/static/docs/concepts/*.md`, `web/static/docs/builtins/*.md` — sweep needed
- `web/static/docs/concepts/runtime-controls.md:96` — `bpm = 100` example (stays correct)

### 1.3 Why this hasn't been ripped out already

The 2026-05-18 mini-notation cycle-timing fix scoped this refactor out explicitly: that fix kept the `cycle_length` plumbing alive and only normalized parser output. The user observed at the time: *"a beat should be a cycle no?"* — flagging this as the eventual proper model. The narrow fix shipped without this cleanup; this PRD picks it up.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. Make CPS the canonical clock unit inside Cedar; BPM remains the canonical user-facing setter.
2. Add `ExecutionContext::beats_per_cycle` (default 1.0); remove the hardcoded `* 4.0f` everywhere.
3. Remove the per-pattern `cycle_length` field from `SequenceState`, `PatternPayload`, `EventSourcePayload`, and `SEQPAT_TRANSPORT`'s bit-packed slots.
4. Replace `MidiQueueState::cycle_length = 1e6f` sentinel with an explicit `streaming: bool` flag.
5. Add `ExecutionContext::cycle_offset`; drop `beat_phase` / `bar_phase`.
6. Keep `bpm` and `samples_per_beat()` working as derived getters/setters so user-facing builtins (`beat()`, `bpm = 120`, web BPM slider) keep their semantics.
7. Port all beat-coupled opcode parameters to *cycles*: `delay_sync`, `SEQPAT_TRANSPORT::step`, etc.
8. Keep `beat()` builtin alive, re-spec'd in terms of `beats_per_cycle`.
9. Ship `beats(n)` / `bars(n)` 1-line stdlib helpers that read `beats_per_cycle` for users who want classical time-signature ergonomics.
10. Coordinated sweep of all in-repo demo/welcome/test patches in the same PR (hard cutover).
11. Sweep all docs of the "1 cycle = 4 beats" wording.

### 2.2 Non-Goals (deferred)

- **Time-signature UI** (4/4, 6/8 dropdown). Explicitly out — user wants just a raw `beats_per_cycle` float.
- **Web transport widget redesign.** UI stays as-is; BPM slider primary, `beats_per_cycle` exposed as a project setting only.
- **Removing BPM entirely.** BPM is the canonical *user-facing* knob; only the *internal* model becomes cycle-canonical.
- **External MIDI / DAW sync.** Out of scope (separate transport-sync PRD if needed).
- **`Time` type system work** (a Time variant that pretty-prints in beats/bars/cycles). Out — pure float fractions are enough.
- **Deprecation warnings or new error codes.** No `E190` — old syntax (`bpm = 120`, `beat(4)`) keeps compiling unchanged.

---

## 3. Target Syntax and User Experience

### 3.1 The default cycle

```akkado
// Default: 1 cycle = 1 beat (Strudel-pure)
bpm = 120          // → cps = 2.0 (because beats_per_cycle = 1)
pat("c4 e4 g4")    // exactly 1 cycle = 0.5s long at 120 BPM
```

### 3.2 Classical-time-signature feel

```akkado
bpm = 120
beats_per_cycle = 4   // recovers the old "1 cycle = 4 beats = 1 bar" feel
                      // cps now = 0.5; one cycle takes 2s
pat("c4 e4 g4 c5")    // one cycle, four notes, 0.5s apart
```

### 3.3 Setting via CPS (akkado-source-level)

```akkado
cps = 0.5      // canonical-internal setter
               // implicitly sets bpm = cps * 60 * beats_per_cycle = 30 * 1 = 30
```

### 3.4 Time-coupled effects (new contract: cycles)

```akkado
// delay_sync — new contract is cycles, not beats
osc("saw", 220) |> delay_sync(@, 0.25)     // 1/4-cycle delay
osc("saw", 220) |> delay_sync(@, beats(1)) // 1 beat — equivalent to 1/4 cycle when beats_per_cycle = 4

// SEQPAT_TRANSPORT
transport(pat("c4 e4 g4"), trig(4), 0.25)  // 1/4-cycle step per trigger
```

### 3.5 `beat()` builtin re-spec

```akkado
// Old definition: fn beat(n) -> {trigger(1/n)}  (n beats per phasor)
// New definition: fn beat(n) -> {trigger(1 / (n / beats_per_cycle))}
//                              = {trigger(beats_per_cycle / n)}
// Semantics preserved: beat(4) = phasor completing every 4 beats.
// With default beats_per_cycle=1: beat(4) = trigger(0.25) = one trigger per 4 cycles.
// With beats_per_cycle=4: beat(4) = trigger(1) = one trigger per cycle (4 beats).
```

### 3.6 stdlib helpers

```akkado
// Defined in akkado/stdlib.hpp (or stdlib/time.ak)
fn beats(n) -> {n / beats_per_cycle}   // returns cycles
fn bars(n)  -> {n * 4 / beats_per_cycle}  // 1 bar = 4 beats convention (or just `n` if bpc=4)

// Usage
delay_sync(in, beats(1))    // delay by 1 beat regardless of beats_per_cycle setting
delay_sync(in, bars(0.5))   // delay by half a bar
```

> **OQ-1**: `bars()` definition. Is `1 bar = 4 beats` hardcoded inside `bars()`, or should it read another `beats_per_bar` knob? See §11.

### 3.7 The `co` builtin variable

```akkado
// User-facing name unchanged. `co` reads ctx.cycle_offset (internal field).
freq = co * 880 + 220    // sweep frequency over each cycle, same as before
```

---

## 4. Architecture / Technical Design

### 4.1 `ExecutionContext` after the refactor

```cpp
// cedar/include/cedar/vm/context.hpp
struct ExecutionContext {
    // ... unchanged fields (buffers, states, arena, env_map, IO, sample_rate, ...) ...

    // Canonical USER-FACING tempo knob
    float bpm = DEFAULT_BPM;

    // Canonical INTERNAL cycle-rate knob, derived from bpm and beats_per_cycle.
    // Recomputed whenever bpm or beats_per_cycle changes.
    // Single source of truth for all sequencing math.
    float cps = DEFAULT_BPM / (60.0f * 1.0f);

    // Strudel-pure default; Web UI / akkado source can override.
    // Used only to translate between user-facing BPM and internal CPS.
    float beats_per_cycle = 1.0f;

    // Timing
    std::uint64_t global_sample_counter = 0;
    std::uint64_t block_counter = 0;

    // Derived timing values (updated per block)
    float cycle_offset = 0.0f;       // 0-1 phase within current cycle (REPLACES beat_phase, bar_phase)

    void update_timing() {
        float spc = samples_per_cycle();
        float sample_in_cycle = std::fmod(static_cast<float>(global_sample_counter), spc);
        cycle_offset = sample_in_cycle / spc;
    }

    void set_bpm(float new_bpm) {
        bpm = new_bpm;
        recompute_cps();
    }
    void set_cps(float new_cps) {
        cps = new_cps;
        bpm = new_cps * 60.0f * beats_per_cycle;
    }
    void set_beats_per_cycle(float new_bpc) {
        beats_per_cycle = new_bpc;
        recompute_cps();   // bpm preserved; cps recomputed
    }

    // CANONICAL helpers
    [[nodiscard]] float samples_per_cycle() const noexcept {
        return sample_rate / cps;
    }
    [[nodiscard]] float cycle_at_sample(std::size_t off) const noexcept {
        return static_cast<float>(global_sample_counter + off) / samples_per_cycle();
    }
    [[nodiscard]] float cycle_offset_at_sample(std::size_t off) const noexcept {
        float spc = samples_per_cycle();
        return std::fmod(static_cast<float>(global_sample_counter + off), spc) / spc;
    }

    // DERIVED helpers — kept so user-facing builtins (beat(), delay_ms math) work
    [[nodiscard]] float samples_per_beat() const noexcept {
        return samples_per_cycle() / beats_per_cycle;
    }
    [[nodiscard]] float beat_at_sample(std::size_t off) const noexcept {
        return cycle_at_sample(off) * beats_per_cycle;
    }

private:
    void recompute_cps() { cps = bpm / (60.0f * beats_per_cycle); }
};
```

### 4.2 `SequenceState` after the refactor

```cpp
// cedar/include/cedar/opcodes/sequence.hpp
struct SequenceState {
    Sequence* sequences = nullptr;
    std::uint32_t num_sequences = 0;
    std::uint32_t seq_capacity = 0;

    // REMOVED: cycle_length
    std::uint64_t pattern_seed = 0;
    bool is_sample_pattern = false;

    // NEW: replaces MidiQueueState cycle_length=1e6 sentinel for streaming sources
    bool streaming = false;

    OutputEvents output;
    // ... rest unchanged ...
};

// query_pattern signature drops cycle_length
inline void query_pattern(SequenceState& state, std::uint64_t cycle) {
    state.output.clear();
    std::uint64_t seed = state.pattern_seed + cycle;
    // time_scale = 1.0f because events are already phase-normalized [0, 1)
    query_sequence(state, 0, seed, 0.0f, 1.0f, state.output);
    state.output.sort_by_time();
    state.current_index = 0;
}
```

### 4.3 `PatternPayload` / `EventSourcePayload` after the refactor

```cpp
// akkado/include/akkado/typed_value.hpp
struct PatternPayload {
    std::array<std::uint16_t, 11> fields = { /* ... */ };
    std::vector<std::uint16_t> voice_freqs;
    std::unordered_map<std::string, std::uint16_t> custom_fields;
    std::uint32_t state_id = 0;
    // REMOVED: float cycle_length = 4.0f;
    bool is_sample_pattern = false;
    std::uint8_t max_voices = 1;
    bool is_runtime_event_source = false;
    std::vector<RequiredSample> sample_refs;
    // ... rest unchanged ...
};

struct EventSourcePayload {
    std::uint32_t state_id = 0;
    // REMOVED: float cycle_length = 4.0f;
    bool streaming = true;   // always true for MIDI streams
};
```

### 4.4 `SEQPAT_TRANSPORT` after the refactor

- `inputs[3]/[4]` bit-packed `cycle_length` slots → **freed**. Document as reserved for future ExtendedParams use.
- `step` argument unit changes from beats to cycles. Old `transport(p, trig, 1.0)` (advance 1 beat per trigger) → new `transport(p, trig, 0.25)` (advance 1/4 cycle per trigger). At default `beats_per_cycle = 1` this is just `1.0` for "one cycle per trigger".

### 4.5 `bpm = 120` and `cps = 0.5` akkado source-level setters

Both compile to setter calls on `ExecutionContext`. Setting either recomputes the other per the rules in §4.1.

### 4.6 The mini-notation parser is unchanged

Per the 2026-05-18 fix: every mini-notation string produces a `Sequence` with events at `time ∈ [0, 1)`. No more `* cycle_length` at query time means the parser's output is consumed directly.

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| Mini-notation parser | **Stays** | Already produces phase-normalized events (2026-05-18 fix). |
| `bpm = N` akkado setter | **Stays** | Re-routes through `ExecutionContext::set_bpm()` which recomputes `cps`. |
| `bpm` web UI slider | **Stays** | No UI changes. |
| `co` builtin variable | **Stays** | Same semantics; now backed by `ctx.cycle_offset` internally. |
| `beat()` builtin | **Modified** | Re-spec'd to read `beats_per_cycle`. Public semantics preserved at default `bpc=1`. |
| `trigger()` builtin | **Stays** | Already in cycle-rate units (Hz-equivalent). |
| `delay_sync` opcode | **Modified** | Arg unit ports from beats to cycles. Existing patches retune. |
| `SEQPAT_TRANSPORT` opcode | **Modified** | `step` arg ports to cycles. `inputs[3]/[4]` freed. |
| `SequenceState::cycle_length` | **Removed** | Dead field. |
| `PatternPayload::cycle_length` | **Removed** | Dead field. |
| `EventSourcePayload::cycle_length` | **Removed** | Replaced by `streaming: bool`. |
| `MidiQueueState::cycle_length=1e6f` sentinel | **Removed** | Replaced by `streaming: bool`. |
| `ExecutionContext::beat_phase` / `bar_phase` | **Removed** | Replaced by `cycle_offset`. |
| `ExecutionContext::samples_per_beat()` | **Stays as derived getter** | Reads `samples_per_cycle() / beats_per_cycle`. |
| `ExecutionContext::beats_per_cycle` | **New** | Float field, default 1.0. |
| `ExecutionContext::cps` | **New** | Float field, derived from `bpm` and `beats_per_cycle`. |
| `ExecutionContext::cycle_offset` | **New** | Replaces `beat_phase`/`bar_phase`. |
| Web `beats_per_cycle` project setting | **New** | Numeric input in project settings; default 1. Does NOT live in the transport widget. |
| `beats(n)` / `bars(n)` stdlib helpers | **New** | 1-line stdlib reading `beats_per_cycle`. |
| `iter()` / `iterBack()` | **Stays** | Already cycle-based via `cycle_index`. |
| Soundfont SPB usage | **Modified** | Reads via `ctx.samples_per_beat()` (now derived). Audit-only — no observable change. |
| All in-repo demo/welcome/test patches | **Modified** | Sweep `delay_sync` args; verify `transport()` calls; confirm musical intent. |
| `CLAUDE.md` "1 cycle = 4 beats" wording | **Modified** | Re-spec'd. Documents `beats_per_cycle` knob. |
| `web/static/docs/**` beat/cycle docs | **Modified** | Doc sweep. |

---

## 6. File-Level Changes

### Cedar (engine)

| File | Change |
|---|---|
| `cedar/include/cedar/vm/context.hpp` | Replace `beat_phase`/`bar_phase` with `cycle_offset`. Add `cps`, `beats_per_cycle`. Add `set_cps()`, `set_beats_per_cycle()`. Update `update_timing()`. Rewrite `samples_per_beat()`/`beat_at_sample()` as derived getters. |
| `cedar/include/cedar/vm/vm.hpp:119, 171, 174` | Drop `cycle_length` from `SEQPAT_TRANSPORT` init path. Add `set_cps()`, `set_beats_per_cycle()`. |
| `cedar/include/cedar/vm/state_pool.hpp:116-138, 477-920` | Drop `cycle_length` from `resolve_output_events()` return, JSON inspector, init signatures. Add `streaming` to JSON. |
| `cedar/include/cedar/opcodes/sequence.hpp:190-356` | Drop `SequenceState::cycle_length`. Drop `cycle_length` param from `query_pattern()`. Add `streaming` flag. |
| `cedar/include/cedar/opcodes/midi.hpp:113-281` | Drop `MidiQueueState::cycle_length`. Replace with `streaming = true` default. |
| `cedar/include/cedar/opcodes/sequencing.hpp:309-816` | Every SEQPAT_* opcode: drop `state.cycle_length` reads (replace with `1.0f` since events are already phase-normalized). Replace `samples_per_beat()` reads with `samples_per_cycle()` where the math previously assumed beats == cycle/4. Free `SEQPAT_TRANSPORT`'s `inputs[3]/[4]`. |
| `cedar/include/cedar/opcodes/delays.hpp:108-145` | `DELAY_SYNC`: change input contract from beats to cycles. `delay_samples = beats_input * samples_per_cycle` (was `samples_per_beat`). Update inline docstring. |
| `cedar/include/cedar/opcodes/soundfont.hpp:303` | Switch to `ctx.samples_per_beat()` derived getter (no observable change; audit-only). |

### Akkado (compiler)

| File | Change |
|---|---|
| `akkado/include/akkado/typed_value.hpp:78, 146` | Drop `cycle_length` from `PatternPayload` and `EventSourcePayload`. Add `streaming` to `EventSourcePayload`. |
| `akkado/src/codegen_patterns.cpp:102-2320` | Delete every `cycle_length = 4.0f` assignment and every `seq_init.cycle_length = ...` / `payload->cycle_length = ...` site (~20 sites). Delete `out_cycle_length` thread through `lower_inner_transform()`. Remove "1 cycle = 4 beats by default (Strudel convention)" comments. |
| `akkado/include/akkado/stdlib.hpp:42` | Re-spec `fn beat(n)` in terms of `beats_per_cycle`. Add `fn beats(n)`, `fn bars(n)`. |
| `akkado/include/akkado/builtins.hpp` | Register `cps`, `beats_per_cycle` as akkado-source-level setters (alongside `bpm`). |
| `akkado/src/codegen_*.cpp` (for `cps =`, `beats_per_cycle =` setter) | Emit setter calls into the program prologue, same path as `bpm =`. |

### Web

| File | Change |
|---|---|
| `web/src/lib/stores/settings.svelte.ts` | Add `beatsPerCycle: number` project setting (default 1). Persist to localStorage. |
| `web/src/lib/components/Settings/` (new control) | Add a numeric input for `beats_per_cycle` in the project settings panel. NOT in the transport widget. |
| `web/wasm/nkido_wasm.cpp` | Plumb `beats_per_cycle` from JS to `ExecutionContext::set_beats_per_cycle()`. |
| `web/static/docs/concepts/clock-system.md` (new or update existing) | Document cycle/CPS as canonical; BPM as user-facing knob; `beats_per_cycle` as the bridge. |
| `web/static/docs/concepts/runtime-controls.md:96` | No-op (`bpm = 100` example still works). |
| `web/static/docs/builtins/beat.md`, `bars.md`, `beats.md`, `delay_sync.md`, `transport.md`, `cps.md` (new), `beats_per_cycle.md` (new) | Doc sweep; new builtins documented. |
| `web/scripts/build:docs` | Rerun after doc updates. |

### Docs (root)

| File | Change |
|---|---|
| `CLAUDE.md` "Clock System" section | Rewrite. Document `beats_per_cycle` (default 1). Mini-notation note kept (`<a b c>` alternation, `.slow/.fast` runtime rescales). Remove "1 cycle = 4 beats". |
| `docs/mini-notation-reference.md`, `docs/mini-notation-implementation.md` | Sweep "beats" wording where it refers to cycle internals (vs. legitimate musical-term uses). |
| `docs/prd-runtime-event-transforms.md` §0.5 | Update once this PRD's status flips to DONE. |

### Tests / experiments

| File | Change |
|---|---|
| `cedar/tests/test_seq_*.cpp`, `test_midi_*.cpp`, `test_delay_*.cpp` | Update assertions where they reference `cycle_length` or beat-unit `delay_sync` args. |
| `akkado/tests/test_pattern_*.cpp`, `test_transport_*.cpp` | Update transport step args; remove cycle_length assertions. |
| `experiments/test_op_seq*.py`, `test_op_delay_sync.py`, `test_op_transport.py` | Update test scaffolds; re-render WAVs and re-check (≥300 s simulated audio). |
| All in-repo `.akk` demo/welcome/test patches | Sweep `delay_sync(...)` args from beats to cycles; verify `transport(...)` calls. Each patch records its decision (`unchanged` / `retuned` / `bpc-set-to-4-for-feel`) in the sweep PR description (mirroring the 2026-05-18 sweep style). |

---

## 7. Implementation Phases

Three sequential phases. Each phase is a single PR with green Catch2 tests + experiment WAVs.

### Phase 1a — `ExecutionContext` field additions (additive, no behavior change)

**Goal**: introduce `cps`, `beats_per_cycle`, `cycle_offset` *alongside* existing `bpm`, `beat_phase`, `bar_phase`. New fields are populated and self-consistent; nothing reads them yet. `set_bpm()` now also writes `cps`. `beats_per_cycle = 1.0f` default but is **unused** until Phase 1b.

**Files**: `cedar/include/cedar/vm/context.hpp`, `cedar/include/cedar/vm/vm.hpp`.

**Tests**: `test_context_bpm_cps_coupling.cpp`, `test_context_cycle_offset.cpp`. No existing tests should change.

### Phase 1b — Migrate opcode call sites to cycle-canonical helpers

**Goal**: every SEQPAT_*, DELAY_SYNC, soundfont, and timing-reader switches from `beat_phase` / `samples_per_beat()` / hardcoded `* 4.0f` to `cycle_offset` / `samples_per_cycle()`. Behavior changes for sites whose math previously assumed "1 cycle = 4 beats" — these now honour `beats_per_cycle` (which is still `1.0f` by default, so the observable effect is the headline 4× speedup at default settings).

**Files**: `cedar/include/cedar/opcodes/sequencing.hpp`, `delays.hpp`, `soundfont.hpp`, `state_pool.hpp`.

**Tests**: existing Catch2 sequence/delay tests retuned to match the new defaults. `experiments/test_op_seq*.py` re-rendered (≥300s simulated).

### Phase 1c — In-repo patch sweep

**Goal**: update every demo/welcome/test `.akk` patch in the repo to either accept the new feel (`retuned`), preserve the old feel (`bpc-4-for-feel` or `wrapped-slow-4`), or confirm `unchanged`. Each patch's decision logged in the sweep-PR description, mirroring the 2026-05-18 cycle-timing-fix style.

**Files**: every `.akk` patch under `web/static/`, `examples/`, `experiments/` that uses `bpm`, `delay_sync`, or `transport`.

**Tests**: manual audition of each patch in the web app; record the decision tag in the PR.

### Phase 2 — Remove `cycle_length` field; SEQPAT_TRANSPORT cleanup; MIDI sentinel removal

**Goal**: drop `cycle_length` from `SequenceState`, `PatternPayload`, `EventSourcePayload`. Free `SEQPAT_TRANSPORT`'s bit-packed input slots. Replace MIDI `1e6f` sentinel with `streaming: bool`. Drop `beat_phase`/`bar_phase` from `ExecutionContext` (now safe — all opcodes updated in Phase 1).

**Files**: cedar sequence.hpp, midi.hpp, sequencing.hpp; akkado typed_value.hpp, codegen_patterns.cpp.

**Tests**: same Catch2 suites; verify JSON state inspector no longer reports `cycle_length`.

### Phase 3 — Akkado builtins + web settings + doc sweep

**Goal**: re-spec `beat()`; add `beats()`, `bars()` stdlib helpers; expose `cps`, `beats_per_cycle` as akkado-source setters; add `beats_per_cycle` web project setting; sweep all `web/static/docs/`, `CLAUDE.md`, `docs/mini-notation-*` files.

**Files**: akkado stdlib.hpp, builtins.hpp; web settings store + UI control; docs sweep.

**Tests**: `bun run check`, `bun run build:docs`, manual UI test of `beats_per_cycle` control.

### Phase 4 (post-PRD) — Update PRD §0.5 references

Once Phase 3 lands, update `docs/prd-runtime-event-transforms.md` status callout in its §0.5 to confirm the prereq is met. Not part of this PRD's deliverable; the runtime-event-transforms PRD owns it.

---

## 8. Edge Cases

### 8.1 `beats_per_cycle = 0` or negative

Reject at setter time. `ExecutionContext::set_beats_per_cycle()` clamps to `max(beats_per_cycle, 1e-3f)` (or rejects with a warning). Web UI input enforces `min=0.0625, max=64`.

### 8.2 `bpm = 0`

Same — clamp at setter. Effectively pauses the transport.

### 8.3 Old patches setting `bpm = 120` with no `beats_per_cycle`

`beats_per_cycle` defaults to 1.0 → `cps = 2.0` → one cycle takes 0.5s. **Different musical feel than before** (used to be: bpm=120 → spb=0.5s, cycle = 2s). This is the headline breaking change.

Old patches that depended on "1 cycle = 4 beats" must either:
- Add `beats_per_cycle = 4` to recover the old feel, OR
- Wrap patterns in `.slow(4)` to recover the old cycle duration, OR
- Accept the new feel (cycles are 4× shorter unless explicitly slowed).

This mirrors the 2026-05-18 per-patch sweep style: each in-repo patch declares its decision.

### 8.4 `beat()` with `beats_per_cycle = 0.5` (fractional)

`beat(4)` with `bpc = 0.5` → `trigger(0.5 / 4) = trigger(0.125)` → one trigger per 8 cycles. Musically: "every 4 beats" where a beat is 2 cycles long. Mathematically consistent.

### 8.5 `delay_sync(in, 0)` and very small delay values

Existing zero-handling preserved. Sub-sample delays clamp to 1 sample.

### 8.6 `cps =` and `bpm =` set in the same program

Setters run in order; last one wins. Both leave `ExecutionContext` in a consistent state (the other field is recomputed).

### 8.7 `beats_per_cycle = 1` setter is a no-op for the default — does it still trigger a `cps` recompute?

Yes; the setter always recomputes. Harmless idempotency.

### 8.8 Hot-swap with changed `beats_per_cycle`

Hot-swap path picks up new `beats_per_cycle` at swap time. Patterns that were in mid-playback re-snap to the new cycle duration at the next cycle boundary. Document this; do not attempt to smoothly cross-fade timing changes.

---

## 9. Testing / Verification

### Per-phase tests

**Phase 1**: existing Cedar Catch2 sequence/delay tests updated to the new defaults. New tests:

- `test_context_bpm_cps_coupling.cpp`: assert `set_bpm(120)` with `bpc=1` yields `cps==2.0`; `set_cps(2.0)` with `bpc=4` yields `bpm==480`; `set_beats_per_cycle(4)` preserves `bpm` and rescales `cps`.
- `test_context_cycle_offset.cpp`: assert `cycle_offset` ramps `[0,1)` over `samples_per_cycle()` samples.

**Phase 2**: 

- `test_sequence_no_cycle_length.cpp`: assert `SequenceState` and `PatternPayload` JSON inspectors no longer emit `cycle_length`. Assert `query_pattern()` produces same events as before given the same parser input.
- `test_midi_streaming_flag.cpp`: assert `MidiQueueState::streaming == true`; assert downstream consumers don't wrap events at any sentinel.

**Phase 3**: 

- `test_stdlib_beats_bars.cpp`: assert `beats(1)` with `bpc=4` returns `0.25`; with `bpc=1` returns `1.0`.
- `test_beat_builtin.cpp`: assert `beat(4)` with `bpc=4` produces 1 trigger per cycle; with `bpc=1` produces 1 trigger per 4 cycles.

### Long-form WAV experiments (≥300s per CLAUDE.md)

- `experiments/test_op_seq_cycles_pure.py`: render `pat("c4 d4 e4 f4")` with `bpm=120` (default `bpc=1` → 0.5s cycles) and with `beats_per_cycle=4` (→ 2s cycles). Confirm both render cleanly for 300s.
- `experiments/test_op_delay_sync_cycles.py`: render `osc("saw", 220) |> delay_sync(@, 0.25) |> out(@)` over 300s. Confirm 1/4-cycle delay at default tempo.
- `experiments/test_op_transport_cycles.py`: re-render the transport step examples with the new cycle-unit step arg.

### Manual verification

- Web app: load each in-repo demo patch. Confirm musical intent matches the sweep PR's per-patch decision log.
- Set `beats_per_cycle = 4` in the project settings UI. Confirm `bpm = 120` patches now feel like the pre-refactor default (1 cycle = 2s instead of 0.5s).
- Set `beats_per_cycle = 1` (default). Confirm a `bpm = 120` patch is Strudel-paced.
- Hot-swap a running patch with a new `beats_per_cycle`. Confirm cycle boundary alignment.

### Build commands

```bash
cmake --build build
./build/cedar/tests/cedar_tests "[cycles-pure]"
./build/akkado/tests/akkado_tests "[cycles-pure]"
cd experiments && ./run_all.sh
cd ../web && bun run check && bun run build && bun run build:docs
```

---

## 10. Migration Story for User Patches

In-repo: hard cutover, sweep all patches in the PR (mirrors 2026-05-18 cycle-timing-fix process). Each patch logs its decision:

| Decision | Meaning |
|---|---|
| `unchanged` | Patch doesn't use `delay_sync` / `transport` and doesn't depend on the old cycle duration. |
| `retuned` | Accepts the new cycle-pure feel; numbers in `delay_sync` etc. ported to cycles by inspection. |
| `bpc-4-for-feel` | Sets `beats_per_cycle = 4` at the top to preserve the old "1 cycle = 4 beats" feel. |
| `wrapped-slow-4` | Wraps pattern in `.slow(4)` to preserve old cycle duration without changing `beats_per_cycle`. |

External user patches: covered in the release CHANGELOG entry. Sample one-line `beats_per_cycle = 4` mitigation called out at the top.

---

## 11. Open Questions

**OQ-1. `bars(n)` definition under cycles-pure with no time-signature concept.**
Current convention: 1 bar = 4 beats. Should `bars(n) = n * 4 / beats_per_cycle` hardcode the 4, or should there be a `beats_per_bar` knob too? Recommendation: hardcode the 4 (musicians' default); if someone wants 3/4 or 7/8, they set `beats_per_cycle` and write `bars(n) = n` style fractions directly. Lock the recommendation or split `bars()` out.

**OQ-2. Setter precedence when both `bpm =` and `cps =` appear at top-level.**
Last writer wins (per §8.6). Confirm this is OK rather than emitting a warning.

**OQ-3. Audit `cycle_length` references in `docs/audits/mini-notation-cycle-timing_audit_2026-05-18.md`.**
Likely no-op (audit is historical), but skim to make sure nothing it references is broken by this refactor.

**OQ-4. Behaviour of `set_beats_per_cycle()` during a block.**
If a setter fires mid-block via `env_map`, do we re-update `cps` before any further opcode reads it, or defer to next block? Recommend: defer to block boundary (consistent with how BPM changes work today).

**OQ-5. `update_timing()` order of operations.**
`cycle_offset` calculation must happen after `bpm` / `cps` / `bpc` are stable for the block. Confirm the existing block-boundary update path handles this; no separate fix needed.

> **Decisions locked from earlier OQs**: field name is `cycle_offset` (matches user-facing `co`); SEQPAT_TRANSPORT freed input slots documented as `unused`; default `beats_per_cycle = 1.0f` (Strudel-pure) with the headline 4× speedup as the accepted breaking change.

---

## 12. Next Step

1. **Resolve §11 open questions.** Promote to "READY FOR IMPLEMENTATION" status only after every OQ has a concrete decision.
2. **Implement Phase 1.** Then Phase 2, then Phase 3 — each merged independently.
3. **Update `docs/prd-runtime-event-transforms.md` §0.5** once Phase 3 lands (confirm prereq met).
4. **Author CHANGELOG entry** flagging the breaking timing change and the `beats_per_cycle = 4` mitigation for external users.

**Do not begin implementation while this PRD is in `FIRST DRAFT` status.**

---

## 13. Rejected Alternatives

Captured here so future re-litigation has the context.

### 13.1 Drop BPM entirely; expose only CPS

**Rejected.** BPM is the universal musician-facing tempo unit; stripping it would be hostile to anyone who learned music outside the Strudel/Tidal world. CPS is canonical *internally* and accessible from akkado source, but the web UI keeps BPM as its primary slider.

### 13.2 Time-signature UI (4/4, 6/8 dropdown driving `beats_per_cycle`)

**Rejected.** User direction: *"no 'time signature' framing — beats per cycle float field pls. Honest about what the math actually is. Less musician-friendly but no fake distinction between 6/8 and 3/4."* A time-sig dropdown would falsely imply 6/8 and 3/4 differ in cycle math (they don't — both = 6 beats per cycle, or 3, depending on user convention). The raw float setter is more honest.

### 13.3 Decouple `beats_per_cycle` from cycles entirely (BPM as pure display)

**Rejected.** Considered: make BPM purely cosmetic, with cycles the only clock unit and no `beats_per_cycle` field at all. Cleaner, but loses the bridge to musical effects (`delay_sync` in beats, `beats(n)` stdlib). The configurable `beats_per_cycle` knob is the compromise.

### 13.4 Remove the `beat()` builtin

**Considered in Round 1, then reversed.** Initial direction was to delete `beat()` and have users write `trigger(n)` directly. User reversed in Round 4: *"we keep beat, no deprecation warnings or new error codes necessary."* `beat()` stays, re-spec'd in terms of `beats_per_cycle`. Honors the soft-cutover preference even though the rest of the refactor is hard-cutover.

### 13.5 Keep `cycle_length` as a per-pattern field, just rename it

**Rejected.** The field is always `4.0f` in current code paths (or `1e6f` sentinel for MIDI). It carries no information that isn't already encoded elsewhere (parser normalization + `beats_per_cycle`). Renaming would preserve the dead plumbing; removal is the honest move.

### 13.6 Default `beats_per_cycle = 4` to preserve old patch feel

**Rejected.** Considered; would have made the refactor invisible to existing patches. But the cycles-pure goal explicitly states Strudel-pure as the target, and shipping a non-default that doesn't match Strudel would mean every external patch *that wants Strudel-pure* has to set `beats_per_cycle = 1`. The breaking change is the lesser evil; mitigation is one line.

### 13.7 Deprecation warnings + auto-translate for one release

**Rejected.** The user picked hard cutover. Matches the 2026-05-18 cycle-timing-fix style: coordinated PR with per-patch decision log, no aliases, no warnings.

### 13.8 Web transport widget redesign (CPS slider, BPM derived display)

**Rejected.** User direction: *"the ui is already perfect."* No widget changes. `beats_per_cycle` lives in a project settings panel, not the transport.

### 13.9 Add a `Time` value type that pretty-prints in beats/bars/cycles

**Rejected as out of scope.** Considered for handling beat/cycle/ms conversions ergonomically. Adds type-system work disproportionate to the value over plain float fractions + `beats(n)` / `bars(n)` helpers. Re-litigate in a separate PRD if friction surfaces.
