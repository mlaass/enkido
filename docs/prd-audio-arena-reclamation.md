> **Status: DONE — all three phases shipped; drift-fuzz regression guard runs
> a single persistent VM clean to 10k iters with zero arena exhaustions
> (2026-06-22).** Follow-up to
> `docs/prd-memory-integrity-tests.md` (§2.2 scoped arena reclamation out of
> the test-infra harness). The drift fuzz (Leg 4) surfaced the bug this PRD
> fixes and is now its regression test.

# PRD: Audio Arena Reclamation

## Executive Summary

`cedar::AudioArena` is a bump allocator: it allocates audio buffers (delay
lines, reverb tanks, chorus/flanger buffers, poly voice pools, per-pattern
sequence/event memory) by advancing an offset, and the only way to free is a
full `reset()`. `audio_arena_.reset()` runs **only** in `VM::reset()` (full
program clear) — never during incremental garbage collection. So every
hot-swap to a structurally different program bump-allocates fresh buffers for
its new DSP states, `gc_sweep` evicts the old states, but their arena memory is
**never reclaimed**. After enough structurally distinct hot-swaps the 32 MB
arena fills, `allocate()` returns `nullptr`, the FX state's `ensure_buffers()`
leaves its buffer pointers null, and the opcode dereferences null and crashes.

The memory-integrity drift fuzz (`akkado/tests/test_drift_fuzz.cpp`) reproduced
this: a single VM hot-swapped through ~150+ mutated FX programs crashed at
`cedar/include/cedar/opcodes/reverbs.hpp:101` (a null comb delay-line buffer).
The fuzz currently works around it by recreating the VM every 12 swaps; this
PRD fixes the root cause so the workaround can be removed and the fuzz becomes
the regression guard.

This PRD ships two things:

1. **Reclamation** — a fixed-capacity, allocation-free, size-classed free list
   inside `AudioArena`. On state eviction (and on buffer growth) a state
   returns its buffers to the free list; a later same-class allocation reuses
   them. The arena stops growing once the working set of live programs
   stabilises, so a long live-coding session no longer drifts toward
   exhaustion.
2. **A null-buffer guard** on every arena-backed FX opcode — if the arena is
   genuinely exhausted (pathological program), the opcode passes the dry signal
   through and emits a single diagnostic instead of crashing.

**Key design decisions (locked — see §10):**

- **Both legs ship:** reclaim (root cause) **and** guard (never crash).
- **Reclaim covers all state-owned arena memory** — FX buffers, poly voice
  pools, and per-pattern sequence/event memory. MIDI sequences (loaded once,
  not per-swap) are out of scope.
- **Per-state `release(AudioArena&)`** dispatched via the existing
  `std::variant` in `StatePool` (`std::visit` on eviction). No new type tag.
- **Rounded size classes**, not exact-fit or a full best-fit allocator.
- **Reclamation runs inside `gc_sweep` / `gc_fading`**, allocation-free, with
  reused buffers re-zeroed — preserving the audio-path zero-allocation
  invariant (memory-integrity Leg 3).
- **Free-list overflow drops the block** (kept in bump space until the next
  full reset), logged once. Never crashes.
- **Grow-path reclaim:** a buffer that reallocates larger returns its old block
  to the free list.
- **Always on** — no flag. Guarded by the drift fuzz + sanitizer suite.

---

## 1. Current State

### 1.1 What exists today

`cedar::AudioArena` (`cedar/include/cedar/vm/audio_arena.hpp`):

- Bump allocator, 32-byte aligned, `DEFAULT_SIZE = CEDAR_ARENA_SIZE` (32 MB).
- `allocate(num_floats)` advances `offset_`; returns `nullptr` when
  `aligned_offset + bytes_needed > size_` (exhausted).
- `reset()` rewinds `offset_` to 0. No per-allocation free.

`cedar::StatePool` (`cedar/include/cedar/vm/state_pool.hpp`):

