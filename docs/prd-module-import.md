# PRD: Module/Import System

> **Status: PARTIALLY SHIPPED** — Phase 1a in the
> [Language Evolution Vision](vision-language-evolution.md). **Phase A
> (direct injection) and Phase B (namespace imports) have SHIPPED**
> (commits `c739f4f`, `d9b3d54`) — the `import` statement is live and
> running. The remaining work — Phase C (URI resolver unification),
> Phase D (web virtual filesystem + multi-tab editor), and the stdlib
> migration — is specified below and ready to implement.

## Status & Shipped Work

The `import` statement is **live today**. Both forms compile and run:

```akkado
import "filters"           // direct injection
import "effects" as fx     // namespaced
```

| Capability | State | Evidence |
|------------|-------|----------|
| `import` keyword + token | SHIPPED | `lexer.cpp:19`, `token.hpp:31` |
| `ImportDecl` AST node | SHIPPED | `ast.hpp:85`, `ImportDeclData` at `ast.hpp:333` |
| `FileResolver` / `FilesystemResolver` / `VirtualResolver` | SHIPPED | `file_resolver.{hpp,cpp}` |
| Import scanner (recursive resolve, topo sort, cycle detection) | SHIPPED | `import_scanner.{hpp,cpp}` |
| `SourceMap` (N-region diagnostic remapping) | SHIPPED | `source_map.{hpp,cpp}` |
| `compile()` takes `const FileResolver*` | SHIPPED | `akkado.hpp:94` |
| Namespace imports (`SymbolKind::Module`) | SHIPPED | `symbol_table.hpp:65`, analyzer |
| Error codes `E500`–`E505` | SHIPPED | wired in `akkado.cpp` |
| Tests | SHIPPED | `test_import.cpp`, `test_import_scanner.cpp`, `test_file_resolver.cpp`, `test_source_map.cpp` — 215 assertions, 36 cases, all passing |

**Remaining (this PRD):**

- **Phase C** — Unify import resolution onto `cedar::UriResolver` so
  imports reuse the same scheme-handler machinery as samples, MIDI, and
  soundfonts.
- **Phase D** — Web virtual filesystem and multi-tab editor.
- **Stdlib migration** — move `stdlib.hpp` content into auto-imported
  `.ak` modules.

The "Syntax", "File Resolution", and "Semantics" sections below describe
the shipped language feature and remain the normative reference. The
"Shipped: Phase A & B (as built)" section condenses the original
implementation plan to a record of what exists. Implementers should
focus on "Remaining Work" onward.

---

## Overview

Before this work, Akkado's entire standard library was a single
`constexpr std::string_view` (`STDLIB_SOURCE` in `stdlib.hpp`, ~115
lines of `.ak` source) prepended to every user program before lexing,
with diagnostic locations corrected by a two-region offset calculation.
That pattern did not scale — there was no way to split the stdlib across
files, and users could not organize multi-file projects.

This PRD specifies a module/import system that generalizes the
stdlib-prepend pattern into multi-file compilation. It is
**compiler-only** — no Cedar VM changes required.

### Motivation

1. **Stdlib migration**: `STDLIB_SOURCE` today holds `osc()`,
   `multiband3fx()`, `beat()`, `unison()`, and `glide()`; the ~30
   forwarding entries in `BUILTIN_ALIASES`; and future `const fn`
   generators (`linspace`, `harmonics`, tuning tables) live as hardcoded
   C++ special-cases. These should live in `.ak` files, not C++.
2. **Multi-file user projects**: Users need to split large patches
   across files — e.g., `synths.ak`, `effects.ak`, `main.ak`.
3. **Web virtual filesystem**: The web app needs an in-memory module
   system for multi-tab editing and bundled stdlib files.
4. **Progressive migration**: The existing `stdlib.hpp` continues
   working during transition. Modules are adopted incrementally.

### Non-Goals

- No Cedar VM changes
- No runtime module loading — all resolution is compile-time
- No package registry or versioning — Akkado is a live-coding DSL
- No per-module hot-reload — the entire program recompiles on change

---

## Syntax

Two import forms:

