**Status: IN PROGRESS — Phase 1 SHIPPED** (SF_VOICE opcode + `sf_voice` builtin
+ `$soundfont_alias` directive + `--soundfont-alias` CLI flag, all additive;
`SOUNDFONT_VOICE` untouched). Phases 2–5 not started; Phase 3 deferred pending
[`prd-poly-callback-event-record.md`](prd-poly-callback-event-record.md).

Execution PRD that consolidates planned future work from [`prd-polyphony-system.md`](prd-polyphony-system.md) §7 ("Future: SF_PLAY", "Configurable Voice Release"), [`prd-poly-callback-event-record.md`](prd-poly-callback-event-record.md), and [`prd-midi-input.md`](prd-midi-input.md) §7.1 into one unified soundfont/poly refactor. Ships a single-voice `SF_VOICE` opcode, per-event MIDI `channel`/`program` fields, envelope-done voice stealing, and a userspace stdlib sugar that lets the old `soundfont(input, file, preset)` API keep working with zero codegen special-casing.

# Soundfont / Poly Unification PRD — collapse soundfont's parallel voice allocator into `poly`

## Executive Summary

Today the SoundFont player is the most "diverse" instrument surface in nkido. It carries its own 32-voice allocator, its own 3-tier stealing policy, *two* input paths inside one opcode (buffer-driven gate/freq/vel + event-driven MidiQueueState drain), and a `consumes_polyphonic_pattern = true` flag that triggers a special-case path in codegen. None of this is necessary once `poly` exists.

This PRD splits `SOUNDFONT_VOICE` into:

1. A new single-voice **`SF_VOICE`** opcode — the per-voice inner loop (zone lookup, sample interpolation, per-voice SVF filter, DAHDSR envelope) with no voice management — wrapped in the standard `(freq, gate, vel) -> signal` instrument convention so it slots into `poly` like any other instrument.
2. A new **envelope-done signal** that lets `poly` free a voice when its envelope finishes, restoring the SF-aware stealing intelligence that today's pool has but generic poly doesn't.
3. Per-event **`channel`** and **`program`** fields on `OutputEvent`, populated from MIDI source (live + file) by preserving program-change events through the parser, so a single `poly(...)` call can route a multi-channel General MIDI file to the right preset per channel.
4. A userspace **akkado stdlib sugar** that keeps the old `n"…" |> soundfont(@, file, preset)` API working as a one-liner — `fn soundfont(input, file, preset) = input |> poly(@, sf_voice(file, preset, _, _, _))` — collapsing all special-case codegen into ordinary user-level akkado.

### Key design decisions (from the question rounds, 2026-05-17)

