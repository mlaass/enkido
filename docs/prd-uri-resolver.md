> **Status: DONE** — Unifies file loading behind a URI-keyed resolver, deletes redundant load entry points, and exposes HTTP sample loading from akkado source.

# PRD: URI Resolver and Akkado HTTP Sample Loading

**Status:** Done
**Author:** Claude
**Date:** 2026-05-02
**Completed:** 2026-05-02
**Related:**
- [File Loading Abstraction (DONE)](prd-file-loading-abstraction.md)
- [SoundFont and Sample Bank PRD](prd-soundfonts-sample-banks.md)
- [Sample Loading Before Playback](prd-sample-loading-before-playback.md)
- [Wavetable Synth](prd-smooch-wavetable-synth.md)
- Investigation: [docs/reports/2026-05-02-file-loading-review.md](reports/2026-05-02-file-loading-review.md)

---

## 1. Executive Summary

File loading in nkido has accreted across five PRDs. The core abstraction shipped (`MemoryView`, `FileLoader`, `AudioDecoder`, IndexedDB cache), but the layers above it didn't converge. There are five WASM load entry points with three different parameter orderings; SoundFont and Wavetable registries each have their own buffer API shape; `BankRegistry` has a parallel `loadFromGitHub` path that double-fetches the manifest; and akkado source code can't reference an asset by URI at all.

This PRD does three things in one stroke:

1. **Introduces `cedar::UriResolver`** — a single, scheme-keyed dispatcher (`file://`, `https://`, `github:`, `bundled://`, `idb://`, `blob://`) that lives on both the C++ and TS sides. All asset loading goes through it.
2. **Aggressively collapses the redundant surfaces** — the WASM bridge drops to one-and-a-half entry points, `SampleBank` to two, `SoundFontRegistry` and `WavetableBankRegistry` standardize on `MemoryView`, the TS `loadFile()` becomes URI-string only, `BankRegistry.loadFromGitHub` is deleted, the audio store's three `loadXFromUrl()` methods collapse to one.
3. **Adds `samples("uri")` to akkado** — programs can declare HTTP, GitHub, or bundled sample sources directly. Compile-time resolution: the host fetches everything before the audio thread sees the new bytecode.

The CLI gets HTTP support for free via cpp-httplib (header-only, no system deps), so `nkido --bank github:tidalcycles/Dirt-Samples song.akkado` works with no extra wiring.

### 1.1 Why now

The five-PRD arc is at its inflection point. One more feature (Strudel-style HTTP loading) would land on top of three different `load_from_memory` shapes and bake in another scheme-specific path. The right move is to consolidate first, ship the new feature on top, and exit with a smaller surface than we started with.

### 1.2 Headline design decisions (confirmed during PRD intake)

- **One PRD, not three.** Resolver, refactor, and akkado HTTP land together. Splitting them risks landing the cleanup half-done.
- **Hard break, no deprecation period.** The only consumers of the redundant entry points are in-tree (worklet, audio store, CLI). Deleting saves us a deprecation cycle and keeps the surface small.
- **`samples()` in akkado, compile-time fetch.** Audio thread never blocks on I/O. The pre-play barrier from `prd-sample-loading-before-playback` already handles the "wait for assets" gate.
- **Default-allow on native HTTP.** No allow-lists or flags in v1. Akkado is a creative-coding language run on the user's own machine; treating every URL as suspicious would make the feature painful with no real threat-model justification.
- **`cedar::UriResolver` as the name.** `akkado::FileResolver` (in `akkado/include/akkado/file_resolver.hpp:12`) already exists for source-file import resolution. Different return type (text vs bytes) and different consumers today, so they stay distinct in this PRD. **Long-term plan:** when akkado gains an `import` statement (planned future feature), the two will be unified — users will want `import "github:user/lib/utils.akkado"` to use the same scheme syntax as `samples("github:...")`. See §11.

---

## 2. Problem Statement

### 2.1 Current state

| Concern | Today's surface | Issue |
|---|---|---|
| Native file loading | `cedar::FileLoader::load(path)` | Fine in isolation. No HTTP, no `github:`, no scheme dispatch. |
| Web file loading | `loadFile({type:'url'\|'file'\|'arraybuffer', ...})` | Discriminated-union source with three shapes. URL caching is opt-in per call. |
| GitHub bank loading | `BankRegistry.loadFromGitHub(repo)` (`web/src/lib/audio/bank-registry.ts:154`) | Hand-rolls URL construction. Raw `fetch()` at line 170 bypasses cache, then `_loadBankInternal` re-fetches the same URL — two network round-trips for every cold load. |
| WASM load entry points | `cedar_load_sample`, `cedar_load_sample_wav`, `cedar_load_audio_data`, `cedar_load_wavetable_wav`, `cedar_load_soundfont` | 5 exports, 3 parameter orderings (`cedar_load_soundfont` puts name last), 2 return-value conventions. `cedar_load_sample_wav` is dead code — no caller in `web/static/worklet/cedar-processor.js`. |
| SoundFont buffer API | `SoundFontRegistry::load_from_memory(const void*, int, ...)` (`cedar/include/cedar/audio/soundfont.hpp:145`) | Raw `void*`/`int` instead of `MemoryView`. Inconsistent with everything else added since the abstraction landed. |
| Wavetable buffer API | `WavetableBankRegistry::load_from_memory(name, uint8_t*, size_t, ...)` | Different again — uses `uint8_t*`, doesn't accept `MemoryView`. |
| SampleBank ingestion | `load_sample`, `load_wav_file`, `load_wav_memory`, `load_audio_data` | Four paths. Last one supersedes middle two but the middle two stay because of WASM bridge calls. |
| Audio store | `loadSampleFromUrl`, `loadSoundFontFromUrl`, `loadWavetableFromUrl` (`web/src/lib/stores/audio.svelte.ts:1288, 1511, 1562`) | Three near-identical methods, each duplicating fetch → decode → register flow. |
| Akkado HTTP loading | (none) | Source code can't reference a URL. Host has to call `bankRegistry.loadBank(url)` out-of-band. |

### 2.2 Proposed state

| Concern | New surface |
|---|---|
| All asset loading | `cedar::UriResolver` (C++) and `uriResolver.load(uri, opts)` (TS), keyed by scheme |
| GitHub bank loading | `loadBank("github:user/repo")` — github scheme handles URL construction internally, then recurses to `https://`. No double-fetch possible. |
| WASM load entry points | `cedar_load_pcm` (already-decoded float shortcut) + `cedar_load_asset(kind, name, data, size)` |
| SoundFont/Wavetable buffer API | Both take `MemoryView` |
| SampleBank ingestion | `load_pcm(name, float*, ...)` + `load_asset(name, MemoryView)` |
| Audio store | `loadAsset(uri, kind?)` — single method, kind sniffed from manifest/extension or passed explicitly |
| Akkado HTTP loading | `samples("uri")` top-level statement; compiler emits `required_uris`; host resolves before bytecode flip |

