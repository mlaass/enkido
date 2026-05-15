> **Status: DONE** — Phase 1 shipped. Phase 2 & 3 deferred pending further testing.

# PRD: Crossfade Audio Artifact Fixes

## 1. Executive Summary

Live reloading (hot-swap) in Cedar produces audible clicks and pops due to two independent bugs in the crossfade system. This PRD addresses them in two isolated phases:

- **Phase 1**: The `OUTPUT` opcode accumulates with `+=` into crossfade buffers that are never zeroed between blocks, causing audio to compound across the 3-block crossfade and produce a massive discontinuity when it ends. One-line surgical fix.
- **Phase 2**: During crossfade, both old and new programs execute against the same shared state pool. The old program mutates oscillator phases, filter states, and arena-allocated delay/reverb buffers before the new program reads them, causing pitch doubling and a state discontinuity when crossfade completes. Fix via snapshot/restore of shared states with deep arena copy.

Key design decisions made:
- Phase 2 uses full `DSPState` variant copy (not selective field copy) for correctness and maintainability
- Arena-allocated buffer contents are deep-copied via a visitor trait pattern on the variant
- Snapshot storage is a pre-allocated VM member (zero audio-thread allocations)
- Snapshot arena buffer matches `AudioArena::DEFAULT_SIZE` (32 MB)

---

## 2. Problem Statement

### 2.1 Current Behavior

| Scenario | Expected | Actual |
|----------|----------|--------|
| Live reload (any code change) | Smooth ~8ms equal-power crossfade, no audible artifacts | Loud click/pop at end of every crossfade |
| Rapid successive reloads | Each crossfade smooth | Clicks compound; stale crossfade buffer data from previous swap bleeds into next |
| Crossfade with shared oscillators | Pitch continuity through crossfade | Oscillators run at 2x rate during crossfade, then snap back to 1x |
| Crossfade with shared filters | Filter response continuity | Filter states double-processed, resonance distortion during crossfade |
| Crossfade with shared delays/reverbs | Delay tail continuity | Old program corrupts arena buffer contents before new program reads them |

### 2.2 Root Cause: Buffer Accumulation (Phase 1)

`process_block()` zeros the final output buffers every block (`vm.cpp:65-66`):

```cpp
std::fill_n(output_left, BLOCK_SIZE, 0.0f);
std::fill_n(output_right, BLOCK_SIZE, 0.0f);
```

But `perform_crossfade()` (`vm.cpp:163-195`) passes `CrossfadeBuffers` member arrays as output targets:

```cpp
execute_program(old_slot, crossfade_buffers_.old_left.data(), ...);
execute_program(new_slot, crossfade_buffers_.new_left.data(), ...);
```

The `OUTPUT` opcode (`utility.hpp:55-56`) accumulates with `+=`:

```cpp
ctx.output_left[i] += l;
ctx.output_right[i] += r;
```

The crossfade buffers are **never zeroed between blocks**. Audio accumulates:

| Block | old_left content | Crossfade gain (old) | Effective output |
|-------|-----------------|---------------------|-----------------|
| N (pos=0.0) | 1x audio | 1.0 | 1.0x |
| N+1 (pos=0.33) | 2x accumulated | 0.866 | 1.73x |
| N+2 (pos=0.67) | 3x accumulated | 0.5 | 1.5x |
| N+3 (pos=1.0) | 4x accumulated | 0.0 (old) / 1.0 (new=4x) | 4.0x |
| N+4 (normal) | — | — | 1.0x |

The 4x → 1x transition at the end of crossfade is the click.

`CrossfadeBuffers::clear()` exists (`crossfade_state.hpp:119-124`) but is never called anywhere.

### 2.3 Root Cause: State Double-Processing (Phase 2)

`perform_crossfade()` calls `execute_program()` for both old and new programs. Both share the same `state_pool_`. Execution order:

```
1. execute_program(old_slot, ...)   // mutates shared states
2. execute_program(new_slot, ...)   // reads already-mutated states
```

For shared state IDs (common during live coding — user tweaks a parameter, oscillator/filter IDs remain the same):
- Oscillator phases advance twice per block (pitch doubles for ~8ms)
- Filter feedback states (`ic1eq`, `ic2eq`) are processed twice
- Arena-allocated delay/reverb buffers are written by old program before new program reads

When crossfade ends and only the new program runs, the state is at the wrong position — it advanced by `2 * crossfade_duration` instead of `1 * crossfade_duration`. This creates a phase/state discontinuity.

