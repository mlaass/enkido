> **Status: DONE** — Cross-platform file loading with multi-format audio decoding (commit a3283d3).

# PRD: Cross-Platform File Loading Abstraction

**Status:** Draft
**Author:** Claude
**Date:** 2026-02-05
**Related:** [SoundFont and Sample Bank PRD](prd-soundfonts-sample-banks.md)

## 1. Overview

### 1.1 Problem Statement

Nkido needs to load various file types (SoundFonts, samples, MIDI files, code files) in two different environments:

| Context | File Access | Memory Model | Challenges |
|---------|-------------|--------------|------------|
| **Native (CLI)** | Direct filesystem via `std::ifstream` | Unrestricted | Simple, synchronous |
| **Web (WASM)** | Fetch API, File API, IndexedDB | Limited to ~2GB, async | No direct FS, memory growth |

Currently:
- Native uses `cedar::WavLoader::load_from_file()` directly
- Web fetches files in TypeScript, decodes, then passes raw float data to WASM via message passing
- No unified abstraction exists for file formats beyond WAV

### 1.2 Proposed Solution

Create a **thin, zero-overhead abstraction layer** that:

1. **Decouples file parsing from file loading** - Parsers receive memory buffers, not file paths
2. **Provides platform-specific loaders** - Native uses `std::ifstream`, web uses Fetch/File API
3. **Standardizes file format support** - SF2, SF3 (Ogg-compressed), WAV, OGG, FLAC, MP3
4. **Enables lazy loading** - Files loaded on-demand, not at compile time
5. **Supports streaming for large files** - Optional chunked loading for files >50MB

### 1.3 Goals

- **Zero runtime overhead** for native builds (no virtual dispatch in hot paths)
- **Minimal memory copies** - Parse directly from mapped/loaded buffers
- **Async-first web API** - All web file operations return Promises
- **Extensible format support** - Easy to add new file types (MIDI, custom formats)
- **Unified error handling** - Consistent error types across platforms

### 1.4 Non-Goals

- Real-time streaming from disk (RAM-only after load)
- Network filesystem abstraction (SFTP, S3, etc.)
- Write/save operations (read-only for now)
- Virtual filesystem in WASM (too complex, little benefit)

---

## 2. Architecture

### 2.1 Design Principles

```
┌──────────────────────────────────────────────────────────────────┐
│                    File Format Parsers (C++)                      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐            │
│  │ WavLoader │ │ SF2Parser│ │ OggLoader│ │ MIDIParser│           │
│  └─────┬────┘ └─────┬────┘ └─────┬────┘ └─────┬────┘            │
│        │            │            │            │                   │
│        └────────────┴────────────┴────────────┘                   │
│                         │                                         │
│              Memory Buffer Interface                              │
│              (const uint8_t*, size_t)                            │
└──────────────────────────────────────────────────────────────────┘
                          │
          ┌───────────────┴───────────────┐
          │                               │
┌─────────▼─────────┐         ┌───────────▼───────────┐
│    Native Loader   │         │      Web Loader       │
│  ┌─────────────┐   │         │  ┌────────────────┐  │
│  │  MappedFile │   │         │  │  FetchLoader   │  │
│  │  (mmap)     │   │         │  └────────────────┘  │
│  └─────────────┘   │         │  ┌────────────────┐  │
│  ┌─────────────┐   │         │  │  FileLoader    │  │
│  │  ReadFile   │   │         │  │  (File API)    │  │
│  │  (ifstream) │   │         │  └────────────────┘  │
│  └─────────────┘   │         │  ┌────────────────┐  │
│                    │         │  │  IDBLoader     │  │
│                    │         │  │  (IndexedDB)   │  │
│                    │         │  └────────────────┘  │
└────────────────────┘         └──────────────────────┘
```

### 2.2 Key Insight: Parse in TypeScript or C++?

For web context, we have two choices:

| Approach | Pros | Cons |
|----------|------|------|
| **Parse in TypeScript** | Richer libraries (SpessaSynth, etc), easier debugging | Data transfer overhead, duplicate code |
| **Parse in C++ (WASM)** | Single codebase, no duplication, faster for large files | Limited library ecosystem |

