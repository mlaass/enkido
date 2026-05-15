> **Status: DONE** — BankRegistry, SoundFont loading, SOUNDFONT_VOICE opcode (commit a741806).

# PRD: SoundFont and Sample Bank System

**Status:** Draft
**Author:** Claude
**Date:** 2026-02-05
**Related:** [File Loading Abstraction PRD](prd-file-loading-abstraction.md)

## 1. Overview

### 1.1 Problem Statement

Currently, Nkido has basic sample playback infrastructure with a fixed set of default drum samples. Users cannot:

- Load custom sample collections (drum kits, instrument libraries)
- Use SoundFont (SF2/SF3) files for multi-sampled instruments
- Switch between sample banks at pattern time (e.g., `bank("TR808")`)
- Access sample variants easily (e.g., `bd:3` for the 4th kick variant)

### 1.2 Proposed Solution

Implement a two-tier sample system:

1. **Sample Banks** - Named collections of samples that can be loaded from URLs, local files, or JSON manifests (inspired by Strudel)
2. **SoundFont Support** - Parse SF2/SF3 files and extract presets as playable instruments with proper multi-sampling, velocity layers, and loop points

### 1.3 Goals

- **Strudel-compatible syntax**: `s("bd sd").bank("TR808")` works as expected
- **SoundFont playback**: Load `.sf2` files and play instruments via MIDI note
- **Lazy loading**: Only download audio files when first triggered
- **Zero-allocation playback**: All samples pre-decoded, no runtime allocations
- **Hot-swap safe**: Bank changes don't interrupt playing sounds

### 1.4 Non-Goals

- DLS (DownLoadable Sounds) format support (future consideration)
- Sample recording/editing (out of scope)
- Real-time sample streaming from disk (RAM-only)
- SoundFont synthesis features (modulators, effects units)

---

## 2. User Experience

### 2.1 Sample Banks in Akkado

```akkado
// Use default bank (built-in 808 kit)
pat("bd sd hh*8") |> out(@, @)

// Switch to a named bank
pat("bd sd hh*8").bank("TR909") |> out(@, @)

// Bank can be patterned
pat("bd sd").bank("<TR808 TR909>") |> out(@, @)

// Sample variants with colon syntax
pat("bd:0 bd:1 bd:2 bd:3") |> out(@, @)

// Or with variant() modifier
pat("bd").variant("<0 1 2 3>") |> out(@, @)
```

### 2.2 SoundFont Instruments

```akkado
// Load a soundfont and use as instrument
piano = soundfont("gm.sf2", 0)  // Bank 0, preset 0 (Acoustic Grand Piano)

// Play with pattern (note patterns auto-trigger)
pat("c4 e4 g4 c5") |> piano |> out(@, @)

// Or with explicit note control
note(pat("c4 e4 g4")) |> piano |> out(@, @)

// Select preset by name (if available)
strings = soundfont("orchestral.sf2", "Violin Section")

// Velocity sensitivity
pat("c4 e4 g4").vel("<0.3 0.6 1.0>") |> piano |> out(@, @)
```

### 2.3 Web UI: Sample Browser

The side panel will include a "Samples" tab with:

- **Installed Banks**: List of loaded sample banks with sample counts
- **Add Bank**: Button to load from URL, file, or GitHub
- **SoundFonts**: List of loaded SF2 files with preset browser
- **Preview**: Click to audition any sample/preset

---

## 3. Technical Design