- Stores each state in a `std::variant` slot (`entry.state.emplace<T>()`).
- `gc_sweep()` (line 255): untouched occupied states move to the fading pool
  (if `fade_blocks_ > 0`) or are cleared immediately.
- `gc_fading()` (line 335): collects faded-out states after the crossfade.
- `reset()` (line 302): clears all slots; `VM::reset()` follows it with
  `audio_arena_.reset()` (`cedar/src/vm/vm.cpp:1984`).

### 1.2 Arena consumers

| Consumer | Site | Size | Reclaim in scope |
|---|---|---|---|
| Delay line (`DelayState`) | `dsp_state.hpp:180` | variable (≤ `MAX_DELAY_SAMPLES` 960k) | Yes |
| Comb/SVF (`MAX_COMB_SAMPLES`) | `dsp_state.hpp:432` | fixed | Yes |
| Flanger (`MAX_FLANGER_SAMPLES`) | `dsp_state.hpp:460` | fixed | Yes |
| Chorus (`MAX_CHORUS_SAMPLES`) | `dsp_state.hpp:487` | fixed | Yes |
| Freeverb (8 comb + 4 allpass × 2 ch) | `dsp_state.hpp:848,853` | fixed (~24 buffers) | Yes |
| Dattorro (predelay + diffusers + delays) | `dsp_state.hpp:931–944` | fixed | Yes |
| Multi-tap delay bank (`MAX_DELAY_SIZE`) | `dsp_state.hpp:1002` | fixed | Yes |
| Granular / wavetable | `dsp_state.hpp:1050` | variable | Yes |
| Poly voice pool | `dsp_state.hpp:642,730` | variable | Yes |
| Sequence / event / output memory | `state_pool.hpp:929,954,978,1087` | variable | Yes |
| FFT probe | `utility.hpp:409–412` | fixed | Yes |
| MIDI sequence (`parse_smf`) | `vm.hpp:431–466` | variable | **No** (loaded once) |

### 1.3 The bug

```
load FX program A → states A.* bump-allocate buffers (offset grows)
hot-swap to program B (different structure)
  → A.* untouched → gc_sweep → fading → gc_fading → slots cleared
  → A.* arena buffers NOT freed (offset stays)
  → states B.* bump-allocate NEW buffers (offset grows again)
... repeat ...
offset_ reaches size_ → allocate() returns nullptr
  → FreeverbState::ensure_buffers leaves comb_buffers[ch][c] == nullptr
  → reverbs.hpp:101  `float delayed = buffer[state.comb_pos[ch][c]];`  ← null deref → SIGSEGV
```

The `ensure_buffers()` pattern (`if (!buffer[0]) buffer[0] = arena->allocate(...)`)
silently accepts a null result; no opcode checks it.

---

## 2. Goals and Non-Goals

### 2.1 Goals

1. A long live-coding session (thousands of structurally distinct hot-swaps)
   holds **bounded** arena usage — peak `offset_` plateaus instead of growing.
2. No FX opcode ever dereferences a null arena buffer; on genuine exhaustion it
   passes the dry signal through and logs once.
3. Reclamation preserves the audio-path zero-allocation invariant (Leg 3) — no
   heap allocation in `gc_sweep` / `process_block`.
4. Reused buffers are re-zeroed, so a recycled delay line never injects stale
   audio (no click/noise).
5. The drift fuzz drops its VM-recreation workaround and asserts bounded arena
   high-water across the whole run.

### 2.2 Non-Goals

- **Reclaiming MIDI sequence memory.** Loaded once per file (deduped), not per
  swap; not a churn source. Future work if needed.
- **A general-purpose allocator** (best-fit, split/coalesce). Rounded size
  classes are sufficient and safer in the audio path.
- **Shrinking the default arena.** 32 MB stays; reclamation makes it
  sufficient. Revisit only if a real workload says otherwise.