- **Opcode name**: `SF_VOICE` (mirrors `SOUNDFONT_VOICE`, reads as "the single-voice version").
- **Builtin name**: `sf_voice(file, preset, freq, gate, vel)` — stereo output.
- **Zone semantics**: 1 SF_VOICE = 1 note. All matched zones (velocity layers, key splits) fire and mix internally; the user sees one logical voice per note.
- **Output**: stereo-native always (consistent with `poly`'s convention; SF2 pan/stereo-pair metadata respected).
- **Stealing**: envelope-done signaling owned by this PRD, supersedes [`prd-polyphony-system.md`](prd-polyphony-system.md) §7 "Configurable Voice Release" Option 2.
- **Migration of old `soundfont()`**: collapse to userspace stdlib; remove `SOUNDFONT_VOICE` opcode, the `consumes_polyphonic_pattern` flag, the codegen special-case, and the two-path state.
- **Alias resolution**: both directive (`$soundfont_alias`) and runtime config (CLI / web UI), runtime as fallback.
- **MIDI program-change**: live + file both honored; per-channel program state tracked in `MidiQueueState`.
- **GM drum routing**: stdlib helper `gm_route(channel, program)` — no opcode-level magic.

---

## 1. Problem Statement / Current State

### 1.1 What exists today

| Component | Role | Source |
|---|---|---|
| `SOUNDFONT_VOICE` opcode | Self-contained polyphonic SoundFont player with hard-coded 32-voice pool | `cedar/include/cedar/opcodes/soundfont.hpp:246-667` |
| `SoundFontVoiceState` | 32-slot voice pool + 3-tier stealing (free → quietest releasing → oldest) | `cedar/include/cedar/opcodes/dsp_state.hpp:616-708` |
| Two input paths inside one opcode | (a) buffer-driven gate/freq/vel, (b) event-driven OutputEvents drain from `MidiQueueState` | `soundfont.hpp:293-376` (event path), `soundfont.hpp:378-667` (buffer path) |
| `consumes_polyphonic_pattern = true` flag | Special-cases `soundfont` in codegen; allows chord patterns directly | `akkado/include/akkado/builtins.hpp:460-464` |
| `poly` opcode | Generic polyphonic voice allocator; default 64 voices, runtime config | `cedar/src/vm/vm.cpp:280` (`execute_poly_block`) |
| MIDI program-change events | **Discarded at parse time** | `cedar/include/cedar/io/midi_sequence.hpp:87` |
| `OutputEvent` | Carries `time`, `duration`, `velocity`, `chance`, `midi_note`, `values[]`, `prop_vals[]`. No `channel`, no `program`. | `cedar/include/cedar/opcodes/sequence.hpp:123-138` |
| MIDI channel filter | Per-`midi()` filter at drain time (drops non-matching notes); channel is not an event attribute | `cedar/src/opcodes/midi.cpp:144-146, 302-303` |

### 1.2 Limitations

1. **Two voice allocators solve the same problem.** `SOUNDFONT_VOICE`'s 32-voice pool duplicates work already done by `poly`. Sampler does the same. Polyphony PRD §1 explicitly calls this "three separate, incompatible polyphony mechanisms" and treats SF_PLAY / SAMPLE_VOICE as the unification path.
2. **The event-driven path inside SOUNDFONT_VOICE exists only because** soundfont can't be used as an instrument inside `poly`. It's an architectural workaround.
3. **Multi-channel MIDI routing is impossible in a single chain.** GM `.mid` files put different instruments on different channels and select them via program-change. Today users must spawn one `midi({channel:N}) |> ...` chain per channel because (a) `channel` isn't on the event record and (b) program-change is dropped before it can reach userspace.
4. **The `consumes_polyphonic_pattern` flag and codegen special-case** exist solely to make soundfont accept patterns directly. Once soundfont is "just an instrument," the flag and special-case can go.

### 1.3 Why this is the right moment

Three already-planned PRDs converge here:

- [`prd-polyphony-system.md`](prd-polyphony-system.md) (Status: IMPLEMENTED Phases 0–5) §1.2, §4.4, §7 specify SF_PLAY as a single-voice extraction of SOUNDFONT_VOICE's inner loop, to be wrapped in poly via a `soundfont(file)`-as-instrument-factory. §7 also discusses envelope-done signaling as "Configurable Voice Release" Option 2.
- [`prd-poly-callback-event-record.md`](prd-poly-callback-event-record.md) (Status: NOT STARTED) specifies record-destructure callbacks `({freq, gate, vel, ...}) ->` that make custom event fields reachable from inside a poly voice. Phase 3 of *this* PRD (GM routing) requires the destructure form.
- [`prd-midi-input.md`](prd-midi-input.md) (Status: SHIPPED) ships `midi()` and OutputEvents emission but leaves per-event channel/program handling as an open extension point.

This PRD is the execution PRD that consolidates all three.

---

## 2. Goals and Non-Goals

### Goals

1. **Engine simplification.** One voice allocator (`poly`). No `SOUNDFONT_VOICE` opcode. No two-path event/buffer state. No `consumes_polyphonic_pattern` flag. No codegen special-case for `soundfont()`.
2. **Language consistency.** `poly(@, sf_voice(...))` works exactly like `poly(@, osc(...))` or any other instrument. No user-visible asymmetry between soundfont and other voices.
3. **GM file routing.** A single `poly(...)` chain can play a multi-channel GM `.mid` file with per-channel program selection driven by the file's own program-change events.
4. **Per-event `channel` and `program` fields** on `OutputEvent`, populated from both live MIDI and file MIDI; overridable from pattern source via mini-notation record suffix `{channel:N, program:M}`.
5. **Envelope-done voice stealing.** `poly` learns to free voices when their envelope finishes (not just oldest/quietest-releasing heuristics), so SF_VOICE stealing is at least as audibly correct as today's `SOUNDFONT_VOICE` pool.
6. **Backward compatibility for the old API.** `n"…" |> soundfont(@, file, preset)` keeps working unchanged, via a one-line userspace akkado stdlib function.

### Non-Goals

- **`SAMPLE_VOICE` (sampler equivalent).** Parallel work, owned by [`prd-polyphony-system.md`](prd-polyphony-system.md) §7 "Future: SAMPLE_VOICE". Same shape; can adopt envelope-done signaling once it lands.
- **Real-time MIDI CC routing into SF_VOICE params** (cutoff, vibrato depth, etc.). Future work.
- **Multi-output / multi-bus SF2 routing.** Future work.
- **Language features owned by other PRDs**: record-destructure callbacks ([`prd-poly-callback-event-record.md`](prd-poly-callback-event-record.md)), closure-pipe operator ([`prd-closure-pipe-operator.md`](prd-closure-pipe-operator.md)), partial-application refinements ([`prd-advanced-functions.md`](prd-advanced-functions.md), already DONE).
- **SF3 (Ogg-Vorbis-compressed) playback parity** — out of scope unless already covered by `prd-soundfonts-sample-banks.md`.

---

## 3. Target Syntax / User Experience

### 3.1 Single-voice SF_VOICE inside an explicit lambda (works today, before any language extensions)

```akkado
// One soundfont voice per pattern note, full poly machinery
n"c4 e4 g4 b4"
    |> poly(@, (f, g, v) -> sf_voice("piano.sf2", 0, f, g, v))
    |> out(@, @)
```

### 3.2 Partial-application shorthand — NOT available

> **Correction (Phase 1, 2026-05-20):** the `_` placeholder is **not** partial
> application. In akkado `_` only substitutes a builtin's *default value* at
> that argument slot (`akkado/src/codegen.cpp`), so `sf_voice("piano.sf2", 0,
> _, _, _)` does **not** produce a `(freq, gate, vel) -> signal` callable for
> `poly`. Use the explicit lambda form of §3.1. The stdlib `fn soundfont`
> sugar (§3.3 / §4.6) must likewise be written with an explicit lambda.

### 3.3 Old `soundfont(input, file, preset)` API still works (userspace stdlib sugar)

```akkado
// Unchanged from today's user-facing API
n"C4' Am7'" |> soundfont(@, "gm", 0) |> out(@, @)

// Lowering happens in the akkado stdlib (Phase 4) — explicit lambda, since
// `_` is not partial application (see §3.2):
fn soundfont(input, file, preset) =
    input |> poly(@, (f, g, v) -> sf_voice(file, preset, f, g, v), 64)
```

### 3.4 GM MIDI file routing (Phase 3 — depends on `prd-poly-callback-event-record.md`)

The motivating use case:

```akkado
// Load a General MIDI .mid file. Each note carries its source channel and
// the currently-selected program for that channel. Route to the matching
// SF2 preset using the stdlib gm_route helper.
midi({file: "song.mid"})
    |> poly(@, ({freq, gate, vel, channel, program}) -> {
           bank_preset = gm_route(channel, program);
           sf_voice("gm", bank_preset, freq, gate, vel)
       }, 64)
    |> out(@, @)
```

`gm_route(channel, program)` is a userspace stdlib helper that returns the drum bank (128) when `channel == 9` (zero-indexed channel 10) and the melodic program otherwise.

### 3.5 Alias registration

Two ways to bind `"gm"` (or any alias) to a SoundFont file:

```akkado
// Akkado top-level directive — per-program, resets on recompile
$soundfont_alias("gm", "static/soundfonts/FluidR3_GM.sf2")
$soundfont_alias("strings", "static/soundfonts/strings_orch.sf2")
```

```bash
# Runtime config (fallback when no directive is present)
nkido render --soundfont-alias gm=/path/to/FluidR3_GM.sf2 input.akk
```

The web UI exposes an equivalent settings panel. Resolution order: directive → runtime config → error (`E5xx: unknown soundfont alias "gm"`).

### 3.6 Pattern-source overrides for channel and program

When the event source is a typed pattern literal or `chord()`, `@.channel` and `@.program` default to `0`. Mini-notation record-suffix syntax overrides per note:

```akkado
// Multi-instrument arrangement driven by patterns rather than a .mid file
n"c4{channel:1, program:0} d4{channel:2, program:24} e4{channel:9, program:0}"
    |> poly(@, ({freq, gate, vel, channel, program}) ->
        sf_voice("gm", gm_route(channel, program), freq, gate, vel))
    |> out(@, @)
```

### 3.7 Envelope-done stealing semantics (Phase 2.5)

```akkado
// Default poly already does envelope-done stealing — no syntax change
// SF_VOICE writes its DAHDSR level to the done-signal buffer each block.
// When level drops below threshold (e.g., -60 dB), poly considers the voice
// free for reuse, even though gate is still nominally on.
n"c1 c1 c1 c1" |> poly(@, sf_voice("gm", 0, _, _, _), 4) |> out(@)
```

For instruments that should be force-released by gate-off only (older behavior), the existing `release:` parameter on `poly` retains the time-based release window from `prd-polyphony-system.md` §7.

---

## 4. Architecture / Technical Design

### 4.1 `SF_VOICE` opcode

**Signature** (akkado-facing):

```
sf_voice(file_or_alias, preset, freq, gate, vel) -> (L, R)
```

| Param | Type | Description |
|---|---|---|
| `file_or_alias` | string | Either a literal file path (`"static/sf/piano.sf2"`) or an alias registered via `$soundfont_alias` / runtime config |
| `preset` | int (0–16383 for SF2 bank*128+program) | Preset index within the bank. Resolved at codegen via `SoundFontRegistry::get_preset_by_index`. For drum bank routing use `gm_route()`. |
| `freq` | signal | Per-sample frequency (Hz) |
| `gate` | signal | Per-sample gate (0/1) |
| `vel` | signal | Per-sample velocity (0..1) |

**Output**: stereo pair (2 buffers, L then R, adjacent — consistent with `poly`'s `voice_out_buf` convention).

**Internals** — factored verbatim from the per-voice section of `op_soundfont_voice` (`cedar/include/cedar/opcodes/soundfont.hpp:246-667`), minus the 32-slot voice allocator:

- On rising edge of gate (per-sample): call `bank->find_zones(preset, note, vel, zones[], MAX_ZONES_PER_NOTE)`. `note` derived from `freq` via `midi_note = round(69 + 12*log2(freq/440))`.
- For each matched zone: initialize a per-zone sub-voice (sample read position, SVF filter state, DAHDSR envelope). Zones share the parent SF_VOICE state slot.
- Per-sample: sum sub-voice outputs into the stereo output pair, respecting per-zone pan (mono samples) and stereo-pair linkage (L/R-tagged samples land in their respective output channels).
- On gate falling edge: trigger DAHDSR release on all sub-voices.
- **Envelope-done buffer**: write the parent SF_VOICE's "done" signal to a dedicated buffer slot (see §4.3).

**State struct**: `SFVoiceState` in `cedar/include/cedar/opcodes/dsp_state.hpp`. Contains an array of up to `MAX_ZONES_PER_NOTE = 8` sub-voice slots (sample position, SVF state, envelope state per zone). Bounded constant; no dynamic allocation.

### 4.2 SOUNDFONT_VOICE opcode removal

- Remove `SOUNDFONT_VOICE` opcode entry from `cedar/include/cedar/vm/instruction.hpp`.
- Remove `op_soundfont_voice` body from `cedar/include/cedar/opcodes/soundfont.hpp` (replaced by SF_VOICE, which is its single-voice inner loop).
- Remove `SoundFontVoiceState` 32-slot pool from `cedar/include/cedar/opcodes/dsp_state.hpp:616-708`.
- Remove `consumes_polyphonic_pattern` flag and its codegen special-case (search for usages in `akkado/src/codegen*.cpp`).
- Remove the `Opcode::NOP` registration for `soundfont` from `akkado/include/akkado/builtins.hpp:460`.

The userspace stdlib function (§4.6) restores the old API surface.

### 4.3 Envelope-done signaling

A new convention added to `poly`'s instrument body contract:

- **Buffer convention**: each voice block reserves one additional scratch buffer, `voice_done_buf`. Instruments that participate in envelope-done stealing write a single per-block scalar (or per-sample stream) to this buffer indicating "this voice is audibly done." For SF_VOICE: the max DAHDSR envelope level across all active sub-zones, in linear amplitude.
- **`poly` behavior**: at each block boundary, `execute_poly_block` reads `voice_done_buf` per voice. When `max_value < DONE_THRESHOLD` (e.g., 1e-4 ≈ -80 dB) AND `gate == 0` (already released), poly marks the voice slot reusable, identical to today's "voice fully released" state but determined by the actual envelope rather than a fixed timeout.
- **Opt-out**: instruments that don't write to `voice_done_buf` (most existing instruments) leave it zero; poly falls back to today's `release:` time-based window (or gate-off-only when no release window is configured). No regression for existing patches.
- **Stealing**: when allocating a new voice and no slot is free, prefer (a) slots flagged done by their envelope, then (b) oldest released, then (c) oldest active (today's policy).

The polyphony PRD §7 "Configurable Voice Release" Option 2 is **superseded** by this section; update the cross-reference there.

### 4.4 Per-event `channel` and `program` fields

**`OutputEvent` struct change** (`cedar/include/cedar/opcodes/sequence.hpp:123`):

```cpp
struct OutputEvent {
    // ... existing fields ...
    std::uint8_t channel = 0;   // 0..15 (zero-indexed MIDI channel); 0 if non-MIDI
    std::uint8_t program = 0;   // 0..127 (MIDI program); 0 if non-MIDI
};
```

Re-verify struct alignment + size after the addition. Bump the `OutputEvents` arena math if needed.

**MIDI parser** (`cedar/include/cedar/io/midi_sequence.hpp:87`): stop discarding program-change (`0xC0`). Add a `ProgramChange` event type to `MidiSequence` with `{tick, channel, program}`. Sort with note events on the timeline.

**`MidiQueueState`**: add `std::uint8_t channel_program[16]` tracking the current program per channel. On parser load, walk the program-change events sequentially and apply them; on live MIDI, update on each 0xC0 byte. When emitting an `OutputEvent` for a note, populate `evt.channel = note.channel` and `evt.program = channel_program[note.channel]`.

**Live MIDI** (`cedar/src/opcodes/midi.cpp`): same logic — program-change byte updates `channel_program[chan]`; next note-on reads the current value.

**Field-access aliases** (`akkado/src/codegen.cpp`): register `channel`, `c` for channel; `program`, `prog`, `n` for program. Update the available-fields list at `codegen.cpp:2623` (the error message that lists valid field names).

**Pattern-source override**: when a pattern event carries a record-suffix like `c4{channel:2, program:24}`, the values land in `evt.channel` and `evt.program` directly (or via `prop_vals[]` if we choose to keep the existing custom-field path; design detail decided during implementation).

### 4.5 Alias resolution

**`SoundFontRegistry` extension**: add an alias map `std::unordered_map<std::string, std::string> aliases_`. Lookup order:

1. Treat the input as a file path; if it exists, load it.
2. Look up in `aliases_`; if found, recursively resolve the value as a path or alias.
3. Else error `E5xx: unknown soundfont alias "..."`.

**Directive** `$soundfont_alias("gm", "path/...")`: handled at akkado top level (parser already supports `$directive` syntax — see `$polyphony` precedent). The codegen emits a `SOUNDFONT_REGISTER_ALIAS` initialization instruction (or, simpler, the codegen calls a registry hook at compile time and emits no instruction — design detail).

**Runtime config**:
- CLI: `nkido render --soundfont-alias gm=/path/to/file.sf2 [--soundfont-alias drums=/path/to/...]`
- Web UI: settings panel with key/path pairs persisted in localStorage (theme-store pattern).
- Both populate the registry's `aliases_` map at startup. Directives override.

### 4.6 Akkado stdlib additions

A new file under `akkado/stdlib/` (or wherever userspace akkado lives — TBD during implementation):

```akkado
// Old soundfont() API restored as a one-liner over the new SF_VOICE
fn soundfont(input, file, preset) =
    input |> poly(@, sf_voice(file, preset, _, _, _), 64)

// GM channel/program -> bank*128+program mapping
// Channel 10 (zero-indexed 9) is the drum kit; bank 128 in standard GM SF2s.
fn gm_route(channel, program) =
    channel == 9 ? 128 * 128 + 0 : 0 * 128 + program
```

(The exact `bank * 128 + program` encoding depends on how `SoundFontRegistry::get_preset_by_index` expects bank/program — finalize during implementation. If preset index is opaque, replace with a `gm_preset(channel, program)` helper that calls into the registry.)

---

## 5. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `SOUNDFONT_VOICE` opcode | **Removed** | Inner loop becomes `SF_VOICE`; 32-slot pool deleted |
| `SoundFontVoiceState` 32-slot pool | **Removed** | Replaced by per-instance `SFVoiceState` (single voice + N sub-zones) |
| `SF_VOICE` opcode | **New** | Single-voice; freq/gate/vel inputs; stereo out; envelope-done buffer |
| `consumes_polyphonic_pattern` flag | **Removed** | No longer needed once `soundfont()` is userspace |
| Codegen special-case for `soundfont` | **Removed** | `soundfont` is now an ordinary userspace function |
| `poly` opcode | **Modified** | Envelope-done buffer convention; per-voice "done" signal read each block |
| `OutputEvent` struct | **Modified** | Adds `channel`, `program` fields (u8 each) |
| MIDI parser | **Modified** | Preserves program-change events; surfaces them via `MidiQueueState` |
| `MidiQueueState` | **Modified** | Tracks per-channel program; populates `evt.channel` / `evt.program` |
| `SoundFontRegistry` | **Modified** | Adds alias map + lookup; runtime config wiring |
| Akkado field aliases | **Modified** | `channel`, `c`, `program`, `prog`, `n` registered |
| Mini-notation record suffix | **Modified** | `{channel:N, program:M}` recognized; routed to event struct |
| Akkado stdlib | **New** | `fn soundfont(...)` + `fn gm_route(...)` |
| `$soundfont_alias` directive | **New** | Parser already accepts `$directives`; new directive name |
| CLI flag `--soundfont-alias` | **New** | One per alias; comma-separated also acceptable |
| Web UI settings | **New** | Settings panel for alias key/path pairs |
| Old `n"…" |> soundfont(@, file, preset)` patches | **Compatible** | Userspace stdlib `fn soundfont` restores the API |
| Live MIDI patches using `midi() |> soundfont(@, ...)` | **Compatible** | Same stdlib sugar; `poly` happily consumes `midi()` events |

---

## 6. File-Level Changes

### Cedar (engine)

| File | Change |
|---|---|
| `cedar/include/cedar/vm/instruction.hpp` | Add `SF_VOICE` opcode enum value; remove `SOUNDFONT_VOICE` |
| `cedar/src/vm/vm.cpp` | Register `SF_VOICE` dispatch in opcode switch; remove `SOUNDFONT_VOICE`; extend `execute_poly_block` to read `voice_done_buf` (envelope-done) |
| `cedar/include/cedar/opcodes/soundfont.hpp` | New `op_sf_voice(...)` function (per-voice loop extracted from old `op_soundfont_voice`); delete `op_soundfont_voice` |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | New `SFVoiceState` (single voice, up to MAX_ZONES_PER_NOTE sub-zones); delete `SoundFontVoiceState` 32-slot pool; extend `PolyAllocState` with envelope-done tracking |
| `cedar/include/cedar/opcodes/sequence.hpp` | Add `std::uint8_t channel`, `std::uint8_t program` to `OutputEvent` |
| `cedar/include/cedar/opcodes/sequencing.hpp` | Update `OutputEvents::add()` overloads to accept channel/program; update emitters in `op_seqpat_step` |
| `cedar/include/cedar/io/midi_sequence.hpp` | Add `ProgramChange` event type; stop discarding 0xC0; preserve in unified timeline |
| `cedar/src/opcodes/midi.cpp` | Per-channel program tracking in `MidiQueueState`; populate `evt.channel`/`evt.program` on emission (live + file paths) |
| `cedar/include/cedar/io/soundfont_registry.hpp` (or wherever the registry lives) | Add alias map + lookup + runtime-config loader |
| `cedar/include/cedar/vm/state_pool.hpp` | If `MAX_STATES` is hit by typical GM-routing patches: bump to 1024 (polyphony PRD §6 already flagged this) |
| `cedar/include/cedar/generated/opcode_metadata.hpp` | Regenerate via `bun run build:opcodes` (auto) |

### Akkado (compiler + stdlib)

| File | Change |
|---|---|
| `akkado/include/akkado/builtins.hpp` | Register `sf_voice` (input_count=3, optional_count=2, requires_state=true, stereo_native=true); remove `soundfont` entry (now userspace) |
| `akkado/src/codegen.cpp` | Remove `consumes_polyphonic_pattern` flag path; remove `soundfont`-specific codegen; register `channel`/`program` field aliases; update available-fields list at `:2623` |
| `akkado/src/codegen_functions.cpp` | No change to `handle_poly_call`; verify SF_VOICE inlines cleanly through existing path |
| `akkado/src/parser*` | Add `$soundfont_alias` directive handling (if directive infrastructure is hand-coded per-name); confirm mini-notation `{channel:N, program:M}` already lands in `prop_vals[]` or extend if needed |
| `akkado/stdlib/soundfont.akk` (new) | `fn soundfont(input, file, preset) = ...` + `fn gm_route(channel, program) = ...` |
| `akkado/include/akkado/stdlib_loader.hpp` (or equivalent) | Ensure the new stdlib file is loaded automatically |

### Tools (CLI + Web)

| File | Change |
|---|---|
| `tools/nkido-cli/main.cpp` | Add `--soundfont-alias key=path` flag (repeatable); populate registry before compile |
| `tools/akkado-cli/main.cpp` | Same flag for consistency |
| `web/src/lib/stores/settings.svelte.ts` | Persistent alias key/path map |
| `web/src/lib/components/Settings/SoundFontAliases.svelte` (new) | Simple key/path editor |
| `web/wasm/nkido_wasm.cpp` | Expose `register_soundfont_alias(key, path)` to JS |

### Tests & Experiments

| File | Change |
|---|---|
| `akkado/tests/test_codegen.cpp` | New tests: SF_VOICE single-voice playback; SF_VOICE via poly with retrigger regression; channel/program field access; pattern record-suffix override; alias resolution failure path |
| `experiments/test_op_sf_voice.py` (new) | Single-voice WAV; stereo verification (per-channel RMS); ≥300s simulated audio (CLAUDE.md rule); compare against legacy `SOUNDFONT_VOICE` output if available during transition |
| `experiments/test_envelope_done_stealing.py` (new) | Verify SF_VOICE with long release isn't stolen until envelope finishes |
| `experiments/test_gm_route.py` (new) | `gm_route(9, 0)` → drum bank; `gm_route(0, 5)` → melodic bank 5; round-trip via compile + render |

### Documentation

| File | Change |
|---|---|
| `web/static/docs/reference/builtins/soundfont.md` | Replace `SOUNDFONT_VOICE` doc with `SF_VOICE`; show userspace `soundfont` sugar; document GM routing worked example |
| `web/static/docs/concepts/soundfont-gm-routing.md` (new) | Full GM `.mid` routing tutorial; alias setup; drum routing; pattern-source overrides |
| `web/static/docs/reference/directives/soundfont-alias.md` (new) | `$soundfont_alias` reference |

### Cross-PRD reverse references (one Edit per file, after this PRD lands)

| File | Edit |
|---|---|
| `docs/prd-polyphony-system.md` §7 "Future: SF_PLAY..." | Prepend: "**Execution PRD:** [`prd-soundfont-poly-unification.md`](prd-soundfont-poly-unification.md). Renamed `SF_PLAY` → `SF_VOICE` to match family naming." |
| `docs/prd-polyphony-system.md` §7 "Configurable Voice Release" Option 2 | Note: "Superseded by [`prd-soundfont-poly-unification.md`](prd-soundfont-poly-unification.md) §4.3 (envelope-done signaling)." |
| `docs/prd-poly-callback-event-record.md` §1 | Add: "Phase 3 of [`prd-soundfont-poly-unification.md`](prd-soundfont-poly-unification.md) depends on the record-destructure form shipping here first." |
| `docs/prd-midi-input.md` §2 | Add: "Per-event `channel`/`program` fields and program-change preservation are scoped under [`prd-soundfont-poly-unification.md`](prd-soundfont-poly-unification.md) §4.4." |
| `docs/prd-soundfonts-sample-banks.md` §5 Phase 4 | Add a "Follow-up" note pointing to this PRD. |
| `docs/prd-soundfont-playback-fixes.md` §1 | Add: "Retrigger semantics established here must survive the SF_VOICE refactor — see [`prd-soundfont-poly-unification.md`](prd-soundfont-poly-unification.md) §9 verification check 2." |

---

## 7. Implementation Phases

### Phase 1 — `SF_VOICE` opcode + builtin + alias table — ✅ SHIPPED (2026-05-20)
**Goal**: `n"c4 e4 g4" |> poly(@, (f,g,v) -> sf_voice("piano.sf2", 0, f, g, v)) |> out(@, @)` plays correctly.

Shipped, purely additive (`SOUNDFONT_VOICE` and the old `soundfont()` path untouched):

- ✅ `SF_VOICE = 66` opcode (`instruction.hpp`, `vm.cpp` dispatch).
- ✅ `op_sf_voice` + `trigger_sf_voice_subzones` + `process_sf_voice_one_sample_stereo`
  in `soundfont.hpp`, factored from the per-voice loop of `op_soundfont_voice`.
- ✅ `SFVoiceState` (single voice + up to 8 sub-zones, **arena-allocated** to keep
  the `DSPState` variant small — mirrors `SoundFontVoiceState`); registered in the variant.
- ✅ `sf_voice` builtin (`builtins.hpp`) + custom codegen handler `handle_sf_voice_call`
  (`codegen_patterns.cpp`) — resolves `file`/`preset` literals at compile time, emits
  one stereo `SF_VOICE`. Errors `E520`–`E522`.
- ✅ `$soundfont_alias` directive (resolved at **codegen time** — no `CompileResult`
  field needed) + CLI `--soundfont-alias name=path` flag with cycle-guarded resolution.
- ✅ Stereo output (adjacent L/R buffer pair, `STEREO_OUTPUT` flag). **Phase 1 ships
  pan-only stereo** (mono mix balanced by `zone.pan`); true SF2 stereo-pair sample
  linkage is deferred to a later phase (see §8.9 / Open Question follow-up).
- ✅ `bun run build:opcodes` regenerated metadata (`SF_VOICE` marked stateful).

**Tests** (this phase): ✅ `[sf-voice]` codegen tests in `test_codegen.cpp` (poly-instrument
compile, stereo flag + wired inputs, `$soundfont_alias` resolution, `E520`/`E521` error
paths, standalone use); ✅ `experiments/test_op_sf_voice.py` (stereo correctness, note-onset
retrigger check, 300 s long-run stability).

### Phase 2 — Per-event `channel` + `program` fields
**Goal**: MIDI sources (live + file) populate `@.channel` and `@.program` on every emitted event.

- Add `channel`, `program` u8 fields to `OutputEvent` (`sequence.hpp`).
- Update `OutputEvents::add()` overloads.
- MIDI parser: preserve program-change (`midi_sequence.hpp:87`); `ProgramChange` event type; merged timeline.
- `MidiQueueState`: per-channel `program[16]`; populated on emission.
- Live MIDI: 0xC0 handler updates the same state.
- Akkado field aliases (`channel`/`c`, `program`/`prog`/`n`); available-fields list update.
- Mini-notation `{channel:N, program:M}` override path.

**Tests** (this phase): `.mid` file with program-change emits correct programs; pattern record-suffix override; live MIDI program-change simulation.

### Phase 2.5 — Envelope-done signaling in `poly`
**Goal**: SF_VOICE writes its envelope level to `voice_done_buf`; `poly` frees the voice when level < threshold AND gate-off.

- Reserve `voice_done_buf` slot per voice in poly's voice-buffer layout (`execute_poly_block`, `vm.cpp`).
- SF_VOICE writes max sub-zone envelope level per block (or per-sample stream — finalize).
- `PolyAllocState` tracks done flag per slot.
- Stealing policy updated: done-flagged slots preferred over oldest released.
- Existing instruments unaffected (don't write to buffer → zero → today's behavior).
- Supersede `prd-polyphony-system.md` §7 "Configurable Voice Release" Option 2 (cross-ref update).

**Tests** (this phase): long-release SF_VOICE under poly pressure; verify not stolen until done; A/B against generic stealing.

### Phase 3 — GM routing end-to-end
**Goal**: A real GM `.mid` file plays correctly through one `poly(...)` call.

- Userspace stdlib: `fn gm_route(channel, program) = ...`.
- Worked example in `web/static/docs/concepts/soundfont-gm-routing.md`.
- **Depends on**: [`prd-poly-callback-event-record.md`](prd-poly-callback-event-record.md) shipping (record-destructure callbacks are needed for the `{freq, gate, vel, channel, program}` callback form). Bridge: positional 5-prefix `(freq, gate, vel, ..., channel, program)` workaround documented if the destructure PRD hasn't shipped yet — exact positions per the canonical order in that PRD §2.1.
- Test fixture: a known GM `.mid` with program-changes on at least 3 channels including drum channel 10.

**Tests** (this phase): render the fixture; verify audibly distinct instruments per channel; drum channel produces percussion; WAV saved for human listening.

### Phase 4 — Migration: collapse `soundfont()` to userspace + delete old opcode
**Goal**: Remove all SOUNDFONT_VOICE-specific machinery; existing patches continue to work.

- Add `fn soundfont(input, file, preset) = input |> poly(@, sf_voice(file, preset, _, _, _), 64)` to akkado stdlib.
- Remove `SOUNDFONT_VOICE` from `instruction.hpp` + `vm.cpp` + `soundfont.hpp` + `dsp_state.hpp`.
- Remove `consumes_polyphonic_pattern` flag + codegen special-case.
- Remove `soundfont` registration from `builtins.hpp` (userspace fn picks up the name).
- Run all existing soundfont tests / examples — must pass unchanged.

**Tests** (this phase): all pre-existing `soundfont(...)` examples in tests, docs, and the web app produce equivalent output (within float tolerance — small differences from generic vs SF-pool stealing are acceptable if envelope-done is enabled).

### Phase 5 — Polish, docs, deprecation cleanup
**Goal**: Web UI alignment, full GM tutorial, cross-PRD reverse references.

- Web UI: SoundFont alias settings panel.
- F1 docs for `sf_voice`, `gm_route`, `$soundfont_alias`.
- Six cross-PRD reverse-reference edits (§6 last table).
- Rebuild docs index: `bun run build:docs`.

---

## 8. Edge Cases

### 8.1 Unknown alias

`sf_voice("undefined_alias", 0, freq, gate, vel)` — error at codegen if alias unknown; runtime if alias resolves to a missing file. Error: `E5xx: cannot resolve soundfont "undefined_alias" (no alias, no such file)`.

### 8.2 Preset index out of range

`sf_voice("gm", 999, ...)` — `SoundFontRegistry::get_preset_by_index` returns nullptr. Behavior: emit silence (no zones matched); log a one-shot warning at first block. Don't crash, don't infinite-loop on retrigger.

### 8.3 Note out of zone range

A note hits the soundfont but no zone matches (out of key range, no fallback zone). Silent output for that voice (current SOUNDFONT_VOICE behavior — preserve).

### 8.4 Program-change *during* a held note

MIDI emits 0xC0 while a note is sounding on the same channel. **Behavior**: the program update applies to *future* note-ons on that channel only. Currently sounding voices are NOT retroactively re-routed (no SF voice swap mid-note). This matches GM hardware.

### 8.5 Channel field on chord events

`chord("C")` produces a single event with `num_values > 1` (one event, multiple frequencies). `evt.channel` is a scalar; all voices spawned from that event share the same channel. To assign different channels to different notes, use `n"c4{channel:1} e4{channel:2} g4{channel:3}"` instead of a chord.

### 8.6 Live MIDI with no program-change ever

Many MIDI controllers never send program-change. `channel_program[chan]` stays at 0; every note on that channel reports `program = 0`. Documented behavior.

### 8.7 Mini-notation `{program:N}` on a pattern that also routes through `midi()`

Not applicable — `midi()` and typed pattern literals are separate event sources. If both feed the same poly (via `+` or similar), each event carries its own channel/program (MIDI from MIDI source, mini-notation override from pattern source).

### 8.8 Envelope-done false-positive

SF2 with very long release tails (e.g., 10s pad) — envelope might drop briefly below threshold during noise floor. **Mitigation**: hysteresis on the done flag (require N consecutive blocks below threshold), e.g., 4 blocks (~10ms at 48kHz/128). Threshold: -80dB (1e-4 linear) by default. Tunable via `poly(@, ..., done_threshold: 1e-4)` if needed (deferred to follow-up if not required for ship).

### 8.9 SF2 stereo-pair sample on a chord voice

A chord event spawns N voices in poly; each voice receives one freq. SF_VOICE inside each voice independently resolves its zones — including stereo-pair samples — so each voice gets correct L/R routing. No interaction between voices.

### 8.10 Drum channel (channel 9) without GM bank 128 in the loaded SF2

`gm_route(9, 0)` returns 128 * 128 + 0. If the SF2 doesn't have bank 128, `SoundFontRegistry::get_preset_by_index` returns nullptr → §8.2 silence + warning. Document: users loading non-GM SF2s shouldn't route drum channel through `gm_route`.

---

## 9. Testing / Verification Strategy

### 9.1 Functional parity

**Goal**: old `soundfont()` API produces equivalent output via the new path.

Test: compile and render `n"C4'" |> soundfont(@, "test.sf2", 0) |> out(@)` both before this PRD lands (capture WAV baseline) and after (via the userspace stdlib sugar). Diff. Small differences from generic-vs-SF-pool stealing acceptable when envelope-done is enabled; bit-identical not required.

### 9.2 Retrigger regression

**Goal**: the fix from [`prd-soundfont-playback-fixes.md`](prd-soundfont-playback-fixes.md) must not regress.

Test: `n"c4 e4 g4" |> soundfont(@, "piano.sf2", 0) |> out(@)`. Verify each pattern step retriggers — measure attack envelope per note-onset, confirm no "single sustained gate eats all notes" bug.

### 9.3 GM routing end-to-end

**Goal**: a real GM `.mid` plays correctly with per-channel program selection.

Test fixture: a known GM `.mid` (e.g., `experiments/fixtures/gm_demo.mid`) with program-changes on channels 1, 2, and 10. Render via the GM-routing example in §3.4. WAV saved; manual listening required (per CLAUDE.md experiment methodology); per-channel RMS over time as automated sanity check.

### 9.4 Stereo correctness

**Goal**: SF2 pan offsets and stereo-pair samples produce correct L/R output.

Test: load an SF2 with a hard-panned zone (or a stereo-linked pair). Render a single note. Assert L-channel RMS / R-channel RMS ratio matches expected pan. WAV saved.

### 9.5 Envelope-done stealing correctness

**Goal**: a voice with long release isn't stolen until envelope finishes.

Test: a 4-voice poly running SF_VOICE with a 2-second release. Trigger 4 notes, then trigger 4 more after 100ms. Verify the original voices' release tails are audible for nearly 2 seconds (envelope-done preserves them) — compared against a generic-stealing baseline which would cut them at 100ms.

### 9.6 Live MIDI program-change

**Goal**: program-change byte from a live device updates per-channel state and subsequent notes use the new program.

Test: requires manual setup (loopback or virtual MIDI device). Send 0xC0 with channel + program, then a note on that channel. Verify rendered output reflects the new program. Document in `experiments/test_live_midi_program.md`.

### 9.7 `gm_route` helper round-trip

**Goal**: stdlib helper returns correct preset indices.

Test: compile-time assertion in `akkado/tests/test_codegen.cpp` — `gm_route(9, 0)` evaluates to the drum bank constant; `gm_route(0, 5)` evaluates to melodic bank * 128 + 5. Snapshot test.

### 9.8 MAX_STATES budget

**Goal**: 64-voice poly running SF_VOICE doesn't overflow `MAX_STATES`.

Test: compile and run a 64-voice GM-routed patch with several stateful opcodes per voice. Verify `state_pool` doesn't error. Bump `MAX_STATES` to 1024 if needed (already flagged in polyphony PRD §6).

### 9.9 Long-run stability (CLAUDE.md rule)

**Goal**: ≥300s simulated audio per the `experiments/` methodology.

Test: render the GM fixture for ≥300s of simulated audio. Verify no voice leaks, no state explosions, no audible glitches every-few-bars (which would replicate the polyphony PRD §6 concern).

### Build & verification commands

```bash
# Cedar + Akkado build + tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests

# Regenerate opcode metadata after adding SF_VOICE
cd web && bun run build:opcodes

# Run experiments
cd experiments
uv run python test_op_sf_voice.py
uv run python test_envelope_done_stealing.py
uv run python test_gm_route.py
./run_all.sh

# Render the GM end-to-end fixture (≥300s)
./build/bin/nkido render \
    --soundfont-alias gm=/path/to/FluidR3_GM.sf2 \
    --seconds 300 \
    experiments/fixtures/gm_routing.akk -o gm_routing.wav
```

---

## 10. Open Questions

1. ~~**`bank * 128 + program` encoding for preset index**~~ — **RESOLVED (Phase 1).**
   `SoundFontRegistry::get_preset_by_index` takes a single opaque preset *index*
   (position in the SF2's preset list), not a bank/program composite. `sf_voice`
   keeps the 5-param signature `sf_voice(file, preset, freq, gate, vel)`; the
   preset is emitted as a constant input buffer (`inputs[3]`). `gm_route`
   (Phase 3) will map channel/program to a preset index via the registry.
2. **Pattern record-suffix `{channel:N, program:M}` field routing** — do these land directly in `evt.channel`/`evt.program`, or go through `prop_vals[]` like other custom fields and get aliased at codegen? Affects how many "fixed" vs "custom" fields the event record advertises. Decide during Phase 2.
3. **Envelope-done buffer dimensionality** — per-sample stream (lets poly steal mid-block) or per-block scalar (cheaper)? Per-block is simpler and matches typical envelope smoothness; commit to per-block unless Phase 2.5 testing shows audible glitches at block boundaries.
4. **Per-zone vs whole-voice envelope-done** — when an SF_VOICE has multiple sub-zones with different release times, do we report "done" when *all* zones are done (safe but holds the slot longer) or when the *loudest* zone is done (tighter but might cut a quiet residual zone)? Commit to "all zones done" (safe) unless testing shows the slot is held too long.
5. **`MAX_STATES` bump** — confirm 1024 vs 512 after Phase 4 measurement. Polyphony PRD §6 flagged 512–1024 as the likely range.

These resolve during implementation spikes rather than blocking the PRD.

---

## 11. Related PRDs

| PRD | Status | Relationship |
|---|---|---|
| [`prd-polyphony-system.md`](prd-polyphony-system.md) | IMPLEMENTED (Phases 0–5); future-work sections open | Foundation. This PRD executes §7 "Future: SF_PLAY" (renamed `SF_VOICE`) and §7 "Configurable Voice Release" Option 2 (envelope-done). |
| [`prd-poly-callback-event-record.md`](prd-poly-callback-event-record.md) | NOT STARTED | Hard dependency for Phase 3 (GM routing destructure callback). |
| [`prd-midi-input.md`](prd-midi-input.md) | SHIPPED (Phases 1–7) | Extends with per-event `channel`/`program` and program-change preservation (Phase 2 of this PRD). |
| [`prd-soundfonts-sample-banks.md`](prd-soundfonts-sample-banks.md) | DONE | Provides the SoundFont registry, parser, and zone-matching infrastructure that SF_VOICE reuses. |
| [`prd-soundfont-playback-fixes.md`](prd-soundfont-playback-fixes.md) | DONE | Retrigger semantics established here must survive the SF_VOICE refactor (verification §9.2). |
| [`prd-advanced-functions.md`](prd-advanced-functions.md) | DONE | Provides the `_` placeholder partial-application used by the stdlib sugar `fn soundfont(input, file, preset) = ... sf_voice(file, preset, _, _, _) ...`. |
| [`prd-records-and-field-access.md`](prd-records-and-field-access.md) | DONE | Provides `@.field` access used to read `@.channel` / `@.program`. |
| [`prd-closure-pipe-operator.md`](prd-closure-pipe-operator.md) | NOT STARTED | Alternative ergonomic path; the GM routing example would also work with `->>` once that ships. Not a dependency. |