---

## 3. Goals and Non-Goals

### 3.1 Goals

- **One conceptual model:** an asset is a URI; loading an asset is `resolver.load(uri)`. Same shape on C++ and TS.
- **Lean surface:** the WASM bridge, SampleBank, registries, audio store, and BankRegistry each lose at least one redundant entry point.
- **Akkado HTTP works on web and native CLI.** `nkido --bank github:tidalcycles/Dirt-Samples song.akkado` runs.
- **RT-safe asset loading.** Audio thread never sees an unresolved URI; all fetches happen in compile/host code before the bytecode swap.
- **Future-friendly.** Adding `res://` for a Godot host is a handler registration, not a refactor. Adding `s3://` later would be the same.
- **Bug fix as a side effect:** the GitHub double-fetch becomes structurally impossible.

### 3.2 Non-Goals

- **Generic VFS.** No directory listing, no partial reads, no writes, no watch-for-changes. The resolver fetches bytes by URI. Period.
- **HTTP allow-lists, host policy files, CORS overrides.** Default-allow on native; web is browser-isolated. Revisit if a real threat case appears.
- **ETag / Last-Modified revalidation.** Cache forever, evict by LRU at 500MB. Cache-bust by changing the URI.
- **mmap / streaming for large files.** Not blocking the way we ship. Defer to a future PRD if a workload demands it.
- **`SerializedZone` SF2 wire format.** "Send raw SF2 bytes to TinySoundFont" continues to work. Deferred indefinitely.
- **Web Audio decoder shim.** C++ `AudioDecoder` remains the decoder of record on both sides.
- **Runtime mutation of bundled://.** Compile-time only.

---

## 4. Target Syntax and User Experience

### 4.1 Akkado: `samples()` top-level statement

```akkado
// Single bank from a strudel.json manifest over HTTP
samples("https://example.com/tr808/strudel.json")

s("bd sn hh cp") |> out(%, %)
```

```akkado
// GitHub shortcut — github:user/repo[/branch][/path]
samples("github:tidalcycles/Dirt-Samples")

s("bd:0 cp:1 hh:2") |> out(%, %)
```

```akkado
// Multiple banks, named bank() modifier disambiguates
samples("github:tidalcycles/Dirt-Samples")  // becomes the "Dirt-Samples" bank
samples("https://example.com/909/strudel.json")  // becomes "909"

s("bd cp").bank("909") |> out(%, %)
```

```akkado
// Bundled (compile-time embedded) defaults still work
samples("bundled://default-808")

s("bd") |> out(%, %)
```

Compile-time semantics:
- Each `samples(uri)` call records a `UriRequest{uri, kind: SampleBank}` in `CompileResult.required_uris`.
- Host fetches all `required_uris` before installing the new bytecode in the audio thread.
- Resolution of individual sample variants (`bd:0`) inside a manifest stays lazy, gated by the existing pre-play barrier.

Out of scope for v1: `soundfont("uri")` and `wavetable("uri")` builtins. Both are mechanical extensions once the URI plumbing is in place — left for follow-up to keep this PRD's scope honest.

### 4.2 TS: URI-only `loadFile`

Before:
```ts
loadFile({ type: 'url', url: 'https://...' }, { cache: true })
loadFile({ type: 'file', file: someFile })
loadFile({ type: 'arraybuffer', data: ab, name: 'foo.wav' })
```

After:
```ts
loadFile('https://...')                          // cache opt-in via options
loadFile('github:tidalcycles/Dirt-Samples')      // resolved through github: handler
loadFile('bundled://default-808')                // embedded asset
loadFile('blob:nkido:abc123')                    // transient handle from registerBlob()
```

For File / ArrayBuffer inputs, callers register a transient blob URI:

```ts
const uri = uriResolver.registerBlob(file);     // returns 'blob:nkido:<uuid>'
try {
    const result = await loadFile(uri);
    // ... use result.data ...
} finally {
    uriResolver.unregisterBlob(uri);            // explicit cleanup, no GC magic
}
```

### 4.3 Audio store: one method

Before (`web/src/lib/stores/audio.svelte.ts:1288, 1511, 1562`):
```ts
await loadSampleFromUrl(name, url);
await loadSoundFontFromUrl(name, url);
await loadWavetableFromUrl(name, url);
```

After:
```ts
await loadAsset(uri, 'sample');      // or 'soundfont' | 'wavetable' | 'sample_bank'
```

Kind is required when the URI doesn't carry enough info to disambiguate (most cases). For sample banks, a `strudel.json` manifest is sniffed automatically.

### 4.4 BankRegistry: one entry point

Before:
```ts
bankRegistry.loadBank(urlOrFile, name?);
bankRegistry.loadFromGitHub('user/repo');
```

After:
```ts
bankRegistry.loadBank('github:user/repo');
bankRegistry.loadBank('https://example.com/strudel.json');
bankRegistry.loadBank('blob:nkido:abc123', 'my-uploaded-bank');  // for File uploads
```

`loadFromGitHub` is deleted. The github: scheme handles URL construction. The double-fetch is structurally impossible because there is now one fetch path.

### 4.5 CLI: URIs everywhere

```bash
# Today (only paths)
nkido --soundfont gm.sf2 song.akkado
nkido --bank ./tr808/strudel.json song.akkado

# After
nkido --soundfont https://example.com/gm.sf2 song.akkado
nkido --bank github:tidalcycles/Dirt-Samples song.akkado
nkido --bank ./tr808/strudel.json song.akkado          # bare path = file:// (back-compat)
```

---

## 5. Architecture

### 5.1 Layering