- **Per-allocation compaction / defrag.** Freed blocks are reused in place by
  class; no moving.

---

## 3. Architecture / Technical Design

### 3.1 Size-classed free list in `AudioArena`

```
AudioArena
├─ memory_ / size_ / offset_        (unchanged bump state)
└─ free_lists_[NUM_CLASSES]         (NEW)
   class c → intrusive singly-linked list of free blocks of that class

allocate(n):
    cls   = size_class(n)            // round n up to a class
    floats = class_floats(cls)
    if free_lists_[cls] non-empty:
        ptr = pop(free_lists_[cls]); memset(ptr, 0, floats*4); return ptr
    else:
        bump-allocate `floats` (rounded), as today; nullptr on exhaustion

release(ptr, n):
    cls = size_class(n)
    if free_lists_[cls] has capacity: push(ptr)        // store next-ptr in *ptr
    else: drop (log once)            // stays in bump space until reset()
```

- **Intrusive list, zero allocation:** the next-pointer is stored in the freed
  block's own memory (a freed buffer is ≥ one pointer wide), so push/pop touch
  no heap. Per-class block count is bounded by a fixed cap.
- **Size classes:** a fixed ladder covering observed sizes — fixed FX buffers
  land on exact classes; variable buffers (delay, granular, sequence, voice)
  round up. Powers of two from the smallest FX buffer to `MAX_DELAY_SAMPLES`,
  ~20 classes. Internal fragmentation is bounded by the class ratio.
- **`bump-allocate the rounded size`** so a freed class-C block satisfies any
  later class-C request interchangeably.

### 3.2 Per-state release hook

Every arena-owning state type gains:

```cpp
void release(AudioArena& arena) {
    if (buffer[0]) { arena.release(buffer[0], BUF_FLOATS); buffer[0] = nullptr; }
    if (buffer[1]) { arena.release(buffer[1], BUF_FLOATS); buffer[1] = nullptr; }
    // ... every owned buffer ...
}
```

`StatePool` dispatches on eviction via the existing variant:

```cpp
// in the final-removal path (NOT when moving to the fading pool — see §9.1)
std::visit([&](auto& s) {
    if constexpr (has_release<decltype(s)>) s.release(*arena_);
}, entry.state);
```

A C++20 `requires`/`if constexpr` detector makes stateless variant members
(e.g. `OscState`) no-ops with zero boilerplate.

### 3.3 GC integration (timing)

- A state moved to the **fading pool** is still processed during the crossfade
  and must keep its buffers. Release fires only at **final removal**:
  `gc_fading()` (post-fade) and the **immediate-delete** branch of `gc_sweep()`
  (`fade_blocks_ == 0`).
- `StatePool::reset()` skips per-state release (the following
  `audio_arena_.reset()` reclaims everything; the free lists are cleared too).

### 3.4 Grow-path reclaim

`DelayState` (and granular/wavetable) already reallocate when a larger buffer
is needed (`dsp_state.hpp:180`). That path returns the old block first:

```cpp
if (buffer[ch] && buffer_size >= new_size) { /* reuse */ }
else {
    if (buffer[ch]) arena.release(buffer[ch], buffer_size);   // NEW
    buffer[ch] = arena.allocate(new_size);
}
```

### 3.5 Null-buffer guard (exhaustion fallback)

Each arena-backed FX opcode, after `ensure_buffers`, checks validity and falls
back to dry passthrough:

```cpp
state.ensure_buffers(ctx.arena);
if (!state.buffers_ready()) {            // arena exhausted, allocate() failed
    cedar::log_once("arena exhausted: <opcode> degraded to passthrough");
    drywet::passthrough(input, out_l, out_r);   // dry → out, no crash
    return;
}
```

- `buffers_ready()` per state returns true iff all owned pointers are non-null.
- `log_once` is a single process-wide latch (atomic flag) so the message
  appears once, not every block — mirrors the existing `[CEDAR BUG]` style.
