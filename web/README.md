# Enkido Web IDE

Browser-based live-coding environment for Akkado programs running in the Cedar synth engine.

## Development

```bash
# Install dependencies
npm install

# Start development server
npm run dev

# Build for production
npm run build

# Preview production build
npm run preview
```

## Project Structure

```
web/
├── src/
│   ├── lib/
│   │   ├── components/     # Svelte components
│   │   │   ├── Editor/     # CodeMirror wrapper + inline widgets
│   │   │   ├── Transport/  # Play/Pause/BPM controls
│   │   │   ├── Controls/   # Faders, knobs, XY pads
│   │   │   └── Panel/      # Side panel container
│   │   ├── audio/          # AudioWorklet + WASM integration
│   │   ├── compiler/       # Akkado WASM compiler wrapper
│   │   ├── stores/         # Svelte 5 state (runes)
│   │   └── docs/           # Documentation content
│   └── routes/             # SvelteKit pages
├── static/
│   └── wasm/               # WASM binaries (akkado.wasm, cedar.wasm)
└── package.json
```

## Requirements

- Node.js 18+
- WASM binaries from parent project (built with Emscripten)

## Building WASM Modules

From the project root:

```bash
cmake --preset wasm
cmake --build build/wasm
cp build/wasm/akkado/akkado.wasm web/static/wasm/
cp build/wasm/cedar/cedar.wasm web/static/wasm/
```