```akkado
import "filters"           // direct injection — all names enter global scope
import "filters" as f      // namespaced — access via f.lp(), f.hp()
```

### Rules

- Imports must appear at the **top of file**, before any other statements
- String argument is the module path (resolved by the resolver)
- All top-level definitions in a module are implicitly exported — no `export` keyword
- The `as` form binds a namespace identifier for qualified access

### Examples

```akkado
// Direct injection — names available unqualified
import "synths"
osc("saw", 440) |> out(@, @)

// Namespaced — names require qualifier
import "effects" as fx
osc("saw", 440) |> fx.chorus(@, 0.5, 0.3) |> out(@, @)

// Multiple imports
import "synths"
import "effects" as fx
import "./my-utils"
```

### Deferred Syntax

| Form | Reason |
|------|--------|
| `import { lp, hp } from "filters"` | Parser complexity, not needed for v1 |
| `export fn ...` | All top-level defs are public in v1 |
| `import "filters" as { lp, hp }` | Selective + namespaced, defer with selective |

---

## File Resolution

Resolution order for `import "path"`:

1. **Relative prefix** (`./` or `../`) — resolve relative to the importing file's directory
2. **Bare name** — search stdlib directory first, then resolve relative to importing file
3. **Extension** — `.ak` auto-appended if the path has no extension
4. **Deduplication** — if a module has already been resolved (by canonical identity), skip it silently

### Examples

| Import | Importing File | Resolution |
|--------|---------------|------------|
| `import "filters"` | `/project/main.ak` | stdlib `filters.ak`, then `/project/filters.ak` |
| `import "./utils"` | `/project/main.ak` | `/project/utils.ak` |
| `import "../shared/fx"` | `/project/src/main.ak` | `/project/shared/fx.ak` |
| `import "std/tuning"` | any | stdlib `std/tuning.ak` |

### Resolver Interface (as shipped)

`akkado/include/akkado/file_resolver.hpp` defines:

```cpp
class FileResolver {
public:
    virtual ~FileResolver() = default;

    /// Resolve an import path to a canonical path.
    virtual std::optional<std::string> resolve(
        std::string_view import_path,
        std::string_view from_file) const = 0;

    /// Read the contents of a resolved (canonical) module path.
    virtual std::optional<std::string> read(
        std::string_view canonical_path) const = 0;
};
```

Two implementations ship today: `FilesystemResolver` (CLI, `std::ifstream`
over a search-path list) and `VirtualResolver` (in-memory
`std::unordered_map<path, source>`, used by tests and intended for
web/WASM). **Phase C retargets this onto `cedar::UriResolver`** — see
"Remaining Work" below.

---

## Semantics

### Direct Injection (`import "path"`)

All top-level definitions from the imported module enter the importing
file's global scope. This is the natural generalization of the current
stdlib prepend — concatenation order determines shadowing.

- If two modules define the same name, the later import wins (last-wins shadowing)
- Local definitions shadow imported names
- This matches the current stdlib behavior: user code can override `osc()`, `multiband3fx()`, etc.

### Namespaced Import (`import "path" as alias`)

Definitions from the imported module are only accessible via
`alias.name`. They do not pollute the global scope.

```akkado
import "effects" as fx

// fx.chorus is accessible
osc("saw", 440) |> fx.chorus(@, 0.5, 0.3)

// chorus alone is NOT accessible
osc("saw", 440) |> chorus(@, 0.5, 0.3)  // ERROR: undefined 'chorus'
```

### Circular Imports

Detected during resolution. The import scanner builds a dependency graph
and runs topological sort — a cycle produces a compile error:

```
E500: Circular import detected: main.ak → utils.ak → helpers.ak → main.ak
```

### Transitive Dependencies

If `A` imports `B` (direct injection) and `B` imports `C` (direct injection):
- All three modules end up in the combined source (topologically sorted: `C`, then `B`, then `A`)
- `C`'s names ARE visible in `A` — a natural consequence of the concatenation model
- This matches the current stdlib behavior: stdlib definitions are visible everywhere

This is the simplest model and the right default for a live-coding DSL —
users shouldn't need to re-import transitive dependencies. If isolation
is needed, use namespaced imports: `B`'s namespace in `A` contains only
`B`'s own top-level definitions, not `C`'s.