### 3.1 Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Web App / TypeScript                           │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  BankRegistry                                                      │  │
│  │  ├─ banks: Map<name, BankManifest>                                │  │
│  │  ├─ loadBank(url | file | github)                                 │  │
│  │  └─ resolveVariant(bankName, sampleName, n) → sampleId            │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  SoundFontManager                                                  │  │
│  │  ├─ fonts: Map<name, ParsedSoundFont>                             │  │
│  │  ├─ loadSoundFont(url | file)                                     │  │
│  │  └─ getPreset(fontName, bank, preset) → SoundFontPreset           │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼ (WASM calls)
┌─────────────────────────────────────────────────────────────────────────┐
│                              Cedar (C++)                                 │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  SampleBank (existing)                                             │  │
│  │  ├─ samples: Map<id, SampleData>                                  │  │
│  │  ├─ names: Map<string, id>                                        │  │
│  │  └─ load_sample() / load_wav_memory()                             │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  SoundFontBank (NEW)                                               │  │
│  │  ├─ presets: Map<presetId, SoundFontPreset>                       │  │
│  │  ├─ zones: vector<InstrumentZone>                                 │  │
│  │  └─ play_note(preset, note, velocity) → buffer                    │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  New Opcodes                                                       │  │
│  │  ├─ SOUNDFONT_NOTE_ON                                             │  │
│  │  ├─ SOUNDFONT_NOTE_OFF                                            │  │
│  │  └─ SOUNDFONT_VOICE (continuous playback)                         │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Sample Bank System

#### 3.2.1 Bank Manifest Format

Compatible with Strudel's `strudel.json`:

```json
{
  "_base": "https://example.com/samples/TR808/",
  "_name": "TR808",
  "bd": ["kick1.wav", "kick2.wav", "kick3.wav"],
  "sd": ["snare1.wav", "snare2.wav"],
  "hh": "hihat.wav",
  "oh": "openhh.wav",
  "cp": "clap.wav",
  "rim": "rimshot.wav"
}
```

Fields:
- `_base`: URL prefix for all sample paths
- `_name`: Display name for UI (optional, defaults to filename)
- `[sampleName]`: Single path (string) or array of variant paths

#### 3.2.2 BankRegistry (TypeScript) — **IMPLEMENTED**

> **Status:** Fully implemented in `web/src/lib/audio/bank-registry.ts` (355 lines).

The BankRegistry provides:
- **Strudel-compatible manifest parsing** — Parses `strudel.json` format with `_base`, `_name`, and sample arrays
- **GitHub shortcut support** — `github:user/repo[/branch][/path]` format for loading from GitHub raw content
- **Lazy loading infrastructure** — Samples loaded on first use, tracked in `loaded` Set
- **Variant wrapping** — `:N` indices wrap around using modulo (Strudel behavior)
- **Qualified name generation** — `bank_name_variant` format for Cedar sample IDs
- **Duplicate load prevention** — In-flight loading promises tracked to avoid redundant fetches

```typescript
// Key interface (see full implementation in bank-registry.ts)
interface BankManifest {
  name: string;
  baseUrl: string;
  samples: Map<string, string[]>;  // name → variant URLs (always arrays)
  loaded: Set<string>;  // Which sample variants have been loaded (e.g., "bd:0")
}
```

**Remaining work:** Integration with the FileLoader/FileCache from [File Loading Abstraction PRD](prd-file-loading-abstraction.md) for IndexedDB caching.

#### 3.2.3 Sample Resolution Flow

```
Pattern: pat("bd:2").bank("TR808")
           │
           ▼
Parser extracts: sample="bd", variant=2, bank="TR808"
           │
           ▼
BankRegistry.resolveSample("TR808", "bd", 2)
           │
           ├─ If sample not loaded:
           │    1. Fetch TR808._base + samples.bd[2]
           │    2. Decode WAV → Float32Array
           │    3. Call cedar_load_sample("TR808_bd_2", data)
           │    4. Cache sample ID
           │
           └─ Return Cedar sample ID (e.g., 42)
           │
           ▼
Codegen emits: SAMPLE_PLAY with sample_id=42
```

#### 3.2.4 Akkado Integration

New pattern modifiers in mini-notation:

```cpp
// akkado/include/akkado/mini_token.hpp

struct MiniSampleData {
    std::string sample_name;   // "bd"
    int variant = 0;           // From :N suffix or variant() modifier
    std::string bank;          // From .bank() modifier, empty = default
};
```

