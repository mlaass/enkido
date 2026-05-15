# PRD: Module/Import System

> **Status: Proposed** — Phase 1a in the [Language Evolution Vision](vision-language-evolution.md).

## Overview

Akkado's entire standard library is a 40-line `constexpr std::string_view` in `stdlib.hpp`, prepended to every user program before lexing. Diagnostic locations are adjusted with a two-region offset calculation (`adjust_diagnostics()` in `akkado.cpp`). This works but doesn't scale — there's no way to split the stdlib across files, and users can't organize multi-file projects.

This PRD specifies a module/import system that generalizes the stdlib-prepend pattern into multi-file compilation. It is **compiler-only** — no Cedar VM changes required.

### Motivation

1. **Stdlib migration**: The ~40 aliases in `BUILTIN_ALIASES`, the `osc()` dispatcher, `multiband3fx()`, and future `const fn` generators (`linspace`, `harmonics`, tuning tables) should live in `.ak` files, not hardcoded C++ strings.
2. **Multi-file user projects**: Users need to split large patches across files — e.g., `synths.ak`, `effects.ak`, `main.ak`.
3. **Web virtual filesystem**: The web app needs an in-memory module system for multi-tab editing and bundled stdlib files.
4. **Progressive migration**: The existing `stdlib.hpp` continues working during transition. Modules are adopted incrementally.

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
- String argument is the module path (resolved by the FileResolver)
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
4. **Deduplication** — if a module has already been resolved (by canonical path), skip it silently

### Examples

| Import | Importing File | Resolution |
|--------|---------------|------------|
| `import "filters"` | `/project/main.ak` | stdlib `filters.ak`, then `/project/filters.ak` |
| `import "./utils"` | `/project/main.ak` | `/project/utils.ak` |
| `import "../shared/fx"` | `/project/src/main.ak` | `/project/shared/fx.ak` |
| `import "std/tuning"` | any | stdlib `std/tuning.ak` |

### FileResolver Abstraction

New file: `akkado/include/akkado/file_resolver.hpp`

```cpp
class FileResolver {
public:
    virtual ~FileResolver() = default;

    /// Resolve an import path to a canonical path.
    /// Returns nullopt if the module cannot be found.
    /// @param import_path  The path string from the import statement
    /// @param from_file    The canonical path of the importing file
    virtual std::optional<std::string> resolve(
        std::string_view import_path,
        std::string_view from_file) const = 0;

    /// Read the contents of a resolved module.
    /// @param canonical_path  Path returned by resolve()
    virtual std::optional<std::string> read(
        std::string_view canonical_path) const = 0;
};
```

Two implementations:

**`FilesystemResolver`** (CLI):
- Constructor takes a list of search paths (stdlib dir + user-specified)
- `resolve()` checks relative paths first, then search paths
- `read()` reads from the filesystem via `std::ifstream`

**`VirtualResolver`** (web):
- Backed by `std::unordered_map<std::string, std::string>` (path -> source)
- Modules registered via `register_module(path, source)` / `unregister_module(path)`
- Stdlib `.ak` files pre-loaded at initialization

---

## Semantics

### Direct Injection (`import "path"`)

All top-level definitions from the imported module enter the importing file's global scope. This is the natural generalization of the current stdlib prepend — concatenation order determines shadowing.

- If two modules define the same name, the later import wins (last-wins shadowing)
- Local definitions shadow imported names
- This matches the current stdlib behavior: user code can override `osc()`, `multiband3fx()`, etc.

### Namespaced Import (`import "path" as alias`)

Definitions from the imported module are only accessible via `alias.name`. They do not pollute the global scope.

```akkado
import "effects" as fx

// fx.chorus is accessible
osc("saw", 440) |> fx.chorus(@, 0.5, 0.3)

// chorus alone is NOT accessible
osc("saw", 440) |> chorus(@, 0.5, 0.3)  // ERROR: undefined 'chorus'
```

### Circular Imports

Detected during resolution. The import scanner builds a dependency graph and runs topological sort — a cycle produces a compile error:

```
E500: Circular import detected: main.ak → utils.ak → helpers.ak → main.ak
```

### Transitive Dependencies

If `A` imports `B` (direct injection) and `B` imports `C` (direct injection):
- All three modules end up in the combined source (topologically sorted: `C`, then `B`, then `A`)
- `C`'s names ARE visible in `A` — this is a natural consequence of the concatenation model
- This matches the current stdlib behavior: stdlib definitions are visible everywhere

