# AudioBackend conformance checklist

Companion to `prd-web-audio-backend.md` §10. Every `AudioBackend`
implementation (today: `WasmAudioBackend`, `JuceAudioBackend` — both in
`web/src/lib/audio/`) must satisfy the behaviors below. The table records
where each row is exercised per backend.

Interface conformance is enforced by `tsc`: both factories return
`... satisfies AudioBackend` (`web/src/lib/audio/audio-backend.ts` is the
source of truth for the surface), so `bun run check` fails on any drift.

Legend for verification columns:

- **web e2e** — Playwright against the built site
  (`web/e2e/hot-swap-audio.spec.ts`, `web/e2e/worker-recovery.spec.ts`),
  driving the real shell → `WasmAudioBackend` → worklet path.
  (The PRD's §10 mention of a "visualization e2e" predates this checklist —
  no such spec exists; viz coverage on web is the hot-swap spec's RMS/level
  assertions plus manual use.)
- **juce unit** — `web/tests/juce-backend.test.ts` against a fake
  `__JUCE__` bridge implementing bridge-protocol v1.
- **studio harness** — the real-bridge run (readiness handshake +
  page↔native RPC over Xvfb) lives in the closed `nkido-studio` repo and
  consumes this checklist; rows marked *(studio)* are asserted there once
  the studio ships its UI PRD.

| # | Behavior | WasmAudioBackend | JuceAudioBackend |
|---|---|---|---|
| 1 | `initialize()` resolves; `isInitialized` pushed true via `onStatus` | web e2e (app boots, compile works) | juce unit: handshake test — `ready` emitted with `protocolVersion`, listeners bound first |
| 2 | Calls issued before readiness never reach an uninitialised engine | web: `initPromise` memo (e2e drop-race behavior) | juce unit: "queues native calls until ready" |
| 3 | `compile(src)` → `CompileResult` with normalized diagnostics (`column`, not `col`) | web e2e: hot-swap + worker-recovery (diagnostics surface) | juce unit: compileResult mapping test |
| 4 | Concurrent compiles: newest wins, stale resolve `{superseded:true}` and cause no UI churn | web e2e: hot-swap rapid re-evals; gen check in `wasm-backend.compile` | juce unit: supersede test |
| 5 | Compile transport failure yields a diagnostic, never a hang (timeout) | web e2e: worker-recovery (crash → synthetic diagnostic → respawn) | juce unit: submit-rejection test; 10 s timeout in code |
| 6 | `play()`/`pause()`/`stop()` are requests; `isPlaying` truth arrives via `onTransport` | web e2e: transport controls drive audible output | juce unit: advisory-transport test *(+ studio: host truth via `playhead`)* |
| 7 | `setBpm`/`setVolume` requests accepted without throw whether or not the engine is up | web: optional-chained posts (e2e sets BPM) | juce unit: unknown-fn degradation (`tryNative`) |
| 8 | `setParam` reaches the engine; host write-back lands via `onParam` (native) | web e2e: param widgets affect audio; web never write-backs | juce unit: `setParam` native-fn args *(write-back: no param event in bridge v1 — studio, when protocol grows)* |
| 9 | Every asset loader resolves a well-formed value and never throws (bool / `SoundFontInfo\|null` / bankId ≥ −1) | web e2e: default samples + banks load during compile; vitest `bank-registry.test.ts` | juce unit: asset-loader degradation test (v1 = native resolver owns assets) |
| 10 | Queries return well-formed data or the documented empty value (`null`/`[]`/`{}`/`0`) | web e2e: pattern highlighting, state inspector, debug panels | juce unit: "polls before any frame" test; `getBuiltins`/`inspectState` JSON parse tests |
| 11 | Viz frame available after compile+play; polls before the first frame resolve empty, never throw | web e2e: hot-swap RMS trace reads live audio | juce unit: playhead + `scope.bin` doorbell-fetch tests |
| 12 | Web-only escape hatches (`getAnalyserNode`/`getAudioContext`/analyser reads/`getMidiController`/`terminateCompileWorker`) are real on web, `null`/no-op/empty on native | web e2e: `test-hooks.readRms()` uses the analyser | juce unit: escape-hatch test |
| 13 | Backend selection: `__JUCE__.backend` present → Juce, absent → Wasm; Juce adapter never constructed on the site path | `web/tests/backend-select.test.ts` (both directions + empty-shim case) | same test |
| 14 | Site bundle contains no `lib/native`/native-only code; native bundle does | `bun run build` → `scripts/check-native-bundle.ts site` | `bun run build:native` → `... native` |

## Running it

```bash
cd web
bun run check                 # interface conformance (tsc, satisfies)
bun run test                  # rows 1–5, 7–13 (unit side)
bunx playwright test          # rows 1, 3–6, 8–12 (web live path)
bun run build                 # row 14 (site)
bun run build:native          # row 14 (native)
```