```
┌─────────────────────────────────────────────────────────────────┐
│                  Asset consumers (registries)                    │
│  SampleBank │ SoundFontRegistry │ WavetableBankRegistry          │
│        ▲                                                          │
│        │ MemoryView (uniform)                                     │
│        │                                                          │
│  ┌─────┴────────────────────────────────────────────────────┐   │
│  │                cedar::AudioDecoder                        │   │
│  │       (WAV/OGG/FLAC/MP3, magic-byte detection)            │   │
│  └─────▲────────────────────────────────────────────────────┘   │
│        │ MemoryView                                              │
│        │                                                          │
│  ┌─────┴────────────────────────────────────────────────────┐   │
│  │              cedar::UriResolver (singleton)               │   │
│  │  ┌───────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌─────────┐  │   │
│  │  │file://│ │https://│ │github: │ │bundled │ │ idb://  │  │   │
│  │  │       │ │ http:  │ │        │ │  ://   │ │(web only)│  │   │
│  │  └───┬───┘ └───┬────┘ └───┬────┘ └───┬────┘ └────┬────┘  │   │
│  │      │         │           │          │           │      │   │
│  │  ┌───┴─────────┴───────────┴──────────┴───────────┴───┐  │   │
│  │  │            FileCache (LRU, 500MB)                  │  │   │
│  │  │     (web: IndexedDB, native: ~/.cache/nkido)        │  │   │
│  │  └─────────────────────────────────────────────────────┘  │   │
│  └────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 The C++ resolver

```cpp
// cedar/include/cedar/io/uri_resolver.hpp

namespace cedar {

/// One scheme handler. Implementations live in cedar/src/io/handlers/.
class UriHandler {
public:
    virtual ~UriHandler() = default;

    /// Lowercase scheme without the "://" (e.g. "https", "github", "file", "bundled").
    [[nodiscard]] virtual std::string_view scheme() const = 0;

    /// Resolve URI to bytes. Synchronous on native (background-threaded by caller),
    /// blocking-on-future on WASM (where TS does the actual work).
    [[nodiscard]] virtual LoadResult load(std::string_view uri) const = 0;
};

class UriResolver {
public:
    /// Process-global singleton. Hosts populate at startup.
    static UriResolver& instance();

    /// Register a handler. Last registration for a scheme wins (for tests).
    void register_handler(std::unique_ptr<UriHandler> handler);

    /// Look up handler by scheme. Returns nullptr if no handler.
    [[nodiscard]] const UriHandler* handler_for(std::string_view scheme) const;

    /// Convenience: parse scheme, dispatch, return bytes.
    /// Bare paths (no scheme) are treated as file://.
    [[nodiscard]] LoadResult load(std::string_view uri) const;

private:
    std::unordered_map<std::string, std::unique_ptr<UriHandler>> handlers_;
};

}  // namespace cedar
```

Built-in handlers shipped in `cedar/src/io/handlers/`:

| Handler | File | Built when |
|---|---|---|
| `FileHandler` | `file_handler.cpp` | `!__EMSCRIPTEN__` (native only) |
| `HttpHandler` | `http_handler.cpp` | `!__EMSCRIPTEN__` (native only — cpp-httplib) |
| `GithubHandler` | `github_handler.cpp` | always (forwards to `https://` handler) |
| `BundledHandler` | `bundled_handler.cpp` | always (looks up name in linker-generated table) |

WASM build does **not** register `FileHandler` or `HttpHandler` — TS handles those before calling into WASM. The WASM-side resolver only sees `bundled://` and synthetic asset names produced by the TS layer.

### 5.3 The TS resolver

```ts
// web/src/lib/io/uri-resolver.ts

export interface UriHandler {
    scheme: string;  // 'https', 'github', 'idb', 'bundled', 'blob', 'file'
    load(uri: string, options: LoadOptions): Promise<LoadResult>;
}

class UriResolver {
    private handlers = new Map<string, UriHandler>();

    register(handler: UriHandler): void;
    handlerFor(scheme: string): UriHandler | undefined;
    async load(uri: string, options?: LoadOptions): Promise<LoadResult>;

    // Transient blob registration for File / ArrayBuffer
    registerBlob(source: File | ArrayBuffer, name?: string): string;
    unregisterBlob(uri: string): void;
}

export const uriResolver = new UriResolver();
```

Built-in handlers in `web/src/lib/io/handlers/`:

| Handler | File | Notes |
|---|---|---|
| `httpHandler` | `http-handler.ts` | Wraps `fetch()`, delegates caching to `FileCache` |
| `githubHandler` | `github-handler.ts` | Parses `github:user/repo[/branch][/path]`, recurses into `httpHandler` with raw.githubusercontent.com URL |
| `idbHandler` | `idb-handler.ts` | Direct read from IndexedDB by key (`idb:<key>`) |
| `bundledHandler` | `bundled-handler.ts` | Looks up name in build-time bundled assets table |
| `blobHandler` | `blob-handler.ts` | Resolves `blob:nkido:<uuid>` to in-memory File / ArrayBuffer |

### 5.4 The github: scheme

Defined identically on both sides:

```
github:user/repo                       → https://raw.githubusercontent.com/user/repo/main/strudel.json
github:user/repo/branch                → https://raw.githubusercontent.com/user/repo/branch/strudel.json
github:user/repo/branch/sub/dir        → https://raw.githubusercontent.com/user/repo/branch/sub/dir/strudel.json
github:user/repo/branch/path/file.wav  → https://raw.githubusercontent.com/user/repo/branch/path/file.wav
```

Heuristic: if the path ends in a known audio extension, fetch as-is. Otherwise treat as a directory containing `strudel.json`. This matches what `BankRegistry.loadFromGitHub` does today.

### 5.5 Caching middleware

`FileCache` is unchanged on the web side (existing `web/src/lib/io/file-cache.ts`). On native, a new `cedar::FileCache` writes to `~/.cache/nkido/` (or `$XDG_CACHE_HOME/nkido/`, falling back to a platform-appropriate location on Windows/macOS). LRU eviction at 500MB on both sides.

Cache participation is per-handler:
- `httpHandler` / `githubHandler`: opt-in via `options.cache` (default `true`)
- `fileHandler`: never cached (the filesystem is the cache)
- `bundledHandler` / `blobHandler` / `idbHandler`: never cached (already local)

Cache key: the canonical URI string post-resolution. So `github:user/repo` and the equivalent `https://raw.githubusercontent.com/user/repo/main/strudel.json` share a cache entry — which is correct because the github handler recurses into the http handler with the resolved URL.

### 5.6 CompileResult extension

```cpp
// akkado/include/akkado/codegen.hpp / akkado.hpp

enum class UriKind {
    SampleBank,   // strudel.json manifest
    SoundFont,    // sf2/sf3
    Wavetable,    // wav for wavetable bank
    Sample,       // single audio file
};

struct UriRequest {
    std::string uri;
    UriKind kind;
};

struct CompileResult {
    // ... existing fields ...
    std::vector<UriRequest> required_uris;  // NEW: URIs declared via samples()/etc.
};
```

The existing `required_samples` / `required_soundfonts` / `required_wavetables` lists stay — they describe assets resolved by *name* (the legacy bundled-asset path). `required_uris` describes assets resolved by *URI*. They coexist; nothing is migrated.

### 5.7 Compile-time fetch flow