- Passthrough = the effect's dry path (copy input to output), already expressed
  by the unified dry/wet convention.

### 3.6 Arena introspection (for the test)

`AudioArena` exposes `std::size_t bytes_used() const { return offset_; }` —
the bump high-water. With reclamation, `offset_` stops growing once the free
list satisfies reallocations, so a plateau is the bounded-memory signal.

---

## 4. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar/include/cedar/vm/audio_arena.hpp` | **Modified** | Add free lists, size classes, `release()`, `bytes_used()`. |
| `cedar/include/cedar/vm/state_pool.hpp` | **Modified** | Call `release()` via `std::visit` on final eviction (`gc_sweep` immediate + `gc_fading`); release sequence/event/voice memory. |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | **Modified** | Each arena-owning state: `release()`, `buffers_ready()`, grow-path reclaim. |
| FX opcode bodies (reverbs/delays/chorus/flanger/phaser/granular) | **Modified** | Null-buffer guard → dry passthrough + `log_once`. |
| `cedar/src/vm/vm.cpp` | **Stays** | `VM::reset()` already resets the arena; free lists clear with it. |
| `akkado/tests/test_drift_fuzz.cpp` | **Modified** | Drop VM-recreation workaround; assert bounded `bytes_used()`. |
| Bytecode / compiler / hot-swap semantics | **Stays** | Pure runtime-memory change; no bytecode or codegen impact. |
| `log_once` helper | **New (or reuse)** | Process-wide one-shot diagnostic latch. |

---

## 5. File-Level Changes

### 5.1 Modified

| File | Change |
|---|---|
| `cedar/include/cedar/vm/audio_arena.hpp` | Size-class free list (`free_lists_`, `size_class()`, intrusive push/pop), `release(ptr, n)`, reuse re-zero, `bytes_used()`, overflow drop + `log_once`. |
| `cedar/include/cedar/vm/state_pool.hpp` | `std::visit` release on final eviction; `release()` for sequence/event/output/voice allocations; clear free lists in `reset()`. |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | `release()` + `buffers_ready()` on each arena-owning state; grow-path reclaim in `DelayState`/granular/wavetable. |
| `cedar/include/cedar/opcodes/reverbs.hpp` | Null guard → passthrough (the observed crash site). |
| `cedar/include/cedar/opcodes/delays.hpp`, `modulation.hpp` (chorus/flanger/phaser), `granular.hpp` | Same null guard. |
| `akkado/tests/test_drift_fuzz.cpp` | Remove `VM_SESSION_SWAPS` recreation; single persistent VM; assert `bytes_used()` plateau. |

### 5.2 New

| File | Purpose |
|---|---|
| `cedar/include/cedar/util/log_once.hpp` (or reuse existing) | One-shot diagnostic latch for exhaustion/overflow messages. |
| `cedar/tests/test_arena_reclaim.cpp` | Unit tests for the free list (§11.1). |

### 5.3 Explicitly NOT changed

| File | Reason |
|---|---|
| Bytecode format, codegen, hot-swap rebind | Reclamation is a runtime-memory concern only. |
| `CEDAR_ARENA_SIZE` default | Stays 32 MB; reclamation makes it sufficient. |

---

## 6. Implementation Phases

### 6.1 Phase 1 — Null-buffer guard (stop the crash)

**Goal:** no FX opcode dereferences a null arena buffer.

**Files:** FX opcode bodies + `buffers_ready()` on each arena-owning state +
`log_once`.

**Verification:** the drift fuzz at high iters (single VM, no recreation) no
longer SIGSEGVs — it degrades effects to passthrough once the arena fills.
(Memory still drifts; Phase 2 fixes that.)

### 6.2 Phase 2 — Free list + reclamation

**Goal:** arena usage stays bounded across structurally distinct hot-swaps.