---

## 3. Goals and Non-Goals

### Goals

1. Eliminate the click/pop artifact on every live reload (Phase 1)
2. Eliminate state discontinuity at crossfade boundaries (Phase 2)
3. Zero runtime allocations in the audio path
4. No regression in crossfade behavior for non-shared states (new/removed nodes)
5. Automated test coverage for both bugs

### Non-Goals

- **Fade-in on first program load**: The abrupt start when loading into an empty VM is intentional design. First load has no crossfade by design (`requires_crossfade()` returns false). This is documented as an optional future phase (Phase 3) but is not part of this work.
- **Crossfade duration tuning**: The 3-block (~8ms) default is adequate. No changes to `CrossfadeConfig`.
- **Per-sample crossfade**: The current per-block crossfade granularity is sufficient. Intra-block sample-level crossfade is out of scope.

---

## 4. Architecture

### 4.1 Phase 1: Buffer Clear

```
Before (buggy):
┌─────────────────────────────────────────────────┐
│ perform_crossfade()                              │
│                                                  │
│   execute_program(old) → crossfade_buffers_ (+= accumulates on stale data)
│   execute_program(new) → crossfade_buffers_ (+= accumulates on stale data)
│   mix_equal_power()                              │
└─────────────────────────────────────────────────┘

After (fixed):
┌─────────────────────────────────────────────────┐
│ perform_crossfade()                              │
│                                                  │
│   crossfade_buffers_.clear()    ← NEW            │
│   execute_program(old) → crossfade_buffers_ (+= from zero ✓)
│   execute_program(new) → crossfade_buffers_ (+= from zero ✓)
│   mix_equal_power()                              │
└─────────────────────────────────────────────────┘
```

### 4.2 Phase 2: State Snapshot/Restore

```
perform_crossfade()
│
├── crossfade_buffers_.clear()
│
├── snapshot_shared_states(old, new)
│   ├── Find state IDs present in BOTH old and new programs
│   ├── For each shared state:
│   │   ├── Copy DSPState variant → snapshot array
│   │   └── Deep-copy arena buffers → snapshot arena
│   └── Record snapshot count
│
├── execute_program(old_slot, old_left, old_right)
│   └── Mutates shared states (phases advance, filters update, arena written)
│
├── restore_shared_states()
│   ├── For each snapshotted state:
│   │   ├── Restore DSPState variant from snapshot
│   │   └── Restore arena buffer contents from snapshot arena
│   └── New program sees pre-old-execution state ✓
│
├── execute_program(new_slot, new_left, new_right)
│   └── Reads pristine state, mutates independently ✓
│
└── mix_equal_power()
```

### 4.3 Snapshot Data Structures

```cpp
// Pre-allocated snapshot entry (one per shared state)
struct StateSnapshot {
    std::size_t slot_idx;           // Index into states_ array
    DSPState state;                 // Full variant copy

    // Arena buffer tracking (filled by visitor)
    struct ArenaRegion {
        float* original_ptr;        // Pointer into main AudioArena
        std::size_t offset;         // Offset into snapshot arena
        std::size_t num_floats;     // Size of the buffer
    };
    static constexpr std::size_t MAX_ARENA_REGIONS = 16;  // FreeverbState has ~12 buffers
    std::array<ArenaRegion, MAX_ARENA_REGIONS> arena_regions{};
    std::size_t arena_region_count = 0;
};
```

### 4.4 Arena Visitor Trait

Each arena-owning state type must implement arena enumeration so the snapshot system can find and copy its buffers. This is done via `std::visit` on the `DSPState` variant with a function object:

```cpp
struct ArenaEnumerator {
    StateSnapshot& snap;
    float* snapshot_arena;
    std::size_t& arena_offset;

    // Default: no arena buffers
    template<typename T>
    void operator()(const T&) {}

    // Specializations for arena-owning types:
    void operator()(const DelayState& s) {
        if (s.buffer && s.buffer_size > 0)
            add_region(s.buffer, s.buffer_size);
    }

    void operator()(const FreeverbState& s) {
        // Enumerate all comb + allpass buffers
        for (auto& comb : s.combs) { ... }
        for (auto& ap : s.allpasses) { ... }
    }
    // ... DattorroState, FDNState, PingPongDelayState,
    //     CombFilterState, FlangerState, ChorusState, PhaserState

    void add_region(float* ptr, std::size_t num_floats) {
        if (snap.arena_region_count >= StateSnapshot::MAX_ARENA_REGIONS) return;
        auto& region = snap.arena_regions[snap.arena_region_count++];
        region.original_ptr = ptr;
        region.offset = arena_offset;
        region.num_floats = num_floats;
        std::memcpy(snapshot_arena + arena_offset, ptr, num_floats * sizeof(float));
        arena_offset += num_floats;
    }
};
```

