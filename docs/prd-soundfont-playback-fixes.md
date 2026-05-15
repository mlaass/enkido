> **Status: COMPLETE** — Fix SoundFont retrigger and web loading bugs

**Author:** Claude
**Date:** 2026-04-01
**Related:** [SoundFont and Sample Bank PRD](prd-soundfonts-sample-banks.md)

# SoundFont Playback Fixes PRD

## Executive Summary

SoundFont playback via `SOUNDFONT_VOICE` has two bugs that together produce silence or incomplete playback:

1. **No retrigger on consecutive notes** — The opcode only triggers voices on gate rising edges (0→1), but the pattern system outputs sustained gate for back-to-back notes. Only the first note in any sequence plays.
2. **Web SF slot/ID mismatch** — The compiler assigns slot indices to SoundFonts that may not match runtime registry IDs, causing the opcode to look up the wrong bank (or nullptr → silence).

Key design decisions:
- **Trigger signal + note-change detection**: Wire `SEQPAT_STEP`'s trigger output to `SOUNDFONT_VOICE` `inputs[4]` for event-boundary retrigger (handles same-note repeats). Fall back to note-change detection when no trigger is wired (non-pattern use cases).
- **Legato overlap** for different notes: old voices ring out naturally while new voices start. Same-note retrigger fast-releases the old voice.
- **Name-based dedup** in the registry as the fix for slot/ID mismatch (simple, sufficient for single-SF use case)
- **Synthetic-signal Python test** to validate retrigger behavior in isolation

---

## 1. Problem Statement

### 1.1 Current State

`SOUNDFONT_VOICE` is a 32-voice polyphonic SoundFont sampler opcode with DAHDSR envelopes, SVF filters, and voice stealing. It works correctly for a single sustained note but fails for melodies.

### 1.2 Bug 1: Only First Note Plays

**Affected:** All platforms (web, embedded, CLI)

The gate edge detection in `soundfont.hpp:78-81`:
```cpp
bool gate_on = (current_gate > 0.0f && state.prev_gate <= 0.0f);
bool gate_off = (current_gate <= 0.0f && state.prev_gate > 0.0f);
```

Only fires `gate_on` when gate transitions from ≤0 to >0. But `SEQPAT_GATE` (`sequencing.hpp:589-655`) outputs sustained 1.0 for any sample inside an event's `[time, time+duration)` window. For sequential notes like `pat("c4 e4 g4")`:

```
beat:  0.0─────1.0─────2.0─────3.0─────4.0
note:  │  C4   │  E4   │  G4   │ (rest) │
gate:  │  1.0  │  1.0  │  1.0  │  0.0   │
       ↑ gate_on here, no edges after
```

Gate stays at 1.0 across all three notes. The SOUNDFONT_VOICE sees:
- Sample 0: `gate=1, prev_gate=0` → `gate_on=true` → C4 voice allocated ✓
- At beat 1.0: `gate=1, prev_gate=1` → `gate_on=false` → E4 never triggered ✗
- At beat 2.0: `gate=1, prev_gate=1` → `gate_on=false` → G4 never triggered ✗

`SoundFontVoiceState` (`dsp_state.hpp:531-536`) has no note tracking:
```cpp
struct SoundFontVoiceState {
    SFVoice* voices = nullptr;
    std::uint16_t num_voices = 0;  // unused
    float prev_gate = 0.0f;        // only tracks gate, not note
};
```

**Note:** `SEQPAT_STEP` produces a per-event trigger signal (`sequencing.hpp:467-488`) that fires a single-sample 1.0 pulse at each event boundary — including same-note repeats. This trigger is already stored in `PatternPayload::TRIG` (`typed_value.hpp:46`) and populated by `emit_per_voice_seqpat()` (`codegen_patterns.cpp:947`). However, `handle_soundfont_call()` only wires gate/freq/vel/preset to `inputs[0-3]` and leaves `inputs[4]` unused (0xFFFF). The fix is to wire the trigger buffer to `inputs[4]` and use it as the primary note-on signal.

### 1.3 Bug 2: Web Silence from SF Slot Mismatch

**Affected:** Web path only

