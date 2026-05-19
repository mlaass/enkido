> **Status: IMPLEMENTED**

# Sample Loading Before Playback

## Executive Summary

Sample patterns are silent for the first ~3 beats on initial play because background sample preloading blocks the AudioWorklet thread. The fix: **never start playback until all required samples are loaded, and never swap programs until new samples are loaded.** Same simple pattern for both paths.

Additionally, a small loading indicator appears next to the play/stop buttons while samples are loading.

## Problem Statement

### Current Behavior

When the user first compiles `s"hh hh hh [hh hh] hh hh hh [hh oh]" |> out(@)`:

1. Required samples (hh, oh) load correctly before the program starts
2. `loadDefaultSamples()` fires immediately on worklet init, sending **all** default drum kit samples (~38 files) to the worklet for C++ decoding
3. Each sample decode runs synchronously in the worklet's message handler, blocking `process()` for 100-300ms
4. The first ~3 pattern events fire during these decode gaps and produce no audible output
5. After background preloading finishes, audio plays correctly

On hot-swap recompile, samples are already loaded so the problem doesn't occur.

### Root Cause

`loadDefaultSamples()` sends encoded audio files (WAV/OGG) to the worklet via `loadSampleAudio`, where C++ decodes them synchronously on the audio thread. Each decode of a 100-400KB file blocks `process()` for ~125 blocks (~333ms). Multiple samples loading back-to-back creates cumulative audio gaps spanning the first few beats.

## Goals

1. **No audio gaps on first play** — all samples the program needs must be fully loaded before playback begins
2. **No audio gaps on hot-swap** — if a recompile introduces new samples, wait for them before swapping
3. **Simple, unified code path** — same await-then-play logic for initial and subsequent compiles
4. **Loading indicator** — small visual feedback next to play/stop buttons while samples load

## Non-Goals

- Changing the C++ sample decoding (move to main-thread decode, chunked decode, etc.) — not needed if we await before play
- Optimizing sample preloading strategy — we just need to not preload during playback
- Changing the Cedar VM or SEQPAT opcodes — the engine is correct, only the web orchestration is wrong

---

## Design

### Core Principle

```
compile → load ALL required samples → THEN start playback / swap program
```

No background loading during active playback. No concurrent decode-while-playing.

### Sequence: First Compile

```
1. User presses Ctrl+Enter
2. Send source to worklet for compilation
3. Receive compile result with requiredSamples list
4. Set loading state = true  (show indicator)
5. For each required sample:
   a. If already loaded → skip
   b. Else → fetch, send to worklet, await confirmation
6. Send loadCompiledProgram to worklet, await confirmation
7. Set loading state = false  (hide indicator)
8. Start playback (resume AudioContext)
```

### Sequence: Hot-Swap Recompile

```
1. User presses Ctrl+Enter (audio already playing)
2. Send source to worklet for compilation
3. Receive compile result with requiredSamples list
4. Check if any samples are NOT yet loaded
5. If new samples needed:
   a. Set loading state = true  (show indicator)
   b. For each unloaded sample: fetch, send to worklet, await confirmation
   c. Set loading state = false  (hide indicator)
6. Send loadCompiledProgram to worklet, await confirmation
   (program swaps at next block boundary)
```

Both paths are identical: **await all samples, then load program.** The only difference is whether the AudioContext is already running.

### Background Preloading

Remove `loadDefaultSamples()` from the worklet `'initialized'` handler. Default samples load lazily — when a compile requires them, they're fetched and loaded in step 5 above. This is the existing behavior for required samples; we just stop the eager background preload that races with playback.

If background preloading is desired for faster subsequent compiles, it can run **only when no program is playing** (e.g., after stop, or before first compile). But this is optional — the lazy approach is simpler and correct.

### Loading Indicator

A small spinner/dot appears to the right of the stop button in the Transport bar while `state.isLoadingSamples` is true. Minimal — no modal, no progress bar, just a subtle visual cue.

```
[Play] [Stop] [Loading...]   BPM [120]   Vol [===] 80%
```

---

## File-Level Changes