---

## 5. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `crossfade_state.hpp` | **Unchanged** | `CrossfadeBuffers::clear()` already exists, already correct |
| `vm.cpp` `perform_crossfade()` | **Modified** | Phase 1: add `clear()` call. Phase 2: add snapshot/restore calls |
| `vm.hpp` | **Modified (Phase 2)** | Add snapshot array + arena members to VM class |
| `state_pool.hpp` | **Unchanged** | No changes to StatePool API |
| `dsp_state.hpp` | **Unchanged** | State structs unchanged; arena visitor reads existing fields |
| `vm.cpp` (new methods) | **New (Phase 2)** | `snapshot_shared_states()`, `restore_shared_states()` |
| `crossfade_snapshot.hpp` | **New (Phase 2)** | `StateSnapshot`, `ArenaEnumerator`, snapshot logic |
| `test_crossfade.cpp` | **Modified** | New test cases for both phases |
| All other files | **Unchanged** | No changes to opcodes, compiler, web app, worklet, etc. |

---

## 6. File-Level Changes

### Phase 1

| File | Change |
|------|--------|
| `cedar/src/vm/vm.cpp` | Add `crossfade_buffers_.clear();` at top of `perform_crossfade()` |
| `cedar/tests/test_crossfade.cpp` | Add integration test: load two programs, process through crossfade, assert no accumulation |

### Phase 2

| File | Change |
|------|--------|
| `cedar/include/cedar/vm/crossfade_snapshot.hpp` | **New** — `StateSnapshot` struct, `ArenaEnumerator` visitor, snapshot/restore functions |
| `cedar/include/cedar/vm/vm.hpp` | Add snapshot members: `std::array<StateSnapshot, MAX_STATES>`, snapshot arena (`AudioArena`), snapshot count |
| `cedar/src/vm/vm.cpp` | Add `snapshot_shared_states()` and `restore_shared_states()` methods; call them in `perform_crossfade()` |
| `cedar/tests/test_crossfade.cpp` | Add tests: shared oscillator phase continuity, shared delay buffer integrity |

---

## 7. Implementation Phases

### Phase 1 — Crossfade Buffer Clear (Critical)

**Goal**: Eliminate the click caused by buffer accumulation during crossfade.

**Change**: Single line insertion in `vm.cpp:perform_crossfade()`:

```cpp
void VM::perform_crossfade(float* out_left, float* out_right) {
    // Zero crossfade buffers before executing programs into them.
    // OUTPUT opcode uses += accumulation; without this, audio from
    // previous blocks compounds across the crossfade duration.
    crossfade_buffers_.clear();

    // ... rest unchanged
```

**Testing**:
1. Catch2 test: Create VM, load a program that outputs DC 1.0, load a second identical program, process blocks through full crossfade. Assert that output level never exceeds ~1.5x (equal-power peak for correlated signals) at any point. Before fix: peaks at ~4x.
2. Manual: Open web app, write `osc("saw", 220) |> out(@, @)`, play, change frequency to 440, listen for click. Before fix: loud pop. After fix: smooth transition.

**Verification command**:
```bash
cmake --build build --target cedar_tests && ./build/cedar/tests/cedar_tests "[crossfade]"
```

**Dependencies**: None. Can be implemented and merged independently.

---

### Phase 2 — State Snapshot/Restore During Crossfade (Moderate)

**Goal**: Eliminate state discontinuity caused by old program mutating shared states before new program reads them.

**Prerequisite**: Phase 1 merged and verified.

**Steps**:

1. **Create `crossfade_snapshot.hpp`**: Define `StateSnapshot` struct and `ArenaEnumerator` visitor. Implement `snapshot_arena_data()` / `restore_arena_data()` free functions that use `std::visit` on the `DSPState` variant.

