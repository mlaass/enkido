> **Status: NOT STARTED** — Phase 1 of compile-time evaluation. Ships `$print` + four query functions; reserves the `$`-prefix syntax for future user-defined macros.

# PRD: Compile-Time Functions (Phase 1)

**Author:** Moritz Laass + Claude
**Date:** 2026-05-03
**Repo:** `nkido`

**Related:**
- [prd-builtin-variables.md](prd-builtin-variables.md) — defines `bpm`/`sr` as compile-time-known names; this PRD reads them
- [prd-module-import.md](prd-module-import.md) — `import` statement; this PRD's `$imports()` returns its resolved-import list once that lands
- [prd-error-handling-recovery.md](prd-error-handling-recovery.md) — NOT STARTED. The `Diagnostic`/`SourceLocation` types this PRD reuses already exist; that PRD's *multi-stage error collection* is what the "failed compiles surface `compile_log`" guarantee depends on (see §10.5)
- [prd-language-extensions.md](prd-language-extensions.md) — broader language roadmap; macros (Phase 2) will live in their own future PRD
- [prd-runtime-functions-control-flow.md](prd-runtime-functions-control-flow.md) — shipped the sibling `#` annotation sigil (`#inline`); §5.2.1 carves the `#` vs `$` boundary

---

## 1. Executive Summary

Akkado today is a runtime-only language. Every identifier compiles to bytecode; there is no way to inspect what the compiler knows about your patch without launching a debugger or stepping through diagnostic output. This PRD adds a small, opinionated layer of **compile-time functions**: builtins that evaluate during compilation and emit human-readable output associated with source locations.

Phase 1 ships:

1. **`$print(...)` builtin** — accepts an f-string-style template (`"bpm is {bpm:.0f}"`) and emits one entry into a new `compile_log` channel on `CompileResult`.
2. **Four query functions** — `$samples()`, `$env(key)`, `$files(glob?)`, `$imports()` — readable both inside `$print` interpolations and as top-level compile-time bindings.
3. **The `$`-prefix syntax** — reserved for all compile-time calls. Plain identifiers stay runtime-only. Sets up the future macro phase without committing to its semantics.
4. **A `CompileTimeContext` interface** — pluggable host for query implementations. v1 ships a default impl backed by `SampleRegistry`, `FileResolver`, and an optional `cedar::EnvMap` snapshot. Future macros plug into the same ABI.

The output channel is **separate from diagnostics**. CLI hosts route `compile_log` entries to **stdout** and diagnostics to **stderr**. The web IDE adds a new **"Compile Log" tab** in the existing bottom output panel. Both share `SourceLocation` metadata so each entry is click-to-jump in the editor.

### 1.1 Why now

The IDE is mature enough that "what does the compiler see?" is a real question users keep asking implicitly: "are my samples loaded?", "did my env var get applied?", "is `bpm = 140` actually doing anything?". Today the answer is "trust the silence." A compile-time print closes that gap with one builtin, costs almost nothing at runtime, and reuses the existing source-location infrastructure.

This PRD is also a **stepping stone**. Reserving the `$`-prefix and defining `CompileTimeContext` now means the future macro PRD becomes additive — it adds new compile-time builtins (and eventually user-defined macros) without re-litigating the syntax/ABI questions.

### 1.2 Headline design decisions (locked during PRD intake)

- **`$`-prefix is mandatory** for all compile-time *calls*. `$print("hi")`, `$samples()`. Plain names (`print`, `samples`) remain available for runtime — neither shadowed nor reserved. The split is grammatical: `$` always introduces a compile-time **invocation** (args, return value, nesting). It is the sibling of — not the same as — the `#` annotation sigil already shipped for `#inline` (see §5.2.1): `#` *annotates a declaration*; `$` *invokes a compile-time function*. Both are compile-time; they are different categories.
- **`compile_log` is a new field on `CompileResult`**, parallel to (not nested in) `diagnostics`. Each entry carries text + level + source location. Lets hosts render prints separately from errors without conflating severity semantics.
- **F-string interpolation** uses `{name[:spec]}`. Format specs are limited to a numeric subset: `.Nf` (float precision), `d`, `x`, `s`. No width, alignment, fill, or sign. `{{` / `}}` escape literal braces.
- **Anything name-resolvable can interpolate.** Constants render as values. Signals render as typed shape: `<signal mono>` / `<chord[3]>` / `<pattern>`. Undefined names render as `<undefined: name>` and emit a Warning diagnostic; compilation continues.
- **Top-level only** for `$print` statements. Inside function bodies, lambdas, and runtime expressions, `$print` is an error. Query functions (`$samples()`, `$env(...)`) are also top-level-only when used as expressions, valid both inside `$print` interpolations and as compile-time bindings (`names = $samples()`).
- **Pipeline stage: after analysis, before codegen.** ConstEvaluator-resolvable values are folded; the symbol table is fully populated; nothing in `CodeGenResult` exists yet. Source order matches lexical order across imported files (depth-first, when imports land).
- **`compile()` gains an optional `CompileTimeContext*` parameter.** Hosts construct the context with their `SampleRegistry`, `FileResolver`, and (when recompiling a running patch) a snapshot of the live `cedar::EnvMap`.
- **Phase-1 query function set:** `$samples()`, `$env(key)`, `$files(glob?)`, `$imports()`. Each list-returning query has `_lines` and `_table` variants for formatting.
- **Hot-swap-failed compiles still emit prints.** The user sees `$print` output on every successful compile, regardless of whether the bytecode subsequently swapped in. Useful for debugging swap rejection.
- **Failed compiles still surface their `compile_log` — scoped to codegen-stage failures.** When errors short-circuit codegen, any logs emitted before the failure are still shipped to the host. The IDE renders them in the Compile Log tab tagged `(compile failed)`. Useful because logs are often the clue that explains the error. **v1 limitation:** the compile-time pass runs *after* the analyzer, so a compile that fails in lex/parse/analysis produces no AST for the pass and therefore no `compile_log`. Full coverage of pre-codegen failures is blocked on `prd-error-handling-recovery`'s multi-stage error collection (see §10.5).
- **Multi-arg `$print` is comma-separated quick-logging.** `$print("foo", x, y)` renders as `"foo, <x>, <y>"` — first arg is the template, additional args are stringified and joined with `", "` after the template's rendered text. Mirrors `console.log` ergonomics.
- **Web IDE log behavior is configurable.** Settings toggle: "Replace on compile" (default, matches diagnostics) or "Append with timestamps." CLI is always stream-as-emitted on stdout.

---

## 2. Problem Statement

### 2.1 Current state

| Concern | Today |
|---|---|
| Inspect compile-time-known values | None. `bpm = 140` compiles; the user has no way to confirm without listening or attaching a debugger. |
| Discover available samples | Open the docs, or trial-and-error in source. `SampleRegistry` is opaque from inside Akkado. |
| Verify env / runtime state during recompile | None. Hot-swap is blind: you change code, listen, and infer. |
| Discover importable / resolvable files | None. `FileResolver` is opaque. |
| Compile-time expressions | None. Every identifier and call is runtime. `ConstEvaluator` exists but only for folding numeric constants in arithmetic. |
| `$`-prefix syntax | Not lexed. `$` is currently a syntax error. (The sibling `#` annotation sigil — `Hash` token — *is* already lexed, for `#inline`; see §5.2.1.) |
| `compile_log` channel | Doesn't exist. The only compile-side output channel is `diagnostics`, which conflates errors, warnings, and info. |

### 2.2 Proposed state

