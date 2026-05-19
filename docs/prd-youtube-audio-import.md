> **Status: NOT STARTED** — No way to import audio from YouTube or other web video/audio platforms directly into nkido. Users must manually download and upload files.

# PRD: YouTube/Video Audio Import via `samples()` with Clips Dict

## Executive Summary

Extend the existing `samples()` directive to accept an optional second argument — a dictionary mapping sample names to clip regions (start/end timestamps). When the URI points to a video or audio source supported by `yt-dlp` (YouTube, SoundCloud, Bandcamp, direct audio URLs, etc.), the host fetches the full audio via a user-configurable backend proxy, extracts each named clip, and registers them as samples in the sample registry. The extracted samples are then usable in patterns exactly like any other bank sample.

On the CLI, `yt-dlp` is invoked directly as a subprocess — no backend server needed.

### Key design decisions

- **No UI panel — "just code."** Users declare imports in their Akkado source via the clips dictionary syntax. This mirrors how `samples("github:...")` already works: a compile-time directive that the host resolves before playback.
- **Backend proxy with yt-dlp.** A small Python/FastAPI server handles audio extraction on the web side. Users self-host and configure the URL in the settings panel.
- **CLI uses yt-dlp directly.** No server needed — `nkido-cli` shells out to `yt-dlp` if available.
- **Strudel-manifest-compatible syntax.** The clips dictionary mirrors the structure of a `strudel.json` manifest: keys are sample names, values are paths (but here, timestamps into the source audio).
- **Any yt-dlp URL works.** Not just YouTube — SoundCloud, Bandcamp, Vimeo, direct MP3/WAV URLs, and anything else yt-dlp supports.

---

## 1. Current State

### 1.1 How samples work today

| Path | Today | With this PRD |
|---|---|---|
| `samples("github:user/repo")` | Fetches strudel.json manifest, registers bank | Unchanged |
| `samples("https://example.com/strudel.json")` | Same as above | Unchanged |
| Upload WAV file via Audio Input panel | Registers as `in('file:name')` source | Unchanged |
| Import audio from YouTube | Not possible — manual download + upload required | `samples("https://youtube.com/...", {kick: {s: 32, e: 34}, snare: {s: 60, e: 62}})` |

### 1.2 Existing infrastructure to build on

| Component | Location | Reuse |
|-----------|----------|-------|
| `handle_samples_call` | `akkado/src/codegen_patterns.cpp:5668` | Extend to accept optional second dict argument |
| `UriRequest` / `UriKind` | `akkado/include/akkado/codegen.hpp` | Add new `UriKind::VideoClip` kind |
| `CompileResult.requiredUris` | `web/src/lib/stores/audio.svelte.ts:174` | New URI kind triggers clip extraction flow |
| `bankRegistry` | `web/src/lib/audio/bank-registry.ts` | Register extracted clips as a synthetic bank |
| `settingsStore` | `web/src/lib/stores/settings.svelte.ts` | Store backend URL + optional API key |
| URI resolver | `web/src/lib/io/uri-resolver.ts` | Unchanged — clips extraction happens before URI resolution |
| Default sample registry | Cedar `SampleBank` | Extracted clips load via `loadAsset(uri, 'sample', name)` |

---

## 2. Goals and Non-Goals

### Goals

- **G1:** `samples(uri, clips_dict)` compiles as a new `UriKind::VideoClip` entry in `requiredUris`.
- **G2:** Web host calls the backend proxy to extract clips; CLI shells out to `yt-dlp` directly.
- **G3:** Extracted clips register as a synthetic sample bank, usable in patterns: `s"kick snare hh" .bank("VideoName") |> out`.
- **G4:** Backend caches the full extracted audio per URL so recompiles are fast.
- **G5:** Per-clip error reporting — if one clip fails, others still load.
- **G6:** Timestamps accept both float seconds (`s: 32.0`) and MM:SS strings (`s: "1:02"`).
- **G7:** Works with any yt-dlp-supported URL, not just YouTube.
- **G8:** Optional API key for backend auth, configured in settings.

### Non-Goals

- **UI panel for import.** Users write the clips dict in code. No drag-drop, no form.
- **Video playback or preview.** Audio extraction only — no video display.
- **Real-time streaming.** Full audio is downloaded, clips extracted, then loaded. Not a live stream.
- **yt-dlp installation management.** CLI checks if `yt-dlp` is on PATH; if not, error with install instructions. No auto-install.
- **SoundFont/wavetable extraction from video.** Audio clips only, registered as samples.
- **Runtime clip extraction.** This is compile-time only. The host resolves all clips before the bytecode swap (existing pre-play barrier).
- **Transcription, beat detection, or auto-naming.** Users specify names and timestamps manually.
- **Godot/Python harness integration.** Web and CLI only in v1.