The compiler assigns SF slot indices based on order of appearance in source code (`codegen_patterns.cpp:2846-2858`):
```cpp
sf_slot = static_cast<std::uint8_t>(required_soundfonts_.size());
// ...
sf_inst.rate = sf_slot;  // stored in bytecode
```

The opcode uses this directly as a registry index (`soundfont.hpp:48`):
```cpp
int sf_id = static_cast<int>(inst.rate);
const SoundFontBank* bank = sf_registry->get(sf_id);
```

The registry assigns IDs by loading order (`soundfont.cpp`): `return banks_.size()` at time of load. Currently works because:
1. Default "gm" SF loads in background → gets sf_id=0
2. Compiler assigns sf_slot=0 for "gm"
3. Compile path skips loading "gm" (already loaded by name check)
4. sf_slot=0 matches sf_id=0

Breaks when:
- The registry already has SFs from a previous session/load
- Background preload races with compilation (SF not yet loaded when compile runs, then both try to load it → sf_id=1 instead of 0)
- Multiple SoundFonts are used with different loading orders

---

## 2. Goals and Non-Goals

### Goals
1. All notes in sequential patterns play correctly — both different notes (legato overlap) and same-note repeats (re-articulation)
2. SoundFont loading is idempotent by name — loading the same SF twice returns the same ID
3. Python experiment test validates retrigger behavior with synthetic signals
4. Zero change to existing Akkado syntax

### Non-Goals
- Legato mode selection (always legato for different notes, re-articulate for same note — configurable mode is future work)
- Full slot remapping system for multi-SF ordering (dedup is sufficient for now)
- Changes to SEQPAT_GATE or SEQPAT_STEP opcodes
- Fixing other potential SoundFont issues (pitch, filter, envelope) — can't evaluate until playback works

---

## 3. Design

### 3.1 Bug 1 Fix: Trigger Signal + Note-Change Fallback

Two-pronged approach: (A) wire the existing trigger signal from patterns to the opcode, and (B) add note-change detection as a fallback for non-pattern inputs.

#### 3.1.1 Wire Trigger to SOUNDFONT_VOICE (codegen change)

The trigger buffer already exists in `PatternPayload::TRIG` (populated at `codegen_patterns.cpp:947`). Wire it to `inputs[4]` in `handle_soundfont_call()`:

**Codegen change** (`codegen_patterns.cpp`, in `handle_soundfont_call`, ~line 2887-2891):
```cpp
sf_inst.inputs[0] = gate_buf;     // Gate signal (sustained, for release detection)
sf_inst.inputs[1] = freq_buf;     // Frequency in Hz
sf_inst.inputs[2] = vel_buf;      // Velocity (0-1)
sf_inst.inputs[3] = preset_buf;   // Preset index (constant)
sf_inst.inputs[4] = trig_buf;     // NEW: trigger pulse from SEQPAT_STEP
```

Where `trig_buf` is read from the pattern payload alongside gate/freq/vel:
```cpp
std::uint16_t trig_buf = pat_fields[PatternPayload::TRIG];
// trig_buf may be 0xFFFF if pattern doesn't produce triggers — that's fine,
// the opcode falls back to note-change detection
```

**Polyphonic patterns:** For monophonic patterns, `trig_buf` comes from `PatternPayload::fields[TRIG]`. For polyphonic patterns (chords via `poly()`), each voice has its own trigger buffer in `PatternPayload::voice_fields[voice_index][TRIG]`. The existing `handle_soundfont_call()` already iterates per-voice fields when emitting polyphonic patterns — the trigger wiring follows the same per-voice pattern. Mono trigger wiring is sufficient for the common case; polyphonic trigger wiring should follow the same structure as the existing gate/freq/vel per-voice emission.

#### 3.1.2 Opcode Note-On Logic (two triggers, one fallback)

**State addition** (`dsp_state.hpp`):
```cpp
struct SoundFontVoiceState {
    SFVoice* voices = nullptr;
    std::uint16_t num_voices = 0;
    float prev_gate = 0.0f;
    std::uint8_t prev_note = 255;   // NEW: for note-change fallback (255 = none)
};
```