| Concern | After this PRD |
|---|---|
| Inspect compile-time values | `$print("bpm: {bpm:.0f}, sr: {sr:.0f}")` emits one log entry per compile. |
| Discover samples | `$print("samples: {$samples()}")` joins names with commas. `$samples_table()` for richer rendering. |
| Verify env state | Host passes `EnvMap` snapshot into `compile()`. `$print("cutoff is {$env(\"__cutoff\"):.1f}")` reads the live value at recompile time. |
| Discover files | `$print("loaded: {$files(\"samples/**\"):lines}")` lists URIs known to the resolver, glob-filtered. |
| Compile-time expressions | `names = $samples()` at top level binds a compile-time list. Usable inside other `$print`s; not usable in runtime expressions. |
| `$`-prefix syntax | Lexed as `TOKEN_DOLLAR_IDENT`. Reserved across the language: any unrecognized `$name(...)` is "compile-time function not found" / "macro syntax reserved" error. |
| `compile_log` channel | New `std::vector<LogEntry>` field on `CompileResult`. Hosts route independently from diagnostics. |

---

## 3. Goals and Non-Goals

### 3.1 Goals

1. **Single-line debug-print at compile time.** `$print("foo: {x}")` works in the editor with zero ceremony.
2. **Visibility into the four most-asked compiler-known sets:** sample registry, env snapshot, file resolver, imports.
3. **Source-located output.** Every log entry has file + line + column; web IDE jumps on click.
4. **Lock in `$`-prefix syntax** for compile-time calls so future macros plug in additively.
5. **Pluggable `CompileTimeContext`** ABI so future builtins (and eventually user-defined macros) share one extension point.
6. **No runtime overhead.** Compile-time builtins emit zero bytecode. Output ships in `CompileResult`, not the program.
7. **Same UX in CLI and web.** CLI: stdout vs stderr. Web: dedicated log tab. Both surface source locations.
8. **Hot-swap-friendly.** Logs from successful compiles surface even if the bytecode swap is later rejected.

### 3.2 Non-Goals (hard cuts)

1. **User-defined macros / AST rewriting.** Phase 2 in a separate PRD. This PRD only ships the syntax reservation and ABI hooks.
2. **Runtime printing.** No `print()` builtin that emits during audio playback. The `$` prefix is structural.
3. **Compile-time control flow.** No `$if` / `$for` / `$match`. Future work.
4. **Compile-time macros that generate signal-rate code.** Same as above.
5. **Full Python f-string mini-language.** No alignment, width, fill, sign, or thousands-separator. Just numeric precision + type specifier.
6. **Live re-rendering at host swap time.** Logs are baked at compile end; they don't update per-block.
7. **Suppressing logs at runtime.** Logs always ship in `CompileResult`. If the host wants to filter, it filters at render time.
8. **`$print` to a file or external sink.** v1 only emits into `compile_log`. Hosts decide where it lands.
9. **Importing modules just for their `$print` side effects.** Imported modules CAN emit logs (depth-first source order), but importing them with the goal of running prints isn't an explicit feature.

### 3.3 Success metrics

| Metric | v1 target | Notes |
|---|---|---|
| `$print` adoption | ≥ 30% of demo patches use it within 2 weeks of release | Indicator that the affordance is reachable. |
| Compile-time overhead | < 1 ms added to a typical patch compile | $print + queries are O(N) over their input sets, no codegen impact. |
| Bytecode size impact | 0 bytes | $-prefixed calls emit no instructions. |
| Web IDE jump-to-source success rate | 100% on logs that have a SourceLocation | Reuses existing source map plumbing. |

---

## 4. Target Syntax / User Experience

### 4.1 Hello, compile time

```akkado
$print("hello from compile time")
```

`compile_log`:
```
script.ak:1:1  hello from compile time
```

### 4.2 Inspecting compile-time variables

```akkado
bpm = 140
$print("bpm: {bpm:.0f}, sr: {sr:.0f}")
```

`compile_log`:
```
script.ak:2:1  bpm: 140, sr: 48000
```

### 4.3 Listing samples

```akkado
$print("available samples: {$samples()}")
```

`compile_log`:
```
script.ak:1:1  available samples: bd, kick, sd, snare, hh, hihat, oh, cp, clap, rim, tom, perc, cymbal, crash
```

Or as a table (each query has three forms):

```akkado
$print("samples:\n{$samples_table()}")
```

`compile_log`:
```
script.ak:1:1  samples:
               bd      1   default
               kick    1   default
               sd      2   default
               ...
```

Or one-per-line:

```akkado
$print("samples:\n{$samples_lines()}")
```

### 4.4 Inspecting live runtime state during recompile

```akkado
cutoff = param("cutoff", 1000, 100, 8000)
$print("cutoff right now: {$env(\"__cutoff\"):.1f} Hz")
```

If the patch is recompiled while running and the user has dragged the cutoff slider to 2400.7, the log shows:
```
script.ak:2:1  cutoff right now: 2400.7 Hz
```

When compiled in the CLI (no live VM), `$env(...)` returns no value:
```
script.ak:2:1  cutoff right now: <unset>
```

### 4.5 Discovering files via the resolver

```akkado
$print("drum samples:\n{$files(\"samples/drums/**\"):lines}")
```

`compile_log`:
```
script.ak:1:1  drum samples:
               file://samples/drums/909/kick.wav
               file://samples/drums/909/snare.wav
               ...
```

### 4.6 Compile-time bindings

Query functions are also valid as top-level compile-time expressions:

```akkado
banks = $samples()
$print("loaded {banks:s}")    // 's' specifier formats as a string
```

These bindings live in a compile-time-only scope. They cannot be referenced inside runtime expressions (e.g., `osc("sin", banks)` is an error). They can only be re-printed or fed to other compile-time functions.

### 4.7 Reserved-but-unused (Phase 2 hook)

```akkado
$double(440)         // error E682: macro '$double' is not defined; user-defined macros are not yet supported
```

The `$` lexer token is recognized; the dispatcher rejects unknown `$names` with a clear "future macro syntax" error.

### 4.8 Errors and warnings

```akkado
$print("typo: {bpmm}")
```

`compile_log` (entry still emitted, with `<undefined: ...>` substitution):
```
script.ak:1:1  typo: <undefined: bpmm>
```

`diagnostics` (warning, not an error):
```
script.ak:1:18  W310  $print: identifier 'bpmm' is not in scope; did you mean 'bpm'?
```

Hard errors:

```akkado
$print(42)              // error E683: $print expects a string literal as first argument
$print("{}")            // error E684: empty interpolation
$print("{x:.2z}")       // error E685: unknown format spec '.2z'
foo(x) = { $print("inside fn") }  // error E686: $print is only allowed at top-level
osc("sin", $samples())  // error E687: compile-time call '$samples' cannot be used in a runtime expression
```

---

## 5. Architecture

### 5.1 Component layout