---

## 3. Target Syntax

### 3.1 Basic clip extraction

```akkado
// Extract two clips from a YouTube video
samples("https://youtube.com/watch?v=dQw4W9WgXcQ", {
    kick:  {s: 32,  e: 34},
    snare: {s: 60,  e: 62},
})

// Use in patterns — bank name derived from video title
s"kick snare kick snare" .bank("Never Gonna Give You Up") |> out(@, @)
```

### 3.2 Multiple variants per sample

```akkado
// A sample with multiple clip regions (variants)
samples("https://youtube.com/watch?v=abc123", {
    hh: [{s: 10, e: 11}, {s: 25, e: 26}, {s: 40, e: 41}],
    bd: {s: 0, e: 1},
})

// Access variants by index
s"hh:0 hh:1 hh:2 hh:0" .bank("abc123") |> out(@, @)
```

### 3.3 Timestamp formats

```akkado
samples("https://youtube.com/watch?v=xyz", {
    // Float seconds (canonical)
    clip_a: {s: 32.5, e: 34.0},

    // MM:SS string (parsed to seconds)
    clip_b: {s: "1:02", e: "1:04.5"},

    // HMS string (hours:minutes:seconds)
    clip_c: {s: "0:05:30", e: "0:05:32"},

    // Mixed formats in the same declaration
    clip_d: {s: 90, e: "2:00"},
})
```

### 3.4 Non-YouTube URLs (any yt-dlp source)

```akkado
// SoundCloud
samples("https://soundcloud.com/artist/track", {
    drop: {s: 45, e: 60},
})

// Bandcamp
samples("https://artist.bandcamp.com/track/song", {
    full: {s: 0, e: 240},
})

// Direct audio URL (yt-dlp will pass through)
samples("https://example.com/audio.mp3", {
    intro: {s: 0, e: 15},
    verse: {s: 15, e: 45},
})
```

### 3.5 Bank name

The bank name is derived from the video/page title returned by yt-dlp (sanitized: spaces → no-op, special chars stripped, max 32 chars). If the title is unavailable, the hostname + path hash is used as fallback.

```akkado
samples("https://youtube.com/watch?v=abc", {kick: {s: 0, e: 2}})
// → bank name: "My Cool Video" (from yt-dlp title)

s"kick" .bank("My Cool Video") |> out(@, @)
```

### 3.6 Error: clip fails

```akkado
samples("https://youtube.com/watch?v=abc", {
    good:  {s: 0, e: 2},
    bad:   {s: 9999, e: 10001},  // past end of video
})

// Compiles successfully. Compile diagnostics include:
// W301: Clip "bad" from "https://youtube.com/watch?v=abc" failed: timestamp 9999s exceeds video duration (247s)
//
// "good" is available; "bad" is silently omitted from the bank.
```

---

## 4. Architecture

### 4.1 End-to-end flow (web)

```
┌──────────────────────────────────────────────────────────────────┐
│  Akkado source                                                    │
│  samples("https://youtube.com/watch?v=abc", {kick: {s:0, e:2}})  │
└──────────────────────────┬───────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│  Akkado compiler (codegen)                                       │
│  - Parses clips dict                                             │
│  - Emits UriRequest{kind: VideoClip, uri, clips: [...]}          │
│  - No audio-time instruction generated                           │
└──────────────────────────┬───────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│  Host (audio.svelte.ts compile() flow)                           │
│  1. Sees requiredUris entry with kind=VideoClip (new kind=4)     │
│  2. Reads backend URL from settingsStore.youtubeBackendUrl       │
│  3. Calls backend: POST /extract {url, clips: [{name,s,e},...]}  │
│  4. Receives per-clip audio data (or per-clip errors)            │
│  5. For each successful clip:                                    │
│     a. Decode audio (WAV/MP3/OGG → float PCM via AudioDecoder)   │
│     b. Register in bankRegistry as synthetic bank                 │
│     c. Load into Cedar SampleBank via loadAsset()                 │
│  6. Block bytecode swap until all clips processed                │
└──────────────────────────┬───────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│  Cedar VM (audio thread)                                         │
│  - Samples available in registry, usable by sample patterns       │
│  - s"kick" .bank("VideoName") resolves to Cedar sample ID    │
└──────────────────────────────────────────────────────────────────┘
```

### 4.2 End-to-end flow (CLI)