| File | Change |
|------|--------|
| `web/src/lib/stores/audio.svelte.ts` | Remove `loadDefaultSamples()` call from `'initialized'` handler. Remove `loadDefaultSoundFonts()` call. Remove `loadSampleDecodedOnMainThread()` (recent addition, no longer needed). Add `state.isLoadingSamples` flag. Simplify `compile()` to set loading flag around sample loading. |
| `web/src/lib/stores/editor.svelte.ts` | No changes needed — `evaluate()` already awaits `compile()` |
| `web/src/lib/components/Transport/Transport.svelte` | Add loading indicator after stop button, bound to `audioEngine.isLoadingSamples` |
| `web/static/worklet/cedar-processor.js` | No changes needed |
| `web/wasm/nkido_wasm.cpp` | Remove diagnostic logging (added during investigation) |
| `cedar/include/cedar/opcodes/samplers.hpp` | Remove debug logging |
| `cedar/include/cedar/opcodes/sequencing.hpp` | Remove debug logging |
| `cedar/include/cedar/opcodes/sequence.hpp` | Remove `debug_step_count` field |
| `cedar/include/cedar/opcodes/dsp_state.hpp` | Remove `debug_trigger_count` field |

## Implementation

### 1. Remove background preloading from init

In `audio.svelte.ts`, remove from the `'initialized'` message handler:

```typescript
// REMOVE these two lines:
loadDefaultSamples();
loadDefaultSoundFonts();
```

### 2. Add loading state

```typescript
// In audio state:
isLoadingSamples: false,
```

### 3. Simplify compile()

The `compile()` function already loads required samples and awaits them (steps 2-3 in the current flow). Just wrap the sample loading section with the loading flag:

```typescript
async function compile(source: string): Promise<CompileResult> {
    // Step 1: Compile (fast)
    const compileResult = await compileInWorklet(source);
    if (!compileResult.success) return compileResult;

    // Step 2: Load required samples (await ALL before proceeding)
    state.isLoadingSamples = true;
    try {
        for (const sample of compileResult.requiredSamplesExtended) {
            await ensureBankSampleLoaded(sample);
        }
        for (const sf of compileResult.requiredSoundfonts) {
            await loadSoundFontIfNeeded(sf);
        }
    } finally {
        state.isLoadingSamples = false;
    }

    // Step 3: Load program (only after all samples ready)
    await loadCompiledProgram();

    return compileResult;
}
```

This is the SAME flow for first compile and hot-swap. The only difference is handled by the existing `ensureBankSampleLoaded()` which skips already-loaded samples.

### 4. Add loading indicator to Transport

```svelte
<!-- After stop button, inside .transport-controls -->
{#if audioEngine.isLoadingSamples}
    <span class="loading-indicator" title="Loading samples...">
        <svg class="spinner" width="14" height="14" viewBox="0 0 24 24">
            <circle cx="12" cy="12" r="10" stroke="currentColor" 
                    stroke-width="2" fill="none" stroke-dasharray="31.4 31.4"
                    stroke-linecap="round" />
        </svg>
    </span>
{/if}
```

With a simple CSS spin animation on `.spinner`.

### 5. Clean up debug logging

Remove all `std::printf("[WASM]..."`, `[SEQPAT_STEP]`, `[SAMPLE_PLAY]` debug logging and `debug_step_count`/`debug_trigger_count` fields added during investigation.

---

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| Sample not found (404) | `ensureBankSampleLoaded` returns false, compile reports error — same as current |
| Sample decode fails | Same as above — error reported, playback doesn't start |
| User presses Ctrl+Enter rapidly | Second compile waits for first compile's sample loading to finish (existing `isEvaluating` guard in editor.svelte.ts) |
| Hot-swap with no new samples | Sample loading loop is a no-op (all already loaded), proceeds instantly to loadCompiledProgram |
| Very large sample file | Loading indicator visible while fetching/decoding, no audio gap since we haven't started yet |
| User presses Stop while loading | Loading continues, program loads but doesn't play (AudioContext paused) |

---

## Verification

1. **First compile test**: Open fresh page, type `s"hh hh hh [hh hh] hh hh hh [hh oh]" |> out(@)`, press Ctrl+Enter. ALL events should play from beat 0 with no silent gap.
2. **Hot-swap test**: While playing, change pattern to `s"bd sd bd sd" |> out(@)`. New samples (bd, sd) load, then swap — no audio gap on the new pattern.
3. **Loading indicator test**: When compiling a pattern with unloaded samples, the spinner appears briefly next to the stop button and disappears when loading completes.
4. **No background blocking**: Check browser console — no `[CedarProcessor] Output silent for N blocks` messages during normal playback.
5. **Rapid recompile test**: Press Ctrl+Enter multiple times quickly — no crashes, no orphaned loading states.