```
┌─────────────────────────────────────────────────────────────────┐
│  akkado/                                                         │
│   include/akkado/                                                │
│   ├── lexer.hpp           +TOKEN_DOLLAR_IDENT                    │
│   ├── parser.hpp          +CompileTimeCall AST node              │
│   ├── ast.hpp             +Node::CompileTimeCall variant         │
│   ├── compile_time.hpp    NEW — CompileTimeContext interface     │
│   │                              + LogEntry / LogLevel           │
│   ├── compile_time_builtins.hpp  NEW — registry of $-prefixed    │
│   │                                     functions                │
│   └── akkado.hpp          CompileResult.compile_log field        │
│   src/                                                           │
│   ├── lexer.cpp           lex `$ident` as one token              │
│   ├── parser.cpp          parse $-call as expression / stmt      │
│   ├── analyzer.cpp        validate $-call placement              │
│   ├── compile_time.cpp    NEW — driver: walks AST, evaluates     │
│   │                              $-calls, populates compile_log  │
│   ├── compile_time_builtins.cpp  NEW — implementations of        │
│   │                              $print, $samples, $env, …       │
│   ├── format.cpp          NEW — f-string interpolation parser    │
│   └── akkado.cpp          wire compile_log into CompileResult    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ optional CompileTimeContext*
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  Hosts                                                           │
│   tools/akkado-cli/   passes a DefaultCompileTimeContext         │
│   tools/nkido-cli/    same; adds EnvMap snapshot when --live     │
│   web/wasm/           passes context with live EnvMap on every   │
│                       recompile                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 The `$`-prefix syntax

#### Lexer

A new token `TOKEN_DOLLAR_IDENT` is produced when the lexer sees `$` immediately followed by an identifier start character (no whitespace between). The lexeme is the `$` plus the identifier (e.g., `"$print"`).

```cpp
// akkado/src/lexer.cpp
case '$': {
    char c = peek();
    if (is_ident_start(c)) {
        consume_identifier();  // sets cursor through the trailing ident
        return make_token(TOKEN_DOLLAR_IDENT);
    }
    return make_error_token("'$' must be immediately followed by an identifier");
}
```

`$ ident` (with whitespace) and bare `$` are syntax errors. Forces a single visual unit.

#### Parser

`$ident(args)` parses as a **CompileTimeCall** AST node:

```cpp
// akkado/include/akkado/ast.hpp
struct CompileTimeCallNode {
    std::string name;            // including the $, e.g. "$print"
    std::vector<NodeIndex> args;
    SourceLocation location;
};
```

Allowed positions in the grammar:

1. **Top-level statement.** `$print("...")` or `$samples()` standing alone as a statement.
2. **Right-hand side of a top-level binding.** `names = $samples()`.
3. **Inside another `$print` interpolation argument expression.** `{$env("k"):.2f}` is parsed as a nested `$env` call inside an interpolation segment (see §5.4).

Disallowed positions:

- Inside any function body or lambda (analyzer error E686).
- Inside any runtime expression (e.g., `osc("sin", $samples())`) — analyzer error E687.
- Inside a pattern, mini-notation string, or chord — same as E687.

The analyzer enforces these rules in `validate_compile_time_calls()`, which runs as part of the analysis pass before codegen.

#### 5.2.1 Relationship to `#` annotations

Akkado already has a second compile-time-only sigil: `#`, shipped with the
`#inline` annotation in `prd-runtime-functions-control-flow.md` (L2). The
lexer's `Hash` token and statement-position annotation parsing are already
in place. This PRD's `$`-prefix is its **sibling, not its replacement** —
the two sigils mark two different grammatical categories, and the split is
deliberate:

| Sigil | Category | Grammar | Examples |
|---|---|---|---|
| `#` | **Annotation** — passive metadata attached to the *following declaration* | Bare prefix; no return value; cannot stand alone, be assigned, or nest | `#inline fn fast_mix(a, b) { ... }` |
| `$` | **Compile-time function** — actively invoked, evaluated, may return a value | Call syntax; statement or expression position; nests; bindable (`names = $samples()`) | `$print("hi")`, `$samples()` |

The one-sentence rule: **`#` annotates a declaration; `$` invokes a
compile-time function.** Both run at compile time and emit zero bytecode,
but they are not interchangeable — `#print(...)` and `$inline fn` are both
ill-formed.

Why two sigils rather than one: unifying them would not simplify the
language, because the two grammars (bare-prefix vs call-syntax) would still
have to coexist. A single sigil would only erase the visual cue that tells
a reader "this modifies the next line" apart from "this runs here." Keeping
`#`/`$` distinct preserves that cue and matches the precedent of every
language that carries both — Rust (`#[inline]` attribute vs `macro!()`),
Nim (`{.inline.}` pragma vs template). `@` is unavailable for either role
(it is the signal-hole token).

**Annotations that take arguments** (none in v1; a hypothetical
`#deprecated("use foo")` is the kind of thing §7.3 of the runtime-functions
PRD anticipates) remain `#`, not `$`: the boundary is "attaches to a
declaration," not "has parentheses." A future parameterized annotation is
still an annotation.

### 5.3 The `compile_log` channel

```cpp
// akkado/include/akkado/compile_time.hpp

enum class LogLevel : std::uint8_t {
    Print,    // $print output
    Note,     // informational from a builtin (e.g., "samples query empty")
    Warning,  // recoverable issue (e.g., undefined identifier in interpolation)
};

struct LogEntry {
    LogLevel level = LogLevel::Print;
    std::string text;             // fully rendered, post-interpolation
    SourceLocation location;      // origin of the $print call
    std::string filename;         // source file (for multi-file imports)
};
```

Added to `CompileResult`:

```cpp
// akkado/include/akkado/akkado.hpp
struct CompileResult {
    // ... existing fields ...
    std::vector<LogEntry> compile_log;
};
```

`compile_log` and `diagnostics` are independent. A compile that emits `$print("hi")` and has zero errors yields `compile_log.size() == 1`, `diagnostics.empty()`, `success == true`. A compile with `$print("typo: {missing}")` yields one `compile_log` entry (text: `"typo: <undefined: missing>"`) and one Warning diagnostic.

### 5.4 F-string interpolation grammar

Format string body (inside `"..."`):

```
fstring     := segment*
segment     := text | escaped_brace | interp
text        := <any char except '{', '}', '\\'>+
escaped_brace := "{{" | "}}"
interp      := "{" expr (":" spec)? "}"
expr        := identifier | compile_time_call | dotted_field
spec        := ("." digit+)? type?
type        := "f" | "d" | "x" | "s"
```

Examples:

- `"plain"` → `"plain"`
- `"a {b} c"` → text + interp + text
- `"x = {x:.2f}"` → text + `{x` with format spec `.2f`
- `"{{literal braces}}"` → `"{literal braces}"`
- `"{$env(\"k\"):.1f}"` → interp where the expr is a nested `$env` call
- `"{rec.freq}"` → interp on a record field access

Format spec rules:

| Spec | Means | Applied to |
|---|---|---|
| (none) | Default `to_string` | Any value |
| `.Nf` | Float, N decimals | Scalar |
| `d` | Integer (truncates) | Scalar |
| `x` | Lowercase hex | Scalar (cast to uint32) |
| `s` | String coercion | Any value (delegates to `to_string`) |

A format spec applied to a non-scalar (e.g., `{$samples():.2f}`) is an error E685.

### 5.5 Interpolation value rendering

```cpp
// akkado/src/format.cpp pseudocode
std::string render(const TypedValue& v, const FormatSpec& fmt) {
    switch (v.kind) {
        case Scalar:   return format_number(v.scalar, fmt);
        case String:   return v.string;
        case Signal:   return fmt::format("<signal {}>", channel_name(v));  // e.g., "<signal mono>"
        case Chord:    return fmt::format("<chord[{}]>", v.chord_size);
        case Pattern:  return "<pattern>";
        case Array:    return fmt::format("<array[{}]>", v.array_size);
        case Record:   return "<record>";
        case List:     return join_compile_time_list(v, ", ");  // for $samples()-style returns
        case Undefined: return fmt::format("<undefined: {}>", v.symbol_name);
    }
}
```

The `List` kind is **compile-time only** — it's produced by query functions and lives only in compile-time scope. Runtime AST cannot construct one.

### 5.6 The `CompileTimeContext` interface

```cpp
// akkado/include/akkado/compile_time.hpp

class CompileTimeContext {
public:
    virtual ~CompileTimeContext() = default;

    // Sample registry — names registered for the current compile.
    virtual std::vector<SampleInfo> list_samples() const = 0;

    // Cedar EnvMap snapshot — read by $env(key). Returns nullopt if no live env.
    virtual std::optional<float> get_env(std::string_view key) const = 0;

    // File resolver — list URIs matching glob (or all, if glob == "").
    virtual std::vector<std::string> list_files(std::string_view glob) const = 0;

    // Imports — list source files imported by current compile (after import lands).
    // v1 returns empty (imports not shipped); the hook is in place.
    virtual std::vector<std::string> list_imports() const = 0;
};

struct SampleInfo {
    std::string name;
    std::uint32_t id;
    std::string bank;   // "default" or named bank
};
```

A default implementation is provided:

```cpp
class DefaultCompileTimeContext : public CompileTimeContext {
public:
    DefaultCompileTimeContext(
        const SampleRegistry* samples = nullptr,
        const FileResolver* resolver = nullptr,
        const cedar::EnvMap* live_env = nullptr,
        const std::vector<std::string>* imports = nullptr);
    // ... impl ...
};
```