```
┌──────────────────────────────────────────────────────────────────┐
│  Akkado source (same as above)                                   │
└──────────────────────────┬───────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│  Akkado compiler (codegen) — same as web                         │
│  Emits UriRequest{kind: VideoClip, uri, clips: [...]}            │
└──────────────────────────┬───────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│  nkido-cli (asset_loader.cpp)                                    │
│  1. Sees UriKind::VideoClip                                      │
│  2. Checks if yt-dlp is on PATH                                  │
│     - If yes: spawns subprocess for each clip:                   │
│       yt-dlp -x --audio-format wav --postprocessor-args          │
│         "-ss {start} -to {end}" -o - "{url}"                     │
│     - If no: error "yt-dlp not found. Install: ..."             │
│  3. Decodes WAV output via AudioDecoder → float PCM              │
│  4. Registers in SampleBank                                      │
│  5. Continues to bytecode swap                                   │
└──────────────────────────────────────────────────────────────────┘
```

### 4.3 Compiler extension

**`akkado/include/akkado/codegen.hpp`:**

```cpp
enum class UriKind {
    SampleBank,   // 0: strudel.json manifest
    SoundFont,    // 1: sf2/sf3
    Wavetable,    // 2: wav for wavetable bank
    Sample,       // 3: standalone audio file
    VideoClip,    // 4: NEW — video/audio URL with clip definitions
};

struct ClipDef {
    std::string name;    // Sample name (e.g., "kick")
    double      start;   // Start time in seconds
    double      end;     // End time in seconds
};

struct UriRequest {
    std::string uri;
    UriKind     kind;
    std::vector<ClipDef> clips;  // Populated when kind == VideoClip
};
```

**`handle_samples_call`** (`akkado/src/codegen_patterns.cpp:5668`): Extended to accept an optional second argument. If the second argument is a record literal (dict), parse it as a clips definition:

```cpp
TypedValue CodeGenerator::handle_samples_call(NodeIndex node, const Node& n) {
    // ... existing URI string parsing ...

    // Check for optional second argument (clips dict)
    auto clips_arg = get_pattern_arg(*ast_, n, 1);
    if (clips_arg != NULL_NODE) {
        // Parse record literal as clips definition
        auto clips = parse_clips_dict(clips_arg);
        if (clips.has_value()) {
            UriRequest req;
            req.uri   = *uri_opt;
            req.kind  = UriKind::VideoClip;
            req.clips = std::move(*clips);
            required_uris_.push_back(std::move(req));
            return TypedValue::void_val();
        }
    }

    // No clips dict — existing SampleBank path
    // ... existing code ...
}
```

**`parse_clips_dict`** walks the record literal AST:
- Top-level keys → sample names
- Values: either a single record `{s: X, e: Y}` or an array of such records
- `s` and `e` values: either numeric literals (float seconds) or string literals (MM:SS / HMS parsed to seconds)
- Errors: E300 for non-numeric/non-string timestamps, E301 for missing `s`/`e` fields, E302 for `start >= end`

### 4.4 Backend API (Python/FastAPI)

**`tools/youtube-backend/main.py`:**

```python
from fastapi import FastAPI, HTTPException, Header
import yt_dlp
import tempfile
import os

app = FastAPI()

API_KEY = os.environ.get("YOUTUBE_BACKEND_API_KEY", "")

@app.post("/extract")
async def extract_clips(
    request: ExtractRequest,
    x_api_key: str = Header("")
):
    if API_KEY and x_api_key != API_KEY:
        raise HTTPException(401, "Invalid API key")

    url = request.url
    clips = request.clips  # [{name, s, e}, ...]

    # Download full audio (cached by URL)
    audio_path = download_and_cache(url)

    results = []
    for clip in clips:
        try:
            # Extract clip segment using ffmpeg
            clip_path = extract_segment(audio_path, clip.s, clip.e)
            with open(clip_path, "rb") as f:
                audio_bytes = f.read()
            results.append({
                "name": clip.name,
                "format": "wav",
                "data": base64.b64encode(audio_bytes).decode(),
            })
        except Exception as ex:
            results.append({
                "name": clip.name,
                "error": str(ex),
            })

    # Return video title for bank naming
    info = yt_dlp.YoutubeDL().extract_info(url, download=False)
    return {
        "title": info.get("title", ""),
        "duration": info.get("duration", 0),
        "clips": results,
    }
```

### 4.5 Backend caching

