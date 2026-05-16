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

## Hosting your own share endpoint

The `/p/<slug>` permalink feature is backed by a small Cloudflare Worker
(`web/share-api/`) that stores anonymous patches in a D1 database. The
reference deployment lives at `share.nkido.cc`, but the SPA can point at
any compatible Worker via the `PUBLIC_SHARE_API_BASE` env var.

If you want to run your own copy — for privacy, for an internal
deployment, or to fork the API surface — see
[`share-api/README.md`](share-api/README.md). The walkthrough takes a
fresh clone to a working share endpoint in under 30 minutes and covers
prerequisites, D1 setup, custom domains, rate limiting, backups, and
takedown procedures.

Leaving `PUBLIC_SHARE_API_BASE` empty is fine too — the IDE falls back
to inline-link sharing (the full patch is encoded in the URL hash),
which needs no backend.