**Files:** `audio_arena.hpp` (free list), `dsp_state.hpp` + `state_pool.hpp`
(`release()` + `std::visit` eviction), grow-path reclaim.

**Verification:** `cedar/tests/test_arena_reclaim.cpp` passes; a many-distinct-
swap test shows `bytes_used()` plateau.

### 6.3 Phase 3 — Drift fuzz becomes the regression guard

**Goal:** lock the fix in the memory-integrity harness.

**Files:** `test_drift_fuzz.cpp` (remove recreation; assert bounded
`bytes_used()` + no crash + no passthrough fallback under normal churn).

**Verification:** `run_all.sh` + `check-release.sh` green; the previously
crashing seed/iteration runs clean on one VM.

---

## 7. Edge Cases

### 7.1 Crossfade-out state still in use
A state moved to the fading pool is processed during the crossfade. **Release
only at final removal** (`gc_fading`, immediate-delete branch), never on the
move to fading — otherwise a freed buffer is reused under a still-playing
program (use-after-free).

### 7.2 Free-list overflow
More blocks freed than a class's fixed cap holds → drop the block (stays in
bump space until `VM::reset()`), `log_once`. Bounded waste, never a crash.

### 7.3 Buffer grows on a live edit
Delay time / grain size increased → larger buffer needed. Release the old
(smaller) block to its class before allocating the new one (§3.4).

### 7.4 Genuine exhaustion after reclamation
A single program whose live working set legitimately exceeds 32 MB → some
`allocate()` returns null → affected opcode passes through + logs once. Loud,
not fatal. Resolution: raise `CEDAR_ARENA_SIZE` (separate change), not silently.

### 7.5 Reused buffer must be clean
A recycled delay/reverb buffer holds stale audio → re-zero on reuse (§3.1) so
no click/noise leaks into the new effect.

### 7.6 Multi-buffer states
Freeverb/Dattorro own ~24/N buffers. `release()` returns every one;
`buffers_ready()` is true only if every one allocated. Partial allocation (some
succeeded, then exhaustion) → `buffers_ready()` false → passthrough, and the
partial blocks are released so they aren't stranded.

### 7.7 Zero-allocation invariant (Leg 3)
Free-list push/pop and the re-zero `memset` allocate nothing. The
`test_zero_alloc` guard must still pass with reclamation active — add a
hot-swap-under-guard case if practical.

---

## 8. Testing / Verification Strategy

### 8.1 Free-list unit tests (`test_arena_reclaim.cpp`)
- allocate → release → allocate same class returns the **same** pointer.
- released buffer is **zeroed** on reuse (write garbage, free, re-alloc, assert
  all zero).
- variable size rounds to the expected class; cross-class requests don't reuse.
- overflow: free more than a class cap → excess dropped, no crash, `bytes_used`
  unchanged by the drops.
- grow path: allocate small, grow larger, assert old block reused by a
  subsequent small request.

### 8.2 Reclamation integration test
Hot-swap N (e.g. 500) structurally distinct FX programs (reverb/delay/chorus)
into one VM; assert `arena.bytes_used()` plateaus (peak after warmup ≈ peak at
end, within a small tolerance) and no null-buffer passthrough fired.

### 8.3 Null-guard test
Force exhaustion (tiny `CEDAR_ARENA_SIZE` build or many large delays); assert
the opcode outputs the dry signal (passthrough) and the process does not crash;
`log_once` fires exactly once.

### 8.4 Drift fuzz regression (the headline guard)
Remove `VM_SESSION_SWAPS`; run `[drift_fuzz] --iters 10000` on a single VM;
assert: no crash, `bytes_used()` bounded, RSS slope/peak still pass. The seed
that crashed at ~155 swaps now runs clean.

### 8.5 Sanitizer + zero-alloc
`build/sanitize` suite clean (no UBSan null-deref at `reverbs.hpp:101`);
`cedar_tests "[zero_alloc]"` still passes with reclamation active.