2. **Add snapshot members to `VM`** in `vm.hpp`:
   ```cpp
   // Crossfade state snapshot (pre-allocated, zero audio-thread allocations)
   std::array<StateSnapshot, MAX_STATES> state_snapshots_{};
   std::size_t snapshot_count_ = 0;
   AudioArena snapshot_arena_{AudioArena::DEFAULT_SIZE};
   ```

3. **Implement `snapshot_shared_states()`** in `vm.cpp`:
   - Get state IDs from both old and new program slots
   - For each ID present in both: copy variant, enumerate arena regions via visitor, memcpy arena data to snapshot arena
   - Track count in `snapshot_count_`

4. **Implement `restore_shared_states()`** in `vm.cpp`:
   - For each snapshot entry: restore variant, memcpy arena data back from snapshot arena
   - Reset `snapshot_count_` and snapshot arena offset

5. **Wire into `perform_crossfade()`**:
   ```cpp
   void VM::perform_crossfade(float* out_left, float* out_right) {
       crossfade_buffers_.clear();

       const ProgramSlot* old_slot = swap_controller_.previous_slot();
       const ProgramSlot* new_slot = swap_controller_.current_slot();

       snapshot_shared_states(old_slot, new_slot);

       // Execute old program (mutates shared states freely)
       if (old_slot && old_slot->instruction_count > 0) {
           execute_program(old_slot, crossfade_buffers_.old_left.data(),
                          crossfade_buffers_.old_right.data());
       } else { /* zero fill */ }

       restore_shared_states();

       // Execute new program (sees pristine state)
       if (new_slot && new_slot->instruction_count > 0) {
           execute_program(new_slot, crossfade_buffers_.new_left.data(),
                          crossfade_buffers_.new_right.data());
       } else { /* zero fill */ }

       float position = crossfade_state_.position();
       crossfade_buffers_.mix_equal_power(out_left, out_right, position);
   }
   ```

6. **Implement ArenaEnumerator specializations** for all arena-owning state types. The specific states and their buffer fields must be enumerated by reading each struct definition in `dsp_state.hpp`. Known arena-owning types:
   - `DelayState` — `buffer` (1 region)
   - `FreeverbState` — comb + allpass buffers (~12 regions)
   - `DattorroState` — delay line buffers
   - `FDNState` — matrix delay buffers
   - `PingPongDelayState` — 2 delay buffers
   - `CombFilterState` — 1 buffer
   - `FlangerState` — 1 modulation delay buffer
   - `ChorusState` — modulation delay buffers
   - `PhaserState` — allpass buffers
   - `SamplerState` — voice buffers (if arena-allocated)

**Testing**:
1. Catch2 test: Load program with `osc("sin", 440)`, swap to same program, capture oscillator phase before and after crossfade. Assert phase advanced by exactly `crossfade_duration * block_size` samples worth, not 2x.
2. Catch2 test: Load program with delay, swap to same program, verify delay buffer contents are preserved correctly across crossfade.
3. Manual: Play a patch with reverb and delay, live-reload, listen for pitch warble or resonance distortion during crossfade.

**Verification command**:
```bash
cmake --build build --target cedar_tests && ./build/cedar/tests/cedar_tests "[crossfade]"
```

---

### Phase 3 — Fade-In on First Load (Optional, Deferred)

**Goal**: Smooth the transition from silence to first audio output when loading a program into an empty VM.

**Rationale for deferral**: The current behavior (no crossfade on first load) is intentional design. The abrupt start is not a bug — it's how the system was designed. This phase is documented for future consideration only if users report it as a usability issue.

**Sketch**: Add a `FadeIn` struct to VM that applies a 2-block gain ramp (`sin(progress * PI/2)`) to the first blocks of output after a program is loaded into an empty slot. Triggered in `handle_swap()` when `requires_crossfade()` returns false.

---

## 8. Edge Cases

### 8.1 Rapid Successive Swaps

**Situation**: User saves twice in quick succession, triggering two swaps before the first crossfade completes.

**Expected behavior**: The second swap is rejected (`LoadResult::SlotBusy`) because all 3 triple-buffer slots are occupied (Active, Fading, Loading). The second compilation's program is retried on the next available slot. Phase 1 fix is safe here because `clear()` runs at the top of every `perform_crossfade()` call regardless.

### 8.2 Crossfade With No Shared States

**Situation**: User completely rewrites the program — no state IDs match between old and new.

**Expected behavior**: `snapshot_shared_states()` finds zero shared IDs, snapshot count is 0, `restore_shared_states()` is a no-op. Both programs execute with independent fresh states. Crossfade mixes old and new audio normally.