**Recommendation: Hybrid approach**
- **Complex formats (SF2, SF3)**: Parse in TypeScript using existing libraries, serialize extracted data to C++
- **Simple formats (WAV, raw PCM)**: Parse directly in C++, receive raw bytes from TypeScript
- **Audio decoding (OGG, FLAC, MP3)**: Use Web Audio `decodeAudioData()` in TypeScript, pass decoded PCM to C++

This leverages the strengths of each platform while minimizing code duplication.

---

## 3. File Format Support

### 3.1 Audio Formats

| Format | Extension | Native Support | Web Support | Implementation |
|--------|-----------|----------------|-------------|----------------|
| WAV | .wav | C++ parser | C++ parser | Existing `cedar::WavLoader` |
| SF2 | .sf2 | TinySoundFont | SpessaSynth Core | Native: C++ / Web: TS |
| SF3 | .sf3 | TinySoundFont+minimp3 | SpessaSynth Core | Native: C++ / Web: TS |
| OGG | .ogg | stb_vorbis | decodeAudioData() | Native: C++ / Web: WebAudio |
| FLAC | .flac | dr_flac | decodeAudioData() | Native: C++ / Web: WebAudio |
| MP3 | .mp3 | minimp3 | decodeAudioData() | Native: C++ / Web: WebAudio |

### 3.2 Other Formats (Future)

| Format | Extension | Purpose | Implementation |
|--------|-----------|---------|----------------|
| MIDI | .mid | Sequence playback | C++ parser (both) |
| Akkado | .akkado | Live code loading | Existing compiler |
| Cedar | .cedar | Bytecode loading | Existing loader |

### 3.3 Recommended Libraries