**Detection logic** (`soundfont.hpp`, replacing lines 78-84):
```cpp
// Read optional trigger input (wired from SEQPAT_STEP when driven by patterns)
const float* trig_buf = (inst.inputs[4] != 0xFFFF) ? ctx.buffers->get(inst.inputs[4]) : nullptr;

// ... per sample loop:

// Gate edge detection (always active)
bool gate_on = (current_gate > 0.0f && state.prev_gate <= 0.0f);
bool gate_off = (current_gate <= 0.0f && state.prev_gate > 0.0f);

// Trigger pulse from pattern sequencer (fires on every event boundary)
bool trigger_on = (trig_buf && trig_buf[i] > 0.0f);

// Fallback: note-change detection for non-pattern inputs (no trigger wired)
bool note_change = (!trig_buf && current_gate > 0.0f
                    && note != state.prev_note && state.prev_note != 255);

state.prev_gate = current_gate;
state.prev_note = (current_gate > 0.0f) ? note : static_cast<std::uint8_t>(255);

// Note on: any of the three triggers
if ((gate_on || trigger_on || note_change) && vel > 0) {
```

**Note:** The `trig_buf` pointer is read once before the sample loop (constant across block). Only the per-sample read `trig_buf[i]` is inside the loop.

#### 3.1.3 Behavior Matrix

| Input scenario | trigger_on | note_change | gate_on | Voice action |
|---|---|---|---|---|
| Pattern, different note (C4→E4) | ✓ fires | N/A (trig wired) | — | New voices, old voices continue (legato) |
| Pattern, same note (C4→C4) | ✓ fires | N/A (trig wired) | — | Old C4 fast-released, new C4 starts (re-articulation) |
| Pattern, first note | ✓ fires | N/A | ✓ also fires | New voices (trigger takes precedence, gate_on also true) |
| Pattern, note after rest | — | N/A | ✓ fires | New voices via gate edge |
| Direct drive, note changes | — | ✓ fires | — | New voices, old continue (legato) |
| Direct drive, same note held | — | ✗ same note | — | No retrigger (use gate cycling) |
| Direct drive, gate gap | — | — | ✓ fires | New voices via gate edge |

The existing same-note fast-release code at lines 85-93 handles re-articulation:
```cpp
// Quick fade-out on same-note re-trigger
for (std::uint16_t v = 0; v < SoundFontVoiceState::MAX_VOICES; ++v) {
    if (state.voices[v].active && state.voices[v].note == note) {
        state.voices[v].releasing = true;
        state.voices[v].env_stage = SFVoice::EnvStage::Release;
        state.voices[v].env_time = 0.0f;
        state.voices[v].env_release = 0.005f; // Fast re-trigger release
    }
}
```
This fires for same-note repeats (C4→C4 via trigger). For different notes (C4→E4), the loop finds no matching `note` — old voices continue undisturbed.

```
beat:  0.0─────1.0─────2.0─────3.0
note:  │  C4   │  E4   │  G4   │
trig:  ↑pulse  ↑pulse  ↑pulse
       ↑ trigger_on → C4 voices start (different note → no fast-release)
               ↑ trigger_on → E4 voices start, C4 voices continue sustaining
                       ↑ trigger_on → G4 voices start, C4+E4 continue
                                ↑ gate_off → all voices release

beat:  0.0─────1.0─────2.0─────3.0
note:  │  C4   │  C4   │  C4   │
trig:  ↑pulse  ↑pulse  ↑pulse
       ↑ trigger_on → C4 voices start
               ↑ trigger_on → same note → old C4 fast-released, new C4 starts
                       ↑ trigger_on → same note → old C4 fast-released, new C4 starts
```

**Why integer MIDI note for the fallback:**
- `note` is already computed from `freq` at line 75 with rounding to nearest semitone
- Comparing integers avoids false retriggering from vibrato or pitch modulation
- 255 sentinel is outside valid MIDI range (0-127), ensures first note always triggers via `gate_on`

### 3.2 Bug 2 Fix: Name-Based Dedup in Registry

Add a name check at the top of `SoundFontRegistry::load_from_memory()`:

```cpp
int SoundFontRegistry::load_from_memory(const void* data, int size,
                                         const std::string& name,
                                         SampleBank& sample_bank) {
    // Deduplicate: if already loaded by this name, return existing ID
    for (std::size_t i = 0; i < banks_.size(); ++i) {
        if (banks_[i].name == name) {
            return static_cast<int>(i);
        }
    }

    // ... existing parsing code ...
}
```

This ensures:
- Background preload loads "gm" → sf_id=0
- Compile path tries to load "gm" → returns sf_id=0 (already loaded)
- sf_slot=0 in bytecode matches sf_id=0 in registry

The JS-side name check at `audio.svelte.ts:962` (`if (state.loadedSoundfonts.some(s => s.name === sf.filename)) continue`) already prevents most double-loads. The C++ dedup is a safety net for race conditions where both the background preload and compile path issue `cedar_load_soundfont` before the JS state updates.

**Name consistency across call sites:** The dedup relies on both paths passing the same `name` string to `cedar_load_soundfont`. The compile path uses `sf.filename` from the compiler's `requiredSoundfonts` list (line 969). The preload path uses `sf.name` from the default soundfonts config (line 1267). Both must resolve to the same short name (e.g., `"gm"`) for dedup to work. Currently this holds because `resolveDefaultSoundFontUrl()` maps short names to URLs while preserving the short name as the identifier.

### 3.3 Python Experiment Test

Create `experiments/test_op_soundfont.py` that validates retrigger behavior with synthetic gate/freq/vel buffers fed directly to `SOUNDFONT_VOICE`.

**Test structure:**
1. Load a SoundFont from a known SF2 file
2. Set up `SOUNDFONT_VOICE` instruction with direct buffer inputs
3. Feed synthetic signals simulating sequential notes

**Test cases:**

| Test | Gate | Trigger | Freq | Expected Behavior |
|------|------|---------|------|-------------------|
| Single note | 0→1→0 | pulse at start | C4 | Voice triggers, sustains, releases |
| Sequential different notes | 1,1,1... (sustained) | pulse at each change | C4→E4→G4 | All three notes, legato overlap |
| Same note with trigger | 1,1,1... (sustained) | pulse at each repeat | C4→C4→C4 | Re-articulation (old fast-released, new starts) |
| Same note without trigger | 1,1,1... (sustained) | none (0xFFFF) | C4→C4→C4 | No retrigger (fallback: same note) |
| Gate gap between notes | 1→0→1→0→1 | none needed | C4, E4, G4 | All trigger via gate edges |
| Note-change fallback | 1,1,1... (sustained) | none (0xFFFF) | C4→E4→G4 | All trigger via note_change |
| Velocity sensitivity | 0→1 | pulse | C4, vel=0.2 vs 0.8 | Amplitude scales with velocity |

**Validation approach:**
- Check output buffer is non-zero during expected note windows
- Check output is ~zero during rest periods
- Save WAV files for human listening verification
- Compare RMS levels between notes to verify all are audible

---

## 4. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `SEQPAT_GATE` | **Stays** | Gate behavior is correct (sustained signal for ADSR) |
| `SEQPAT_STEP` | **Stays** | Trigger output already exists and is already wired to `PatternPayload::TRIG` |
| `handle_soundfont_call()` codegen | **Modified** | Wire `trig_buf` from `PatternPayload::TRIG` to `inputs[4]` |
| `SoundFontVoiceState` | **Modified** | Add `prev_note` field |
| `op_soundfont_voice()` | **Modified** | Add trigger + note-change detection logic |
| `SoundFontRegistry::load_from_memory()` | **Modified** | Add name-based dedup at top |
| Voice allocation/stealing | **Stays** | Existing same-note fast-release handles re-articulation |
| Envelope processing | **Stays** | No changes needed |
| Web compile/load flow | **Stays** | Dedup in C++ is sufficient; no JS changes needed |

---

## 5. File Changes

### Modified Files

| File | Change |
|------|--------|
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Add `std::uint8_t prev_note = 255;` to `SoundFontVoiceState` (~line 536) |
| `cedar/include/cedar/opcodes/soundfont.hpp` | Read trigger from `inputs[4]`, add trigger + note-change detection (~15 lines changed around lines 33, 78-84) |
| `akkado/src/codegen_patterns.cpp` | Wire `trig_buf` to `sf_inst.inputs[4]` in `handle_soundfont_call()` (~3 lines changed around line 2797) |
| `cedar/src/audio/soundfont.cpp` | Add name-based dedup check at top of `load_from_memory()`, ~5 lines added |