This is the simplest model and the right default for a live-coding DSL — users shouldn't need to re-import transitive dependencies. If isolation is needed, use namespaced imports (Phase B): `B`'s namespace in `A` contains only `B`'s own top-level definitions, not `C`'s.

### Import Order

The import scanner resolves all imports recursively, then topologically sorts modules so that dependencies are concatenated before dependents. Within a single file's import list, order determines shadowing priority (later imports shadow earlier ones).

---

## Implementation

Three phases, each independently shippable.

### Phase A: File Resolver + Direct Injection

Core work. Generalizes the stdlib-prepend pattern to N source files.

#### New Files

| File | Purpose |
|------|---------|
| `akkado/include/akkado/file_resolver.hpp` | `FileResolver` interface, `FilesystemResolver`, `VirtualResolver` |
| `akkado/src/file_resolver.cpp` | Implementations |
| `akkado/include/akkado/source_map.hpp` | N-region source mapping (replaces `adjust_diagnostics()`) |
| `akkado/src/source_map.cpp` | Implementation |
| `akkado/include/akkado/import_scanner.hpp` | Pre-parse import directives, recursive resolution, topological sort, cycle detection |
| `akkado/src/import_scanner.cpp` | Implementation |

#### Modified Files

| File | Change |
|------|--------|
| `akkado/include/akkado/token.hpp` | Add `Import` token type (`As` already exists for pipe bindings, reused for `import ... as`) |
| `akkado/src/lexer.cpp` | Add `"import"` to keyword map |
| `akkado/include/akkado/ast.hpp` | Add `ImportDecl` node type + `ImportDeclData` |
| `akkado/src/parser.cpp` | Parse import statements (before other statements) |
| `akkado/include/akkado/akkado.hpp` | Add `FileResolver*` parameter to `compile()` |
| `akkado/src/akkado.cpp` | Import resolution pre-pass; replace `adjust_diagnostics()` with SourceMap; concatenate stdlib + sorted modules + user source |
| `tools/akkado-cli/main.cpp` | Create `FilesystemResolver`, pass to `compile()` |
| `tools/nkido-cli/main.cpp` | Same |

#### SourceMap Design

The current `adjust_diagnostics()` handles two regions: stdlib and user code. The new `SourceMap` generalizes this to N regions:

```cpp
class SourceMap {
public:
    struct Region {
        std::string filename;       // e.g., "filters.ak", "<stdlib>", "<web>"
        std::size_t byte_offset;    // Start offset in combined source
        std::size_t byte_length;    // Length of this region
        std::size_t line_offset;    // Number of lines before this region
    };

    /// Add a region to the source map
    void add_region(std::string filename, std::size_t byte_offset,
                    std::size_t byte_length, std::size_t line_count);

    /// Adjust a diagnostic's location from combined-source coordinates
    /// back to the originating file's local coordinates.
    /// Adjusts the diagnostic's primary location, all Related locations,
    /// and the Fix location if present — matching the current
    /// adjust_diagnostics() behavior.
    void adjust(Diagnostic& diag) const;

    /// Adjust all diagnostics in a vector
    void adjust_all(std::vector<Diagnostic>& diagnostics) const;

    /// Find which region contains a given byte offset
    const Region* find_region(std::size_t byte_offset) const;
};
```

Each region tracks where a module's source begins in the concatenated string and how many lines precede it. `adjust()` finds the containing region and subtracts the region's byte/line offsets — the same math as the current `adjust_diagnostics()`, but generalized.

#### Import Scanner Design

The import scanner runs **before** lexing. It does a lightweight pre-parse of the source to extract import directives, then recursively resolves them:

```cpp
struct ImportDirective {
    std::string path;                   // The import path string
    std::string alias;                  // Empty for direct injection
    SourceLocation location;            // For error reporting
};

struct ResolvedModule {
    std::string canonical_path;         // Unique identifier for deduplication
    std::string source;                 // File contents
    std::vector<ImportDirective> imports; // This module's own imports
};

struct ImportResult {
    /// Modules in topological order (dependencies first)
    std::vector<ResolvedModule> modules;
    std::vector<Diagnostic> diagnostics;
    bool success = true;
};

/// Scan source for import directives and recursively resolve them.
/// Returns modules in dependency order (topologically sorted).
ImportResult resolve_imports(
    std::string_view source,
    std::string_view filename,
    const FileResolver& resolver);
```