```
akkado source: samples("github:foo/bar")
        │
        ▼
Compiler emits required_uris.push_back({uri, SampleBank})
        │
        ▼
Host receives CompileResult, NOT YET swapped into audio thread
        │
        ▼
For each required_uris entry:
    bytes = uriResolver.load(uri)
    switch (kind):
        case SampleBank:    bankRegistry.loadBankFromBytes(name, bytes)
        case SoundFont:     soundFontRegistry.load_from_memory(MemoryView(bytes))
        case Wavetable:     wavetableRegistry.load_from_memory(MemoryView(bytes))
        case Sample:        sampleBank.load_audio_data(name, MemoryView(bytes))
        │
        ▼
All required URIs resolved → swap bytecode into audio thread
        │
        ▼
Existing pre-play barrier (prd-sample-loading-before-playback) gates first audio block
```

If any URI fails to resolve, compile is reported as success but install fails with a host-side error. The audio thread continues running the old program. This matches the existing `required_samples` failure mode.

---

## 6. Impact Assessment

### 6.1 What stays, what changes, what's new

| Component | Status | Notes |
|---|---|---|
| `cedar/include/cedar/io/buffer.hpp` (`MemoryView`, `OwnedBuffer`) | **Stays** | Foundation; no API changes. |
| `cedar/include/cedar/io/audio_decoder.hpp` | **Stays** | Decoder is the decoder of record. |
| `cedar/include/cedar/io/file_loader.hpp` | **Modified** | Becomes the implementation behind `FileHandler`; public API kept for direct callers. |
| `cedar/include/cedar/io/errors.hpp` | **Modified** | Add `NetworkError` if not already there; add `Aborted`. |
| `cedar/include/cedar/audio/wav_loader.hpp` | **Stays** | `MemoryView` overload already exists; path-based overload kept as convenience. |
| `cedar/include/cedar/audio/soundfont.hpp` | **Modified** | `load_from_memory(void*, int, ...)` → `load_from_memory(MemoryView, ...)`. |
| `cedar/include/cedar/wavetable/registry.hpp` | **Modified** | Add `load_from_memory(MemoryView)` overload; deprecate `uint8_t*/size_t` form (delete in same PRD). |
| `cedar/include/cedar/vm/sample_bank.hpp` | **Modified** | Collapse to `load_pcm(name, float*, ...)` and `load_audio_data(name, MemoryView)`. Delete `load_wav_file`, `load_wav_memory`. |
| `cedar/include/cedar/io/uri_resolver.hpp` | **New** | `UriHandler`, `UriResolver`. |
| `cedar/src/io/handlers/file_handler.cpp` | **New** | Wraps `FileLoader::load`. |
| `cedar/src/io/handlers/http_handler.cpp` | **New** | cpp-httplib. |
| `cedar/src/io/handlers/github_handler.cpp` | **New** | URL transform + recurse. |
| `cedar/src/io/handlers/bundled_handler.cpp` | **New** | Lookup table populated by linker-generated symbols. |
| `cedar/src/io/file_cache.cpp` | **New** | Disk LRU cache for native. |
| `cedar/third_party/httplib.h` | **New** | cpp-httplib header-only. |
| `akkado/include/akkado/akkado.hpp` (`CompileResult`) | **Modified** | Add `required_uris`. |
| `akkado/include/akkado/codegen.hpp` | **Modified** | Add `UriRequest`, `UriKind`. Codegen for `samples()` builtin. |
| `akkado/include/akkado/builtins.hpp` | **Modified** | Register `samples` as a top-level builtin that records URI in codegen state. |
| `akkado/include/akkado/file_resolver.hpp` | **Stays** | Different concern (source-file imports). Untouched. |
| `web/wasm/nkido_wasm.cpp` | **Modified** | Drop `cedar_load_sample_wav`. Reorder `cedar_load_soundfont` params (name first). Standardize return on `int32_t`/`-1`. |
| `web/src/lib/io/file-loader.ts` | **Modified** | `loadFile(uri, options)` only. `FileSource` discriminated union deleted. |
| `web/src/lib/io/uri-resolver.ts` | **New** | `UriResolver`, handler registry. |
| `web/src/lib/io/handlers/*.ts` | **New** | `http-handler`, `github-handler`, `idb-handler`, `bundled-handler`, `blob-handler`. |
| `web/src/lib/io/file-cache.ts` | **Stays** | No interface change; consumed by `httpHandler`. |
| `web/src/lib/audio/bank-registry.ts` | **Modified** | `loadBank(uri, name?)` only. `loadFromGitHub` deleted. Internal manifest fetch goes through `uriResolver`. |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** | Three `loadXFromUrl` methods → one `loadAsset(uri, kind?)`. |
| `web/static/worklet/cedar-processor.js` | **Modified** | Drop `cedar_load_sample_wav` handler; align param order on remaining loads. |
| `tools/nkido-cli/main.cpp` (or equivalent) | **Modified** | All `--soundfont`, `--bank`, `--sample` flags accept any URI; bare paths treated as `file://`. |
| `tools/nkido-cli/CMakeLists.txt` | **Modified** | Link cpp-httplib (or include via `cedar` target). |

### 6.2 What gets deleted outright

| File / API | Why |
|---|---|
| `BankRegistry.loadFromGitHub` | Subsumed by `loadBank("github:...")`. Bug fix is structural. |
| `cedar_load_sample_wav` (WASM export) | No callers. Strict subset of `cedar_load_audio_data`. |
| `SampleBank::load_wav_file` | Superseded by `load_audio_data`. |
| `SampleBank::load_wav_memory` | Superseded by `load_audio_data`. |
| `SoundFontRegistry::load_from_memory(const void*, int, ...)` overload | Replaced by `MemoryView` overload. |
| `WavetableBankRegistry::load_from_memory(name, uint8_t*, size_t, ...)` overload | Replaced by `MemoryView` overload. |
| `audio.svelte.ts::loadSampleFromUrl` | Subsumed by `loadAsset(uri, 'sample')`. |
| `audio.svelte.ts::loadSoundFontFromUrl` | Subsumed by `loadAsset(uri, 'soundfont')`. |
| `audio.svelte.ts::loadWavetableFromUrl` | Subsumed by `loadAsset(uri, 'wavetable')`. |
| `FileSource` union type in `file-loader.ts` | Replaced by URI-only API; `blob:` scheme covers File/ArrayBuffer cases. |

No `[[deprecated]]` shims, no compat wrappers. The audit table in §10 verifies every deletion has zero remaining call sites at PR time.

---

## 7. File-Level Changes

### 7.1 Files to create

