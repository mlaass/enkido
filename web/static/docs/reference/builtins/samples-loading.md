---
title: Samples Loading
category: builtins
order: 17
keywords: [samples, loading, bank, github, http, url, file, drag-drop, asset, registry, samples-directive, dirt-samples]
group: instruments
subgroup: sample-based
icon: FolderOpen
tagline: Pull sample banks from GitHub, HTTP, or local files.
---

# Samples Loading

How user-supplied audio reaches the sampler at runtime. The `samples()` directive declares a bank URI to fetch before the program runs; the IDE also accepts drag-and-drop and bundled banks. For the full URI scheme reference (github:, http:, file:, blob:, idb:, asset:, bundled:), see [URI Schemes](../uri-schemes).

## samples

**Sample-bank loader** - Compile-time directive that fetches a bank URL before playback begins.

| Param | Type   | Default | Description |
|-------|--------|---------|-------------|
| uri   | string | -       | Bank URI (string literal) |

The directive emits no audio-time instruction. It adds a `UriRequest{kind=SampleBank}` entry that the host (web app, CLI) drains before swapping bytecode. Multiple `samples()` calls accumulate in source order; identical URIs are deduplicated.

```akk
// Load the TidalCycles Dirt-Samples bank
samples("github:tidalcycles/Dirt-Samples")

// Now sample names from that bank are available in patterns
pat("bd sd cp ~") |> out(@)
```

## bank

A **bank** is a directory of named samples. The default 808 kit ships with names like `bd`, `sd`, `hh`, `oh`, `cp`. External banks add more; Dirt-Samples adds hundreds (`ho`, `bell`, `casio`, etc.).

Use `.bank("Name")` on a sample pattern to route events to a specific bank instead of the default kit:

```akk
samples("github:tidalcycles/Dirt-Samples")

pat("ho:0 ho:2 ho:4 ho:6").bank("Dirt-Samples") |> out(@)
```

## github

Use `github:user/repo` to fetch a sample bank from a GitHub repo. The host clones the repo's audio assets the first time the URI is referenced and caches them.

```akk
samples("github:tidalcycles/Dirt-Samples")
```

## http

Use `https://...` (or `http://...`) for samples hosted on any web server. CORS rules apply; the server must allow cross-origin reads.

```akk
samples("https://example.com/my-bank.zip")
```

## url

Any registered URI scheme (`github:`, `https:`, `file:`, `blob:`, `idb:`, `asset:`, `bundled:`) is accepted. The resolver dispatches to the right backend automatically.

## file

`file://` resolves to a local path. Used most often by the CLI; the web app's sandbox restricts direct filesystem access in favor of drag-drop into IndexedDB.

## drag-drop

In the web IDE, dragging a folder onto the editor stores its contents in IndexedDB and registers them under a `idb:` URI. Reference them with the same `samples()` syntax.

```akk
samples("idb:my-kit")
pat("kick snare hat").bank("my-kit") |> out(@)
```

## asset

`asset:` is for samples bundled into a packaged build (CLI binary, ESP32 image). The resolver reads from the bundled-asset registry baked at build time.

Related: [samplers](samplers), [soundfonts](soundfonts), [URI Schemes](../uri-schemes)