The scanner:
1. Scans the source for lines matching `import "..."` or `import "..." as ident` using lightweight line-based matching that skips `//` comment lines and only matches `import` as the first token on a non-comment line
2. For each import, calls `resolver.resolve()` to get the canonical path and source
3. Replaces import lines in each module's source with blank lines (preserving byte offsets and line counts for SourceMap accuracy) before the source is concatenated
4. Recursively scans imported modules for their own imports
5. Builds a dependency graph and runs topological sort (DFS post-order)
6. Detects cycles and reports errors with the full cycle path

The scanner uses lightweight line-based matching (not the full lexer) — it only needs to find `import` as the first token on a non-comment line followed by a string literal. This avoids bootstrapping issues with the lexer needing to know about imports.

**Scanner vs. Parser roles**: The scanner (`import_scanner.hpp`) runs before lexing and handles all file resolution, cycle detection, topological sorting, and import line stripping. The `Import` token and `ImportDecl` AST node in the parser exist for **validation and future extensibility** (e.g., `import { a, b } from "path"`). In Phase A, the parser recognizes import syntax to produce clear errors like "import after code" (E501), but the scanner has already resolved and stripped the import lines. The parser emits `ImportDecl` nodes that codegen treats as no-ops.

#### Compile Flow Change

Current flow (`akkado.cpp:56-169`):

```
1. Prepend stdlib to user source
2. Lex → Parse → Analyze → Codegen
3. adjust_diagnostics() with stdlib offset
```

New flow:

```
1. resolve_imports(source, filename, resolver)
      → topologically sorted modules + diagnostics
2. Build combined source: stdlib + modules (dep order) + user source
3. Build SourceMap with byte/line ranges per region
4. Lex → Parse → Analyze → Codegen (unchanged)
5. source_map.adjust_all(diagnostics)
6. source_map.adjust source_locations and state_inits
```

The parser, analyzer, and codegen are untouched — they still see a single concatenated source string. The SourceMap replaces the manual `adjust_diagnostics()` calls.

#### API Change

```cpp
// Before
CompileResult compile(std::string_view source,
                     std::string_view filename = "<input>",
                     SampleRegistry* sample_registry = nullptr);

// After
CompileResult compile(std::string_view source,
                     std::string_view filename = "<input>",
                     SampleRegistry* sample_registry = nullptr,
                     const FileResolver* resolver = nullptr);
```

When `resolver` is `nullptr`, imports are not supported (error if encountered). The existing `stdlib.hpp` prepend still works regardless — it's orthogonal to the import system.

---

### Phase B: Namespace Imports (`as` alias)

Depends on Phase A.

#### Symbol Table Changes

Add `SymbolKind::Module` and module-awareness fields:

```cpp
enum class SymbolKind : std::uint8_t {
    Variable,
    Builtin,
    Parameter,
    UserFunction,
    Pattern,
    Array,
    FunctionValue,
    Record,
    Module,          // NEW: namespace import
};

struct Symbol {
    // ... existing fields ...
    bool hidden = false;                // NEW: true for namespaced-import definitions
    std::string origin_module;          // NEW: module path this symbol came from
};
```

#### Module Symbol

A `Module` symbol stores the module's canonical path. It doesn't hold definitions directly — they're in the symbol table with `hidden = true` and `origin_module` set.

```cpp
struct ModuleInfo {
    std::string canonical_path;
    std::string alias;
};
```

#### Resolution Rules

For namespaced imports (`import "filters" as f`):
1. During concatenation, the module's source is still included (definitions must be compiled)
2. During analysis, after the definition-collection pass, mark all symbols originating from this module as `hidden = true` and set `origin_module`
3. Register a `Module` symbol named `f` pointing to the module
4. Hidden symbols are skipped during normal lookup

For qualified access (`f.lp()`):
- The analyzer already handles `FieldAccess` and `MethodCall` nodes
- When the LHS of a field access / method call resolves to `SymbolKind::Module`, perform a **module-qualified lookup** instead of dot-call desugaring
- Module-qualified lookup: find a symbol with `origin_module == module.canonical_path` and the given name

#### Modified Files