- Full audio from each URL is cached on disk (default: `~/.cache/nkido/youtube-backend/`).
- Cache key: SHA-256 hash of the URL.
- Cache eviction: LRU at 5GB (configurable via `--max-cache-gb`).
- Clip extraction reads from cached audio — no re-download on recompile.
- Cache entries include the video duration at download time; if the cached entry's duration doesn't match a fresh metadata check, the cache is invalidated.

### 4.6 Web host integration

**`web/src/lib/stores/settings.svelte.ts`** — new fields:

```ts
youtubeBackendUrl: string;       // e.g., "http://localhost:8765"
youtubeBackendApiKey: string;    // optional API key
```

**`web/src/lib/stores/audio.svelte.ts`** — compile() extension:

```ts
// In the requiredUris drain loop (after kind 0 / SampleBank):
if (req.kind === 4) {  // VideoClip
    const backendUrl = settingsStore.youtubeBackendUrl;
    if (!backendUrl) {
        return {
            success: false,
            diagnostics: [{
                severity: 2,
                message: `Video clip import requires a backend URL. Set it in Settings.`,
                line: 1, column: 1
            }]
        };
    }

    const response = await fetch(`${backendUrl}/extract`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            ...(settingsStore.youtubeBackendApiKey
                ? { 'X-Api-Key': settingsStore.youtubeBackendApiKey }
                : {}),
        },
        body: JSON.stringify({
            url: req.uri,
            clips: req.clips.map(c => ({
                name: c.name,
                s: c.start,
                e: c.end,
            })),
        }),
    });

    const result = await response.json();
    const bankName = result.title || deriveBankName(req.uri);

    for (const clip of result.clips) {
        if (clip.error) {
            // Warning diagnostic — clip failed, others proceed
            diagnostics.push({
                severity: 1,
                message: `Clip "${clip.name}" failed: ${clip.error}`,
                line: 1, column: 1
            });
            continue;
        }

        // Decode base64 → ArrayBuffer → load as sample
        const audioBytes = Uint8Array.from(atob(clip.data), c => c.charCodeAt(0));
        const qualifiedName = bankRegistry.getQualifiedName(bankName, clip.name, 0);
        const success = await audioEngine.loadAsset(
            `data:audio/wav;base64,${clip.data}`,
            'sample',
            qualifiedName
        );
        // ... register in bankRegistry as synthetic bank ...
    }
}
```

### 4.7 CLI integration

**`tools/nkido-cli/asset_loader.cpp`** — new handler for `UriKind::VideoClip`:

```cpp
bool extract_video_clips(const UriRequest& req, SampleBank& bank) {
    // Check yt-dlp availability
    if (!is_program_available("yt-dlp")) {
        log_error("yt-dlp not found on PATH. Install it to use video clip imports:");
        log_error("  pip install yt-dlp   or   https://github.com/yt-dlp/yt-dlp#installation");
        return false;
    }

    // Download full audio to temp file
    std::string audio_path = download_audio_ytdlp(req.uri);

    std::string bank_name = get_video_title_ytdlp(req.uri);

    for (const auto& clip : req.clips) {
        std::string clip_path = extract_segment_ffmpeg(audio_path, clip.start, clip.end);
        auto wav_data = load_wav_file(clip_path);
        bank.load_sample(clip.name, wav_data.data.data(), wav_data.channels, wav_data.sample_rate);
    }

    return true;
}
```

---

## 5. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `akkado/include/akkado/codegen.hpp` | **Modified** | Add `UriKind::VideoClip`, `ClipDef` struct, `clips` field on `UriRequest` |
| `akkado/src/codegen_patterns.cpp` | **Modified** | Extend `handle_samples_call` to parse optional clips dict |
| `akkado/src/analyzer.cpp` | **Modified** | Validate clips dict structure (field types, start < end) |
| `web/src/lib/stores/settings.svelte.ts` | **Modified** | Add `youtubeBackendUrl`, `youtubeBackendApiKey` fields + persistence |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** | Handle `kind === 4` in requiredUris drain; call backend; register clips as bank |
| `web/src/lib/audio/bank-registry.ts` | **Modified** | `registerBuiltinBank` already exists; add `registerSyntheticBank(name, samples)` helper |
| `tools/youtube-backend/main.py` | **New** | FastAPI server with yt-dlp integration |
| `tools/youtube-backend/requirements.txt` | **New** | `fastapi`, `uvicorn`, `yt-dlp` |
| `tools/nkido-cli/asset_loader.cpp` | **Modified** | Add `UriKind::VideoClip` handling with yt-dlp subprocess |
| `tools/nkido-cli/asset_loader.hpp` | **Modified** | Declare new functions |
| `web/src/lib/settings.svelte.ts` (settings store) | **Modified** | Persist backend URL + API key |
| `web/static/worklet/cedar-processor.js` | **Unaffected** | No changes — samples load through existing paths |
| `cedar/` (VM, opcodes) | **Unaffected** | No changes — samples are just samples |
| Existing `samples(uri)` (no clips) | **Unaffected** | Backwards compatible — falls through to existing SampleBank path |