`compile()` gains an optional parameter:

```cpp
CompileResult compile(std::string_view source,
                      std::string_view filename = "<input>",
                      SampleRegistry* sample_registry = nullptr,
                      const FileResolver* resolver = nullptr,
                      const CompileTimeContext* compile_ctx = nullptr);
```

If `compile_ctx == nullptr`, a `DefaultCompileTimeContext` is constructed internally from `sample_registry` and `resolver` (no live env, no imports). This keeps existing call sites working unchanged.

### 5.7 Phase-1 builtin set

| Builtin | Args | Returns | Default render in `{...}` |
|---|---|---|---|
| `$print(template, ...)` | string template, then 0+ extras | void | (statement only — emits LogEntry; extras stringified and appended `", a, b"` to rendered template) |
| `$samples()` | none | List of sample names | `"bd, kick, sd, ..."` (comma-joined) |
| `$samples_lines()` | none | string | newline-joined |
| `$samples_table()` | none | string | aligned `name id bank` columns |
| `$env(key)` | string | scalar or undefined | `"2400.7"` or `"<unset>"` |
| `$files(glob?)` | optional string | List of URIs | `"file://..., file://..."` |
| `$files_lines(glob?)` | optional string | string | newline-joined |
| `$files_table(glob?)` | optional string | string | aligned `kind uri` columns |
| `$imports()` | none | List of paths | `"a.ak, b.ak"` (empty until imports lands) |
| `$imports_lines()` | none | string | newline-joined |
| `$imports_table()` | none | string | aligned `path symbols` columns |

`$print`'s second-and-later positional args are **stringified and appended to the rendered template**, comma-separated. `$print("debug", x, y, z)` renders as `"debug, <x>, <y>, <z>"`. Each extra arg is rendered using the same `render(TypedValue, default-spec)` path as interpolations. This is a quick-logging affordance modeled on `console.log` — for formatted output, use `{...}` interpolation in the template instead.

The set is **closed** in v1: any `$name` not in this table is an error E682. Adding a builtin requires this PRD's follow-ups (or the macro PRD).

### 5.8 Pipeline placement

```
source
  │
  ▼
lexer        ← +TOKEN_DOLLAR_IDENT
  │
  ▼
parser       ← +CompileTimeCallNode
  │
  ▼
analyzer     ← +validate $-call placement (top-level only, etc.)
  │            +populate symbol table; ConstEvaluator folds numeric consts.
  │            NOTE: `bpm`/`sr` are NOT folded here — `prd-builtin-variables`
  │            shipped them as `ENV_GET`-backed builtins. The compile-time pass
  │            resolves `{bpm}`/`{sr}` via the BUILTIN_VARIABLES registry +
  │            env snapshot (user `bpm = N` override, else host default).
  │
  ▼
compile_time pass    ← NEW
  │   walks AST in source order, evaluates each $-call:
  │     - $print: parse template, resolve interpolations, emit LogEntry
  │     - query functions: invoke CompileTimeContext, store result in
  │       compile-time symbol scope or feed into surrounding $print
  │   records source-located warnings on undefined interpolations
  │
  ▼
codegen      ← unchanged; $-calls produce no bytecode
  │
  ▼
CompileResult { bytecode, diagnostics, compile_log, ... }
```

A separate pass keeps codegen ignorant of compile-time calls. The pass is no-op (early return) when no `$`-prefixed calls appear in the AST.

### 5.9 Multi-file ordering (post-`import`)

When `import` lands, the compile-time pass walks AST in **depth-first source order**:

```
main.ak
  $print("hi from main")
  import "lib.ak"        ← descend
    lib.ak:
      $print("hi from lib")  ← emitted first? second?
    end of lib
  $print("back in main")
```

Depth-first means imports are walked at their statement position, so the order is:

```
main.ak:1:1   hi from main
lib.ak:1:1    hi from lib
main.ak:3:1   back in main
```

Each `LogEntry.filename` records the originating file so the IDE can route click-to-jump correctly.

---

## 6. UX Specifications

### 6.1 CLI output

`akkado-cli compile script.ak` renders prints to **stdout** and diagnostics to **stderr**. Both are interleaved by source order at the byte level — a single output stream when redirected together, but cleanly separable when piped.

```
$ akkado-cli compile script.ak
script.ak:1:1  hello from compile time         ← stdout
script.ak:5:1  bpm: 140, sr: 48000             ← stdout
script.ak:7:18  W310  $print: identifier 'bpmm' not in scope  ← stderr
```

```
$ akkado-cli compile script.ak 2>/dev/null
script.ak:1:1  hello from compile time
script.ak:5:1  bpm: 140, sr: 48000

$ akkado-cli compile script.ak 1>/dev/null
script.ak:7:18  W310  $print: identifier 'bpmm' not in scope
```

`nkido-cli` follows the same rule when compiling a patch via `--ak`.

The `--quiet` flag (existing) suppresses `compile_log` from stdout but still ships entries in the `CompileResult` JSON output (when JSON mode is on).

### 6.2 Web IDE — Compile Log tab

The bottom output panel currently has a single "Diagnostics" view. After this PRD, it has tabs:

```
┌──────────────────────────────────────────────────────────────────┐
│  Diagnostics (2)  │  Compile Log (3)  │  ← active                │
├──────────────────────────────────────────────────────────────────┤
│  script.ak:1   hello from compile time                           │
│  script.ak:5   bpm: 140, sr: 48000                               │
│  script.ak:7   ⚠ typo: <undefined: bpmm>                         │
└──────────────────────────────────────────────────────────────────┘
```

- Each entry is click-to-jump (uses the existing source-location plumbing).
- Warnings (interpolation problems) are shown inline in the Compile Log tab AND as proper Warning diagnostics in the Diagnostics tab. Same source location.
- Tab badge shows entry count; resets on each compile (or accumulates, per setting).
- Settings panel adds a toggle: **Compile Log behavior: [Replace on compile / Append with timestamps]**. Default: Replace. Persisted in `nkido-settings`.

When **Append** is on, each compile prepends a separator:

```
─── 14:23:11 ───
script.ak:1   hello from compile time
─── 14:23:18 ───
script.ak:1   hello from compile time
script.ak:5   bpm: 140, sr: 48000
```

### 6.3 Web IDE — failed swap behavior

If the bytecode is rejected during hot-swap (e.g., resource constraint, validation failure), the Compile Log entries from that compile **still surface**. The Diagnostics tab gets an entry: "Compile succeeded but swap was rejected: <reason>." The Compile Log entries are tagged with a small `(not running)` chip in the UI. The user can still click-to-jump.

This is critical for debugging swap failures: prints often hold the clue.

### 6.4 Hosts that don't pass a `CompileTimeContext`

When the host doesn't pass a context (or passes one with no live env, no resolver, no samples):

| Builtin | Fallback behavior |
|---|---|
| `$print("...")` | Works. Constants resolve. Signals show typed shape. Warnings unchanged. |
| `$samples()` | Returns empty list. Renders as `""` or `<empty>` (TBD §13). |
| `$env(key)` | Returns `<unset>`. Warning W314 emitted: "no live env; $env returned <unset>". |
| `$files(glob)` | Returns empty. Same warning pattern. |
| `$imports()` | Always empty in v1 (imports not shipped). No warning. |

---