| File | Change |
|------|--------|
| `akkado/include/akkado/symbol_table.hpp` | `SymbolKind::Module`, `hidden`, `origin_module` fields |
| `akkado/src/symbol_table.cpp` | Skip hidden symbols in `lookup()` unless module-qualified |
| `akkado/src/analyzer.cpp` | Module-qualified resolution in `collect_definitions()`, `rewrite_pipes()`, `desugar_method_call()` |
| `akkado/src/analyzer.cpp` | During analysis: after `collect_definitions()` but before reference resolution, use SourceMap to identify symbols from namespaced modules and mark them hidden |

#### Interaction with Dot-Call

The analyzer's `desugar_method_call()` currently rewrites `x.f(a)` to `f(x, a)`. With module namespaces:

1. If `x` resolves to a `Module` symbol → do NOT desugar as dot-call; instead rewrite `x.f(a)` to a direct call to the module-qualified `f` with args `(a)`
2. If `x` resolves to anything else → desugar as before (dot-call)

This is checked by looking up `x` in the symbol table before deciding the rewrite strategy.

---

### Phase C: Web Virtual Filesystem

Separate from compiler work — UI + WASM glue.

#### WASM API

New C exports in `nkido_wasm.cpp`:

```cpp
WASM_EXPORT void akkado_register_module(const char* path, uint32_t path_len,
                                         const char* source, uint32_t source_len);
WASM_EXPORT void akkado_unregister_module(const char* path, uint32_t path_len);
WASM_EXPORT void akkado_clear_modules();
```

A global `VirtualResolver` instance is maintained in `nkido_wasm.cpp`. `akkado_compile()` passes it to `compile()`.

#### Web Integration

| Component | Change |
|-----------|--------|
| `web/wasm/nkido_wasm.cpp` | Global `VirtualResolver`, register/unregister/clear exports |
| `web/src/lib/stores/editor.svelte.ts` | Multi-file state, active file tracking |
| `web/src/lib/components/Editor/` | Multi-tab editor UI (tab bar, file create/rename/delete) |
| `web/src/lib/workers/cedar-processor.js` | Pass module map to WASM before compilation |
| `web/static/stdlib/` | Bundled `.ak` stdlib files, loaded at init |

#### Stdlib Bundling

Stdlib `.ak` files are placed in `web/static/stdlib/`. At web app initialization, they're fetched and registered with the `VirtualResolver` via `akkado_register_module()`. This means the web app can use the same stdlib files as the CLI.

---

## Hot-Swap Semantic ID Stability

The CodeGenerator tracks a `path_stack_` (`codegen.hpp:448`) to generate stable semantic IDs via FNV-1a hashing of the joined path (e.g., `main/osc1/delay` → `fnv1a("main/osc1/delay")`). These IDs are used by the `StatePool` to preserve DSP state across hot-swaps.

When processing definitions from imported modules, the module's origin must be pushed onto `path_stack_` so that state IDs include the module context. This ensures that:

1. A `delay()` in `filters.ak` gets a different state ID than a `delay()` in `synths.ak`
2. Reordering imports doesn't change state IDs (the module origin is part of the path, not the concatenation position)

**Implementation**: Pass the `SourceMap` to the `CodeGenerator`. Before visiting a top-level statement, check which region it falls in. If the region's filename differs from the current path context, push/pop the module origin on `path_stack_`.

**Stdlib path stability**: Stdlib modules (those resolved from the stdlib search path) use a stable `<stdlib>` prefix on `path_stack_`, not their actual filesystem path. User modules push their canonical filename. This preserves backward compatibility: current stdlib definitions produce the same semantic IDs as before migration, since the existing stdlib contributes nothing to the path stack and `<stdlib>` matches the filename already used in diagnostics.

---

## Stdlib Migration Plan

Progressive migration — `stdlib.hpp` continues working during the entire transition.

### Stage 1: Core Functions

```
stdlib/osc.ak        — osc() dispatcher (currently in stdlib.hpp)
stdlib/effects.ak    — multiband3fx() (currently in stdlib.hpp)
```

These two functions are the entire current `stdlib.hpp` content. Once migrated, `STDLIB_SOURCE` becomes empty.

### Stage 2: Aliases

