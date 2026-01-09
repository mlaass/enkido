# Cedar VM: Hot-Swap System Implementation

## Overview

Implement glitch-free hot-swapping for live coding with triple buffering, atomic swaps at block boundaries, and 5-10ms crossfade for structural changes.

## Architecture

```
Compiler Thread                    Audio Thread
      │                                 │
      ▼                                 ▼
┌─────────────┐                 ┌─────────────┐
│ acquire()   │                 │ process()   │
│ write prog  │                 │             │
│ submit()    │                 │ check swap? │
└──────┬──────┘                 └──────┬──────┘
       │                               │
       ▼                               ▼
  ┌─────────────────────────────────────────┐
  │          Triple Buffer Slots            │
  │  ┌────────┐ ┌────────┐ ┌────────┐      │
  │  │ SLOT A │ │ SLOT B │ │ SLOT C │      │
  │  │Current │ │Previous│ │  Next  │      │
  │  └────────┘ └────────┘ └────────┘      │
  └─────────────────────────────────────────┘
                     │
                     ▼
            ┌─────────────────┐
            │   StatePool     │
            │ (preserved via  │
            │  semantic IDs)  │
            └─────────────────┘
```

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Buffering | Triple buffer | Compiler never blocks, always has write slot |
| Crossfade level | Output-level | Simple, effective, semantic IDs handle state continuity |
| Crossfade curve | Equal-power | Maintains perceived loudness during transition |
| Duration | 3 blocks (~8ms) | Imperceptible, configurable 2-5 blocks |
| Sync | Lock-free atomics | No mutexes in audio path |



## Crossfade State Machine

```
IDLE ──[new program + structural change]──► PENDING
                                               │
                                    [block boundary]
                                               │
                                               ▼
                                            ACTIVE ◄──┐
                                               │      │
                                         [blocks > 0]─┘
                                               │
                                      [blocks == 0]
                                               │
                                               ▼
                                          COMPLETING
                                               │
                                    [cleanup done]
                                               │
                                               ▼
                                             IDLE
```

## Critical Files

| File | Purpose |
|------|---------|
| `cedar/include/cedar/vm/vm.hpp` | swap controller, crossfade state |
| `cedar/include/cedar/vm/state_pool.hpp` |  fade-out tracking |
| `cedar/include/cedar/dsp/constants.hpp` | MAX_STATES constant |
| `cedar/src/vm/vm.cpp` | handle_swap, perform_crossfade |



## Verification

1. **Unit tests**: ProgramSlot load/signature, SwapController rotation, CrossfadeState transitions
2. **Integration test**: Load program A, swap to B, verify state preserved for matching IDs
3. **Audio test**: Record output during swap, verify no clicks/pops (spectral analysis)
4. **Stress test**: Rapid program updates (10+ per second), verify no crashes or memory leaks
5. **Thread safety**: Run with ThreadSanitizer enabled

## Test Commands
```bash
cmake --build build --target cedar_tests
./build/cedar/tests/cedar_tests "[hotswap]"
./build/cedar/tests/cedar_tests "[crossfade]"
```

## Summary of changes:

| File | Description |
|------|-------------|
| `cedar/include/cedar/vm/program_slot.hpp` | New: ProgramSlot with signature tracking |
| `cedar/include/cedar/vm/swap_controller.hpp` | New: Triple-buffer lock-free swap controller |
| `cedar/include/cedar/vm/crossfade_state.hpp` | New: Crossfade state machine & equal-power mixer |
| `cedar/include/cedar/vm/vm.hpp` | Updated: New hot-swap API, crossfade integration |
| `cedar/src/vm/vm.cpp` | Updated: handle_swap, perform_crossfade, requires_crossfade |
| `cedar/tests/test_vm.cpp` | Updated: Added hot-swap & crossfade tests |

Key features implemented:
- Triple-buffer program slots (compiler never blocks)
- Lock-free atomic swaps at block boundaries
- Semantic ID-based state preservation
- ~8ms equal-power crossfade on structural changes
- State GC for orphaned states

Remaining (optional): StatePool fade-out tracking for smoother orphaned state cleanup.