### Import Order

The import scanner resolves all imports recursively, then topologically
sorts modules so dependencies are concatenated before dependents. Within
a single file's import list, order determines shadowing priority (later
imports shadow earlier ones).

---

## Shipped: Phase A & B (as built)

This section records the implemented design. The files exist and the
tests pass; it is documented here for context, not as work to do.

### Phase A — File Resolver + Direct Injection (commit `c739f4f`)

The compile pipeline (`akkado.cpp`) runs an import pre-pass before
lexing:

```
1. resolve_imports(source, filename, resolver)
      → topologically sorted modules + diagnostics
2. Build combined source: stdlib + modules (dep order) + user source
3. Build SourceMap with byte/line ranges per region
4. Lex → Parse → Analyze → Codegen (unchanged — they see one string)
5. source_map.adjust_all(diagnostics) for each stage
6. source_map.adjust_source_locations() / adjust_state_inits() /
   adjust_viz_decls()
```

- **`import_scanner.{hpp,cpp}`** — lightweight line-based pre-parse for
  `import` directives (skips `//` comments, matches `import` only as the
  first token), recursive resolution via the `FileResolver`, dependency
  graph, DFS post-order topological sort, cycle detection. Import lines
  are replaced with blank lines to preserve byte/line offsets.
- **`source_map.{hpp,cpp}`** — `SourceMap` generalizes the old
  two-region `adjust_diagnostics()` to N regions. Each `Region` tracks
  `{filename, byte_offset, byte_length, line_offset}`; `adjust()` maps a
  diagnostic from combined-source coordinates back to its origin file,
  including related locations and fixes. The analyzer and codegen both
  receive the `SourceMap` (`analyzer.analyze(..., &source_map, ...)`,
  `codegen.generate(..., &source_map)`).
- **`Import` token / `ImportDecl` AST node** — the parser recognizes
  import syntax to emit clear errors (`E501` "import after code"); the
  scanner has already resolved and stripped import lines, so codegen
  treats `ImportDecl` as a no-op. The nodes exist for validation and
  future extensibility (e.g. selective imports).
- **`compile()` API** — gained `const FileResolver* resolver = nullptr`.
  When `nullptr`, an `import` produces `E505`. `compile_file()` builds a
  default `FilesystemResolver` rooted at the file's parent directory.

### Phase B — Namespace Imports (commit `d9b3d54`)

- `SymbolKind::Module` added to the symbol table. Symbols from a
  namespaced module are marked hidden (skipped by normal lookup) and
  carry their origin module; a `Module` symbol binds the alias.
- Qualified access (`f.lp()`): when the LHS of a field access / method
  call resolves to a `Module` symbol, the analyzer performs a
  module-qualified lookup instead of dot-call desugaring. An unknown
  member produces `E504`.

### Hot-Swap Semantic ID Stability (shipped)

The `CodeGenerator` tracks `path_stack_` (`codegen.hpp:834`) to generate
stable semantic IDs via FNV-1a hashing of the joined path. Stdlib
modules use a stable `<stdlib>` prefix on `path_stack_`; user modules
push their canonical path. Reordering imports does not change state IDs,
because the module origin is part of the path rather than the
concatenation position. The `SourceMap` lets codegen identify which
region a top-level statement belongs to.

---

## Remaining Work

Two phases plus the stdlib migration. Each is independently shippable.

### Phase C: URI Resolver Unification

**Goal**: imports resolve through the same machinery as samples, MIDI,
and soundfonts. Today those assets flow through `cedar::UriResolver` — a
process-global, scheme-keyed dispatcher (`cedar/include/cedar/io/uri_resolver.hpp`)
with handlers for `file`, `http`, `github`, and `bundled`. Import
resolution should reuse it rather than maintain a parallel
`FilesystemResolver` / `VirtualResolver` pair.

#### Why unify

- One asset-loading code path for the whole project (the
  [unified-resolver plan](vision-language-evolution.md) goal).
- Imports gain scheme support for free: `import "github://user/repo/fx"`,
  `import "bundled://std/filters"`.