---

## 6. File-Level Changes

### Modify

| File | Change |
|------|--------|
| `akkado/include/akkado/codegen.hpp:UriKind` | Add `VideoClip = 4` |
| `akkado/include/akkado/codegen.hpp` | Add `struct ClipDef { std::string name; double start; double end; }` |
| `akkado/include/akkado/codegen.hpp:UriRequest` | Add `std::vector<ClipDef> clips` field |
| `akkado/src/codegen_patterns.cpp:handle_samples_call` | Accept optional second arg; parse as clips dict; emit `VideoClip` UriRequest |
| `akkado/src/codegen_patterns.cpp` | New `parse_clips_dict(NodeIndex)` function — walks record literal AST |
| `akkado/src/analyzer.cpp` | Validate clips dict: keys are strings, values have `s`/`e`, start < end |
| `web/src/lib/stores/settings.svelte.ts` | Add `youtubeBackendUrl: ""` and `youtubeBackendApiKey: ""` with `setYoutubeBackendUrl()` / `setYoutubeBackendApiKey()` |
| `web/src/lib/stores/audio.svelte.ts:compile()` | Add `kind === 4` case in requiredUris drain loop |
| `web/src/lib/audio/bank-registry.ts` | Add `registerSyntheticBank(name: string, samples: Map<string, string[]>)` — creates a bank from in-memory audio data |
| `tools/nkido-cli/asset_loader.cpp` | Handle `UriKind::VideoClip`: check yt-dlp, download, extract clips, register in bank |
| `tools/nkido-cli/asset_loader.hpp` | Add declarations for clip extraction functions |

### Create

| File | Purpose |
|------|---------|
| `tools/youtube-backend/main.py` | FastAPI server: `/extract` endpoint with yt-dlp + ffmpeg |
| `tools/youtube-backend/requirements.txt` | `fastapi>=0.100`, `uvicorn`, `yt-dlp` |
| `tools/youtube-backend/README.md` | Setup + run instructions |
| `web/static/docs/reference/youtube-import.md` | Docs: syntax, backend setup, examples |
| `akkado/tests/test_samples_clips.cpp` | Tests: clips dict parsing, timestamp formats, error cases |
| `experiments/test_op_video_clip.py` | Python experiment: verify end-to-end clip extraction |

### Explicitly NOT changed

- `cedar/` — VM, opcodes, sample playback unchanged. Extracted clips become regular samples.
- `web/static/worklet/cedar-processor.js` — no changes.
- Existing `samples(uri)` without clips dict — backwards compatible.
- `AudioInputPanel.svelte` — no changes (import is code-only, not UI).
- `SampleBrowser.svelte` — no changes (extracted clips appear in bank registry, browsable like any bank).

### Post-change commands

- `cd web && bun run build:docs` — if docs page is indexed
- `cd web && bun run check` — type check

---

## 7. Implementation Phases

### Phase 1 — Compiler: clips dict parsing + new UriKind (1 day)

**Goal:** `samples(uri, {name: {s: X, e: Y}})` compiles and emits `UriRequest{kind: VideoClip}`.

**Files to modify:**
- `akkado/include/akkado/codegen.hpp` — `UriKind::VideoClip`, `ClipDef`, `UriRequest.clips`
- `akkado/src/codegen_patterns.cpp` — extend `handle_samples_call`, add `parse_clips_dict`
- `akkado/src/analyzer.cpp` — validate clips dict structure

**Verification:**
- `samples("url", {kick: {s: 0, e: 2}})` → `required_uris` contains one `VideoClip` entry
- `samples("url", {kick: {s: "1:00", e: "1:02"}})` → timestamps parsed correctly
- `samples("url", {kick: {s: 10, e: 5}})` → compile error E302 (start >= end)
- `samples("url", {kick: {s: 0}})` → compile error E301 (missing `e` field)
- `samples("url")` (no clips) → existing `SampleBank` path, unchanged
- `./build/akkado/tests/akkado_tests "[samples-clips]"` passes

### Phase 2 — Backend proxy (Python/FastAPI) (1 day)

**Goal:** Backend accepts POST `/extract`, returns clips as base64 WAV + video title.