### New Files

| File | Purpose |
|------|---------|
| `experiments/test_op_soundfont.py` | Python experiment validating retrigger behavior with synthetic signals |

---

## 6. Implementation Phases

### Phase 1: Trigger + Note-Change Retrigger (Bug 1)

**Goal:** All notes in patterns play — different notes with legato overlap, same notes with re-articulation.

**Files:**
- `cedar/include/cedar/opcodes/dsp_state.hpp` — add `prev_note` to `SoundFontVoiceState`
- `cedar/include/cedar/opcodes/soundfont.hpp` — read trigger from `inputs[4]`, add detection logic
- `akkado/src/codegen_patterns.cpp` — wire `PatternPayload::TRIG` to `sf_inst.inputs[4]`

**Verify:** Build and run `./build/cedar/tests/cedar_tests` and `./build/akkado/tests/akkado_tests`. If no SF-specific tests exist, proceed to Phase 3 for Python validation.

### Phase 2: Registry Dedup (Bug 2)

**Goal:** Loading the same SoundFont twice returns the same ID.

**Files:**
- `cedar/src/audio/soundfont.cpp` — add name check at top of `load_from_memory()`

**Verify:** Build and add a C++ unit test that loads the same SF data twice with the same name and asserts the returned IDs are equal. Also test that loading with a different name returns a different ID. This prevents regression if the registry is refactored later.

### Phase 3: Python Experiment Test

**Depends on:** Phase 1 (trigger + note-change retrigger must be implemented first).

**Goal:** Validate retrigger behavior with synthetic signals.

**Prerequisite:** The `cedar_core` Python bindings (`cedar/bindings/bindings.cpp`) do **not** currently expose SoundFont loading (`SoundFontRegistry::load_from_memory()`) or the `SoundFontRegistry` class. Before this phase can proceed, either:
1. **Add SF bindings to `cedar_core`:** Expose `load_from_memory()` and ensure `SOUNDFONT_VOICE` can access the registry from the Python test host. This is the preferred approach as it enables ongoing SF opcode testing.
2. **Alternative:** Skip Python validation and rely on C++ unit tests (Phase 1 verify) + web end-to-end (Phase 4). This avoids binding work but loses the synthetic-signal isolation benefit.

A test SF2 file is also needed. Options: use the GM SoundFont from the web app's default set, or include a minimal test SF2 in the experiments directory.

**Files:**
- `cedar/bindings/bindings.cpp` — add SoundFont registry bindings (if option 1)
- `experiments/test_op_soundfont.py` — new file

**Verify:** `cd experiments && uv run python test_op_soundfont.py` — all tests pass, WAV files saved for listening.

### Phase 4: End-to-End Web Verification

**Goal:** Confirm playback works in the browser.

**Steps:**
1. Rebuild WASM: `cd web && bun run build:wasm`
2. Start dev server: `bun run dev`
3. Test with: `pat("c4 e4 g4") |> soundfont(@, "gm", 0) |> out(@, @)`
4. Verify all three notes are audible with legato overlap

---

## 7. Edge Cases