| File | Purpose |
|---|---|
| `cedar/include/cedar/io/uri_resolver.hpp` | Resolver and handler interface. |
| `cedar/src/io/uri_resolver.cpp` | Resolver implementation. |
| `cedar/src/io/handlers/file_handler.cpp` | `file://` handler. |
| `cedar/src/io/handlers/http_handler.cpp` | `https://` / `http://` handler (cpp-httplib). |
| `cedar/src/io/handlers/github_handler.cpp` | `github:` handler (URL transform). |
| `cedar/src/io/handlers/bundled_handler.cpp` | `bundled://` handler. |
| `cedar/include/cedar/io/file_cache.hpp` | Native disk cache interface. |
| `cedar/src/io/file_cache.cpp` | Native disk cache implementation. |
| `cedar/third_party/httplib.h` | cpp-httplib header. |
| `cedar/tests/test_uri_resolver.cpp` | Unit tests for resolver + each handler. |
| `web/src/lib/io/uri-resolver.ts` | TS resolver. |
| `web/src/lib/io/handlers/http-handler.ts` | HTTP handler. |
| `web/src/lib/io/handlers/github-handler.ts` | GitHub URL transform handler. |
| `web/src/lib/io/handlers/idb-handler.ts` | IndexedDB direct-read handler. |
| `web/src/lib/io/handlers/bundled-handler.ts` | Bundled assets handler. |
| `web/src/lib/io/handlers/blob-handler.ts` | Transient File/ArrayBuffer handler. |
| `web/src/lib/io/uri-resolver.test.ts` | TS tests. |

### 7.2 Files to modify

| File | Change |
|---|---|
| `cedar/include/cedar/io/file_loader.hpp` | Add `Aborted` to errors; otherwise stable. |
| `cedar/include/cedar/audio/soundfont.hpp:145` | `load_from_memory(MemoryView, ...)`. |
| `cedar/include/cedar/wavetable/registry.hpp:51,55` | Add `MemoryView` overload, delete raw-pointer one. |
| `cedar/include/cedar/vm/sample_bank.hpp:88,129,144,162` | Collapse to two ingestion paths. |
| `cedar/CMakeLists.txt` | Add new `io/handlers/` sources, link cpp-httplib for non-WASM builds. |
| `akkado/include/akkado/codegen.hpp` | Add `UriKind`, `UriRequest`. |
| `akkado/include/akkado/akkado.hpp:42` | Add `required_uris` to `CompileResult`. |
| `akkado/include/akkado/builtins.hpp` | Register `samples(uri)` builtin. |
| `akkado/src/codegen.cpp` | Implement `samples(uri)` codegen — record URI, no opcodes emitted. |
| `web/wasm/nkido_wasm.cpp:320,339,357,419,1855` | Delete `cedar_load_sample_wav`. Reorder `cedar_load_soundfont` to `(name, data, size)`. Standardize return type on `int32_t`. Add `cedar_get_required_uris` accessor for the new `CompileResult` field. |
| `web/src/lib/io/file-loader.ts` | URI-only `loadFile(uri, options)`. Delete `FileSource`. |
| `web/src/lib/audio/bank-registry.ts:154-185` | Delete `loadFromGitHub`. `loadBank(uri, name?)`. Internal fetch via `uriResolver`. |
| `web/src/lib/stores/audio.svelte.ts:1288,1511,1562` | Replace three methods with `loadAsset(uri, kind?)`. Update call sites at lines 832, 906, 922, 965, 1073, 1108, 1335, 1367, 1404. |
| `web/static/worklet/cedar-processor.js:930,979,1032,1104` | Drop `_cedar_load_sample_wav` handler (no caller anyway). Reorder `_cedar_load_soundfont` arg order. |
| `tools/nkido-cli/*.cpp` | Replace direct `FileLoader::load` with `UriResolver::instance().load(uri)` for `--bank`, `--soundfont`, `--sample` flags. Register `file/http/github/bundled` handlers in `main()`. |

### 7.3 Files explicitly NOT changing

| File | Why |
|---|---|
| `cedar/include/cedar/io/buffer.hpp` | `MemoryView` / `OwnedBuffer` are the foundation; consumed by everything new. |
| `cedar/include/cedar/io/audio_decoder.hpp` | Decoder API is good. |
| `cedar/include/cedar/audio/wav_loader.hpp` | Already `MemoryView`-aware. |
| `web/src/lib/io/file-cache.ts` | LRU semantics unchanged; just gets a new caller (httpHandler). |
| `akkado/include/akkado/file_resolver.hpp` | Different concern (source-file imports for `import` statements), kept distinct from the new URI resolver. |
| Existing `required_samples` / `required_soundfonts` / `required_wavetables` in `CompileResult` | Kept as-is. New `required_uris` coexists; no migration. |

---

## 8. Implementation Phases

Each phase ships independently and leaves the codebase in a working state. Tests gate each phase.

### Phase 1 — C++ resolver foundation (2 days) ✅ DONE

**Goal:** `cedar::UriResolver` exists and dispatches to handlers. `file://` and `bundled://` work on native.

- `cedar/include/cedar/io/uri_resolver.hpp` + `.cpp` ✅
- `FileHandler`, `BundledHandler` ✅
- Unit test: round-trip a known file via `file://`, lookup a bundled asset. ✅

No API users yet. Existing code unchanged. `FileError` enum extended with `NetworkError` and `Aborted` for use by later phases. Tests: 10 cases / 51 assertions in `[uri-resolver]`.

### Phase 2 — Native HTTP + github: + native cache (2 days) ✅ DONE

**Goal:** `nkido --bank github:user/repo manifest.json` returns bytes (not yet wired into bank loading).

- Vendor cpp-httplib into `cedar/third_party/` ✅ (v0.18.5)
- `HttpHandler` (uses cpp-httplib) ✅
- `GithubHandler` (URL transform, recurse) ✅
- `cedar::FileCache` with `~/.cache/nkido/` directory ✅ (XDG-aware, FNV-1a-64 hex keys, mtime-LRU eviction at 500MB)
- Unit test: fetch a small file from `https://raw.githubusercontent.com/...`, second fetch hits cache. ✅ (gated on `CEDAR_ENABLE_NETWORK_TESTS=1`)

OpenSSL linked as a hard dep on native; SYSTEM include path quiets vendored-header warnings. Tests: 13 cases / 67 assertions in `[uri-resolver]` (offline) + 1 case / 7 assertions in `[network]` (live).

### Phase 3 — TS resolver + URI-only loadFile (2 days) ✅ DONE

**Goal:** `loadFile(uri)` works for all schemes on the web. `FileSource` deleted.