**Files to create:**
- `tools/youtube-backend/main.py`
- `tools/youtube-backend/requirements.txt`
- `tools/youtube-backend/README.md`

**Verification:**
- `uv run main.py` → server starts on port 8765
- `POST /extract` with valid URL + clips → returns clips with audio data
- `POST /extract` with invalid URL → per-clip error in response
- Cache: second request for same URL uses cached audio, no re-download
- API key: `YOUTUBE_BACKEND_API_KEY` env var enables auth

### Phase 3 — Web host: clip extraction + bank registration (1.5 days)

**Goal:** Web app calls backend, registers clips as synthetic bank, loads samples.

**Files to modify:**
- `web/src/lib/stores/settings.svelte.ts` — add backend URL + API key fields
- `web/src/lib/stores/audio.svelte.ts` — handle `kind === 4` in compile flow
- `web/src/lib/audio/bank-registry.ts` — `registerSyntheticBank()` helper

**Verification:**
- Settings panel has new fields for backend URL + API key (stored in localStorage)
- `samples("https://youtube.com/...", {kick: {s: 0, e: 2}})` → backend called, clip loaded
- Sample appears in bank registry, usable via `.bank("VideoTitle")`
- Failed clip → warning diagnostic, other clips still load
- No backend URL configured → compile error with helpful message
- Recompiling same source → backend uses cache, fast response

### Phase 4 — CLI: yt-dlp subprocess (1 day)

**Goal:** `nkido-cli` handles `VideoClip` URIs by shelling out to `yt-dlp`.

**Files to modify:**
- `tools/nkido-cli/asset_loader.cpp` — `UriKind::VideoClip` handler
- `tools/nkido-cli/asset_loader.hpp` — declarations

**Verification:**
- `nkido-cli render --seconds 1 -o out.wav --source 'samples("https://youtube.com/...", {kick: {s: 0, e: 2}}); s"kick" |> out'` works
- yt-dlp not installed → error with install instructions
- Clip extraction uses cached audio on re-run
- `nkido-cli` without video clip sources → unchanged behavior

### Phase 5 — Tests, docs, polish (0.5 day)

**Files to create:**
- `akkado/tests/test_samples_clips.cpp` — compiler tests
- `web/static/docs/reference/youtube-import.md` — user docs

**Verification:**
- `bun run check` clean
- `./build/akkado/tests/akkado_tests` green
- `./build/tools/nkido-cli/nkido-cli --help` still works
- Docs indexed: `bun run build:docs`

**Total estimated effort:** ~5 working days.

---

## 8. Edge Cases

### 8.1 Clips dict parsing

| Input | Expected Behavior |
|-------|-------------------|
| `{kick: {s: 0, e: 2}}` | Valid — one sample, one clip |
| `{hh: [{s: 0, e: 1}, {s: 5, e: 6}]}` | Valid — one sample, two variants |
| `{kick: {s: "1:00", e: "1:02"}}` | Valid — MM:SS strings parsed to 60.0, 62.0 |
| `{kick: {s: "0:05:30", e: "0:05:32"}}` | Valid — HMS strings parsed to 330.0, 332.0 |
| `{kick: {s: 10, e: 5}}` | Compile error E302: start (10) must be less than end (5) |
| `{kick: {s: 0}}` | Compile error E301: missing required field `e` |
| `{kick: {e: 2}}` | Compile error E301: missing required field `s` |
| `{kick: "not a record"}` | Compile error E303: clip value must be record or array of records |
| `{}` | Valid but useless — no clips extracted. Warning W300: "samples() declared with empty clips dict" |
| `{kick: {s: 0, e: 2}, snare: {s: 3, e: 5}}` | Valid — two samples, one clip each |
| Non-string-literal second arg (e.g., variable) | Compile error E304: clips dict must be a literal record (not a variable) |

### 8.2 Backend errors

| Situation | Expected Behavior |
|-----------|-------------------|
| Backend URL not configured | Compile error: "Video clip import requires a backend URL. Set it in Settings." |
| Backend unreachable (connection refused) | Compile error: "Cannot reach YouTube backend at http://localhost:8765: connection refused" |
| Backend returns 401 (bad API key) | Compile error: "YouTube backend auth failed: invalid API key" |
| yt-dlp cannot download URL | Per-clip error: all clips fail with "Download failed: [yt-dlp error]" |
| Clip timestamp past video end | Per-clip error: "Clip 'kick': timestamp 9999s exceeds video duration (247s)" |
| One clip succeeds, one fails | Successful clip loads; failed clip produces warning W301 |
| Video title unavailable | Bank name falls back to hostname + path hash (e.g., "youtube_com_abc123") |