```
stdlib/aliases.ak    — ~40 forwarding functions from BUILTIN_ALIASES
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

This removes the `BUILTIN_ALIASES` map from the compiler, making aliases user-visible and documented.

### Stage 3: Const Fn Generators

```
stdlib/math.ak       — mtof, ftom, dbtoa, atodb
stdlib/tuning.ak     — edo_scale, just_intonation, pythagorean
stdlib/wavetable.ak  — linspace, harmonics, normalize, wavetable
```

These move the hardcoded `codegen_arrays.cpp` special-cases to stdlib `const fn` definitions.

### Stage 4: Remove Hardcoded stdlib

Once all stages are migrated and tested:
- `stdlib.hpp` reduced to an empty string or removed entirely
- `BUILTIN_ALIASES` map removed from `builtins.hpp`
- Codegen special-cases for `linspace`/`harmonics`/`random` removed

---

## Test Cases

### Phase A: Direct Injection

```akkado
// Basic import
// -- utils.ak --
fn double(x) -> x * 2

// -- main.ak --
import "utils"
osc("sin", double(220)) |> out(@, @)    // double() available

// Transitive imports (all resolved modules are visible)
// -- a.ak --
fn a_fn(x) -> x + 1

// -- b.ak --
import "a"
fn b_fn(x) -> a_fn(x) * 2

// -- main.ak --
import "b"
b_fn(10)                                 // OK
a_fn(10)                                 // Also OK: a.ak is transitively included

// Shadowing
// -- utils.ak --
fn gain(sig, amt) -> sig * amt

// -- main.ak --
import "utils"
fn gain(sig, amt) -> sig * amt * 0.5     // shadows imported gain
gain(noise(), 0.8)                       // uses local definition

// Circular dependency error
// -- a.ak --
import "b"

// -- b.ak --
import "a"
// ERROR: E500: Circular import detected: a.ak -> b.ak -> a.ak

// Import after code error
osc("sin", 440) |> out(@, @)
import "utils"
// ERROR: E501: Import statements must appear before other code

// Deduplication
// -- a.ak --
import "utils"

// -- b.ak --
import "utils"

// -- main.ak --
import "a"
import "b"
// utils.ak is included only once in the combined source

// Relative imports
import "./local-utils"                   // resolves to ./local-utils.ak
import "../shared/helpers"               // resolves to ../shared/helpers.ak
```

### Phase B: Namespace Imports

```akkado
// Namespace access
import "filters" as f
osc("saw", 440) |> f.lp(@, 800) |> out(@, @)

// Hidden names
import "filters" as f
lp(osc("saw", 440), 800)                // ERROR: undefined 'lp'

// Module-qualified errors
import "filters" as f
f.nonexistent(440)                       // ERROR: module 'filters' has no definition 'nonexistent'

// Mixed direct + namespaced
import "synths"
import "effects" as fx
osc("saw", 440) |> fx.chorus(@, 0.5) |> out(@, @)
```

### Phase C: Web Virtual Filesystem

- WASM: `akkado_register_module("utils", source)` followed by compile with `import "utils"` succeeds
- WASM: `akkado_clear_modules()` followed by compile with `import "utils"` produces error
- Web: multi-tab editor saves/loads modules, compilation includes all registered modules

---

## Error Codes

Error codes E200-E4xx are already used by const evaluation, tap_delay, and poly. Import errors use the E500 range.

| Code | Message | Phase |
|------|---------|-------|
| `E500` | `Circular import detected: {cycle path}` | A |
| `E501` | `Import statements must appear before other code` | A |
| `E502` | `Module not found: '{path}'` | A |
| `E503` | `Failed to read module: '{path}'` | A |
| `E504` | `Module '{alias}' has no definition '{name}'` | B |
| `E505` | `Import requires a file resolver (not available in this context)` | A |

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

---

## Design Principles

This PRD follows the principles from the [Language Evolution Vision](vision-language-evolution.md):

1. **The VM stays minimal** — module resolution is compiler-only. Same opcodes, same instruction format, same bytecode. A program with imports produces identical bytecode to the equivalent single-file program.
2. **Zero-allocation runtime** — imports are resolved at compile time. No runtime module loading, no dynamic linking.
3. **Live-coding ergonomics** — direct injection is the default, matching the current "everything in scope" behavior. Namespaces are opt-in for when organization matters.
4. **Backward compatibility** — existing programs compile identically. The stdlib prepend continues working. Imports are additive syntax.
5. **Progressive migration** — the stdlib can be migrated one file at a time. Each migration step is independently testable and reversible.
