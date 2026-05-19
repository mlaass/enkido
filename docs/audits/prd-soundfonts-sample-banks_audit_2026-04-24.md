# Audit: SoundFont and Sample Bank System

**PRD:** `docs/prd-soundfonts-sample-banks.md`
**Audit base:** `1b7f8278c33f9fbee72b459b45f536daf2a1055c`
**Audit head:** `8fc1e05c81ca10d27bde68e6519bc65deb4f9859`
**Audited:** 2026-04-24
**Mode:** Read-only bulk audit

## Summary
- Goals met: 4 of 5 (Section 1.3); Success metrics: 6 of 9 checked as met, 3 unverifiable
- Critical findings: 6 (Unmet=0 core / 3 scope-divergent, Stubs=0, Coverage Gaps=4, Missing Tests=1, Scope Drift=1)
- Recommended PRD Status: `Shipped (with scope divergence)` — update from Draft to Done/Shipped and reconcile the "Non-Goals -> Used TinySoundFont" discrepancy
- One-line verdict: Core feature shipped end-to-end (BankRegistry + bank()/variant() + SOUNDFONT_VOICE opcode + SF2/SF3 parsing via TinySoundFont + web UI), but PRD status still says "Draft" while the header line says "DONE", Success Metrics checkboxes are all unchecked, SoundFont has no automated tests, and several UI elements promised in §2.3/§6 (BankList, PresetList, AddBankDialog, per-sample preview) are absent.

## Goal Verification

