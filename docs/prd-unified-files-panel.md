# Unified Files Panel PRD — One Browser for Samples, SoundFonts, MIDI

> **Status: Phase A in flight (2026-05-16).** Phase B (upload persistence)
> deferred. Supersedes the browser-surface decisions in
> [`prd-midi-input.md`](prd-midi-input.md) §7.6 — the §7.6 drop-zone
> consolidation shipped, but the browsing experience it implied did not.

## Executive Summary

The web side panel currently has two file-related tabs that confuse the
user: a **Files** tab (drop zone + last-8 receipts, no real list) and a
**Samples** tab (`SampleBrowser.svelte`) that despite its name only shows
SoundFonts, with an SF2-only URL loader. Drop zones are unified, but
"what is loaded right now?" has no good answer. The auto-loaded BPB 808
drum kit (53 samples) is invisible. Uploaded files vanish on refresh.

This PRD collapses both tabs into one **Files** tab that is a real
browser of every asset the engine knows about, then (in a follow-up
phase) persists user uploads across reloads.

### Why?

User feedback (2026-05-16): *"we were supposed to unify file upload and
samples but now we have two tabs this has clearly been a mistake."*

The root cause is that `prd-midi-input.md` §7.6 specified
*drop-zone* consolidation but left the *browsing* surface to "existing
per-type panels … for explicit browsing." Those panels were never
designed to be the browsing surface for the unified flow — `SampleBrowser`
was a SoundFont browser in disguise, and no panel ever browsed loaded
samples or MIDI files. The split was an accident of incomplete scope.

### Key design decisions

- **One Files tab, full browser.** Sections for Samples, SoundFonts,
  MIDI. Each section is expand-collapsible and lists everything
  currently registered with the audio engine. Drop zone and URL loader
  live at the top of the tab.
- **Built-in vs user uploads are visually distinct.** The bundled BPB
  808 kit appears in a *collapsed sub-section* under Samples so it
  doesn't drown out user uploads but stays discoverable.