## 7. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar/` (synth engine) | **No change** | This PRD is compile-time only. No new opcodes, no VM changes. |
| `cedar::EnvMap` | **No change** | We snapshot from it; don't write to it. |
| `akkado` lexer | **Modified** | Adds `TOKEN_DOLLAR_IDENT`. |
| `akkado` parser | **Modified** | Adds `CompileTimeCallNode`; recognizes call positions. |
| `akkado` analyzer | **Modified** | New validation pass for $-call placement. |
| `akkado` codegen | **No change** | $-calls produce no bytecode; codegen never sees them (the compile-time pass strips them from AST traversal). |
| `akkado::CompileResult` | **Modified** | New `compile_log` field. |
| `akkado::compile()` signature | **Modified** | New optional `const CompileTimeContext*` param. |
| `akkado::SymbolTable` | **Modified** | Adds compile-time scope for `names = $samples()` bindings. Doesn't leak into runtime scope. |
| `akkado::ConstEvaluator` | **No change** | Reused by the compile-time pass. |
| `akkado::FileResolver` | **No change** | Read-only consumer. |
| `akkado::SampleRegistry` | **No change** | Read-only consumer. |
| `tools/akkado-cli` | **Modified** | Routes `compile_log` → stdout, diagnostics → stderr. |
| `tools/nkido-cli` | **Modified** | Same. Adds `--live-env <file>` flag for snapshotting? (See §13 Open Questions.) |
| `web/wasm/nkido_wasm.cpp` | **Modified** | Adds exports to read `compile_log`; passes live `EnvMap` snapshot into `compile_ctx`. |
| `web/static/worklet/cedar-processor.js` | **Modified** | On recompile, snapshots EnvMap into a host-side struct, passes it to the wasm compile call. |
| `web/src/lib/stores/audio.svelte.ts` | **Modified** | Receives `compile_log` from worklet messages; routes to a new `compileLog` rune-store. |
| `web/src/lib/stores/settings.svelte.ts` | **Modified** | Adds `compileLogBehavior: 'replace' \| 'append'`. |
| `web/src/lib/components/Panel/` | **Modified** | Add `CompileLogPanel.svelte`; update bottom panel tab list. |
| `web/static/docs/reference/language/` | **Modified** | New `compile-time-functions.md` page. F1 docs index regenerated. |

---

## 8. File-Level Changes

### 8.1 New files

| File | Phase | Purpose |
|---|---|---|
| `akkado/include/akkado/compile_time.hpp` | 1 | `CompileTimeContext` interface, `LogEntry`, `LogLevel`, `SampleInfo`, `DefaultCompileTimeContext`. |
| `akkado/include/akkado/compile_time_builtins.hpp` | 1 | Registry table mapping `$name` → handler function pointer. |
| `akkado/src/compile_time.cpp` | 1 | The compile-time pass driver: walks AST, dispatches to handlers, builds `compile_log`. |
| `akkado/src/compile_time_builtins.cpp` | 1 | Implementations of `$print`, `$samples`, `$env`, `$files`, `$imports` and their `_lines` / `_table` variants. |
| `akkado/src/format.cpp` | 1 | F-string template parser + value renderer. |
| `akkado/include/akkado/format.hpp` | 1 | Public types: `FormatTemplate`, `FormatSegment`, `FormatSpec`. |
| `akkado/tests/test_compile_time.cpp` | 1 | Catch2 tests, tag `[compile-time]`. |
| `akkado/tests/test_format.cpp` | 1 | Format-string tests, tag `[format]`. |
| `web/src/lib/stores/compile-log.svelte.ts` | 2 | Svelte 5 rune store: list of `LogEntry`, `clear()`, `append()`, replace/append behavior. |
| `web/src/lib/components/Panel/CompileLogPanel.svelte` | 2 | New tab in bottom output panel. |
| `web/static/docs/reference/language/compile-time-functions.md` | 2 | User-facing docs page. |

### 8.2 Modified files

| File | Phase | Change |
|---|---|---|
| `akkado/include/akkado/lexer.hpp` | 1 | Add `TOKEN_DOLLAR_IDENT`. |
| `akkado/src/lexer.cpp` | 1 | Recognize `$` + ident as one token; reject `$ ident` and bare `$`. |
| `akkado/include/akkado/ast.hpp` | 1 | Add `CompileTimeCallNode`; extend `Node` variant. |
| `akkado/include/akkado/parser.hpp` | 1 | Declare parse paths for `$`-call statements + expressions. |
| `akkado/src/parser.cpp` | 1 | Implement parsing; emit error tokens for misplaced `$`-calls. |
| `akkado/src/analyzer.cpp` | 1 | New `validate_compile_time_calls()` pass; symbol-table compile-time scope. |
| `akkado/include/akkado/akkado.hpp` | 1 | Add `compile_log` to `CompileResult`; add `compile_ctx` parameter to `compile(...)`. |
| `akkado/src/akkado.cpp` | 1 | Wire compile-time pass between analyzer and codegen; populate `compile_log`. |
| `akkado/include/akkado/diagnostics.hpp` | 1 | Add error codes E680–E699 (see §10). |
| `tools/akkado-cli/main.cpp` | 1 | Print `compile_log` entries to stdout. |
| `tools/nkido-cli/main.cpp` | 1 | Same. Optional: `--live-env <file>` flag (TBD). |
| `web/wasm/nkido_wasm.cpp` | 2 | Add `akkado_get_compile_log_count()`, `akkado_get_compile_log_entry(i, fields...)`; accept env snapshot via existing `cedar_set_param` calls before compile. |
| `web/static/worklet/cedar-processor.js` | 2 | Pre-compile: collect EnvMap snapshot keys/values; on `compiled` message: include `compile_log` array. |
| `web/src/lib/stores/audio.svelte.ts` | 2 | On `compiled` message: forward `compile_log` to the new store. |
| `web/src/lib/stores/settings.svelte.ts` | 2 | Add `compileLogBehavior` setting. |
| `web/src/lib/components/Panel/Panel.svelte` | 2 | Add Compile Log tab; badge with entry count. |
| `web/scripts/build-docs.ts` | 2 | Index the new compile-time-functions docs page. |

### 8.3 Files explicitly NOT changed

| File | Why |
|---|---|
| `cedar/**` | No engine changes. |
| `cedar/include/cedar/vm/instruction.hpp` | No new opcodes. |
| `cedar/include/cedar/state/*` | No state changes. |
| `web/wasm/build_debug.sh` / wasm CMake | Builds unchanged; just new exports. |
| `experiments/**` | No DSP-experiment changes. |

---

## 9. Implementation Phases

Each phase ends with a deployable, demo-able artifact.

### Phase 1 — Akkado compile-time pass + CLI integration

**Goal:** A user running `akkado-cli compile script.ak` with `$print` in the source sees the print on stdout.

**Steps:**

1. Add `TOKEN_DOLLAR_IDENT` to lexer; lex `$ident` atomically; error on `$` + whitespace.
2. Add `CompileTimeCallNode` to AST; extend parser to recognize:
   - Top-level statement `$name(args)`.
   - Top-level binding RHS: `name = $name(args)`.
   - Inside string interpolation argument expression (parsed inside the format string at compile-time pass time, not by the main parser).
3. Implement analyzer `validate_compile_time_calls()`:
   - Reject `$`-calls inside function/lambda bodies (E686).
   - Reject `$`-calls inside runtime expressions (E687).
   - Resolve `$name` against `BUILTIN_COMPILE_TIME_FUNCTIONS`; emit E682 for unknowns.
4. Create `compile_time.hpp`/`.cpp` with the `CompileTimeContext` interface and `DefaultCompileTimeContext`.
5. Create `compile_time_builtins.hpp`/`.cpp`:
   - Implement `$print` handler (calls into format.cpp).
   - Implement `$samples`, `$samples_lines`, `$samples_table`.
   - Implement `$env` (returns `<unset>` when no live env).
   - Implement `$files`, `$files_lines`, `$files_table`.
   - Implement `$imports` (returns empty list in v1).
6. Create `format.cpp` with template parser + interpolation evaluator. Supports `{name}`, `{name:.Nf}`, `{name:d}`, `{name:x}`, `{name:s}`, `{{`, `}}`, nested `$call`.
7. Add the compile-time pass driver in `compile_time.cpp`. Walks AST in source order, dispatches each `$`-call to the handler table, builds `compile_log`.
8. Wire into `akkado.cpp::compile()`: run the pass after analyzer, before codegen.
9. Add error codes E680–E699 to `diagnostics.hpp`.
10. Modify `tools/akkado-cli/main.cpp`: route `compile_log` to stdout (line per entry, with `file:line:col  text`).
11. Modify `tools/nkido-cli/main.cpp`: same.

