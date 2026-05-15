---
title: URI Schemes
category: reference
order: 50
keywords: [uri, resolver, samples, soundfont, wavetable, github, http, https, file, bundled, blob, idb, asset loading]
---

# URI Schemes

Every asset loaded by nkido — sample banks, SoundFonts, wavetables, single audio files — is identified by a URI. The same scheme syntax works in akkado source code (`samples("...")`), on the CLI (`nkido-cli --bank ...`), and in the web app. A single resolver dispatches each URI to the right backend based on its scheme prefix.

## Supported schemes

| Scheme | Example | Native | Web | Notes |
|---|---|---|---|---|
| `file://` | `file:///abs/path.wav` | ✓ | — | Absolute or relative filesystem paths. Bare paths (no scheme) are treated as `file://`. |
| `http://` | `http://example.com/x.json` | ✓ | ✓ | Plain HTTP. Cached on disk natively; in IndexedDB on the web. |
| `https://` | `https://example.com/x.sf2` | ✓ | ✓ | TLS via system cert store (native uses OpenSSL). |
| `github:` | `github:tidalcycles/Dirt-Samples` | ✓ | ✓ | Sugar for `raw.githubusercontent.com`. See below. |
| `bundled://` | `bundled://default-808` | ✓ | ✓ | Looks up an in-process table populated at host startup. Used for shipping default kits with the binary. |
| `blob:nkido:<id>` | `blob:nkido:abc123` | — | ✓ | Web only. Transient handle for `File` / `ArrayBuffer` uploads. Register via `uriResolver.registerBlob(file)`. |
| `idb:<key>` | `idb:my-bank` | — | ✓ | Web only. IndexedDB-stored asset (currently a stub; reserved). |

Bare paths like `./song.akkado` or `/etc/foo.wav` are treated as `file://` on native; on the web they have no handler and fail loudly. Windows drive letters (`C:\foo`) are detected and routed to `file://` rather than parsed as a `c:` scheme.

## The `github:` scheme

A shorthand for `https://raw.githubusercontent.com/...`. The resolver does the URL transform and recurses through the `https` handler, so caching and certificate handling are shared between the two paths.

```text
github:user/repo                          → main branch, root strudel.json
github:user/repo/branch                   → branch's root strudel.json
github:user/repo/branch/sub/dir           → branch's sub/dir/strudel.json
github:user/repo/branch/file.wav          → branch's file.wav (audio extension fetched as-is)
```

Audio-extension heuristic: `.wav` `.ogg` `.flac` `.mp3` `.aiff` `.sf2` `.sf3` `.json` are fetched as files; anything else is treated as a directory and `/strudel.json` is appended.

Default branch is `main`. Specify a different branch as the third path segment.

## In akkado source: `samples("...")`

The `samples()` directive declares a sample-bank URI for the host to fetch before the program runs. Combine with `.bank("Name")` on a sample pattern to route events to that bank instead of the bundled default kit:

```akkado
samples("github:tidalcycles/Dirt-Samples")

s"amencutup:0 amencutup:1 amencutup:2 amencutup:3"
    .bank("Dirt-Samples")
    .out()
```

Bank names come from the manifest's `_name` field, falling back to the last URL segment (so `github:tidalcycles/Dirt-Samples` → `Dirt-Samples`). Live demo: `dnb-amen` in the patch browser.

Multiple `samples()` calls are allowed; they're resolved in source order. Identical URIs are deduplicated. Each call must take exactly one string-literal argument (variables are not resolved at compile time).

The compiler emits no audio-time instruction for `samples()` — it's a compile-time directive that adds a `UriRequest{kind=SampleBank}` entry to `CompileResult.required_uris`. The host (web app, CLI) drains this list before swapping bytecode.

## On the CLI

`nkido-cli` accepts URI-keyed flags for assets supplied outside the program:

```bash
# Sample bank from GitHub
nkido-cli render --bank github:tidalcycles/Dirt-Samples \
  --seconds 5 -o out.wav --source 'sin(440) |> out(@)'

# SoundFont from a local path
nkido-cli render --soundfont ~/Music/gm.sf2 \
  --seconds 4 -o out.wav song.akkado

# Single sample from HTTPS, with explicit registry name
nkido-cli render --sample 'kick=https://example.com/kick.wav' \
  --seconds 1 -o out.wav --source '0 |> out(@)'
```

Every flag accepts any registered scheme. Bare paths are `file://`. Multiple `--bank` / `--soundfont` / `--sample` flags may be combined; banks accumulate in declaration order and are searched first-hit-wins for default-bank `RequiredSample` lookups.

`samples()` directives in source code are resolved alongside `--bank` flags (CLI flags first, source declarations after).

## Caching

Native runs cache HTTP responses under the platform-default user cache directory:

- Linux: `$XDG_CACHE_HOME/nkido/` (fallback `~/.cache/nkido/`)
- macOS: `~/Library/Caches/nkido/`
- Windows: `%LOCALAPPDATA%/nkido/cache/`

500 MB cap, evicted by mtime (least-recently-touched first). Cold fetch + parse runs in ~300 ms for the Dirt-Samples manifest; cached re-runs complete in ~30 ms.

Web runs cache in IndexedDB via the existing `FileCache` middleware around the `https` handler. The same URI is fetched once across the page session and then served from IDB.

## See also

- [Mini-notation reference](mini-notation-reference.md) — sample patterns (`sample`, `seq` with sample IDs)
- [`docs/prd-uri-resolver.md`](prd-uri-resolver.md) — design + history
