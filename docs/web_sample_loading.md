# Web Sample Loading Guide

## Overview

The Enkido web interface supports loading audio samples for use with the sampler system. Samples can be loaded from files, URLs, or programmatically generated.

## Sample Loading Architecture

Samples are loaded **at runtime**, not at compile time. The compile and load phases are separated:

### Compile Flow

```
1. compile(source)          → Returns {success, requiredSamples: ["bd", "sd", ...]}
2. ensureSampleLoaded(name) → Loads sample if not already loaded
3. loadCompiledProgram()    → Resolves sample IDs and loads program
```

The `compile()` function handles all three steps automatically. If any required sample cannot be loaded, compilation returns an error.

### Lazy Background Preloading

Default samples are preloaded in the background when the audio engine initializes. This means:
- Compilation doesn't block waiting for sample downloads
- If a required sample is still loading, `compile()` waits for it
- Unknown samples (not in default kit) cause a runtime error

## API Methods

### audioEngine.loadSampleFromFile()

Load a sample from a user-selected file.

```typescript
async function loadSampleFromFile(name: string, file: File | Blob): Promise<boolean>
```

**Example:**
```typescript
import { audioEngine } from '$lib/stores/audio.svelte';

// File input handler
async function handleFileUpload(event: Event) {
  const input = event.target as HTMLInputElement;
  const file = input.files?.[0];
  
  if (file) {
    const success = await audioEngine.loadSampleFromFile('kick', file);
    if (success) {
      console.log('Sample loaded successfully!');
    }
  }
}
```

### audioEngine.loadSampleFromUrl()

Load a sample from a URL.

```typescript
async function loadSampleFromUrl(name: string, url: string): Promise<boolean>
```

**Example:**
```typescript
// Load from static assets
await audioEngine.loadSampleFromUrl('kick', '/samples/kick.wav');

// Load from external URL (requires CORS)
await audioEngine.loadSampleFromUrl('snare', 'https://example.com/snare.wav');
```

### audioEngine.loadSamplePack()

Load multiple samples at once.

```typescript
async function loadSamplePack(samples: Array<{ name: string; url: string }>): Promise<number>
```

**Example:**
```typescript
const drumKit = [
  { name: 'bd', url: '/samples/kick.wav' },
  { name: 'sd', url: '/samples/snare.wav' },
  { name: 'hh', url: '/samples/hihat.wav' },
  { name: 'cp', url: '/samples/clap.wav' }
];

const loaded = await audioEngine.loadSamplePack(drumKit);
console.log(`Loaded ${loaded} samples`);
```

### audioEngine.loadSample()

Load a sample from raw float audio data (advanced).

```typescript
function loadSample(name: string, audioData: Float32Array, channels: number, sampleRate: number)
```

**Example:**
```typescript
// Generate a simple sine wave sample
const sampleRate = 48000;
const duration = 0.5; // seconds
const frequency = 440; // Hz
const samples = new Float32Array(sampleRate * duration);

for (let i = 0; i < samples.length; i++) {
  samples[i] = Math.sin(2 * Math.PI * frequency * i / sampleRate);
}

audioEngine.loadSample('sine', samples, 1, sampleRate);
```

### audioEngine.clearSamples()

Clear all loaded samples.

```typescript
function clearSamples()
```

## UI Component Example

Here's a complete Svelte component for sample management:

```svelte
<script lang="ts">
  import { audioEngine } from '$lib/stores/audio.svelte';
  
  let fileInput: HTMLInputElement;
  let sampleName = 'mysample';
  let loading = false;
  
  async function handleFileSelect(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    
    if (!file) return;
    
    loading = true;
    const success = await audioEngine.loadSampleFromFile(sampleName, file);
    loading = false;
    
    if (success) {
      alert(`Sample "${sampleName}" loaded!`);
    } else {
      alert('Failed to load sample');
    }
  }
  
  async function loadDefaultKit() {
    loading = true;
    
    const kit = [
      { name: 'bd', url: '/samples/drums/kick.wav' },
      { name: 'sd', url: '/samples/drums/snare.wav' },
      { name: 'hh', url: '/samples/drums/hihat.wav' },
      { name: 'cp', url: '/samples/drums/clap.wav' }
    ];
    
    const loaded = await audioEngine.loadSamplePack(kit);
    loading = false;
    
    alert(`Loaded ${loaded} of ${kit.length} samples`);
  }
</script>

<div class="sample-manager">
  <h3>Sample Manager</h3>
  
  <div class="upload-section">
    <input
      type="text"
      bind:value={sampleName}
      placeholder="Sample name (e.g., kick, snare)"
    />
    
    <input
      type="file"
      accept=".wav"
      bind:this={fileInput}
      on:change={handleFileSelect}
      disabled={loading}
    />
    
    <button on:click={() => fileInput.click()} disabled={loading}>
      {loading ? 'Loading...' : 'Upload Sample'}
    </button>
  </div>
  
  <div class="presets">
    <button on:click={loadDefaultKit} disabled={loading}>
      Load Default Drum Kit
    </button>
    
    <button on:click={() => audioEngine.clearSamples()}>
      Clear All Samples
    </button>
  </div>
</div>

<style>
  .sample-manager {
    padding: 1rem;
    border: 1px solid #ccc;
    border-radius: 4px;
  }
  
  .upload-section {
    display: flex;
    gap: 0.5rem;
    margin-bottom: 1rem;
  }
  
  .presets {
    display: flex;
    gap: 0.5rem;
  }
</style>
```

## Usage in Akkado Code

Once samples are loaded, they can be used in mini-notation patterns:

```akkado
// Simple drum pattern
pat("bd sd bd sd")

// With modifiers
pat("bd sd hh sd")*2

// Multiple patterns
kick = pat("bd ~ bd ~")
snare = pat("~ sd ~ sd")
hats = pat("hh hh hh hh")*2

out(kick + snare + hats)
```

## Sample Organization

### Recommended Directory Structure

```
/public/samples/
  /drums/
    kick.wav
    snare.wav
    hihat.wav
    clap.wav
    rim.wav
  /percussion/
    conga.wav
    bongo.wav
    shaker.wav
  /fx/
    laser.wav
    explosion.wav
```

### Standard Sample Names

For compatibility with the default sample registry, use these names:

| Name | Description |
|------|-------------|
| `bd`, `kick` | Bass drum / Kick |
| `sd`, `snare` | Snare drum |
| `hh`, `hihat` | Hi-hat (closed) |
| `oh` | Open hi-hat |
| `cp`, `clap` | Clap |
| `rim` | Rimshot |
| `tom` | Tom |
| `perc` | Percussion |
| `cymbal` | Cymbal |
| `crash` | Crash cymbal |

## Technical Details

### Supported Formats

- **WAV files**: 16-bit, 24-bit, 32-bit PCM, and 32-bit IEEE float
- **Sample rates**: Any (automatically resampled during playback)
- **Channels**: Mono and stereo

### Memory Considerations

- Samples are stored in WASM memory
- A 1-second stereo sample at 48kHz uses ~384KB
- Consider lazy-loading samples or using compressed formats for large libraries

### CORS Requirements

When loading samples from external URLs:
- The server must send appropriate CORS headers
- Use relative URLs for same-origin requests
- Consider hosting samples on a CDN with CORS enabled

## Example: Auto-load Samples on Init

```typescript
import { audioEngine } from '$lib/stores/audio.svelte';

// Load samples when audio engine initializes
audioEngine.initialize().then(async () => {
  console.log('Loading default samples...');
  
  const samples = [
    { name: 'bd', url: '/samples/kick.wav' },
    { name: 'sd', url: '/samples/snare.wav' },
    { name: 'hh', url: '/samples/hihat.wav' }
  ];
  
  await audioEngine.loadSamplePack(samples);
  console.log('Samples ready!');
});
```

## Troubleshooting

### Sample Not Playing

1. Check that the sample was loaded successfully (check console logs)
2. Verify the sample name matches what's used in the pattern
3. Ensure the audio engine is initialized and playing

### File Upload Not Working

1. Check file format (must be WAV)
2. Verify file size is reasonable (<10MB recommended)
3. Check browser console for errors

### CORS Errors

1. Ensure the server sends `Access-Control-Allow-Origin` header
2. Use relative URLs for same-origin requests
3. Consider hosting samples on your own domain

## Future Enhancements

- Drag-and-drop sample upload
- Sample preview/playback
- Visual waveform display
- Sample library browser
- Automatic sample pack detection
- Support for more formats (MP3, OGG via Web Audio API decoding)