- `web/src/lib/io/uri-resolver.ts` ✅ (with `registerBlob` / `unregisterBlob` and in-flight dedup)
- All six web handlers (`http`, `https`, `github`, `idb`, `bundled`, `blob`) ✅
- Rewrite `file-loader.ts` to be a thin wrapper: `loadFile(uri, opts) = uriResolver.load(uri, opts)` ✅
- Update every existing call site of `loadFile({type:'url',...})` to `loadFile(url)` ✅ (4 sites: bank-registry.ts:95, audio.svelte.ts:1296/1513/1564, plus removed `FileSource` import)
- Test: URI dispatch table, blob round-trip, github URL transform. ✅ (19 cases passing)

`bun run check` clean (0 errors). Existing call sites untouched outside the migration. The `loadFromGitHub` path in BankRegistry stays for now — it's deleted in Phase 6 along with the audio store collapse.

### Phase 4 — Aggressive C++ cleanup (1.5 days) ✅ DONE

**Goal:** SoundFont, Wavetable, SampleBank all use `MemoryView`. No more `void*`/`int`.

- `SoundFontRegistry::load_from_memory(MemoryView, name, bank)` ✅
- `WavetableBankRegistry::load_from_memory(name, MemoryView, error)` ✅ (raw-pointer overload deleted)
- `SampleBank::load_wav_file` and `load_wav_memory` deleted ✅. `load_sample` (raw float) and `load_audio_data` (any format via MemoryView) are the only paths. The PRD's `load_pcm` rename is deferred — the existing `load_sample` name is retained because it's idiomatic and the rename adds churn for no semantic gain.
- `cedar_load_sample_wav` (WASM) deleted in this phase too — the symbol depended on `SampleBank::load_wav_memory`. The PRD's phase 5 still owns `cedar_load_soundfont` param reorder + return-type standardization.
- WASM `cedar_load_wavetable_wav` and `cedar_load_soundfont` updated to wrap their input with `MemoryView`. `sample_pack.hpp` ported off `bank.load_wav_file` to a shared `detail::load_sample_file` helper backed by `FileLoader::load + load_audio_data`.

Cedar tests: 184 passing / 1 skipped (network) — all 334,848 assertions green. Akkado tests: 7 failures in `test_codegen.cpp` for pat-chord parsing are pre-existing (caused by uncommitted akkado/ source changes from before phase 1) and don't touch any refactored API.

### Phase 5 — WASM bridge collapse (1 day) ✅ DONE

**Goal:** Lean WASM API. Worklet updated.

- ✅ `cedar_load_sample_wav` already removed in phase 4 (no worklet handler existed). Stale CMakeLists.txt export entry deleted alongside.
- ✅ `cedar_load_soundfont` reordered to `(name, data, size)`.
- ✅ All four loaders (`cedar_load_sample`, `cedar_load_audio_data`, `cedar_load_soundfont`, `cedar_load_wavetable_wav`) standardized on `int32_t` return with `-1` on failure. Worklet success checks unified to `id >= 0`.
- ✅ `cedar-processor.js` SoundFont call site updated; type-check + WASM build clean.

### Phase 6 — BankRegistry collapse + audio store collapse (1.5 days) ✅ DONE

**Goal:** `loadFromGitHub` gone, three audio store methods collapsed to one. GitHub double-fetch fixed structurally.

- ✅ `BankRegistry.loadFromGitHub` deleted. `loadBank('github:user/repo')` flows through the resolver. The github URL transform is shared via `githubToHttps` so the bank registry can derive base URLs without re-fetching.
- ✅ Internal manifest fetch goes through `uriResolver.load(uri)` only — single network round-trip per cold load.
- ✅ `audio.svelte.ts` `loadSampleFromUrl` / `loadSoundFontFromUrl` / `loadWavetableFromUrl` deleted. New unified `loadAsset(uri, kind, name)` with overloaded return type per kind. `loadBankFromGitHub` wrapper removed (callers use `loadBank(uri)`).
- ✅ All call sites updated (9 in audio store, 1 in bank-registry, 1 in SampleBrowser).
- ✅ Regression test: `web/tests/bank-registry.test.ts` spies `fetch`, asserts `loadBank('github:tidalcycles/Dirt-Samples')` does exactly one network call.

### Phase 7 — Akkado samples() builtin + CompileResult.required_uris (2 days) ✅ DONE

**Goal:** `samples("github:...")` in akkado source compiles; host fetches before bytecode swap.

- ✅ `UriKind` enum and `UriRequest` struct in `codegen.hpp`; `required_uris` field on `CodeGenResult` and `CompileResult`.
- ✅ `samples` registered in `builtins.hpp` (Opcode::NOP, 1 required arg) and dispatched via the special-handlers table.
- ✅ `handle_samples_call` in `codegen_patterns.cpp`: validates string literal, dedups, rejects empty URI, appends `UriRequest{kind = SampleBank}`. Emits no audio-time instruction.
- ✅ WASM accessors `akkado_get_required_uri_count` / `akkado_get_required_uri` / `akkado_get_required_uri_kind` exposed and listed in CMakeLists.
- ✅ Worklet extracts `requiredUris` and forwards in the `compiled` message; audio store drains it (kind 0 → `loadAsset(uri, 'sample_bank')`) between the wavetable and SoundFont drains, blocking bytecode swap on resolution.
- ✅ Tests: 7 cases / 23 assertions in `[samples-builtin]` (single URI, source order, dedup, non-literal arg rejection, empty-URI rejection, arity errors, no instruction emitted). Full akkado suite still green at 137,577 assertions.
- 🟡 Native CLI flow (mentioned in PRD goal) lands in phase 8 with `--bank` URI flag.

### Phase 8 — CLI URI flags + docs (1 day) ✅ DONE

**Goal:** CLI flags accept URIs uniformly. Docs updated.

- ✅ `nkido --bank/--soundfont/--sample` accept any URI; bare paths route to `file://`. Flags repeat; banks accumulate as default banks searched in order. `--sample` also accepts `name=uri` for explicit registry-name binding.
- ✅ `nkido` registers all native handlers (`FileHandler`, `HttpHandler×2`, `GithubHandler`, `BundledHandler`) on `cedar::UriResolver::instance()` at startup, with a process-scoped `FileCache`.
- ✅ `samples()` declarations from source flow through the same path: render mode iterates `cr.required_uris`, fetches each manifest, resolves every `RequiredSample` against the loaded banks.
- ✅ New `tools/nkido-cli/asset_loader.{hpp,cpp}` adds a minimal strudel.json scanner (handles `_base`, `_name`, string and string-array fields), `register_native_handlers`, and the bank/soundfont/sample loaders.
- ✅ `akkado --uris` lists `required_uris` (text + JSON modes).
- ✅ `docs/uri-schemes.md` covers the scheme list, `samples()` syntax, CLI usage, and caching. Mirrored to `web/static/docs/reference/` and indexed by `bun run build:docs`.
- ✅ Smoke test: `nkido render --bank github:tidalcycles/Dirt-Samples --seconds 1 -o /tmp/x.wav --source 'sin(440) |> out(%, %)'` succeeds; cold run 301 ms, cache-hit 29 ms (10× speedup).