### 8.3 CLI errors

| Situation | Expected Behavior |
|-----------|-------------------|
| `yt-dlp` not on PATH | Error: "yt-dlp not found. Install: pip install yt-dlp" |
| `ffmpeg` not available (needed for clip extraction) | Error: "ffmpeg required for clip extraction. Install: sudo apt install ffmpeg" |
| Network error during download | Error: "Failed to download audio from URL: [error]" |
| Video is live stream (no fixed duration) | Error: "Live streams cannot be clipped. Use a recorded video URL." |
| Video is age-restricted or requires login | Error: "yt-dlp cannot download this video: [reason]" |

### 8.4 Caching

| Situation | Expected Behavior |
|-----------|-------------------|
| Same URL, same clips, recompile | Backend serves from cache, no re-download |
| Same URL, different clips | Backend serves from cache, extracts new clip ranges |
| Cache exceeds 5GB | LRU eviction — oldest unused cached audio removed |
| Cached video was deleted from platform | Metadata check detects duration mismatch → cache invalidated, re-download attempted |
| Cache write fails (disk full) | Non-fatal — warning logged, extraction proceeds without caching |

### 8.5 Bank naming

| Situation | Expected Behavior |
|-----------|-------------------|
| Video has title "My Cool Beat" | Bank name: "My Cool Beat" |
| Video has title "🔥 AMAZING DROP!! (prod. XYZ) [4K]" | Bank name: "AMAZING DROP prod XYZ 4K" (emoji removed, special chars stripped) |
| Video title is longer than 32 chars | Truncated to 32 chars |
| Two different videos have same title | Second `samples()` call produces a warning W302: "Bank name 'Title' already exists, using 'Title (2)'" |
| URL is not a video (e.g., direct MP3) | Bank name derived from filename: "my_beat.mp3" → "my_beat" |

---

## 9. Testing / Verification Strategy

### 9.1 Compiler unit tests (`akkado/tests/test_samples_clips.cpp`)

| Test | Input | Expected |
|------|-------|----------|
| Single clip, float timestamps | `samples("url", {kick: {s: 0, e: 2}})` | `required_uris` has 1 VideoClip entry, clips=[{name:"kick", start:0, end:2}] |
| Single clip, MM:SS timestamps | `samples("url", {kick: {s: "1:00", e: "1:02"}})` | start=60.0, end=62.0 |
| Multiple clips | `samples("url", {kick: {s:0,e:2}, snare: {s:3,e:5}})` | 2 entries in clips array |
| Variants (array) | `samples("url", {hh: [{s:0,e:1}, {s:5,e:6}]})` | 1 clip entry with 2 ClipDefs |
| HMS timestamp | `samples("url", {x: {s: "0:05:30", e: "0:05:32"}})` | start=330.0, end=332.0 |
| Start >= end | `samples("url", {x: {s: 10, e: 5}})` | Compile error E302 |
| Missing `s` field | `samples("url", {x: {e: 2}})` | Compile error E301 |
| Missing `e` field | `samples("url", {x: {s: 0}})` | Compile error E301 |
| Non-record value | `samples("url", {x: "string"})` | Compile error E303 |
| Empty clips dict | `samples("url", {})` | Warning W300, no UriRequest emitted |
| Variable as clips arg | `samples("url", my_clips)` | Compile error E304 |
| No clips arg (backwards compat) | `samples("github:foo/bar")` | Existing SampleBank path, unchanged |
| Mixed timestamp formats | `samples("url", {a: {s: 0, e: "1:00"}})` | start=0.0, end=60.0 |

### 9.2 Backend integration tests

```bash
# Start backend
cd tools/youtube-backend && uv run uvicorn main:app --port 8765

# Test extraction (using a known short video)
curl -X POST http://localhost:8765/extract \
  -H "Content-Type: application/json" \
  -d '{
    "url": "https://www.youtube.com/watch?v=dQw4W9WgXcQ",
    "clips": [
      {"name": "intro", "s": 0, "e": 5},
      {"name": "chorus", "s": 60, "e": 90}
    ]
  }'

# Verify: response contains title, duration, clips with base64 audio data
# Verify: second request returns instantly (cache hit)
# Verify: invalid clip timestamps produce per-clip error entries
```

### 9.3 Web manual tests