**Verification:**

- `echo '$print("hi")' | akkado-cli compile -` → stdout has `<input>:1:1  hi`, exit 0.
- `echo 'bpm = 140 \n $print("bpm: {bpm:.0f}")' | akkado-cli compile -` → stdout has `<input>:2:1  bpm: 140`.
- `$samples()` → stdout has the registered sample names.
- `$print("typo: {missing}")` → stdout has `typo: <undefined: missing>`, stderr has W310.
- `$print(42)` → stderr has E683, exit nonzero.
- `foo(x) = { $print("nope") }` → stderr has E686.
- `osc("sin", $samples())` → stderr has E687.
- All `[compile-time]` and `[format]` Catch2 tests pass.

### Phase 2 — Web IDE integration

**Goal:** A user with a running web IDE recompiles a patch with `$print("cutoff: {$env(\"__cutoff\"):.1f}")` and sees the live cutoff value in a new "Compile Log" tab.

**Steps:**

1. Extend `nkido_wasm.cpp`:
   - On compile, accept an optional EnvMap snapshot (collected from the worklet just before compile).
   - Construct a `DefaultCompileTimeContext` from the host's `SampleRegistry` and `FileResolver` plus the snapshot.
   - Pass to `akkado::compile()`.
   - Expose new exports: `akkado_get_compile_log_count()`, `akkado_get_compile_log_entry_text(i)`, `akkado_get_compile_log_entry_level(i)`, `akkado_get_compile_log_entry_file(i)`, `akkado_get_compile_log_entry_line(i)`, `akkado_get_compile_log_entry_col(i)`.
2. Modify `cedar-processor.js`:
   - Before each compile, build an EnvMap snapshot (subset of keys, configurable cap to avoid huge payloads — e.g., 256 entries).
   - Send via existing wasm wrappers.
   - Read back `compile_log` and include in the `compiled` postMessage payload.
3. Create `web/src/lib/stores/compile-log.svelte.ts`:
   - Rune-state list of entries.
   - `replace(entries)` and `append(entries)` per setting.
   - `clear()` action.
4. Update `audio.svelte.ts` to forward `compile_log` from `compiled` message to the store.
5. Update `settings.svelte.ts` with `compileLogBehavior: 'replace' | 'append'` (default `'replace'`); persist in localStorage.
6. Create `CompileLogPanel.svelte`:
   - Renders entries; click-to-jump via existing source-location plumbing.
   - Header shows "Replace" / "Append" toggle (mirrors setting); "Clear" button when in append mode.
   - Empty state: "No compile-time output yet — try `$print(\"hi\")`".
7. Update `Panel.svelte` to include the new tab; badge shows entry count.
8. Add `web/static/docs/reference/language/compile-time-functions.md`. Run `bun run build:docs`.

**Verification:**

- IDE shows "Compile Log (0)" tab on first load.
- Type `$print("hi")` + Run → tab shows "Compile Log (1)" with `script.ak:1  hi`.
- Click entry → editor cursor jumps to line 1.
- Type `$print("cutoff: {$env(\"__cutoff\"):.1f}")` with a `cutoff = param(...)` declaration → tab shows live value; drag the slider, recompile, log updates.
- Settings: switch to Append → next compile prepends a timestamp separator; old entries remain.
- Hot-swap rejection (force one by exceeding bytecode size limits): Compile Log entries still visible, tagged `(not running)`. Diagnostics tab shows the swap-rejection error.

### Phase 3 — Documentation + polish

**Goal:** A user who has never seen this feature can discover it from the docs and use it productively.

**Steps:**

1. Flesh out `compile-time-functions.md`: motivation, syntax, all five builtins with examples, format spec reference, edge-cases, limitations.
2. Add a brief tutorial section to `web/static/docs/tutorials/`: "Debugging your patch with `$print`".
3. Add F1 help keywords for `$print`, `$samples`, `$env`, `$files`, `$imports`. Run `bun run build:docs`.
4. Update CLAUDE.md (project root): add `$`-prefix syntax to "Akkado Language Concepts" section.
5. Add a `bpm/sr` discovery example to the default editor sample code: `$print("ready: bpm={bpm:.0f}, sr={sr:.0f}")` so first-time users see compile output immediately.

**Verification:**

- Docs page lints with the existing markdown linter.
- F1 on each `$builtin` opens the right anchor.
- Default editor patch on a fresh load shows one Compile Log entry on first compile.

---

## 10. Edge Cases

### 10.1 Lexer / parser

- **`$` followed by whitespace:** lexer error: `'$' must be immediately followed by an identifier`.
- **`$` followed by digit:** lexer error: `'$' must be followed by an identifier (got '0')`.
- **`$$name`:** lexer error: `'$' must be followed by an identifier (got '$')`. (No `$$` operator in v1.)
- **`$reserved_keyword` (e.g., `$let`, `$if`):** parse error E682: `unknown compile-time function '$let'`. Keywords are not exempt.
- **`$print(` then EOF:** standard parser error: unterminated argument list. Same path as `foo(`.

### 10.2 Format strings

- **`"a {b"`:** error E684: unterminated interpolation segment.
- **`"a } b"`:** error E684: stray `}` (not `}}`).
- **`"a {} b"`:** error E684: empty interpolation.
- **`"a {b:}"`:** error E684: empty format spec after `:`.
- **`"a {b:.}"`:** error E685: invalid precision specifier.
- **`"a {b:.5z}"`:** error E685: unknown format type `'z'`.
- **`"a {b:.999f}"`:** clamped to `:.20f` (max precision); warning W310 about clamping.
- **`"a {{not interp}} b"`:** renders as `"a {not interp} b"`.
- **`"a {bpm:.2f}{sr:.0f}"`:** adjacent interpolations work; no separator inserted.
- **Interpolation expression contains `}`:** must be escaped or quoted. v1 doesn't allow `}` inside the expr (parser bails at first `}`).
- **Nested `$call` with quotes:** `"{$env(\"k\"):.1f}"` works; the `\"` is in source. Inside the parsed string body, the expr parser sees `$env("k"):.1f`.

### 10.3 Interpolation values

- **Identifier resolves to a record field:** `{rec.freq}` works if `rec` is a compile-time record literal. If `rec` is a runtime binding, it renders as `<signal mono>` or similar — the field access doesn't drill into runtime values.
- **Identifier resolves to a function:** renders as `<function: name>` and emits W311 (unusual usage).
- **Identifier resolves to a builtin (non-`$`):** same as function — `<builtin: osc>`.
- **Identifier resolves to a chord literal `C4'`:** `<chord[3]>` (size from compile-time chord expansion).
- **Identifier shadowed by both compile-time and runtime binding:** compile-time wins. Warning W312: shadow note.
- **Format spec on a non-scalar:** error E685: `'.2f' applied to <signal mono>`.
- **`$env(key)` with key not in EnvMap:** renders `<unset>`. No warning if the key starts with `__` (Cedar convention for internal); warning W314 otherwise.

### 10.4 Placement

- **`$print` inside a function body:** E686.
- **`$print` inside a lambda:** E686 (same code).
- **`$print` inside an `if` arm:** E686 — though if/else doesn't exist as an Akkado runtime construct yet, this is forward-compatible.
- **`$print` at the end of a chained `|>`:** E687 — the pipe is a runtime expression.
- **`$samples()` inside `osc("sin", ...)`:** E687.
- **`names = $samples()` followed by `osc("sin", names)`:** E687 on the `names` reference (typed as compile-time list, not signal).
- **`names = $samples()` followed by `$print("{names}")`:** works.
- **`x = 1 + 2` (runtime const) then `$print("{x}")`:** works — `ConstEvaluator` folds it; the compile-time pass sees the folded value.
- **`x = osc("sin", 440)` then `$print("{x}")`:** renders `<signal mono>`; no error.