- **Per-row removal**: MIDI files in Phase A (where `midiBank.revoke`
  already disconnects the lookup); samples and SoundFonts deferred to
  Phase B alongside the new worklet remove exports (`cedar_remove_sample`,
  `cedar_remove_soundfont` don't exist yet).
- **One URL loader for everything.** Infer kind from the URL's
  extension, dispatch through the existing `audioEngine.loadAsset()`
  surface (already typed for all 4 kinds).
- **MIDI device control stays in Settings.** Web MIDI permission and
  device selection is not a file concern; relocating it would muddy
  the new tab.
- **Persistence is a separate phase.** Restoring user uploads across
  reloads requires an IndexedDB-backed manifest and a restore-on-init
  flow with non-trivial sequencing against the worklet. Phase B.

## 1. Current State

### Tabs (`web/src/lib/components/Panel/SidePanel.svelte`)

| Tab | Component | What it actually shows |
|---|---|---|
| Controls | `ParamsPanel` | Parameter sliders |
| **Files** | `FilesPanel` | Drop zone, kind counts, last-8 receipts |
| **Samples** | `SampleBrowser` | **SoundFonts only**, with SF2-URL loader |
| Settings | inline | Theme, font, audio rate, MIDI device |
| Docs | `DocsPanel` | Documentation viewer |
| Debug | `DebugPanel` | Opt-in |

### Registries (`web/src/lib/stores/audio.svelte.ts`)

| Asset | Storage | Reactive? | Origin tracked? |
|---|---|---|---|
| Samples | `loadedSamples = new Set<string>()` (line 414) | ❌ no | ❌ no |
| SoundFonts | `state.loadedSoundfonts: SoundFontInfo[]` | ✅ yes | ❌ no |
| MIDI files | `midiBank` (`Map<string, string>` in `midi-bank.ts`) | ❌ no | ❌ no |

The non-reactive `loadedSampleCount` getter at line 2356 carries an
inline TODO acknowledging it won't drive a `$derived`.

### Routing (`web/src/lib/audio/file-router.ts`)

`routeFile(file)` already infers kind from extension and dispatches to
the correct per-kind audioEngine API. Drag-drop and the new URL loader
share this routing.

## 2. Goals and Non-Goals

### Goals

- A single Files tab that answers "what is loaded?" at a glance for all
  three asset kinds.
- Built-in default kit visible but visually distinct from user uploads.
- Per-row remove on user uploads.
- One URL loader, all kinds.
- (Phase B) User uploads survive page reload.

### Non-Goals

- **Replacing Web MIDI device control.** Stays in Settings.
- **Editing assets.** No rename, no in-place WAV trimming, no preset
  remapping. View + remove only.
- **Multiple selection / batch operations.** Phase A is one-at-a-time.
- **Per-file metadata UI** beyond what already exists (SoundFont preset
  expansion). No size/length/sample-rate for `.wav`, no MIDI event-count
  preview. Possible future work.
- **Sample-bank manifest editing.** The bundled BPB kit is fixed by
  `default-samples.ts`; user-managed banks are out of scope.

## 3. Target Surface

### Files tab layout

```
┌─ Files ────────────────────────────────┐
│ [ Drop .wav / .sf2 / .mid here ]       │
│ [ URL: https://...        ] [Load]     │
│ ────────────────────────────────────── │
│ ▼ Samples  (1 user, 53 built-in)       │
│    my-loop.wav                  [×]    │
│    ▸ Built-in: 808 drum kit (53)       │
│ ▼ SoundFonts  (1)                      │
│    ▾ gm.sf2   128 presets       [×]    │
│        0  Acoustic Grand        0:0    │
│        1  Bright Acoustic Piano 0:1    │
│        ...                             │
│ ▼ MIDI  (2)                            │
│    song.mid                     [×]    │
│    drums.mid                    [×]    │
│ ────────────────────────────────────── │
│ Errors: <name>: <reason>               │
└────────────────────────────────────────┘
```

Behaviour:

- Sections are independently collapsible (default: Samples & SoundFonts &
  MIDI expanded; built-in sub-section collapsed).
- SoundFont rows expand to show their preset list (lifted from
  `SampleBrowser.svelte:46-70`).
- `[×]` is only rendered on `origin === 'user'` rows.
- URL loader infers kind from the URL's trailing extension. If unknown,
  the error message says so without dispatching.
- Drop and URL errors share the same error region above the sections.

## 4. Architecture

### Reactive registries

Each asset type exposes a reactive list `LoadedFile[]` from `audioEngine`:

```ts
interface LoadedSample {
  name: string;
  origin: 'builtin' | 'user';
  sourceUri?: string;  // present for built-ins (the bundled URL),
                       // absent for user drag-drop blobs.
}
interface LoadedMidiFile {
  name: string;
  origin: 'user';  // no built-in MIDI today; field is for symmetry.
}
// SoundFontInfo gains: origin: 'builtin' | 'user'.
```

The `LoadedSample` array is the canonical source of truth replacing the
`Set<string>` at `audio.svelte.ts:414`. A single
`addLoadedSample(name, origin, sourceUri?)` helper handles dedupe and
push, called from every place that currently does
`loadedSamples.add(name)`.

MIDI gets a parallel `state.loadedMidiFiles` reactive array; the
existing `MidiBank` class continues to own blob URL lifecycle, but
every `register` / `revoke` is mirrored into the reactive array.

### URL loader

`file-router.ts` gains:

```ts
export function inferFromUrl(url: string): FileKind;
```

— a one-liner that pulls the basename from the URL and reuses the
existing `endsWithAny` + extension tables. The FilesPanel calls it,
dispatches through `audioEngine.loadAsset(url, kind, name)`, and
surfaces errors the same way drop errors are surfaced.

### Removal (Phase A scope: MIDI only)

New helper on the audio store:

- `unregisterMidiFile(name)` — wraps `midiBank.revoke(name)` (which
  releases the blob URL on the main thread) and removes the entry from
  the reactive `loadedMidiFiles` list. After removal, the next
  `compile()` cannot resolve `midi({file: name})` and will surface a
  "missing asset" error — same failure mode the engine already handles
  when a `.mid` fails to load.

**Sample and SoundFont removal is deferred to Phase B.** The worklet
exposes `cedar_clear_samples` and `cedar_clear_soundfonts` (full
clears) but no per-asset remove. Phase B will add `cedar_remove_sample`
and `cedar_remove_soundfont` exports and the matching `removeSample` /
`removeSoundFont` worklet messages, then surface remove buttons on
those rows. Until then the UI shows no remove on sample/soundfont rows
to avoid promising behaviour the engine can't deliver.

## 5. File-Level Changes (Phase A)

### `web/src/lib/stores/audio.svelte.ts`

- Line 414: replace `const loadedSamples = new Set<string>()` with a
  reactive array in `state`.
- Lines 600, 951, 1041, 1524, 1556, 1559: every `loadedSamples.add(...)`
  becomes `addLoadedSample(name, origin)` (origin = `'builtin'` for the
  default-kit code paths; `'user'` for drag-drop and URL paths).
- Line 2356: drop `loadedSampleCount`, export `loadedSamples` directly.
- Add `unregisterSample(name)` and `unregisterSoundFont(sfId)`.
- Add `loadedMidiFiles: LoadedMidiFile[]` reactive state; mirror
  midiBank register/revoke calls.
- Extend exported `SoundFontInfo` with `origin: 'builtin' | 'user'`;
  set on all load paths (lines 1268, 1593 for builtin; file-router for
  user).

### `web/src/lib/audio/file-router.ts`

- Add `inferFromUrl(url: string): FileKind`.
- No other changes — drop-routing already correct.

### `web/src/lib/components/Panel/FilesPanel.svelte`

- Full rewrite of the body.
- Markup: drop zone (kept), URL loader (new), 3 collapsible sections
  with per-row remove buttons on user-origin entries.
- SoundFont preset-expand UI lifted verbatim from
  `SampleBrowser.svelte:46-70`.
- Built-in samples appear as a single collapsed row under Samples that
  expands to a virtualized-friendly list of all 53 default names.

### `web/src/lib/components/Panel/SidePanel.svelte`

- Remove the Samples tab entry, its content slot, and the
  `SampleBrowser` import.

### Deletes

- `web/src/lib/components/Samples/SampleBrowser.svelte`
- `web/src/lib/components/Samples/` (if empty after delete)

### Docs

- `docs/prd-midi-input.md` §7.6 (lines 1212-1218): append a one-line
  pointer to this PRD.
- Scan `web/static/docs/` for stale "Samples tab" references; update
  any that remain.

## 6. Phase B — Upload Persistence (Deferred)

### Goal

User-dropped samples, soundfonts, and MIDI files survive a page reload.

### Sketch

- New IndexedDB object store `uploads-manifest` in
  `web/src/lib/io/file-cache.ts` (existing DB: `nkido-file-cache`).
- Each row: `{ name, kind, blobKey, addedAt }`. `blobKey` references an
  entry in the existing `files` object store (or a parallel one for
  uploaded blobs — Phase B decision).
- On `audioEngine.initialize()`: after worklet is up but before the
  first compile, read the manifest, fetch each blob, and re-register
  through the same paths `routeFile` uses.
- `unregisterSample` / `unregisterSoundFont` / `midiBank.revoke` must
  also remove from the manifest.
- `routeFile` writes to the manifest after a successful load.
- Size cap: reuse the existing 500MB `MAX_CACHE_SIZE` LRU eviction in
  `file-cache.ts`.

### Sequencing risks (Phase B will work through)

- The initial compile must wait for restoration, or the first
  compilation will see "missing asset" errors for assets that are about
  to be restored. Either delay first compile or accept the racy state
  and recompile once restoration finishes.
- Restoring a SoundFont takes a separate worklet message + ack;
  parallelizing restoration is fine, but order must be deterministic
  if two manifest rows share a `name`.
- Built-ins are not in the manifest; they continue to load from
  bundled URLs.

### Stop / reset asymmetry — DO NOT regress (fixed in 729cf77)

`stop()` posts `{type: 'reset'}` → `_cedar_reset()` → `VM::reset()`,
which calls `audio_arena_.reset()` and `clear_midi_sequences()`. Every
`MidiSequence*` is arena-owned (prd-midi-input Phase 5), so the
worklet's `midi_sequences_` map is wiped on every Stop. **`SampleBank`,
`SoundFontRegistry`, and `WavetableRegistry` use heap storage and
survive `VM::reset()` — only MIDI is wiped.**

The compile-time loader at `audio.svelte.ts:1402` short-circuits when
`loadedMidiFilesIndex.has(name)`, so without a recovery step the next
compile after Stop never re-uploads the bytes, `init_midi_queue_state`
sets `file_seq = nullptr`, and `advance_file_seq_into_output` produces
silence with no diagnostic. `stop()` currently re-fetches each tracked
MIDI file from `midiBank`'s native blob URL and re-pushes via
`loadMidiFile`.

When Phase B routes uploads through IndexedDB, **at least one of these
must remain true** or the Stop → Play regression returns:

1. `stop()` continues to re-upload MIDI files to the worklet (whether
   from `midiBank`, the IDB blob, or a fresh fetch). Cheapest option.
2. `VM::reset()` stops wiping `midi_sequences_` (would require
   detaching `MidiSequence*` from `audio_arena_`, e.g. moving them to
   a heap-owned registry parallel to `SampleBank`). Cleaner symmetry
   but bigger refactor.
3. `VM::reset()` is split into "light reset" (clock + state pool) and
   "hard reset" (everything), and `stop()` uses the light variant.
   Also cleaner but every caller of `_cedar_reset` would need
   re-evaluating.

The compile-time loader at `audio.svelte.ts:1411-1418` cannot reuse
`midiBank`'s native blob URLs through `loadAsset` because the
uri-resolver `blob:` handler (`web/src/lib/io/handlers/blob-handler.ts`)
only recognises `blob:nkido:` URIs. If Phase B unifies upload reads
through the uri-resolver, either extend the blob handler to accept
native browser blob URLs or use `fetch()` directly the way `stop()`
does today.

## 7. Verification

### Phase A (manual)

1. `cd web && bun run dev`
2. Open the app. Files tab is the only file tab.
3. 808 kit appears as a collapsed sub-section under Samples with the
   correct count (`DEFAULT_DRUM_KIT.length === 53`).
4. Drop a `.wav`: appears under Samples → user, count updates live.
5. `[×]` on the dropped sample: row removed, count decremented.
6. Drop a `.sf2`: appears under SoundFonts; expand → preset list shows.
7. Drop a `.mid`: appears under MIDI; compiling
   `midi({file: "<name>.mid"}) |> poly(...)` resolves correctly.
8. URL loader: paste a SoundFont URL, hit Load, appears in SoundFonts.
9. `bun run check`: TS clean.
10. Refresh: built-ins re-appear; user uploads vanish (Phase B will
    close this).

### Phase B (manual)

- After Phase A's manual flow, refresh the page: dropped sample,
  soundfont, and MIDI file all reappear under their respective
  sections as user-origin entries.

## 8. Related Work

- [`prd-midi-input.md`](prd-midi-input.md) §7.6 — original drop-zone
  consolidation; this PRD supersedes its browsing-surface decisions.
- `web/src/lib/io/file-cache.ts` — existing IndexedDB URI cache; will
  host the upload manifest in Phase B.
- `web/src/lib/audio/default-samples.ts` — canonical built-in kit; also
  consumed by `nkido-cli` (`program_loader.cpp::find_default_bank_uri`).
  Out of scope for changes here.