- The web app already mirrors `UriResolver` in TypeScript
  (`web/src/lib/io/uri-resolver.ts`) — imports reuse it instead of a
  bespoke registration path.

#### Design

`akkado::FileResolver` keeps its two-method shape (`resolve()` does
path → canonical-URI logic: relative prefixes, `.ak` extension, stdlib
search; `read()` fetches bytes). But:

- `read()` delegates to `cedar::UriResolver::instance().load(uri)`
  instead of `std::ifstream` / an in-memory map.
- The **canonical URI string** is the identity used for deduplication
  and cycle detection in the import scanner (replacing the canonical
  filesystem path). Two imports that resolve to the same URI are the
  same module.
- `FilesystemResolver` and `VirtualResolver` collapse into a single
  `UriFileResolver` that holds only path-canonicalization config (search
  paths, stdlib root) and forwards all byte-fetching to `UriResolver`.
- Bare/relative import paths canonicalize to `file://` URIs on native;
  stdlib modules canonicalize to `bundled://` URIs (see Stdlib Migration).

#### Compile-time availability — pre-bundle model

Imports differ from samples: a sample is loaded *after* compilation (the
compiler emits `required_uris`, the host resolves them, bytes are fed
back). Import **source text is needed during compilation**, and
resolving one import reveals more recursively. A post-compile
required-asset loop cannot supply them.