### 10.5 Multiple compiles / hot-swap

- **Two consecutive identical compiles:** each emits the same `compile_log`. With "Replace" mode, only the latest is shown. With "Append" mode, both appear with timestamps.
- **Compile succeeds but swap is rejected:** `compile_log` still surfaces. Tab badge updates. Entries tagged `(not running)`.
- **Compile fails (errors):** `compile_log` is **still surfaced** by hosts. The compile-time pass runs to completion (collecting logs) before short-circuiting on errors. The IDE renders entries with a `(compile failed)` tag at the top of the panel; CLI prints them to stdout normally and the diagnostics to stderr. Logs are often the fastest path to understanding the error.

  > **Dependency note (resolved on review — v1 scope).** This guarantee is
  > deliverable only for failures **at or after the compile-time pass** —
  > i.e. codegen-stage errors. The compile-time pass runs *after* the
  > analyzer, and `prd-error-handling-recovery`'s **multi-stage error
  > collection** is NOT STARTED: today a lex error blocks parsing and a
  > parse error blocks semantic analysis, so a compile that fails before
  > the analyzer completes produces *no AST for the pass to walk* and
  > therefore no `compile_log`. **v1 decision: scope the guarantee to
  > codegen-stage failures.** A compile that reaches codegen and fails there
  > surfaces its logs; a compile that fails earlier surfaces none. Full
  > coverage of pre-codegen failures is explicitly out of v1 scope and
  > blocked on `prd-error-handling-recovery` shipping multi-stage
  > collection. (The alternative — a self-contained partial-AST tweak in
  > this PRD — was considered and rejected as scope creep for v1.)
- **Multiple `$print`s in source, second one panics during interpolation:** partial logs preserved; the offending entry is replaced with `<error: rendering failed>` and a corresponding diagnostic.
- **Recompile spam (every keystroke):** Replace mode keeps tab tidy. Append mode users will see flooded log; they can `Clear`.

### 10.6 EnvMap snapshot

- **Snapshot captured but env is empty:** `$env(key)` returns `<unset>` for everything.
- **Snapshot captured but specific key missing:** same.
- **Snapshot capture fails (e.g., worklet → main thread message size cap):** worklet logs to `console.warn`; sends compile request without snapshot; `$env` returns `<unset>` everywhere.
- **EnvMap size > some cap (256?):** worklet truncates to most-recently-touched 256 keys; warning W315 in `compile_log` (auto-emitted by host, not user-triggered).
- **Race: snapshot captured at t0, compile runs at t1 with t1 > t0 + 100ms:** the snapshot is point-in-time; user sees the t0 value. Acceptable; recompiling will refresh it.

### 10.7 Multi-file (post-`import`)

- **Imported file has `$print`:** emitted in source order, with imported file's `filename` in the `LogEntry`. IDE click-to-jump opens the imported file.
- **Imported file has `$samples()` referencing the parent's registry:** works — `CompileTimeContext` is shared across all files in the compile.
- **Diamond import (A imports B and C; B and C both import D):** D is compiled once; D's `$print`s emit once.
- **Cyclic import:** existing import resolver rejects this with its own error before compile-time pass runs. No special handling here.

### 10.8 Empty / boundary cases

- **`$print("")`:** emits an empty-text LogEntry. Hosts may or may not render it; v1 renders an empty line.
- **`$samples()` with no registered samples:** returns empty list. Renders as `""` in `$print("{$samples()}")`. Open question §13: should this be `<empty>` instead?
- **`$files("")`:** same as `$files()` — returns all files (no glob filter).
- **`$files("nomatch/*")`:** returns empty list.
- **A file with only `$print` and nothing else:** compiles to zero bytecode but `success == true`. Hosts handle gracefully (no audio output, but log is shown).

---

## 11. Testing Strategy

### 11.1 Akkado unit tests (`akkado/tests/test_compile_time.cpp`, tag `[compile-time]`)

| Test | Expected |
|---|---|
| `$print("hi")` | `compile_log` has 1 entry; text == `"hi"`; level == Print; location at `1:1` |
| `$print("a {b} c")` with `b = 5` | text == `"a 5 c"` |
| `$print("a {bpm:.0f}")` with `bpm = 140` | text == `"a 140"` |
| `$print("a {bpm:.2f}")` | text == `"a 140.00"` |
| `$print("a {n:d}")` with `n = 3.7` | text == `"a 3"` |
| `$print("a {n:x}")` with `n = 255` | text == `"a ff"` |
| `$print("a {missing}")` | text == `"a <undefined: missing>"`; one Warning W310 |
| `$print("a {sig}")` with `sig = osc("sin", 440)` | text == `"a <signal mono>"` |
| `$print("{$samples()}")` with default registry | text contains `"bd, kick, sd, snare, hh, ..."` |
| `$samples()` (statement form) | not allowed as a statement (no value to do anything with); warning W313 |
| `names = $samples()` then `$print("{names}")` | works; text == joined names |
| `$env("__bpm")` with snapshot `{"__bpm": 130.0}` | renders `"130"` (default precision) |
| `$env("missing")` no snapshot | renders `"<unset>"` |
| `$print("{{literal}}")` | text == `"{literal}"` |
| `$print(42)` | error E683 (first arg must be a string template) |
| `$print("debug", 5, "x")` | text == `"debug, 5, x"` (comma-append behavior) |
| `$print("a {b}", 99)` with `b = 1` | text == `"a 1, 99"` (template renders, then append) |
| `$print("{}")` | error E684 |
| `$print("{x:.5z}")` | error E685 |
| `foo(x) = { $print("inside") }` | error E686 |
| `osc("sin", $samples())` | error E687 |
| `$nonexistent()` | error E682 |
| `$ print("hi")` (whitespace after $) | lexer error |
| `$print("a") $print("b")` | two log entries in source order |
| `$print` from imports (when imports land) | preserves depth-first source order |

### 11.2 Format-string unit tests (`akkado/tests/test_format.cpp`, tag `[format]`)

Targeted at the format parser/evaluator in isolation. Same coverage as 11.1 but at the format-template granularity, without involving the AST.

### 11.3 CLI integration tests

Bash scripts in `akkado/tests/cli/`:

- `test_print_stdout.sh`: `echo '$print("hi")' | akkado-cli compile -` produces `<input>:1:1  hi` on stdout, nothing on stderr.
- `test_print_with_warning.sh`: `echo '$print("{x}")'` produces stdout entry + stderr warning.
- `test_pipe_separation.sh`: stdout and stderr separable (test with `2>/dev/null`).

### 11.4 Web integration tests (manual, per Phase 2 verification list in §9)

No Playwright in v1; manual checklist per Phase 2.

### 11.5 Hot-path performance check

Catch2 `[compile-time][bench]` tag:

- Source with **100 `$print` calls**, each interpolating 4 values → compile-time pass adds < 5 ms total.
- Source with **zero `$`-calls** → pass is no-op; < 50 µs added.
- Source with **`$samples_table()` over 200 samples** → < 1 ms.

### 11.6 Adversarial

- 10 KB single `$print` template → renders correctly; no parser stack issues.
- `$print` template with 1000 interpolations → renders correctly.
- Recursive interpolation (a name resolves to a string containing `{...}`) → does NOT recurse; `{...}` in the resolved value is rendered literally. Test asserts this.
- Snapshot of EnvMap with 10,000 keys → host-side cap (256) kicks in; warning emitted; compile succeeds.

---

## 12. Operational Readiness

| Concern | v1 handling |
|---|---|
| Backward compat | `compile()` signature gains an **optional** parameter with a default (`nullptr`). All existing call sites remain valid. |
| ABI stability for `CompileTimeContext` | This PRD is the first version. Future PRDs may extend the interface but not modify existing methods. |
| WASM bundle size impact | New exports + `compile_time.cpp` + `format.cpp` → estimated +6 KB gzipped. Acceptable. |
| LSP integration | LSP reads `diagnostics` already; the new `compile_log` is exposed via a parallel JSON-LSP extension method (out of scope for this PRD; future LSP work). |
| Telemetry | None. |
| Localization | All log text is user-authored (`$print` content). Format specifiers are stable. No translation needed. |