**Native (C++):**
- [TinySoundFont](https://github.com/schellingb/TinySoundFont) - SF2 parsing + synthesis (MIT, single-header)
- [stb_vorbis](https://github.com/nothings/stb) - OGG Vorbis decoding (public domain, single-header)
- [dr_flac](https://github.com/mackron/dr_libs) - FLAC decoding (public domain, single-header)
- [minimp3](https://github.com/lieff/minimp3) - MP3 decoding (CC0, single-header)

**Web (TypeScript):**
- [SpessaSynth Core](https://github.com/spessasus/spessasynth_core) - SF2/SF3 parsing (LGPL-3.0)
- Web Audio API `decodeAudioData()` - OGG/FLAC/MP3 (built-in)

---

## 4. API Design

### 4.1 C++ Memory Buffer Interface

All parsers receive a simple memory buffer, not a file path:

```cpp
// cedar/include/cedar/io/buffer.hpp

namespace cedar {

/// Non-owning view into memory buffer
struct MemoryView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] bool empty() const { return size == 0 || data == nullptr; }
    [[nodiscard]] const std::uint8_t* begin() const { return data; }
    [[nodiscard]] const std::uint8_t* end() const { return data + size; }
};

/// Owning memory buffer (for native file loading)
class OwnedBuffer {
public:
    OwnedBuffer() = default;
    explicit OwnedBuffer(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    [[nodiscard]] MemoryView view() const {
        return {data_.data(), data_.size()};
    }
    [[nodiscard]] bool empty() const { return data_.empty(); }
    [[nodiscard]] std::size_t size() const { return data_.size(); }

private:
    std::vector<std::uint8_t> data_;
};

} // namespace cedar
```

### 4.2 Native File Loader

```cpp
// cedar/include/cedar/io/file_loader.hpp

namespace cedar {

/// Result of a file load operation
struct LoadResult {
    OwnedBuffer buffer;
    std::string error;
    bool success = false;

    static LoadResult ok(OwnedBuffer buf) {
        return {std::move(buf), "", true};
    }
    static LoadResult fail(std::string err) {
        return {{}, std::move(err), false};
    }
};

/// Load file from filesystem (native only)
/// For WASM builds, this is a no-op that always returns an error
class FileLoader {
public:
    /// Load entire file into memory
    static LoadResult load(const std::string& path);

    /// Load file with memory mapping (zero-copy for large files)
    /// Falls back to regular load if mmap unavailable
    static LoadResult load_mapped(const std::string& path);

    /// Check if file exists
    static bool exists(const std::string& path);

    /// Get file size without loading
    static std::optional<std::size_t> file_size(const std::string& path);
};

} // namespace cedar
```

### 4.3 WASM Interface Extensions

```cpp
// web/wasm/nkido_wasm.cpp

extern "C" {

/// Load audio file from raw bytes (WAV, OGG decoded, etc.)
/// @param name Unique identifier for the sample
/// @param data Raw audio file bytes (WAV) or decoded PCM (float32)
/// @param size Size in bytes
/// @param format 0=WAV, 1=decoded_mono_f32, 2=decoded_stereo_f32
/// @return Sample ID or 0 on failure
WASM_EXPORT uint32_t cedar_load_audio_data(
    const char* name,
    const uint8_t* data,
    uint32_t size,
    uint8_t format
);

/// Load SoundFont preset zone data
/// SF2 is parsed in TypeScript, zone metadata sent to C++
/// @param font_name Unique identifier for the soundfont
/// @param preset_data Serialized SoundFontPreset (see format below)
/// @param preset_size Size of preset data
/// @return Preset ID or 0 on failure
WASM_EXPORT uint32_t cedar_load_soundfont_preset(
    const char* font_name,
    const uint8_t* preset_data,
    uint32_t preset_size
);

/// Get loading status for async operations
/// @return JSON string with loading progress
WASM_EXPORT const char* cedar_get_loading_status();

} // extern "C"
```

### 4.4 TypeScript File Loader

```typescript
// web/src/lib/io/file-loader.ts

export type FileSource =
    | { type: 'url'; url: string }
    | { type: 'file'; file: File }
    | { type: 'arraybuffer'; data: ArrayBuffer; name: string }
    | { type: 'indexeddb'; key: string };

export interface LoadOptions {
    /** Progress callback (0-1) */
    onProgress?: (progress: number) => void;
    /** Abort signal for cancellation */
    signal?: AbortSignal;
    /** Cache in IndexedDB after loading */
    cache?: boolean;
}

export interface LoadResult {
    data: ArrayBuffer;
    name: string;
    size: number;
    mimeType?: string;
}

/**
 * Unified file loader for web context
 */
export class FileLoader {
    /**
     * Load file from any source
     */
    async load(source: FileSource, options?: LoadOptions): Promise<LoadResult>;

    /**
     * Check if file is cached in IndexedDB
     */
    async isCached(key: string): Promise<boolean>;

    /**
     * Clear cached file
     */
    async clearCache(key: string): Promise<void>;

    /**
     * Get cache statistics
     */
    async getCacheStats(): Promise<{ count: number; totalSize: number }>;
}

/**
 * Decode audio file to PCM data
 * Uses Web Audio API decodeAudioData for OGG/FLAC/MP3
 * Uses WavLoader for WAV files
 */
export async function decodeAudioFile(
    data: ArrayBuffer,
    audioContext: AudioContext
): Promise<{
    samples: Float32Array;
    channels: number;
    sampleRate: number;
}>;
```

### 4.5 SoundFont Preset Serialization Format

For transferring parsed SF2 data from TypeScript to C++:

```typescript
// TypeScript serialization
interface SerializedZone {
    sampleId: number;      // 4 bytes - Cedar sample ID
    keyLow: number;        // 1 byte
    keyHigh: number;       // 1 byte
    velLow: number;        // 1 byte
    velHigh: number;       // 1 byte
    rootKey: number;       // 1 byte (signed)
    tuneCents: number;     // 2 bytes (signed)
    loopStart: number;     // 4 bytes
    loopEnd: number;       // 4 bytes
    loopMode: number;      // 1 byte
    // Envelope parameters (4 bytes each, float32)
    volAttack: number;
    volDecay: number;
    volSustain: number;
    volRelease: number;
    filterCutoff: number;
    filterQ: number;
    // Total: 40 bytes per zone
}

interface SerializedPreset {
    nameLength: number;    // 1 byte
    name: string;          // variable (UTF-8)
    bank: number;          // 2 bytes
    program: number;       // 2 bytes
    zoneCount: number;     // 2 bytes
    zones: SerializedZone[]; // zoneCount * 40 bytes
}
```

---

## 5. Data Flow

### 5.1 Native: Loading a SoundFont

```
CLI: nkido-cli --soundfont gm.sf2 song.akkado
         │
         ▼
FileLoader::load("gm.sf2") → OwnedBuffer
         │
         ▼
TinySoundFont: tsf_load_memory(buffer.data, buffer.size)
         │
         ▼
Extract presets → SoundFontBank
         │
         ▼
Akkado compile: soundfont("gm.sf2", 0)
         │
         ▼
Codegen emits SOUNDFONT_VOICE opcode
```

### 5.2 Web: Loading a SoundFont

```
User drags gm.sf2 onto browser
         │
         ▼
File API → ArrayBuffer
         │
         ▼
SpessaSynth: new SoundBank(arrayBuffer)
         │
         ├─ For each sample:
         │    └─ decodeAudioData() → Float32Array
         │         │
         │         ▼
         │    cedar_load_sample(name, floatData, ...)
         │
         └─ For each preset:
              └─ Serialize zones → Uint8Array
                   │
                   ▼
              cedar_load_soundfont_preset(name, data, size)
         │
         ▼
Ready for playback
```

### 5.3 Web: Loading a Sample Bank

```
BankRegistry.loadBank("https://example.com/tr808/strudel.json")
         │
         ▼
Fetch manifest → parse JSON
         │
         ▼
Register bank (samples NOT loaded yet)
         │
         ▼
Compile: s"bd".bank("TR808")
         │
         ▼
CompileResult.required_samples = [{bank: "TR808", name: "bd", variant: 0}]
         │
         ▼
Before playback:
for each required sample:
    FileLoader.load({type: 'url', url: baseUrl + samplePath})
         │
         ▼
    decodeAudioFile(data) → Float32Array
         │
         ▼
    cedar_load_audio_data("TR808_bd_0", floatData, ...)
```

---

## 6. IndexedDB Caching (Web)

### 6.1 Cache Strategy

Large files (>1MB) are cached in IndexedDB to avoid re-downloading:

```typescript
// web/src/lib/io/file-cache.ts

const DB_NAME = 'nkido-file-cache';
const STORE_NAME = 'files';
const MAX_CACHE_SIZE = 500 * 1024 * 1024; // 500MB

interface CachedFile {
    key: string;           // URL or unique identifier
    data: ArrayBuffer;
    size: number;
    timestamp: number;     // Last access time
    mimeType?: string;
}

export class FileCache {
    private db: IDBDatabase | null = null;

    async init(): Promise<void>;
    async get(key: string): Promise<ArrayBuffer | null>;
    async set(key: string, data: ArrayBuffer, mimeType?: string): Promise<void>;
    async delete(key: string): Promise<void>;
    async clear(): Promise<void>;

    // LRU eviction when cache exceeds MAX_CACHE_SIZE
    private async evictIfNeeded(): Promise<void>;
}
```

### 6.2 Cache Key Generation

```typescript
function getCacheKey(source: FileSource): string {
    switch (source.type) {
        case 'url':
            return `url:${source.url}`;
        case 'file':
            // Use file name + size + lastModified for uniqueness
            return `file:${source.file.name}:${source.file.size}:${source.file.lastModified}`;
        case 'arraybuffer':
            return `buffer:${source.name}`;
        case 'indexeddb':
            return source.key;
    }
}
```

---

## 7. Error Handling

### 7.1 Error Types

```cpp
// cedar/include/cedar/io/errors.hpp

namespace cedar {

enum class FileError {
    NotFound,           // File doesn't exist
    PermissionDenied,   // Can't read file
    TooLarge,           // Exceeds memory limits
    InvalidFormat,      // Parser can't handle format
    Corrupted,          // File is damaged
    UnsupportedFormat,  // Known format, unsupported variant
    NetworkError,       // Web: fetch failed
    Aborted,            // Operation cancelled
};

struct FileLoadError {
    FileError code;
    std::string message;
    std::string path;
};

} // namespace cedar
```

### 7.2 TypeScript Error Types

```typescript
// web/src/lib/io/errors.ts

export class FileLoadError extends Error {
    constructor(
        public code: 'not_found' | 'network' | 'invalid_format' | 'too_large' | 'aborted',
        public path: string,
        message: string
    ) {
        super(message);
        this.name = 'FileLoadError';
    }
}
```

---

## 8. Implementation Plan

### Phase 1: Core Infrastructure (2-3 days)

1. **C++ Memory Buffer Types**
   - Add `cedar/include/cedar/io/buffer.hpp`
   - Add `cedar/include/cedar/io/file_loader.hpp`
   - Implement native `FileLoader` with mmap support

2. **Refactor Existing Loaders**
   - Update `WavLoader` to use `MemoryView` interface
   - Keep `load_from_file()` as convenience wrapper

### Phase 2: Web File Loader (2-3 days)

3. **TypeScript FileLoader**
   - Implement `web/src/lib/io/file-loader.ts`
   - Add Fetch, File API, and IndexedDB backends
   - Implement progress tracking

4. **IndexedDB Cache**
   - Implement `web/src/lib/io/file-cache.ts`
   - Add LRU eviction logic
   - Add cache management UI

### Phase 3: Audio Format Support (3-4 days)

5. **Native Audio Decoders**
   - Integrate stb_vorbis for OGG
   - Integrate dr_flac for FLAC
   - Integrate minimp3 for MP3
   - Create unified `AudioDecoder` class

6. **Web Audio Decoding**
   - Implement `decodeAudioFile()` using Web Audio API
   - Handle format detection from magic bytes

### Phase 4: SoundFont Support (4-5 days)

7. **Native SF2 Support**
   - Integrate TinySoundFont (parsing only, not synthesis)
   - Create `SoundFontLoader` that extracts samples and zones
   - Add SF3 support via Ogg decoder integration

8. **Web SF2 Support**
   - Integrate SpessaSynth Core
   - Implement preset serialization format
   - Add `cedar_load_soundfont_preset()` WASM function

### Phase 5: WASM Interface (2 days)

9. **Extended WASM API**
   - Add `cedar_load_audio_data()` with format detection
   - Add loading status/progress API
   - Update audio worklet message handling

### Phase 6: Testing and Polish (2-3 days)

10. **Tests**
    - Unit tests for each loader
    - Integration tests for web<->WASM data transfer
    - Performance benchmarks for large files

11. **Documentation**
    - API documentation
    - Usage examples
    - Supported format matrix

---

## 9. File Locations

### New Files

```
cedar/include/cedar/io/
├── buffer.hpp          # MemoryView, OwnedBuffer
├── file_loader.hpp     # Native file loading
├── audio_decoder.hpp   # Unified audio format decoding
├── soundfont_loader.hpp # SF2/SF3 parsing
└── errors.hpp          # Error types

cedar/src/io/
├── file_loader.cpp
├── audio_decoder.cpp
└── soundfont_loader.cpp

cedar/third_party/
├── stb_vorbis.h        # OGG decoder
├── dr_flac.h           # FLAC decoder
├── minimp3.h           # MP3 decoder
└── tsf.h               # TinySoundFont

web/src/lib/io/
├── file-loader.ts      # Unified file loading
├── file-cache.ts       # IndexedDB caching
├── audio-decoder.ts    # Web Audio decoding
├── soundfont-loader.ts # SF2 parsing wrapper
└── errors.ts           # Error types
```

### Modified Files

```
cedar/include/cedar/audio/wav_loader.hpp  # Use MemoryView
cedar/include/cedar/vm/sample_bank.hpp    # Add load_audio_data()
web/wasm/nkido_wasm.cpp                  # New WASM exports
web/src/lib/stores/audio.svelte.ts        # Use new loaders
web/src/lib/audio/bank-registry.ts        # Use FileLoader
```

---

## 10. Performance Considerations

### 10.1 Memory Mapping (Native)

For large files (>10MB), use memory mapping to avoid copying:

```cpp
#ifdef __unix__
    // Linux/macOS: mmap
    int fd = open(path.c_str(), O_RDONLY);
    struct stat sb;
    fstat(fd, &sb);
    void* mapped = mmap(nullptr, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
#elif defined(_WIN32)
    // Windows: CreateFileMapping
    HANDLE file = CreateFile(path.c_str(), ...);
    HANDLE mapping = CreateFileMapping(file, ...);
    void* mapped = MapViewOfFile(mapping, ...);
#endif
```

### 10.2 Web Worker Decoding

Decode audio in a Web Worker to avoid blocking main thread:

```typescript
// web/src/lib/io/decode-worker.ts
self.onmessage = async (e) => {
    const { id, data } = e.data;
    const audioContext = new OfflineAudioContext(2, 1, 48000);
    const decoded = await audioContext.decodeAudioData(data);
    self.postMessage({ id, decoded }, [decoded]);
};
```

### 10.3 Streaming for Large SoundFonts

For SF2 files >100MB, consider streaming samples on-demand:

```typescript
interface StreamingSoundFont {
    // Load just the preset/zone metadata
    loadMetadata(data: ArrayBuffer): Promise<void>;

    // Load samples for a specific preset on-demand
    loadPresetSamples(preset: number): Promise<void>;

    // Unload samples not used in N seconds
    unloadUnusedSamples(): void;
}
```

---

## 11. Security Considerations

### 11.1 File Size Limits

```typescript
const LIMITS = {
    maxSingleFile: 200 * 1024 * 1024,    // 200MB per file
    maxTotalLoaded: 1024 * 1024 * 1024,  // 1GB total loaded
    maxCacheSize: 500 * 1024 * 1024,     // 500MB cache
};
```

### 11.2 Format Validation

Always validate magic bytes before parsing:

```typescript
const MAGIC_BYTES = {
    wav: [0x52, 0x49, 0x46, 0x46],  // "RIFF"
    sf2: [0x52, 0x49, 0x46, 0x46],  // "RIFF" (check sfbk chunk)
    ogg: [0x4F, 0x67, 0x67, 0x53],  // "OggS"
    flac: [0x66, 0x4C, 0x61, 0x43], // "fLaC"
    mp3: [0xFF, 0xFB] | [0x49, 0x44, 0x33], // Frame sync or "ID3"
};
```

### 11.3 CORS for Remote URLs

Web loader respects CORS; include appropriate headers:

```typescript
const response = await fetch(url, {
    mode: 'cors',
    credentials: 'omit', // Don't send cookies
});
```

---

## 12. Finalized Design Decisions

| Question | Decision | Rationale |
|----------|----------|-----------|
| **SF3 on native** | Decompress SF3→SF2 on load | Bundle stb_vorbis. Temporary memory overhead acceptable. |
| **Sample rate mismatch** | Resample on load to 48kHz | Simpler playback code, consistent latency. |
| **Large file streaming** | RAM-only for v1 | Streaming deferred to future version. |
| **Preset name conflicts** | Qualified names always | Use `fontname:Piano` format. Explicit and unambiguous. |
| **Storage quota** | 500MB cache limit with LRU eviction | Per §6.1 design. Prompt user when approaching limit. |

---

## 13. Success Metrics

- [ ] Native: Load 100MB SF2 in <2 seconds
- [ ] Web: Load 10MB SF2 in <3 seconds (cold) / <100ms (cached)
- [ ] Memory: No intermediate copies during parsing
- [ ] Formats: WAV, SF2, OGG, FLAC, MP3 all working
- [ ] Cache: IndexedDB persistence working across sessions
- [ ] Errors: Clear error messages for all failure modes

---

## 14. References

- [Emscripten File System Overview](https://emscripten.org/docs/porting/files/file_systems_overview.html)
- [TinySoundFont](https://github.com/schellingb/TinySoundFont) - SF2 library
- [SpessaSynth Core](https://github.com/spessasus/spessasynth_core) - Web SF2 parser
- [stb libraries](https://github.com/nothings/stb) - Single-header audio decoders
- [Audio Worklet Design Pattern](https://developer.chrome.com/blog/audio-worklet-design-pattern) - SharedArrayBuffer patterns
- [ringbuf.js](https://github.com/padenot/ringbuf.js/) - Thread-safe ring buffer for audio