**Resolution**: the host pre-bundles. Before calling `compile()` /
`akkado_compile()`, the host registers every `.ak` module the program
could need (stdlib + the user's open files) with the resolver so that
`UriResolver::load()` resolves them synchronously during compilation.
The `bundled` scheme is synchronous and available on native and WASM —
`cedar::BundledHandler::register_asset(name, bytes)` — making it the
vehicle for both stdlib and web modules.

> Async schemes (`http`, `github`) cannot be resolved synchronously
> inside WASM compilation. A program importing `github://...` on the web
> requires the host to pre-fetch that module (via the TS resolver) and
> register it as a `bundled` asset before compiling. Native CLI builds
> may resolve `github`/`http` imports directly, since `UriResolver::load()`
> is synchronous there.

#### Modified Files (Phase C)

| File | Change |
|------|--------|
| `akkado/include/akkado/file_resolver.hpp` | Replace `FilesystemResolver`/`VirtualResolver` with `UriFileResolver` delegating to `cedar::UriResolver` |
| `akkado/src/file_resolver.cpp` | URI canonicalization; `read()` → `UriResolver::load()` |
| `akkado/src/import_scanner.cpp` | Use canonical URI string as dedup/cycle key |
| `akkado/src/akkado.cpp` | `compile_file()` builds a `UriFileResolver`; ensure `file`/`bundled` handlers registered |
| `tools/akkado-cli/main.cpp`, `tools/nkido-cli/main.cpp` | Register stdlib `bundled` assets at startup (shared with sample/soundfont URI setup) |
| `akkado/tests/test_file_resolver.cpp` | Update for `UriFileResolver` |

### Phase D: Web Virtual Filesystem & Multi-Tab Editor

Depends on Phase C. UI + WASM glue.

#### WASM API

The web host pre-bundles modules via the `bundled` handler before
compiling. New C exports in `nkido_wasm.cpp`:

```cpp
WASM_EXPORT void akkado_register_module(const char* path, uint32_t path_len,
                                        const char* source, uint32_t source_len);
WASM_EXPORT void akkado_unregister_module(const char* path, uint32_t path_len);
WASM_EXPORT void akkado_clear_modules();
```

These forward to the WASM-side `cedar::BundledHandler` (the same handler
used for bundled samples), registering each module under a
`bundled://`-resolvable name. `akkado_compile()` runs after all modules
are registered; the `UriFileResolver` resolves `import` statements
synchronously against the bundled table.

#### Stdlib Bundling & Manifest

Stdlib `.ak` files live in `web/static/stdlib/`. The web app must know
which files exist without hardcoding the list:

- A build script (`web/scripts/`) globs `web/static/stdlib/**/*.ak` and
  emits a manifest (`web/static/stdlib/manifest.json` — an array of
  relative paths), run as part of `bun run build` alongside
  `build:docs` / `build:opcodes`.
- At web-app initialization, the host fetches `manifest.json`, then
  fetches and `akkado_register_module()`s each listed file before the
  first compile.

This gives the web app the same stdlib modules as the CLI.

#### Web Integration

| Component | Change |
|-----------|--------|
| `web/wasm/nkido_wasm.cpp` | `register/unregister/clear` module exports forwarding to `BundledHandler` |
| `web/src/lib/stores/editor.svelte.ts` | Multi-file state, active file tracking |
| `web/src/lib/components/Editor/` | Multi-tab editor UI (tab bar, file create/rename/delete) |
| `web/src/lib/workers/cedar-processor.js` | Register all modules with WASM before each compile |
| `web/scripts/` | Stdlib manifest generation script |
| `web/static/stdlib/` | Bundled `.ak` stdlib files |

---

## Stdlib Migration Plan

Progressive migration — `stdlib.hpp` continues working during the entire
transition. Depends on Phase C (stdlib modules resolve via the `bundled`
scheme).

### Auto-Import

Today `STDLIB_SOURCE` is prepended unconditionally — `osc()` works with
zero imports. To preserve this (Design Principle #4), **migrated stdlib
modules are auto-imported**: the compiler implicitly performs a
direct-injection import of every stdlib module into the global scope
before the user's first statement. Users never write `import "osc"`.

- The set of auto-imported stdlib modules is a fixed list known to the
  compiler (or derived from the stdlib manifest).
- Auto-imported modules participate in the same scanner / SourceMap /
  topological-sort pipeline as explicit imports — they are simply
  injected as implicit `import` directives ahead of the user source.
- User definitions still shadow stdlib names (last-wins concatenation,
  unchanged).

### Stdlib Search Path

The CLI must locate the stdlib `.ak` files. `compile_file()` currently
builds a resolver rooted only at the source file's parent directory —
the stdlib root must be added explicitly. Resolution order for the
stdlib root:

1. `AKKADO_STDLIB_DIR` environment variable, if set.
2. A path relative to the executable (e.g. `<bindir>/../share/akkado/stdlib`).
3. A compile-time default (`AKKADO_STDLIB_DIR` CMake define) for
   developer builds (`<repo>/stdlib`).

The CLI registers the discovered stdlib files as `bundled://std/*`
assets at startup (or adds the directory as a `file` search path). The
web app registers them from `web/static/stdlib/` via the manifest
(Phase D). Either way the compiler resolves stdlib imports through the
`bundled` scheme, so CLI and web share identical stdlib source.

### Migration Stages

#### Stage 1: Core Functions

```
stdlib/osc.ak        — osc() dispatcher
stdlib/effects.ak    — multiband3fx()
stdlib/voices.ak     — unison(), glide()
stdlib/clock.ak      — beat()
```

These cover the entire current `STDLIB_SOURCE`. Once migrated,
`STDLIB_SOURCE` becomes empty.

#### Stage 2: Aliases

```
stdlib/aliases.ak    — ~30 forwarding functions from BUILTIN_ALIASES
```

```akkado
// stdlib/aliases.ak
fn lowpass(sig, cut, q = 0.707) -> lp(sig, cut, q)
fn highpass(sig, cut, q = 0.707) -> hp(sig, cut, q)
fn bandpass(sig, cut, q = 1.0) -> bp(sig, cut, q)
fn reverb(sig, room = 0.5, damp = 0.5) -> freeverb(sig, room, damp)
fn distort(sig, drive = 2.0) -> saturate(sig, drive)
// ... etc
```

This removes the `BUILTIN_ALIASES` map from the compiler, making aliases
user-visible and documented.

#### Stage 3: Const Fn Generators

```
stdlib/math.ak       — mtof, ftom, dbtoa, atodb
stdlib/tuning.ak     — edo_scale, just_intonation, pythagorean
stdlib/wavetable.ak  — linspace, harmonics, normalize, wavetable
```

These move the hardcoded `codegen_arrays.cpp` special-cases
(`handle_linspace_call`, `handle_harmonics_call`, etc.) to stdlib
`const fn` definitions.

#### Stage 4: Remove Hardcoded stdlib

Once all stages are migrated and tested:
- `STDLIB_SOURCE` reduced to an empty string or removed entirely
- `BUILTIN_ALIASES` map removed from `builtins.hpp`
- Codegen special-cases for `linspace`/`harmonics` removed

---

## Test Cases

### Phase A & B (shipped — see `test_import*.cpp`)

The shipped suite covers direct injection, transitive imports,
shadowing, circular-dependency errors (`E500`), import-after-code
(`E501`), deduplication, relative imports, namespace access, hidden
names, and module-qualified errors (`E504`). 215 assertions, all
passing.

### Phase C: URI Resolver Unification

```akkado
// Stdlib via bundled scheme
osc("saw", 440) |> out(@, @)        // osc() resolved from bundled://std/osc.ak

// Explicit bundled import
import "bundled://std/filters"

// github import (native CLI; pre-bundled on web)
import "github://user/repo/fx" as fx
```

- `UriFileResolver` resolves a bare import to a `file://` URI and a
  stdlib name to a `bundled://` URI.
- Two imports resolving to the same canonical URI are deduplicated.
- An `import` of an unregistered `bundled` name produces `E502`.
- A `UriResolver::load()` failure on a resolved URI produces `E503`.

### Phase D: Web Virtual Filesystem

- WASM: `akkado_register_module("utils", source)` then compile with
  `import "utils"` succeeds.
- WASM: `akkado_clear_modules()` then compile with `import "utils"`
  produces `E502`.
- Web: the stdlib manifest loads all `web/static/stdlib/*.ak`; `osc()`
  works with no explicit import (auto-import).
- Web: multi-tab editor saves/loads modules; compilation registers all
  open files before compiling.

---

## Error Codes

Import errors use the E500 range (E200–E4xx are taken by const
evaluation, tap_delay, and poly). All codes below are **shipped and
wired** in `akkado.cpp`.

| Code | Message | Phase |
|------|---------|-------|
| `E500` | `Circular import detected: {cycle path}` | A (shipped) |
| `E501` | `Import statements must appear before other code` | A (shipped) |
| `E502` | `Module not found: '{path}'` | A (shipped) |
| `E503` | `Failed to read module: '{path}'` | A (shipped) |
| `E504` | `Module '{alias}' has no definition '{name}'` | B (shipped) |
| `E505` | `Import requires a file resolver (not available in this context)` | A (shipped) |

Phase C reuses `E502`/`E503` for URI resolution failures; no new codes
are required.

---

## Deferred

| Feature | Reason |
|---------|--------|
| Selective imports (`import { a, b } from "path"`) | Parser complexity, not needed for v1 |
| `export` keyword | All top-level defs are public in v1 |
| Re-exports (`export import "path"`) | Not needed for stdlib migration |
| Package registry / versioning | Akkado is a live-coding DSL |
| Dynamic import / per-module hot-reload | Module resolution is compile-time only |
| Conditional imports | No use case identified |
| Async (`http`/`github`) imports resolved inside WASM | WASM compilation is synchronous; web hosts pre-bundle instead |

---

## Design Principles

This PRD follows the principles from the
[Language Evolution Vision](vision-language-evolution.md):

1. **The VM stays minimal** — module resolution is compiler-only. Same
   opcodes, same instruction format, same bytecode. A program with
   imports produces identical bytecode to the equivalent single-file
   program.
2. **Zero-allocation runtime** — imports are resolved at compile time.
   No runtime module loading, no dynamic linking.
3. **One asset path** — imports resolve through `cedar::UriResolver`,
   the same dispatcher used for samples, MIDI, and soundfonts (Phase C).
4. **Live-coding ergonomics** — direct injection is the default, and
   stdlib modules are auto-imported, matching the current "everything in
   scope" behavior. Namespaces are opt-in for when organization matters.
5. **Backward compatibility** — existing programs compile identically.
   The stdlib prepend continues working until Stage 4. Imports are
   additive syntax.
6. **Progressive migration** — the stdlib migrates one file at a time.
   Each step is independently testable and reversible.