### 7.1 Vibrato / Continuous Pitch Modulation
**Situation:** User feeds a continuously modulated frequency to SOUNDFONT_VOICE.
**Behavior:** No spurious retriggering. The comparison uses integer MIDI notes (rounded), so sub-semitone pitch changes don't trigger new voices.
**Boundary:** A pitch sweep crossing a semitone boundary (e.g., 261→277 Hz, C4→C#4) will trigger a new voice. This is correct behavior — the SoundFont zone may change at semitone boundaries.

### 7.2 Same Note Repeated
**Situation:** Pattern like `pat("c4 c4 c4")` — same note, back-to-back.
**Behavior (with trigger wired):** `trigger_on` fires at each event boundary. The same-note fast-release code (lines 85-93) detects matching note number, fast-releases the old voice (5ms), and starts a new one. Each repetition re-articulates cleanly.
**Behavior (without trigger, fallback):** `note_change` is false (same MIDI note). The existing voice sustains through. Users must use gaps (`pat("c4 ~ c4 ~")`) for re-articulation. This is acceptable for direct-drive use cases.

### 7.3 Voice Exhaustion Under Legato
**Situation:** Long sequence of different notes with legato overlap. Each note allocates new voices while old ones sustain.
**Behavior:** After 32 voices are active, voice stealing kicks in (quietest releasing → oldest active). This is existing behavior and works correctly.
**Boundary:** With 8-zone SoundFont presets, just 4 overlapping notes could use all 32 voices (4×8=32). Voice stealing then activates, which may cause audible artifacts on dense passages.

### 7.4 Gate Off During Note Change
**Situation:** Gate drops to 0 at the exact same sample as a frequency change.
**Behavior:** `gate_off=true`, `note_change=false` (gate is not >0). All voices release. No new voice triggered. Correct.

### 7.5 Default SF Not Yet Loaded
**Situation:** User compiles code immediately after page load, before background SF preload finishes.
**Behavior:** Compile path at `audio.svelte.ts:962` sees SF not in `loadedSoundfonts`, calls `loadSoundFontFromUrl()` which awaits the download. The SF loads into the registry, program loads after. The bytecode sf_slot=0 matches sf_id=0 (first loaded). Works correctly.

### 7.6 Registry Dedup with Different Data
**Situation:** Two different SF2 files loaded with the same name.
**Behavior:** Second load returns the first file's ID. The second file's data is ignored.
**Rationale:** Name collision is a user error. The dedup prioritizes consistency over completeness.

---

## 8. Testing Strategy

### 8.1 C++ Unit Tests
Run existing test suites to verify no regressions:
```bash
cmake --build build
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests
```

### 8.2 Python Experiment (`test_op_soundfont.py`)

**Test 1: Single sustained note**
- Input: gate = [0,0,...,1,1,...,1,1,...,0,0,...], freq = 261.6 Hz, vel = 0.8
- Expected: Output non-zero during gate=1 (after attack), decays to zero after gate=0
- Save WAV, report RMS in gate-on vs gate-off windows

**Test 2: Sequential different notes with trigger (THE critical test)**
- Input: gate = [1,1,...] (constant 1.0 for ~1 second)
- Input: trigger = single-sample 1.0 pulse at each note boundary
- Input: freq = [261.6→329.6→392.0] (C4→E4→G4, switching every ~0.33s)
- Expected: RMS is non-zero in all three note windows, not just the first
- This is the exact scenario that currently fails

**Test 3: Same-note retrigger with trigger**
- Input: gate = [1,1,...] (constant 1.0), trigger = pulse every ~0.33s, freq = constant 261.6 Hz
- Expected: Three distinct attacks visible in the waveform (re-articulation)
- Save WAV — listen for individual note onsets, not a single sustained tone

**Test 4: Note-change fallback (no trigger wired)**
- Input: gate = [1,1,...] (constant 1.0), trigger = none (inputs[4] = 0xFFFF)
- Input: freq = [261.6→329.6→392.0] (C4→E4→G4)
- Expected: All three notes play via note_change detection

**Test 5: Gate gaps between notes**
- Input: gate pulses with brief 0-gaps between notes, no trigger
- Expected: All notes play via gate edge detection

**Test 6: Velocity scaling**
- Input: Two notes, vel=0.2 and vel=0.8
- Expected: Second note ~4x louder (RMS ratio approximately 4:1)

### 8.3 Web End-to-End
Manual test in browser after WASM rebuild:
```akkado
// Test 1: Sequential melody — different notes with legato overlap
pat("c4 e4 g4") |> soundfont(@, "gm", 0) |> out(@, @)

// Test 2: Same-note repetition — should re-articulate each hit
pat("c4 c4 c4 c4") |> soundfont(@, "gm", 0) |> out(@, @)

// Test 3: Sustained single note — verify no spurious retrigger
pat("c4") |> soundfont(@, "gm", 0) |> out(@, @)

// Test 4: Notes with rests — verify gate release works
pat("c4 ~ e4 ~ g4 ~") |> soundfont(@, "gm", 0) |> out(@, @)
```