---

## 13. Open Questions

- **[OPEN QUESTION] `$samples()` empty rendering.** Today: empty list renders as `""`. Should we render `<empty>` to make it visible? Lean toward `<empty>` for clarity, but it's a minor UX call.
- **[OPEN QUESTION] CLI `--live-env` flag.** `nkido-cli` could accept a JSON file with EnvMap key/value pairs to populate the snapshot for offline `$env` testing. Useful for testing patches that rely on host-set env. Defer if low demand.
- **[OPEN QUESTION] Positional `{0}`/`{1}` interpolation.** v1's multi-arg behavior is "comma-append after the template." Future PRD may add `{0}`, `{1}` positional refs that pull from those args. Forward-compatible: a template with no `{N}` references keeps today's append behavior; a template that uses `{0}` skips the append for that index.
- **[OPEN QUESTION] Nested `$call` arity.** `{$env("k")}` is one call. Are deeper nests (`{$env($name())}`) allowed? v1 says yes, parser supports it; tests should cover at least one level. Document the recursion limit (e.g., 8) explicitly.
- **[OPEN QUESTION] `compile_log` for failed compiles.** §10.5 says hosts discard logs from failed compiles. Should the IDE surface them anyway, tagged `(broken compile)`? Lean toward "hide by default; debug-mode toggle" — defer to a Phase 3 polish PR.
- **[RESOLVED on review] Failed-compile guarantee scope.** `prd-error-handling-recovery` (multi-stage error collection) is NOT STARTED. v1 scopes the "failed compiles surface `compile_log`" guarantee to **codegen-stage failures only** — the compile-time pass needs a complete AST, so lex/parse/analysis failures surface no log. Full coverage is out of v1 scope and blocked on `prd-error-handling-recovery`. See the §10.5 dependency note.
- **[RESOLVED on review] Error/warning code ranges.** The PRD originally drafted onto the 180s, which are now fully occupied by shipped features. Rebased on review onto verified-free ranges: errors `E680–E699`, warnings `W310–W319`. All references throughout the PRD were updated in lockstep.
- **[OPEN QUESTION] Timestamp granularity in Append mode.** HH:MM:SS or HH:MM:SS.mmm? Milliseconds add noise but help diagnose recompile spam. Lean HH:MM:SS for v1.
- **[OPEN QUESTION] Source-order interleaving in CLI.** When stdout and stderr are merged in a terminal, ordering depends on libc buffering. Should we explicitly flush after each entry? Lean yes — `fflush(stdout)` and `fflush(stderr)` after every emission.

---

## 14. Future Work

Out of v1 scope, but anticipated and worth flagging so v1 doesn't paint future PRDs into a corner:

- **Phase 2: User-defined macros.** `$double(x) = x * 2` defined in source; `$double(440)` rewrites to `880` at compile time. Plugs into the `CompileTimeContext` ABI defined here. Separate PRD.
- **Phase 2: Compile-time control flow.** `$if(condition) { ... } else { ... }`, `$for(s in $samples()) { ... }`. Lets users generate signal-rate code from compile-time iteration.
- **Phase 3: AST-rewriting macros.** Macros that take AST nodes and return AST nodes; hygiene; macro expansion stages.
- **More query functions.** `$bpm_estimate(pattern)`, `$keys()`, `$tuning()`, `$voicings()`, `$bytecode_size()`, etc. Each adds an entry to `BUILTIN_COMPILE_TIME_FUNCTIONS`.
- **Format spec: alignment / width.** `{name:>20}`, `{n:08.2f}`. Adds a parser branch; useful for table output.
- **Format spec: positional args.** `$print("{0} {1}", a, b)` for templating cleanliness.
- **`$assert(...)` builtin.** Compile-time invariants: `$assert($samples().has("bd"), "kit must have bd")`. Promotes a failed assert to an error.
- **JSON output mode for `compile_log`.** `--json` flag in CLI emits a JSON array; LSP consumes it.
- **Live re-render.** Per-block re-evaluation of `$print` templates against the running EnvMap. Heavy; deferred until a real use case appears.
- **`compile_log` filtering / search in IDE.** When logs grow long, add a search bar.

---

## 15. Cross-PRD Dependencies

| PRD | Status | Dependency |
|---|---|---|
| `prd-builtin-variables.md` | SHIPPED | `bpm`/`sr` shipped as **`ENV_GET`-backed builtin variables** — codegen desugars the identifier to `ENV_GET`, they are *not* plain const-folded identifiers. The compile-time pass must resolve `{bpm}`/`{sr}` interpolations through the `BUILTIN_VARIABLES` registry + env-snapshot path (the same channel `$env` uses): read the user's constant `bpm = N` override when present, the host default otherwise. See §5.8. |
| `prd-module-import.md` | READY FOR IMPLEMENTATION | Not yet shipped. `$imports()` returns its resolved-import list when this lands; v1 of this PRD ships an empty stub regardless. |
| `prd-error-handling-recovery.md` | **NOT STARTED** | The basic `Diagnostic` / `SourceLocation` types this PRD reuses already exist independently of that PRD. But its **multi-stage error collection** (lex+parse+semantic errors collected instead of aborting between stages) has *not* shipped — today the pipeline still aborts between stages. This bounds the "failed compiles still surface `compile_log`" guarantee to codegen-stage failures (§1.2, §10.5): see the dependency note in §10.5. Error/warning codes were rebased on review onto verified-free ranges `E680–E699` / `W310–W319` (the 180s were occupied). |
| `prd-language-extensions.md` | COMPLETE | Macros (Phase 2 of this PRD's lineage) live in a separate future PRD. |
| `prd-shareable-patches.md` | CODE-COMPLETE | Independent. Compile-time logs should NOT be embedded into shared patches (shares carry source code; the log is host-side, regenerated on recompile). |
| `prd-runtime-functions-control-flow.md` | L2 SHIPPED | Shipped the sibling `#` annotation sigil (`#inline`) and its `Hash` lexer token. This PRD's `$`-prefix must coexist with it as a distinct category — see §5.2.1. No code coordination needed; the two sigils are lexically independent. |

---

## 16. Glossary

| Term | Meaning |
|---|---|
| **Compile-time function** | A `$`-prefixed builtin evaluated during compilation, not at runtime. Emits zero bytecode. |
| **`$`-prefix** | Mandatory marker for compile-time *calls*. `$print(...)`, `$samples()`. Distinct from runtime, and distinct from the `#` annotation sigil — see §5.2.1. |
| **`#`-prefix (annotation)** | Compile-time annotation sigil, shipped with `#inline` (`prd-runtime-functions-control-flow.md`). Attaches passive metadata to the following declaration; no call syntax, no return value. Sibling of `$`, not a synonym. |
| **`compile_log`** | Field on `CompileResult` carrying `LogEntry`s. Independent of `diagnostics`. |
| **`LogEntry`** | One entry: text + level + source location + filename. |
| **F-string interpolation** | `{name[:spec]}` syntax inside `$print` templates. Supports `.Nf`, `d`, `x`, `s` format specs. |
| **`CompileTimeContext`** | Pluggable host interface providing samples, env snapshot, files, imports. v1's default impl + future macros plug into it. |
| **EnvMap snapshot** | Point-in-time copy of `cedar::EnvMap` passed to `compile()` for `$env` evaluation. |
| **Compile-time scope** | Symbol scope holding `name = $samples()` style bindings. Doesn't leak into runtime expressions. |
| **Phase 1 builtins** | `$print`, `$samples`, `$samples_lines`, `$samples_table`, `$env`, `$files`, `$files_lines`, `$files_table`, `$imports`, `$imports_lines`, `$imports_table`. |