### 8.3 Snapshot Arena Overflow

**Situation**: Patch has many large delays/reverbs whose arena data exceeds the 32MB snapshot arena.

**Expected behavior**: The snapshot arena is a bump allocator like the main one. When `allocate()` returns nullptr (arena exhausted), `ArenaEnumerator::add_region()` skips the copy for that region. The state variant is still copied (struct fields restored), but the arena buffer contents are not — the new program reads old-program-modified arena data for those overflowing states. This is graceful degradation: small states (oscillators, filters) are always protected, large states degrade only when the snapshot arena is genuinely full.

### 8.4 Polyphonic State Isolation

**Situation**: Shared states are accessed with `state_id_xor_` set for voice isolation during `POLY_BEGIN` blocks.

**Expected behavior**: Snapshot uses the same XOR masking as normal execution. The `get_or_create()` path applies `state_id ^ state_id_xor_` — snapshot must use the same effective IDs. Since `state_id_xor_` is set per-voice during `execute_poly_block()`, and snapshot happens *before* execution, it uses the default `state_id_xor_ = 0`. This is correct because the program-level state IDs (from `ProgramSlot::get_state_ids()`) are the base IDs before XOR masking.

### 8.5 State Type Mismatch After Swap

**Situation**: Old program had an oscillator at state_id=X, new program has a filter at the same state_id=X (semantic path collision after major code change).

**Expected behavior**: `get_or_create<T>()` detects the type mismatch and re-emplaces the correct type. The snapshot would contain the old type, restore would put it back, and the new program would then replace it. The snapshot/restore is wasted work for mismatched types but causes no harm.

### 8.6 `begin_frame()` Called Twice During Crossfade

**Situation**: Both `execute_program(old)` and `execute_program(new)` call `state_pool_.begin_frame()`, which clears the `touched_` array.

**Expected behavior**: This only affects GC tracking, not state values. The new program's `begin_frame()` clears touched, then the new program touches its states. After crossfade completes, `gc_sweep()` runs and correctly identifies orphaned states (those only in the old program). No change needed.

---

## 9. Testing / Verification Strategy

### Phase 1 Tests

**Test: Crossfade output level stays bounded** (Catch2, `[crossfade][integration]`)
```
1. Create VM, set sample rate
2. Load program A: PUSH_CONST 1.0 → OUTPUT (DC 1.0 on both channels)
3. Process 10 blocks (establish steady state)
4. Load program B: identical to A
5. Process 10 more blocks (covers entire crossfade + post-crossfade)
6. Assert: no output sample exceeds 1.5 (equal-power peak for identical signals is sqrt(2) ≈ 1.414)
7. Assert: after crossfade completes, output returns to exactly 1.0
8. Assert: maximum sample-to-sample delta across all blocks is < 0.5
```

**Before fix**: Fails — output reaches ~4.0x at end of crossfade.
**After fix**: Passes — output peaks at ~1.414x (equal-power midpoint).

### Phase 2 Tests

**Test: Oscillator phase advances at 1x during crossfade** (Catch2, `[crossfade][state]`)
```
1. Create VM, load program with stateful oscillator
2. Record oscillator phase before crossfade
3. Load identical program, process through crossfade (3 blocks)
4. Record oscillator phase after crossfade
5. Assert: phase difference equals exactly 3 * BLOCK_SIZE samples of advancement
   (not 6 * BLOCK_SIZE which would indicate double-processing)
```

**Test: Delay buffer integrity across crossfade** (Catch2, `[crossfade][state]`)
```
1. Create VM, load program with delay effect
2. Process blocks to fill delay buffer with known signal
3. Load identical program, process through crossfade
4. Assert: delay buffer contents match expected (not corrupted by double-write)
```

### Manual Verification

1. Open web app, write `osc("saw", 220) |> out(@, @)`, press play
2. Change `220` to `440`, observe live reload — no click
3. Add `|> lp(1000, 1)` after the oscillator, reload — no click
4. Add `|> delay(0.25, 0.5, 1, 0)` — reload — no click, delay tail continuous
5. Press stop, press play, recompile — no pop

### Build & Run

```bash
# Build and run crossfade tests
cmake --build build --target cedar_tests
./build/cedar/tests/cedar_tests "[crossfade]"

# Run all tests to check for regressions
./build/cedar/tests/cedar_tests
```