1. Open web app, go to Settings → enter backend URL (e.g., `http://localhost:8765`)
2. Compile:
   ```akkado
   samples("https://youtube.com/watch?v=dQw4W9WgXcQ", {
       kick:  {s: 32, e: 34},
       snare: {s: 60, e: 62},
       hh:    [{s: 10, e: 11}, {s: 25, e: 26}],
   })

   s"kick snare hh:0 hh:1" .bank("Never Gonna Give You Up") |> out(@, @)
   ```
3. Verify: samples load, then play. No audio gaps.
4. Recompile same source — verify: fast (cache hit).
5. Clear backend URL in settings → compile → verify: helpful error message.
6. Test with invalid timestamps → verify: per-clip warning, other clips still work.

### 9.4 CLI manual tests

```bash
# yt-dlp installed
nkido-cli render --seconds 5 -o /tmp/test.wav \
  --source '
    samples("https://youtube.com/watch?v=dQw4W9WgXcQ", {
        kick: {s: 32, e: 34},
        snare: {s: 60, e: 62},
    })
    s"kick snare kick snare" .bank("Never Gonna Give You Up") |> out(@, @)
  '

# Listen to /tmp/test.wav

# yt-dlp not installed
nkido-cli render --seconds 1 -o /tmp/x.wav \
  --source 'samples("https://youtube.com/watch?v=abc", {x: {s:0, e:1}}) |> out'
# → Error: "yt-dlp not found. Install: pip install yt-dlp"
```

---

## 10. Open Questions

### 10.1 Output format from backend

**Question:** Should the backend return raw WAV (uncompressed, larger) or let yt-dlp pick the format (smaller but requires decoder on the client)?

**Recommendation:** Return WAV. The Cedar `AudioDecoder` already handles WAV natively on both web and CLI. Predictable size, no format negotiation complexity. yt-dlp's `-x --audio-format wav` ensures WAV output.

### 10.2 Clip overlap

**Question:** What if two clips overlap in the source video?

**Recommendation:** Allowed. Each clip is extracted independently. Overlapping clips are just overlapping samples — the user presumably wants this (e.g., a long pad and a short hit from the same region).

### 10.3 Very long clips

**Question:** What if a clip is very long (e.g., 10 minutes)?

**Recommendation:** No hard limit. A 10-minute WAV at 48kHz stereo is ~55MB — large but manageable. If memory becomes an issue, users can split into smaller clips. Could add a warning W303 for clips > 5 minutes in a future PRD.

### 10.4 Bank name collisions

**Question:** What if a user declares `samples()` from two different videos that happen to have the same title?

**Recommendation:** As noted in §8.5 — append a numeric suffix (`"Title (2)"`). This is consistent with how `bankRegistry` handles duplicate bank names today.

---

## 11. Security & Privacy Notes

### 11.1 Backend is self-hosted

The backend proxy runs on the user's own machine (or a server they control). nkido does not host or operate any extraction service. The backend URL is configured locally and stored in localStorage — it is never sent to any third party.

### 11.2 API key (optional)

If the user exposes the backend to a network (not just localhost), they should set the `YOUTUBE_BACKEND_API_KEY` environment variable. The key is then required in the `X-Api-Key` header. Without it, the backend rejects requests with 401.

### 11.3 YouTube Terms of Service

Downloading audio from YouTube violates YouTube's Terms of Service. The backend does not circumvent this — it uses `yt-dlp`, a widely-available tool that the user installs independently. The feature provides a warning in the docs about TOS implications but does not block usage. Users are responsible for complying with applicable terms and laws.

### 11.4 URL validation

The backend does not validate URLs beyond what yt-dlp does. Malicious URLs (e.g., `file:///etc/passwd`) are not a risk because yt-dlp only handles video/audio platforms and rejects non-media URLs. The backend does not execute arbitrary commands — the URL is passed to yt-dlp as a string argument, not interpolated into a shell command.

---

## 12. Related Work

- [`prd-uri-resolver.md`](prd-uri-resolver.md) — URI scheme system that `samples()` builds on. This PRD adds a new `UriKind` but does not add a new URI scheme.
- [`prd-audio-input.md`](prd-audio-input.md) — Audio input system. Different concern (live signal processing), but shares the `in('file:NAME')` file loading path. This PRD's extracted clips go into the sample registry (patterns), not the input buffer.
- [`prd-sample-loading-before-playback.md`](prd-sample-loading-before-playback.md) — Pre-play barrier that this PRD's clip extraction plugs into. Clips must be fully loaded before the bytecode swap.
- [`web_sample_loading.md`](web_sample_loading.md) — Sample loading guide. Extracted clips follow the same `loadAsset()` path.
- [`prd-records-and-field-access.md`](prd-records-and-field-access.md) — Record literal syntax used by the clips dict.