### Phase 9 — Verification sweep (0.5 day) ✅ DONE

- ✅ §10.4 audit run: zero hits for `loadFromGitHub`, `_cedar_load_sample_wav` / `cedar_load_sample_wav`, `load_wav_file` / `load_wav_memory`, `load_from_memory.*void`, `loadSampleFromUrl` / `loadSoundFontFromUrl` / `loadWavetableFromUrl`. The lone `FileSource` match is `acquireFileSource` (audio-input PRD), unrelated to the deleted file-loader source-type union.
- ✅ Found and ported one orphan: `cedar/include/cedar/audio/sample_pack.hpp:load_samples` was still calling the deleted `bank.load_wav_file`. Routed through `detail::load_sample_file` (already used by the sibling `load_drum_kit`), which goes via `FileLoader::load + load_audio_data`.
- ✅ Cedar tests: 334,788 / 334,788 assertions passing, 184 / 185 cases passing (1 skipped).
- ✅ Akkado tests: 137,577 / 137,577 assertions passing across 570 cases.
- ✅ Web type-check + targeted vitest suites (uri-resolver, bank-registry): 22 / 22 passing.
- ✅ E2E smoke: `nkido render --bank github:tidalcycles/Dirt-Samples --seconds 1 -o out.wav --source 'sin(440) |> out(%, %)'` succeeds; cold 332 ms, cache-hit 28 ms (12× speedup). `samples("github:tidalcycles/Dirt-Samples")` in source resolves identically.
- ✅ Web demo path: `audio.svelte.ts::compile()` drains `requiredUris` between wavetable and SoundFont drains, dispatches kind=SampleBank → `loadAsset(uri, 'sample_bank')`. The `bank-registry.test.ts` regression test pins single-fetch invariant for `loadBank('github:...')`.
- ✅ CHANGELOG entry filed under the next release; PRD §1 status flipped to DONE.

**Total estimated effort:** ~13 working days. Phases 1-3 unblock the rest; phases 4-6 can parallelize if desired; phases 7-9 are sequential.

---

## 9. Edge Cases

### 9.1 URI parsing

| Input | Behavior |
|---|---|
| `/abs/path/file.wav` | Bare path, treated as `file:///abs/path/file.wav`. |
| `./rel/file.wav` | Bare relative path. Resolved against the current working directory (CLI) or treated as a URL relative to the document base (web). |
| `C:\Windows\path` | Windows native path. Treated as `file://C:/Windows/path`. |
| `https://example.com/file.wav?query=1#frag` | Query and fragment preserved end-to-end. Cache key includes them. |
| `github:user/repo` | Resolves to `https://raw.githubusercontent.com/user/repo/main/strudel.json`. |
| `github:user/repo/branch` | Resolves to `https://raw.githubusercontent.com/user/repo/branch/strudel.json`. |
| `github:user/repo/branch/file.wav` | Audio extension detected, fetched as-is — not as `<path>/strudel.json`. |
| `unknown://foo` | Resolver returns `LoadResult` with `FileError::UnsupportedFormat` and message naming the missing scheme. No crash. |
| Empty string | `FileError::InvalidFormat`. |

### 9.2 Caching

| Situation | Behavior |
|---|---|
| Same URI fetched twice | Second hit served from cache, zero network. |
| `github:user/repo` and the equivalent `https://raw.githubusercontent.com/...` URI both fetched | The github handler recurses into http; both share the http handler's cache entry. No double caching. |
| Cache exceeds 500MB | LRU eviction. The just-fetched item is never evicted in the same call. |
| Cache write fails (disk full, IDB quota) | Non-fatal. Resolver returns the bytes; logs a warning; subsequent calls re-fetch. |
| Cache read corrupted | Treated as miss. Re-fetch and overwrite. |

### 9.3 Concurrent loads of the same URI

| Situation | Behavior |
|---|---|
| Two parallel `loadFile(url)` for the same URI | Single in-flight network request; both promises resolve from the same `ArrayBuffer`. (This already works in `BankRegistry.loadingPromises` for banks; lift to the resolver.) |

### 9.4 Akkado source

| Source | Behavior |
|---|---|
| `samples("github:foo/bar"); samples("github:foo/bar")` | Same URI declared twice → recorded once in `required_uris` (deduplication by URI string). |
| `samples("github:foo/bar"); samples("https://..../bar/strudel.json")` (resolves to same URL) | Recorded as two distinct `required_uris` entries because URI strings differ. Cache eats the duplicate fetch. Bank registers under whichever name the manifest's `_name` says, second registration is a no-op. |
| `samples()` with no argument | Compile error: "samples() requires a URI string". |
| `samples(some_var)` (non-literal) | Compile error in v1: URIs must be string literals. (Runtime URI construction deferred — would require an audio-thread-safe registration path.) |
| `samples("bundled://foo")` for a name not bundled | Compile succeeds. Host-side load fails after compile; error surfaced via the existing diagnostics path. Audio thread keeps the old program. |

### 9.5 Native HTTP

| Situation | Behavior |
|---|---|
| HTTP request times out | `FileError::NetworkError`, message includes timeout. Default timeout: 30 seconds. |
| Server returns redirect | Followed up to 5 hops, then `NetworkError`. |
| Server returns 4xx/5xx | `FileError::NetworkError` (or `NotFound` for 404), message includes status code. |
| HTTPS cert validation fails | `FileError::NetworkError`. cpp-httplib uses the system cert store. No `--insecure` flag in v1. |
| User passes `http://` (not https) | Allowed. cpp-httplib supports both. No browser to warn about mixed content. |

### 9.6 Resolver registration

| Situation | Behavior |
|---|---|
| Two handlers registered for the same scheme | Last registration wins. (Tests rely on this.) |
| Handler not registered for a scheme | `load()` returns `FileError::UnsupportedFormat` naming the scheme. |
| Handler throws unexpectedly | Caught at the resolver boundary, converted to `FileError::Corrupted`. |

### 9.7 blob: scheme lifecycle (web)

| Situation | Behavior |
|---|---|
| `registerBlob` then never `unregisterBlob` | Memory leak — File/ArrayBuffer kept alive forever. Linter rule or doc note: register in a try/finally. |
| `unregisterBlob` for a URI that was never registered | No-op. |
| `loadFile` on a blob URI after `unregisterBlob` | `FileError::NotFound`. |

