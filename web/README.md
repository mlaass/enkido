# NKIDO Web IDE

Browser-based live-coding environment for Akkado programs running in the Cedar synth engine.

## Development

```bash
# Install dependencies
bun install

# Start development server
bun run dev

# Build for production
bun run build

# Preview production build
bun run preview
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

- Bun (or Node.js 18+)
- Emscripten (for WASM builds)

## Building WASM Module

```bash
# From the web/ directory
bun run build:wasm
```

This runs `emcmake cmake` in `wasm/` and automatically copies the output to `static/wasm/`.