| Goal | Status | Evidence |
|---|---|---|
| Strudel-compatible `s("bd sd").bank("TR808")` | Met | `akkado/include/akkado/builtins.hpp:852` defines `bank()` builtin; `akkado/src/codegen_patterns.cpp:2386-2398` emits a bank override that rewrites `required_samples_extended` bank field; `akkado/tests/test_codegen.cpp:4125-4130` exercises `.bank()` method call syntax. |
| SoundFont playback: load `.sf2` and play via MIDI note | Met (scope-divergent) | C++ path: `cedar/src/audio/soundfont.cpp:1-264` parses SF2/SF3 via bundled TinySoundFont (`tsf.h`); opcode at `cedar/include/cedar/opcodes/soundfont.hpp:31-338` handles frequency->MIDI note conversion, zone lookup, DAHDSR envelope, loop modes, SVF filter, voice stealing; Akkado builtin at `akkado/include/akkado/builtins.hpp:265-268` plus handler at `akkado/src/codegen_patterns.cpp:2883-2990`. NOTE: PRD §3.3.2 specified SpessaSynth in TypeScript; shipped implementation uses C++/TinySoundFont instead. |
| Lazy loading (download samples on first trigger) | Met | `web/src/lib/audio/bank-registry.ts:190-250` `resolveSample()` defers sample loading and marks loaded set on success; `web/src/lib/stores/audio.svelte.ts:930-1000` performs sample resolution before playback. |
| Zero-allocation playback | Met | `cedar/include/cedar/opcodes/dsp_state.hpp:534-620` `SoundFontVoiceState` uses arena-allocated voice array, caller-provided zone buffer in `SoundFontBank::find_zones` (`cedar/include/cedar/audio/soundfont.hpp:119-131`). |
| Hot-swap safe (bank changes don't interrupt playing sounds) | Unable to verify | No explicit tests for hot-swap crossfade of SoundFont voices; SoundFont state uses standard StatePool machinery so this should work, but there's no dedicated test or evidence in the PRD that was exercised. |

## Findings

### Unmet Goals

- **Success Metrics (§10) — all 9 checkboxes still unchecked in the PRD.** Six are demonstrably met by code (bank loading, `s"bd".bank("TR808")`, variants via `:N` and `variant()`, SF2 preset listing, polyphonic playback, no audio glitches design). Three are not directly testable from static inspection: correct pitch across keyboard (relies on runtime), volume envelopes/no clicks on note-off (runtime behavior, no automated test), memory usage <500MB (runtime).
- **Non-Goals vs. implementation drift.** PRD §1.4 lists "SoundFont synthesis features (modulators, effects units)" as non-goals, but the shipped opcode implements per-voice SVF lowpass filter (`cedar/include/cedar/opcodes/soundfont.hpp:178-190`) and DAHDSR envelope — which is consistent with "basic playback" but goes past the minimalist non-goal list. Not a violation per se; PRD should be updated to reflect the synthesis features that WERE included.

### Stubs

No `TODO`/stub markers observed in shipped SoundFont/bank code paths.

### Coverage Gaps

- **UI §2.3 "Samples" tab — partial.** PRD promises: Installed Banks list, Add Bank (URL/file/GitHub), SoundFonts list with preset browser, Preview on click. Shipped `web/src/lib/components/Samples/SampleBrowser.svelte:1-126` only shows SoundFonts (list, expand, drop zone, URL loader) and has no banks list, no "Add Bank" dialog, and no audio preview/audition button.
- **Files §6 "New Files" missing.** PRD enumerates `SampleBrowser.svelte`, `BankList.svelte`, `PresetList.svelte`, `AddBankDialog.svelte` plus a dedicated `soundfont-manager.ts`. Only `SampleBrowser.svelte` exists; the other three Svelte components and the TypeScript manager are absent (not needed due to C++-side parsing, but PRD still lists them).
- **Default banks §7.** PRD ships four named banks (`default`, `TR808`, `TR909`, `linndrum`) registered on startup. No registration of `TR808`/`TR909`/`linndrum` is performed anywhere in `web/src/lib/audio/` or `web/src/lib/stores/audio.svelte.ts` — only default GM soundfonts are pre-declared (`web/src/lib/audio/default-soundfonts.ts:7-11`).
- **WASM exports §3.4.** PRD proposes `cedar_load_soundfont_preset` / `cedar_get_soundfont_preset_id`. Shipped WASM API is different: `cedar_load_soundfont` (loads whole SF2), `cedar_soundfont_preset_count`, `cedar_soundfont_presets_json`, `cedar_soundfont_count` at `web/wasm/nkido_wasm.cpp:1668-1710`. Functionally equivalent but API surface diverges from PRD spec.

### Missing Tests

- **No SoundFont tests at all.** Searched `cedar/tests/`, `akkado/tests/`, `experiments/`: zero hits for `soundfont`, `SF2`, `.sf2`. `SoundFontBank::find_zones`, envelope/filter processing in `op_soundfont_voice`, and the SF2 parsing path in `cedar/src/audio/soundfont.cpp` are entirely untested.
- **No bank-registry tests.** No unit tests observed for `web/src/lib/audio/bank-registry.ts` (manifest parsing, GitHub shortcut, variant wrapping, qualified-name generation). Bank codegen path is tested in `akkado/tests/test_codegen.cpp:4079-4180`, but the TypeScript registry is not.
- **No "Phase 0" FileLoader/FileCache audit scope verifiable here.** PRD declares these a prerequisite; files exist (`web/src/lib/io/file-loader.ts`, `file-cache.ts`, `errors.ts`) but that PRD owns its own audit.

### Scope Drift

- **Parser choice (major).** PRD §3.3.2 decisively picks SpessaSynth (TypeScript) over sf2-parser. Shipped code uses TinySoundFont embedded in C++ and parses/decodes entirely within Cedar/WASM — no TypeScript parser, no `web/src/lib/audio/soundfont-manager.ts`. This is a reasonable engineering choice (WASM-native parsing, no JS dep, SF3/Ogg decode via `stb_vorbis`) but the PRD should be updated to reflect it.
- **Opcode naming.** PRD §3.1 lists three opcodes (`SOUNDFONT_NOTE_ON`, `SOUNDFONT_NOTE_OFF`, `SOUNDFONT_VOICE`). Only `SOUNDFONT_VOICE` was implemented (`cedar/include/cedar/vm/instruction.hpp:74`); note-on/off semantics are folded into the voice opcode via gate edge detection. Simpler and acceptable, but PRD diagram is stale.

### Suggestions

- Flip PRD Status from `Draft` to `Shipped (Done)`; also drop or reconcile the stale top-of-file marker ("> Status: DONE — ... commit a741806") with the `**Status:** Draft` line — both coexist today.
- Fix §9 "Implementation Status" — currently says SoundFontManager, SoundFont opcodes, mini-notation `.bank()`, and mini-notation `:N` variants are "Not Yet Implemented"; all four are shipped.
- Tick §10 Success Metrics that are verifiably met, or convert to a post-ship evaluation checklist with evidence links.
- Update §3.3.2 / §3.4 / §6 to describe the TinySoundFont-based parser and the actual WASM exports, and remove the non-shipped Svelte component names.
- Add either C++ unit tests for `SoundFontRegistry::load_from_memory`, zone matching, and envelope timing OR a Python experiment under `experiments/test_op_soundfont_voice.py` following `docs/dsp-experiment-methodology.md`. The opcode currently has no test coverage.
- Add at least a smoke test for `bank-registry.ts` manifest parsing and `getQualifiedName` semantics.
- Decide whether to ship the preconfigured TR808/TR909/linndrum banks promised in §7 or remove that section.

## PRD Status
- Current: `Draft` (contradicted by the top-line "> Status: DONE — ... (commit a741806)")
- Recommended: `Shipped (Done with scope divergence)`
- Reason: End-to-end functionality is present in the repo — BankRegistry (355 lines), mini-notation `:N` variants and `bank()`/`variant()` modifiers, `SOUNDFONT_VOICE` opcode with DAHDSR + SVF filter + voice stealing, SF2/SF3 parsing via TinySoundFont, WASM exports, audio-store integration, and a (partial) Samples panel. Remaining concerns are documentation drift (parser library, WASM API naming, opcodes list), absent UI pieces (banks list, preview, add-bank dialog), missing automated tests for SoundFont code, and un-registered default banks — none of which block shipping the core feature.