---

## 10. Verification Strategy

### 10.1 Unit tests

| Layer | Test |
|---|---|
| `cedar::UriResolver` | Round-trip through each handler; unknown scheme returns proper error; concurrent loads share network. |
| `FileHandler` | Read existing file; nonexistent returns `NotFound`; large file (>10MB) loads completely. |
| `HttpHandler` | Live test against `https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/main/bd/BT0A0A7.wav` (small file, stable URL); 404 produces `NotFound`; cache hit on second call. |
| `GithubHandler` | URL transform table tests for all four `github:` syntactic shapes; fallthrough to http handler. |
| `BundledHandler` | Lookup by name; missing name returns `NotFound`. |
| `cedar::FileCache` | Set/get round-trip; LRU eviction at 500MB; concurrent set safe. |
| TS handlers | Same shape as C++ tests, plus blob lifecycle, IDB direct read. |
| `bank-registry.ts` | `loadBank("github:...")` produces a manifest with samples; second call returns cached manifest, **no second fetch**. |
| `audio.svelte.ts::loadAsset` | Each kind (sample, soundfont, wavetable, sample_bank) routes to the right registry. |
| Akkado: `samples()` codegen | `samples("github:foo/bar")` produces `required_uris == [{uri:"github:foo/bar", kind:SampleBank}]`; duplicate URIs deduped; non-literal arg rejected at compile. |

### 10.2 Integration tests

```bash
# Build
cmake --preset debug && cmake --build build

# Cedar tests pass
./build/cedar/tests/cedar_tests
./build/cedar/tests/cedar_tests "[uri-resolver]"

# Akkado tests pass
./build/akkado/tests/akkado_tests
./build/akkado/tests/akkado_tests "[samples-builtin]"

# Native CLI: github bank end-to-end
./build/bin/nkido render \
    --bank github:tidalcycles/Dirt-Samples \
    --seconds 5 \
    --out /tmp/test.wav \
    -e 's("bd:0 cp:0 hh:0") |> out(%, %)'
# Listen to /tmp/test.wav

# Native CLI: HTTP soundfont
./build/bin/nkido render \
    --soundfont https://example.com/gm.sf2 \
    -e 'soundfont("gm.sf2", 0) |> out(%, %)' \
    --out /tmp/sf.wav

# Cache works on second run (much faster)
time ./build/bin/nkido render --bank github:tidalcycles/Dirt-Samples ...
time ./build/bin/nkido render --bank github:tidalcycles/Dirt-Samples ...
# Second run should be near-instant for the manifest fetch
```

### 10.3 Web integration test

- Open the web app
- Paste `samples("github:tidalcycles/Dirt-Samples"); s("bd cp hh") |> out(%, %)` into the editor
- Hit play
- Verify samples download, then play
- Refresh, repeat — second load should hit IDB cache (visible in DevTools network tab)

### 10.4 Audit before merge

Verify every deletion has zero call sites:

```bash
# §6.2 deletion audit
grep -rn "loadFromGitHub" web/src/ web/static/                          # expect: zero
grep -rn "_cedar_load_sample_wav\|cedar_load_sample_wav" .              # expect: zero (decl + def removed too)
grep -rn "load_wav_file\|load_wav_memory" cedar/ akkado/ tools/         # expect: zero (or only the deleted-line context)
grep -rn "load_from_memory.*void\b" cedar/include/cedar/audio/          # expect: zero
grep -rn "FileSource" web/src/                                          # expect: zero
grep -rn "loadSampleFromUrl\|loadSoundFontFromUrl\|loadWavetableFromUrl" web/src/  # expect: zero
```

If any of those return hits, the cleanup phase is incomplete.

### 10.5 Regression coverage for the GitHub double-fetch

```ts
// web/src/lib/audio/bank-registry.test.ts
test('loadBank("github:...") performs exactly one network fetch', async () => {
    const fetchSpy = vi.spyOn(global, 'fetch');
    await bankRegistry.loadBank('github:tidalcycles/Dirt-Samples');
    expect(fetchSpy).toHaveBeenCalledTimes(1);
});
```

Today this would fail (two fetches). After the refactor, structurally impossible to fail.

---

## 11. Open Questions

None at draft time — all decisions captured in §1.2 and the section bodies. Items deliberately deferred to follow-up PRDs:

- `soundfont(uri)` and `wavetable(uri)` akkado builtins. Adding them is mechanical once `samples(uri)` lands; left out to keep this PRD's scope honest.
- **Akkado `import` statement + unified resolver.** Akkado will gain an `import` keyword for pulling in other akkado modules. When that lands, `import "github:user/lib/utils.akkado"` should reuse the URI scheme system from this PRD. At that point, `akkado::FileResolver` (today's source-file resolver in `akkado/include/akkado/file_resolver.hpp`) and `cedar::UriResolver` should collapse into one — both resolve names to bytes; the only difference today is return type (`std::string` vs `OwnedBuffer`). To keep that future merge clean, **do not introduce new diverging resolver patterns** between this PRD and the import PRD: any new file-loading code should go through `cedar::UriResolver`, not invent a third abstraction.
- ETag/Last-Modified revalidation for HTTP cache.
- mmap support for very large files.
- Allow-list / domain restriction for native HTTP.
- Godot host with `res://` handler.
- Strudel-style runtime URI variables (`samples(my_url_var)`).

---

## 12. References

- [docs/reports/2026-05-02-file-loading-review.md](reports/2026-05-02-file-loading-review.md) — investigation that motivated this PRD
- [docs/prd-file-loading-abstraction.md](prd-file-loading-abstraction.md) — the foundational abstraction (DONE)
- [docs/prd-soundfonts-sample-banks.md](prd-soundfonts-sample-banks.md) — BankRegistry, SoundFont opcode
- [docs/prd-sample-loading-before-playback.md](prd-sample-loading-before-playback.md) — pre-play barrier (load before audio thread sees it)
- [docs/prd-smooch-wavetable-synth.md](prd-smooch-wavetable-synth.md) — wavetable preprocessor
- [docs/prd-soundfont-playback-fixes.md](prd-soundfont-playback-fixes.md) — name-based dedup fix
- [docs/audits/prd-file-loading-abstraction_audit_2026-04-24.md](audits/prd-file-loading-abstraction_audit_2026-04-24.md) — earlier audit consistent with the report
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — header-only HTTP library for the native handler (MIT license)
- [Strudel `samples()` reference](https://strudel.cc/learn/samples/) — for syntax inspiration