Compiler collects required samples with bank context:

```cpp
// akkado/include/akkado/akkado.hpp

struct RequiredSample {
    std::string bank;      // Bank name (empty = default)
    std::string name;      // Sample name
    int variant;           // Variant index
};

struct CompileResult {
    // ...existing fields...
    std::vector<RequiredSample> required_samples;  // Extended format
};
```

### 3.3 SoundFont Support

#### 3.3.1 SF2 Format Overview

SoundFont 2 files contain:

1. **Samples**: Raw PCM audio data (mono, various bit depths)
2. **Instruments**: Groups of sample zones with key/velocity ranges
3. **Presets**: Named instruments with bank/program numbers (GM-compatible)
4. **Modulators**: Envelope, LFO, filter routing (complex, often skipped)

Key concepts:
- **Zone**: A sample mapped to a key range (e.g., C3-C4) and velocity range
- **Generator**: Parameter controlling playback (pitch, volume, envelope, filter)
- **Loop Points**: Start/end markers for sustain looping

#### 3.3.2 SF2 Parsing Strategy

Use [SpessaSynth Core](https://github.com/spessasus/spessasynth_core) for parsing:

**Pros:**
- Active development (2025), TypeScript types
- Handles SF2, SF3 (Ogg compressed), 8/16/24-bit samples
- WebAssembly for Ogg decoding
- Batteries-included, no dependencies

**Alternative:** [sf2-parser](https://github.com/colinbdclark/sf2-parser) - lighter weight, but less maintained

```typescript
// web/src/lib/audio/soundfont-manager.ts

import { SoundBank } from 'spessasynth_core';

interface SoundFontPreset {
  name: string;
  bank: number;
  program: number;
  zones: InstrumentZone[];
}

interface InstrumentZone {
  keyLow: number;
  keyHigh: number;
  velLow: number;
  velHigh: number;
  sampleId: number;        // Cedar sample ID after loading
  rootKey: number;         // Original pitch
  tuning: number;          // Cents adjustment
  loopStart: number;       // Loop point (samples)
  loopEnd: number;
  loopMode: 'none' | 'sustain' | 'continuous';
  // Envelope generators
  volEnv: ADSRParams;
  modEnv: ADSRParams;
  // Filter
  filterCutoff: number;
  filterQ: number;
}

class SoundFontManager {
  private fonts: Map<string, SoundBank> = new Map();
  private presets: Map<string, SoundFontPreset[]> = new Map();

  async loadSoundFont(name: string, source: ArrayBuffer): Promise<void> {
    const bank = new SoundBank(source);
    this.fonts.set(name, bank);

    // Extract presets and upload samples to Cedar
    for (const preset of bank.presets) {
      await this.loadPresetSamples(name, preset);
    }
  }

  private async loadPresetSamples(fontName: string, preset: Preset): Promise<void> {
    for (const zone of preset.zones) {
      // Get sample data from soundfont
      const sample = zone.sample;
      const sampleName = `${fontName}_${preset.program}_${sample.name}`;

      // Convert to float and upload to Cedar
      const floatData = this.convertToFloat(sample.data, sample.bitDepth);
      const sampleId = await audioStore.loadSample(
        sampleName,
        floatData,
        sample.channels,
        sample.sampleRate
      );

      zone.cedarSampleId = sampleId;
    }
  }

  // Find correct zone for note + velocity
  findZone(fontName: string, preset: number, note: number, velocity: number): InstrumentZone | null {
    const presets = this.presets.get(fontName);
    const p = presets?.find(p => p.program === preset);
    if (!p) return null;

    return p.zones.find(z =>
      note >= z.keyLow && note <= z.keyHigh &&
      velocity >= z.velLow && velocity <= z.velHigh
    ) ?? null;
  }
}
```

#### 3.3.3 Cedar SoundFont Integration

New C++ structures for soundfont playback:

```cpp
// cedar/include/cedar/audio/soundfont.hpp

struct SoundFontZone {
    std::uint32_t sample_id;    // Reference to SampleBank
    std::uint8_t key_low;
    std::uint8_t key_high;
    std::uint8_t vel_low;
    std::uint8_t vel_high;
    std::int8_t root_key;       // Original pitch
    std::int16_t tune_cents;    // Fine tuning
    std::uint32_t loop_start;   // Loop points in samples
    std::uint32_t loop_end;
    std::uint8_t loop_mode;     // 0=none, 1=sustain, 3=continuous

    // ADSR envelope times (in samples at 48kHz)
    float vol_attack;
    float vol_decay;
    float vol_sustain;
    float vol_release;

    float filter_cutoff;  // Hz
    float filter_q;
};

struct SoundFontPreset {
    std::string name;
    std::uint16_t bank;
    std::uint16_t program;
    std::vector<SoundFontZone> zones;

    const SoundFontZone* find_zone(std::uint8_t note, std::uint8_t velocity) const;
};

class SoundFontBank {
public:
    bool load_preset_data(const std::uint8_t* data, size_t size);
    const SoundFontPreset* get_preset(std::uint16_t bank, std::uint16_t program) const;
    const SoundFontPreset* get_preset_by_name(std::string_view name) const;

private:
    std::vector<SoundFontPreset> presets_;
    std::unordered_map<std::uint32_t, size_t> preset_index_;  // bank<<16|program → index
};
```

#### 3.3.4 SoundFont Opcodes

```cpp
// cedar/include/cedar/vm/instruction.hpp

// New opcodes for soundfont playback
SOUNDFONT_VOICE,   // Polyphonic voice with envelope + filter
                   // in0: gate, in1: note (MIDI), in2: velocity
                   // state_id: preset reference
                   // out: audio buffer
```

Voice state for polyphonic playback:

```cpp
// cedar/include/cedar/opcodes/dsp_state.hpp

struct SoundFontVoice {
    bool active = false;
    const SoundFontZone* zone = nullptr;

    // Playback
    double position = 0.0;      // Sample position (fractional)
    double speed = 1.0;         // Pitch-adjusted speed
    bool looping = false;

    // Volume envelope
    EnvelopeStage vol_stage = EnvelopeStage::Off;
    float vol_level = 0.0f;
    float vol_increment = 0.0f;

    // State for filter
    float filter_z1 = 0.0f;
    float filter_z2 = 0.0f;

    void trigger(const SoundFontZone* z, float note, float velocity);
    void release();
    float process(const SampleBank& samples, float sample_rate);
};

struct SoundFontState {
    static constexpr int MAX_VOICES = 32;
    SoundFontVoice voices[MAX_VOICES];
    float prev_gate[BLOCK_SIZE] = {};  // For edge detection

    int allocate_voice();
    void release_all();
};
```

### 3.4 WASM Interface Extensions

> **Note:** The canonical WASM API for file/audio loading is defined in [File Loading Abstraction PRD §4.3](prd-file-loading-abstraction.md#43-wasm-interface-extensions). This section covers SoundFont-specific extensions only.

**Existing WASM exports (already implemented):**
- `cedar_load_sample()` — Load pre-decoded Float32Array
- `cedar_load_sample_wav()` — Load raw WAV bytes (C++ parses)
- `cedar_has_sample()` / `cedar_get_sample_id()` — Sample lookup by name
- `akkado_get_required_sample_*()` — Extended sample info with bank/variant

**New exports needed for SoundFonts:**

```cpp
// web/wasm/nkido_wasm.cpp

/// Load SoundFont preset zone data (SF2 parsed in TypeScript)
/// See §4.5 of file-loading-abstraction.md for serialization format
WASM_EXPORT uint32_t cedar_load_soundfont_preset(
    const char* font_name,
    const uint8_t* preset_data,  // Serialized per file-loading PRD §4.5
    uint32_t preset_size
);

/// Get preset ID for playback
WASM_EXPORT uint32_t cedar_get_soundfont_preset_id(
    const char* font_name,
    uint16_t bank,
    uint16_t program
);
```

For audio loading (`cedar_load_audio_data`), loading status (`cedar_get_loading_status`), and serialization format, see [File Loading Abstraction PRD §4.3](prd-file-loading-abstraction.md#43-wasm-interface-extensions).

---

## 4. Data Flow Diagrams

### 4.1 Sample Bank Loading

```
User clicks "Add Bank" → URL input
                │
                ▼
        Fetch strudel.json
                │
                ▼
     Parse manifest (TypeScript)
                │
                ▼
     Register bank in BankRegistry
     (samples NOT loaded yet)
                │
                ▼
        Compile Akkado code
                │
                ▼
      Parser finds: pat("bd").bank("TR808")
                │
                ▼
     CompileResult.required_samples = [
       {bank: "TR808", name: "bd", variant: 0}
     ]
                │
                ▼
     Before playback, resolve samples:
     for each required sample:
       ├─ Fetch audio file from _base + path
       ├─ Decode to Float32Array
       └─ cedar_load_bank_sample("TR808", "bd", 0, data...)
                │
                ▼
         Playback ready
```

### 4.2 SoundFont Note Playback

```
pat("c4 e4 g4") |> soundfont("piano.sf2", 0)
                │
                ▼
      Each note generates trigger + pitch
                │
                ▼
      SOUNDFONT_VOICE opcode executes:
                │
      ┌─────────┴─────────┐
      │ Rising edge on gate │
      └─────────┬─────────┘
                │
                ▼
      Find zone for (note=60, velocity=100)
                │
                ▼
      Allocate voice, set:
      - sample_id from zone
      - speed from (note - root_key) pitch calc
      - envelope params from zone
                │
                ▼
      Each sample block:
      - Read sample with interpolation
      - Apply volume envelope
      - Apply filter (optional)
      - Mix to output
                │
                ▼
      On gate→0: start release phase
                │
                ▼
      When envelope reaches 0: voice inactive
```

---

## 5. Implementation Phases

> **Prerequisite:** Phases 2-5 depend on [File Loading Abstraction PRD](prd-file-loading-abstraction.md) being implemented first for FileLoader, FileCache, and audio format decoders.

### Phase 0: File Loading Abstraction (prerequisite)

Implement per [File Loading Abstraction PRD](prd-file-loading-abstraction.md):
- `FileLoader` with Fetch/File API/IndexedDB backends
- `FileCache` with LRU eviction (500MB limit)
- Audio format decoders (OGG, FLAC, MP3 via Web Audio `decodeAudioData`)
- WASM interface: `cedar_load_audio_data()`, `cedar_get_loading_status()`

### Phase 1: Sample Bank Integration (2 days)

> **Status:** BankRegistry already implemented. Remaining work is integration.

**TypeScript:**
1. ~~Create `bank-registry.ts` with manifest parsing~~ ✓ Done
2. ~~Add lazy loading infrastructure~~ ✓ Done
3. ~~Implement GitHub shortcut (`github:user/repo`)~~ ✓ Done
4. Integrate BankRegistry with FileLoader/FileCache from Phase 0
5. Add bank management UI component

**Akkado:**
6. Extend `MiniSampleData` with bank field
7. Add `.bank()` modifier to mini-notation parser
8. Extend `CompileResult.required_samples` with bank context

### Phase 2: Sample Variants (2 days)

**Akkado:**
1. Parse `:N` variant suffix in mini-notation
2. Add `variant()` modifier for variant selection
3. Support variant patterns: `variant("<0 1 2 3>")`

**TypeScript:**
4. ~~Track variant counts per sample~~ ✓ Done in BankRegistry
5. ~~Handle variant wrapping (excess indices loop)~~ ✓ Done in BankRegistry

### Phase 3: SoundFont Parsing (3-4 days)

**TypeScript:**
1. Integrate SpessaSynth Core (see [File Loading PRD §3.3](prd-file-loading-abstraction.md#33-recommended-libraries))
2. Create `SoundFontManager` class
3. Extract presets and zones from SF2/SF3
4. Use `decodeAudioData()` for sample conversion (from Phase 0)
5. Serialize zone data per [File Loading PRD §4.5](prd-file-loading-abstraction.md#45-soundfont-preset-serialization-format)

### Phase 4: SoundFont Playback (4-5 days)

**Cedar:**
1. Add `SoundFontZone` and `SoundFontPreset` structs
2. Add `SoundFontBank` class
3. Implement `SOUNDFONT_VOICE` opcode
4. Add voice allocation and envelope processing
5. Implement pitch calculation from root key

**Akkado:**
6. Add `soundfont()` builtin function
7. Handle preset selection (by number or name)

### Phase 5: UI and Polish (2-3 days)

1. Sample browser panel component
2. SoundFont preset browser
3. Audio preview on click
4. Loading progress indicators (using `cedar_get_loading_status()`)
5. Error handling and user feedback
6. Documentation

---

## 6. File Locations

> **Note:** File loading infrastructure (`file-loader.ts`, `file-cache.ts`, `audio-decoder.ts`) is defined in [File Loading Abstraction PRD §9](prd-file-loading-abstraction.md#9-file-locations). This section covers SoundFont-specific files only.

### Existing Files

```
web/src/lib/audio/
├── bank-registry.ts       # ✓ IMPLEMENTED - Sample bank management
└── default-samples.ts     # ✓ EXISTS - 32 BPB Cassette 808 samples
```

### New Files

```
web/src/lib/audio/
└── soundfont-manager.ts   # SF2/SF3 parsing via SpessaSynth (NEW)

web/src/lib/components/Samples/
├── SampleBrowser.svelte   # Main browser panel (NEW)
├── BankList.svelte        # Installed banks (NEW)
├── PresetList.svelte      # SoundFont presets (NEW)
└── AddBankDialog.svelte   # URL/file/GitHub input (NEW)

cedar/include/cedar/audio/
└── soundfont.hpp          # SoundFontZone, SoundFontPreset, SoundFontBank (NEW)

cedar/include/cedar/opcodes/
└── soundfont.hpp          # SOUNDFONT_VOICE opcode (NEW)
```

### Modified Files

```
web/src/lib/stores/audio.svelte.ts     # SoundFont loading methods
web/wasm/nkido_wasm.cpp               # cedar_load_soundfont_preset()
akkado/include/akkado/mini_token.hpp   # Extend MiniSampleData with bank
akkado/src/parser.cpp                  # .bank() and :N parsing
akkado/src/codegen.cpp                 # soundfont() handler
akkado/include/akkado/builtins.hpp     # Add soundfont() builtin
cedar/src/vm/vm.cpp                    # SOUNDFONT_VOICE execution
cedar/include/cedar/vm/sample_bank.hpp # Bank prefix support
```

---

## 7. Default Banks

Ship with these pre-configured banks:

| Name | Source | Description |
|------|--------|-------------|
| `default` | Built-in | BPB Cassette 808 (current default samples) |
| `TR808` | tidal-drum-machines | Classic Roland TR-808 |
| `TR909` | tidal-drum-machines | Roland TR-909 |
| `linndrum` | tidal-drum-machines | LinnDrum |

Banks are registered but samples are lazy-loaded on first use.

---

## 8. Design Decisions

The following questions have been resolved with decisions documented in [File Loading Abstraction PRD §12](prd-file-loading-abstraction.md#12-finalized-design-decisions):

| Question | Decision | Reference |
|----------|----------|-----------|
| **Memory limits** | 500MB cache, 1GB total loaded | File Loading PRD §11.1 |
| **SoundFont modulators** | Basic playback only (no modulators) | §1.4 Non-Goals |
| **Sample rate conversion** | Resample on load to 48kHz | File Loading PRD §12 |
| **Preset naming conflicts** | Qualified names: `fontname:Piano` | File Loading PRD §12 |
| **MIDI file playback** | Deferred to future PRD | §3.2 Other Formats |

### Additional Decisions

- **Voice limit**: 32 simultaneous SoundFont voices per instance (see §3.3.4)
- **Loop modes**: Support `none`, `sustain`, and `continuous` (SF2 modes 0, 1, 3)
- **Envelope precision**: Process at block rate (128 samples), not sample-accurate

---

## 9. Implementation Status

### Already Implemented

| Component | Location | Status |
|-----------|----------|--------|
| BankRegistry | `web/src/lib/audio/bank-registry.ts` | ✓ Complete (355 lines) |
| Default samples | `web/src/lib/audio/default-samples.ts` | ✓ Complete |
| WASM sample loading | `web/wasm/nkido_wasm.cpp` | ✓ `cedar_load_sample()`, `cedar_load_sample_wav()` |
| Sample info exports | `web/wasm/nkido_wasm.cpp` | ✓ `akkado_get_required_sample_*()` |

### Not Yet Implemented

| Component | Location | Blocked By |
|-----------|----------|------------|
| FileLoader/FileCache | `web/src/lib/io/` | File Loading PRD |
| SoundFontManager | `web/src/lib/audio/` | Phase 3 |
| SoundFont opcodes | `cedar/include/cedar/opcodes/` | Phase 4 |
| Mini-notation `.bank()` | `akkado/src/parser.cpp` | Phase 1 |
| Mini-notation `:N` variants | `akkado/src/parser.cpp` | Phase 2 |

---

## 10. Success Metrics

- [ ] Banks load from URL/file/GitHub without errors
- [ ] `pat("bd").bank("TR808")` plays correct sample
- [ ] Sample variants accessible via `:N` and `variant()`
- [ ] SF2 files parse and list presets correctly
- [ ] SoundFont playback has correct pitch across keyboard
- [ ] Volume envelopes work (no clicks on note off)
- [ ] Polyphonic playback (multiple simultaneous notes)
- [ ] Memory usage stays reasonable (<500MB for typical use)
- [ ] No audio glitches during sample loading

---

## 11. Prerequisites and Dependencies

This PRD depends on [File Loading Abstraction PRD](prd-file-loading-abstraction.md) for:

| Component | Location | Purpose |
|-----------|----------|---------|
| **FileLoader** | `web/src/lib/io/file-loader.ts` | Unified file loading from URL/File/IndexedDB |
| **FileCache** | `web/src/lib/io/file-cache.ts` | IndexedDB caching with LRU eviction |
| **Audio decoders** | `web/src/lib/io/audio-decoder.ts` | OGG/FLAC/MP3 via Web Audio API |
| **WASM: cedar_load_audio_data()** | `web/wasm/nkido_wasm.cpp` | Format-aware audio loading |
| **WASM: cedar_get_loading_status()** | `web/wasm/nkido_wasm.cpp` | Progress tracking |
| **MemoryView/OwnedBuffer** | `cedar/include/cedar/io/buffer.hpp` | C++ memory buffer abstraction |

### Implementation Order

1. **File Loading Abstraction** (prerequisite) — Must be complete before Phase 1
2. **This PRD Phase 1-2** — Sample banks and variants (BankRegistry mostly done)
3. **This PRD Phase 3-5** — SoundFont support

---

## 12. References

- [Strudel Samples Documentation](https://strudel.cc/learn/samples)
- [SpessaSynth Core](https://github.com/spessasus/spessasynth_core) - JavaScript SF2/SF3 parser
- [sf2-parser](https://github.com/colinbdclark/sf2-parser) - Lightweight alternative
- [SoundFont 2.04 Specification](http://www.synthfont.com/sfspec24.pdf)
- [tidal-drum-machines](https://github.com/ritchse/tidal-drum-machines) - Sample source
- [File Loading Abstraction PRD](prd-file-loading-abstraction.md) - Prerequisite PRD
