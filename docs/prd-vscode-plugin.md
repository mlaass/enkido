> **Status: DONE (v0.1.0)** — VS Code extension shipped in sibling repo [`nkido-vscode`](https://github.com/mlaass/nkido-vscode). Packaged as `nkido-0.1.0.vsix` with TextMate grammar, language config, autocomplete/diagnostics/signature-help providers, command palette integration, and `nkido serve` integration. The required `serve` mode now exists in this repo.

# PRD: VS Code Extension for Nkido/Akkado

**Date:** 2026-05-01

---

## 1. Overview

### 1.1 Problem Statement

Users who prefer VS Code as their primary editor cannot live code Akkado with the same experience as the web IDE. They must switch contexts between VS Code (for editing) and the web IDE or CLI (for audio playback), breaking workflow continuity. The web IDE has rich features (syntax highlighting, autocomplete, diagnostics) that are unavailable in VS Code.

### 1.2 Proposed Solution

Create a VS Code extension that brings the full web IDE experience to VS Code:
- Syntax highlighting matching the web IDE's CodeMirror language mode (via TextMate grammar)
- Autocomplete with function signatures (via VS Code Direct API)
- Real-time diagnostics with inline squiggly underlines and Problems panel
- Signature help tooltips when typing function calls
- Integration with `nkido` for compilation and playback (per-command for `check`/`compile`, persistent `serve` mode for live hot-swap)
- User-configurable keyboard shortcuts (defaulting to web IDE conventions: Ctrl+Enter to evaluate, Escape to stop)

### 1.3 Goals (v1)

- **Syntax highlighting**: Akkado language support with colors matching the web IDE (TextMate grammar)
- **Autocomplete**: Builtin functions, aliases, keywords, user-defined variables/functions (VS Code Direct API)
- **Diagnostics**: Compilation errors shown as squiggly underlines + Problems panel (VS Code Direct API)
- **Signature help**: Parameter tooltips when typing function calls (VS Code Direct API)
- **CLI integration**: Play/stop/compile via `nkido` (spawn-per-command for `check`/`compile`, persistent `serve` mode for hot-swap)
- **Configurable shortcuts**: User can customize keyboard bindings, defaults match web IDE
- **Dedicated output channel**: "Nkido" channel showing compilation status, errors, playback state

### 1.4 Non-Goals (v1)

- **Inline visualizations**: No waveforms, frequency responses, or pattern timelines in the editor
- **Parameter controls UI**: No sliders, knobs, or buttons for `param()`/`toggle()`/`button()`
- **Debug panels**: No state inspector, pattern debugger, or instruction highlighting
- **Multi-file support**: Single file only, no module imports
- **WASM integration**: No embedded WASM compiler — always use `nkido` as the backend

---

## 2. User Experience

### 2.1 Basic Workflow

1. User opens `.akkado` or `.akk` file in VS Code
2. Extension activates, providing syntax highlighting
3. User writes Akkado code with autocomplete assistance
4. User presses `Ctrl+Enter` (configurable) to compile and play
5. `nkido` is spawned, compilation errors appear as squiggly underlines
6. On success, audio plays; status shown in Nkido output channel
7. User presses `Escape` (configurable) to stop playback

### 2.2 Keyboard Shortcuts (Default)

| Shortcut | Action | Notes |
|----------|--------|-------|
| `Ctrl+Enter` (Cmd on Mac) | Evaluate/Play | Compiles current file, starts playback |
| `Escape` | Stop | Stops audio playback |
| `Ctrl+Space` | Trigger autocomplete | Standard VS Code shortcut |
| `Ctrl+Shift+Space` | Signature help | Shows function signature tooltip |
| `F1` then type "Nkido" | Access commands | Command palette integration |

All shortcuts are user-configurable via VS Code's keyboard shortcuts editor.

### 2.3 Autocomplete Example

```
User types: l
Popup shows:
  lp(in, cut, q?)        - State-variable lowpass filter
  limiter(in, ceiling?, ...) - Peak limiter
  lfo(rate, duty?)        - Low frequency oscillator
  lowpass → lp            - Alias redirect
  ...

User selects lp, inserts: lp()
Cursor placed inside parentheses, signature help appears: lp(in, cut, q?)
```

### 2.4 Error Display Example

When compilation fails:
- Red squiggly underline on the offending line
- Hover tooltip shows error message with line/column
- Problems panel lists all errors with severity
- Nkido output channel shows full compilation log

### 2.5 CLI Integration Modes

**Spawn-per-command mode** (simple operations, available today):
- `nkido check file.akkado --json` — Syntax check (JSON diagnostics)
- `nkido compile -o out.cedar file.akkado` — Compile to bytecode
- `nkido play file.akkado` — Play once and exit

**Persistent hot-swap mode** (live coding):
- Today, `nkido ui` is a self-contained SDL2 editor with no external command channel — it only consumes SDL keyboard/text events.
- For VS Code v1, `nkido` must grow a headless persistent mode (working name: `nkido serve`) that reads newline-delimited JSON commands from stdin (`{"cmd": "load", "source": "..."}`, `{"cmd": "stop"}`) and emits status/diagnostics to stdout.
- The extension launches one `serve` process per workspace, sends source on Ctrl+Enter for hot-swap, sends stop on Escape, and kills the process on deactivation.
- See section 11 (Impact Assessment) for the prerequisite work in `tools/nkido`.

---

## 3. Architecture

### 3.1 Repository Structure

```
~/workspace/nkido-vscode/                    # Standalone repository
├── .vscode/
│   ├── launch.json
│   └── tasks.json
├── src/
│   ├── extension.ts                          # Extension entry point
│   ├── providers/
│   │   ├── completion-provider.ts            # Autocomplete logic (Direct API)
│   │   ├── diagnostic-provider.ts           # Error reporting (Direct API)
│   │   ├── signature-help.ts                # Parameter tooltips (Direct API)
│   │   └── document-symbols.ts             # Document symbols (optional)
│   ├── syntax/
│   │   ├── akkado.tmLanguage.json          # TextMate grammar
│   │   └── grammar-generator.ts            # Generates grammar from web IDE tokens
│   ├── cli/
│   │   ├── cli-client.ts                   # nkido wrapper (spawn-per-command)
│   │   ├── serve-mode-manager.ts           # Persistent serve-mode process (JSON over stdio)
│   │   └── audio-engine.ts                 # Playback state management
│   ├── commands/
│   │   ├── evaluate.ts                     # Ctrl+Enter handler
│   │   ├── stop.ts                        # Escape handler
│   │   ├── compile.ts                     # Compile command
│   │   └── show-output.ts                 # Focus output channel
│   └── utils/
│       ├── config.ts                       # Extension settings
│       └── logger.ts                      # Output channel logging
├── syntaxes/
│   └── akkado.tmLanguage.json             # Generated TextMate grammar
├── icons/
│   ├── akkado-light.svg                   # File icon (light theme) — human-delivered
│   └── akkado-dark.svg                    # File icon (dark theme) — human-delivered
├── language-configuration.json
├── package.json
├── tsconfig.json
├── vsc-extension-quickstart.md
└── README.md
```

### 3.2 System Overview

```
┌─────────────────────────────────────────────────────────────┐
│                      VS Code                                │
│                                                              │
│  ┌──────────────┐    ┌─────────────────┐    ┌───────────┐  │
│  │   Editor     │◄──►│ Extension Host  │◄──►│  Output   │  │
│  │  (TextMate   │    │ (Node.js)        │    │  Channel  │  │
│  │   Grammar)   │    │                 │    │  "Nkido"  │  │
│  └──────┬───────┘    └────────┬────────┘    └───────────┘  │
│         │                      │                                │
│         │ VS Code API         │ spawns/communicates           │
│         ▼                      ▼                                │
│  ┌─────────────────────────────────────────────┐              │
│  │  - extension.ts (activation, commands)      │              │
│  │  - providers/*.ts (completion, diagnostics) │              │
│  │  - cli-client.ts (nkido wrapper)       │              │
│  │  - serve-mode-manager.ts (persistent JSON) │              │
│  └──────────────────────┬──────────────────────┘              │
└─────────────────────────┼─────────────────────────────────────┘
                          │ spawns
                          ▼
┌─────────────────────────────────────────────┐
│            nkido (C++ binary)           │
│                                             │
│  Modes:                                     │
│  - check: Syntax check (spawn, exit)        │
│  - compile: Compile to bytecode (spawn)     │
│  - play: Play and exit (spawn)              │
│  - serve: Headless JSON-over-stdio mode     │
│           (NEW — prerequisite, see §11)     │
│  - ui: SDL2 standalone editor (unchanged,   │
│        not used by the VS Code extension)   │
└─────────────────────────────────────────────┘
```

### 3.3 Extension Architecture (Direct API)

The extension uses VS Code's Direct API for completions, diagnostics, and signature help. No separate LSP server process needed.

```typescript
// extension.ts - registers providers directly
import * as vscode from 'vscode';
import { CompletionProvider } from './providers/completion-provider';
import { DiagnosticProvider } from './providers/diagnostic-provider';
import { SignatureHelpProvider } from './providers/signature-help';

export function activate(context: vscode.ExtensionContext) {
    const completionProvider = new CompletionProvider();
    const diagnosticProvider = new DiagnosticProvider();
    const signatureHelpProvider = new SignatureHelpProvider();
    
    // Register providers using VS Code API
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            'akkado', completionProvider, '.', '"'  // trigger chars
        ),
        vscode.languages.registerSignatureHelpProvider(
            'akkado', signatureHelpProvider, '(', ','
        )
    );
}
```

### 3.4 TextMate Grammar

Syntax highlighting uses a TextMate grammar (`akkado.tmLanguage.json`) ported from the web IDE's CodeMirror tokenizer (`akkado-language.ts`).

Key tokens to highlight:
- **Keywords**: `true`, `false`, `fn`, `as`, `match`, `post`
- **Builtins**: `osc`, `lp`, `adsr`, `delay`, etc. (90+ functions)
- **Operators**: `|>`, `->`, `==`, `!=`, `+`, `-`, `*`, `/`, `^`, `@`
- **Strings**: Double-quoted strings with escape support
- **Numbers**: Integers, floats, scientific notation
- **Comments**: `//` line comments
- **Special**: `$directive`, `~rest`, chord notation (`C4'`)

---

## 4. API Reference

### 4.1 Extension Settings

Configurable via VS Code settings (`settings.json`):

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `nkido.cliPath` | string | `""` (search PATH) | Path to `nkido` executable |
| `nkido.shortcuts.evaluate` | string | `Ctrl+Enter` | Keyboard shortcut for evaluate/play |
| `nkido.shortcuts.stop` | string | `Escape` | Keyboard shortcut for stop |
| `nkido.autoCompileOnSave` | boolean | `false` | Auto-compile on save |
| `nkido.audio.sampleRate` | number | `48000` | Audio sample rate |
| `nkido.audio.bufferSize` | number | `128` | Audio buffer size |
| `nkido.diagnostics.enabled` | boolean | `true` | Show squiggly underlines |
| `nkido.outputChannel.enabled` | boolean | `true` | Enable Nkido output channel |

### 4.2 Commands (Command Palette)

| Command | ID | Description |
|---------|-----|-------------|
| `Nkido: Evaluate/Play` | `nkido.evaluate` | Compile and play current file |
| `Nkido: Stop` | `nkido.stop` | Stop playback |
| `Nkido: Compile` | `nkido.compile` | Compile to bytecode without playing |
| `Nkido: Check Syntax` | `nkido.check` | Syntax check only |
| `Nkido: Show Output` | `nkido.showOutput` | Focus Nkido output channel |
| `Nkido: Restart CLI` | `nkido.restartCli` | Restart `nkido serve` process |

### 4.3 VS Code API Features

| Feature | API | Status |
|---------|-----|--------|
| Autocomplete | `registerCompletionItemProvider` | ✅ v1 |
| Signature help | `registerSignatureHelpProvider` | ✅ v1 |
| Diagnostics | `createDiagnosticCollection` | ✅ v1 |
| Document symbols | `registerDocumentSymbolProvider` | ⏳ future |
| Hover info | `registerHoverProvider` | ⏳ future |
| Go to definition | `registerDefinitionProvider` | ⏳ future |

---

## 5. Implementation Details

### 5.1 Syntax Highlighting (TextMate Grammar)

The `akkado.tmLanguage.json` is generated from the web IDE's tokenizer logic.

**Pattern mapping from CodeMirror to TextMate:**

```json
{
  "scopeName": "source.akkado",
  "fileTypes": ["akkado", "akk"],
  "name": "Akkado",
  "patterns": [
    { "include": "#comments" },
    { "include": "#strings" },
    { "include": "#numbers" },
    { "include": "#keywords" },
    { "include": "#builtins" },
    { "include": "#operators" },
    { "include": "#special" }
  ],
  "repository": {
    "comments": {
      "patterns": [{
        "match": "//.*$",
        "name": "comment.line.double-slash.akkado"
      }]
    },
    "strings": {
      "patterns": [{
        "begin": "\"",
        "end": "\"",
        "name": "string.quoted.double.akkado",
        "patterns": [{ "match": "\\\\.", "name": "constant.character.escape.akkado" }]
      }]
    },
    "keywords": {
      "patterns": [{
        "match": "\\b(true|false|fn|as|match|post)\\b",
        "name": "keyword.control.akkado"
      }]
    },
    "builtins": {
      "patterns": [{
        "match": "\\b(abs|acos|add|adsr|ar|...)\\b",
        "name": "support.function.builtin.akkado"
      }]
    },
    "operators": {
      "patterns": [
        { "match": "\\|>|->|==|!=|<=|>=|&&|\\|\\|", "name": "keyword.operator.akkado" },
        { "match": "[+\\-*/^=<>!?|.%@~]", "name": "keyword.operator.akkado" }
      ]
    },
    "special": {
      "patterns": [
        { "match": "\\$\\w+", "name": "meta.preprocessor.akkado" },
        { "match": "~", "name": "constant.numeric.akkado" },
        { "match": "\\b\\w+'(?=\\W|$)", "name": "string.other.chord.akkado" }
      ]
    }
  }
}
```

### 5.2 Completion Provider (VS Code Direct API)

Uses VS Code's `CompletionItemProvider` API, reusing logic from web IDE's `akkado-completions.ts`:

```typescript
// providers/completion-provider.ts
export class CompletionProvider implements vscode.CompletionItemProvider {
    triggerCharacters = ['"', '.'];
    
    async provideCompletionItems(
        document: vscode.TextDocument,
        position: vscode.Position
    ): Promise<vscode.CompletionItem[]> {
        const text = document.getText();
        const wordRange = document.getWordRangeAtPosition(position, /[a-zA-Z_][a-zA-Z0-9_]*/);
        const word = wordRange ? document.getText(wordRange) : '';
        
        if (word.length < 2) return [];
        
        const completions: vscode.CompletionItem[] = [];
        
        // Add builtins (from generated list, same as web IDE)
        for (const [name, info] of Object.entries(builtins)) {
            if (name.startsWith(word)) {
                const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
                item.detail = this.formatSignature(name, info);
                item.documentation = info.description;
                item.insertText = new vscode.SnippetString(`${name}($0)`);
                completions.push(item);
            }
        }
        
        // Add user-defined functions
        const userFns = this.extractUserFunctions(text);
        for (const fn of userFns) {
            if (fn.name.startsWith(word)) {
                const item = new vscode.CompletionItem(fn.name, vscode.CompletionItemKind.Function);
                item.detail = `${fn.name}(${fn.params.join(', ')})`;
                item.documentation = fn.docstring;
                completions.push(item);
            }
        }
        
        // Add user-defined variables
        const userVars = this.extractUserVariables(text);
        for (const varName of userVars) {
            if (varName.startsWith(word)) {
                completions.push(new vscode.CompletionItem(varName, vscode.CompletionItemKind.Variable));
            }
        }
        
        return completions;
    }
}
```

### 5.3 CLI Client (nkido Wrapper)

```typescript
// cli-client.ts
import { spawn, ChildProcess } from 'child_process';
import * as vscode from 'vscode';

export class CliClient {
    private cliPath: string;
    private outputChannel: vscode.OutputChannel;
    
    constructor(cliPath: string, outputChannel: vscode.OutputChannel) {
        this.cliPath = cliPath;
        this.outputChannel = outputChannel;
    }
    
    async check(sourceFile: string): Promise<vscode.Diagnostic[]> {
        return this.runCli(['check', sourceFile, '--json']);
    }
    
    async compile(sourceFile: string, outputFile?: string): Promise<vscode.Diagnostic[]> {
        const args = ['compile', sourceFile];
        if (outputFile) args.push('-o', outputFile);
        return this.runCli(args);
    }
    
    async play(sourceFile: string): Promise<void> {
        return this.runCliAsync(['play', sourceFile]);
    }
    
    private async runCli(args: string[]): Promise<vscode.Diagnostic[]> {
        return new Promise((resolve, reject) => {
            const proc = spawn(this.cliPath, args);
            let stdout = '';
            let stderr = '';
            
            proc.stdout.on('data', (d: Buffer) => stdout += d);
            proc.stderr.on('data', (d: Buffer) => stderr += d);
            
            proc.on('close', (code: number) => {
                if (code === 0) {
                    resolve([]);
                } else {
                    resolve(this.parseDiagnostics(stderr || stdout));
                }
            });
        });
    }
}
```

### 5.4 Serve Mode Manager (Persistent Process)

Spawns `nkido serve` (headless mode, prerequisite — see section 11) and exchanges newline-delimited JSON over stdio. One process per workspace; restarted on crash.

Wire format (subject to refinement when the CLI side is designed):

```jsonl
→ {"cmd": "load", "source": "...", "uri": "file:///..."}
← {"event": "compiled", "ok": true}
← {"event": "diagnostic", "severity": "error", "line": 3, "col": 5, "message": "..."}
→ {"cmd": "stop"}
← {"event": "stopped"}
```

```typescript
// serve-mode-manager.ts
export class ServeModeManager {
    private process: ChildProcess | null = null;
    private cliPath: string;

    start(): void {
        if (this.process) return;

        this.process = spawn(this.cliPath, ['serve'], {
            stdio: ['pipe', 'pipe', 'pipe']
        });

        this.process.stdout?.on('data', (d: Buffer) => {
            // Parse newline-delimited JSON events (compiled, diagnostic, stopped, ...)
        });

        this.process.on('exit', () => {
            this.process = null;
        });
    }

    stop(): void {
        this.send({ cmd: 'stop' });
    }

    kill(): void {
        if (this.process) {
            this.process.kill();
            this.process = null;
        }
    }

    send(message: object): void {
        if (this.process?.stdin) {
            this.process.stdin.write(JSON.stringify(message) + '\n');
        }
    }
}
```

### 5.5 File Icon (Custom .akk / .akkado Icon)

VS Code lets a language contribution declare a custom icon that file-icon themes (and the default theme) will use for files of that language. The icon shows up in the Explorer, tab strips, breadcrumbs, and Quick Open.

**Asset delivery:** The icon is human-delivered. The maintainer provides two SVG files (light + dark variants) sized for VS Code's icon slots (typically 32×32 viewBox, line/glyph weight tuned to match the bundled file icons in Seti / vs-seti). These files live at:

```
icons/
├── akkado-light.svg   # used when the active theme is light
└── akkado-dark.svg    # used when the active theme is dark
```

**`package.json` contribution:**

```json
{
  "contributes": {
    "languages": [
      {
        "id": "akkado",
        "aliases": ["Akkado", "akkado"],
        "extensions": [".akkado", ".akk"],
        "configuration": "./language-configuration.json",
        "icon": {
          "light": "./icons/akkado-light.svg",
          "dark": "./icons/akkado-dark.svg"
        }
      }
    ]
  }
}
```

**Notes:**
- The `icon` field on a language contribution is the only built-in mechanism — no separate icon theme is required for the icon to apply to `.akkado` / `.akk` files in any theme that does not already define a more specific icon.
- Icons must be SVG. PNG is not supported in the language-icon slot.
- The icon should remain legible at 16×16 (Explorer scale). Maintainer is responsible for visual QA; this PRD does not prescribe the design.
- If the maintainer also wants a richer file-icon theme (icons for other Nkido-related files like `.cedar` bytecode), that is out of scope for v1.

**Verification:** After install, files named `foo.akkado` and `bar.akk` show the supplied icon in the Explorer in both light and dark themes. Files of other languages are unaffected.

---

## 6. Extension Activation

```typescript
// extension.ts
import * as vscode from 'vscode';
import { CliClient } from './cli/cli-client';
import { ServeModeManager } from './cli/serve-mode-manager';
import { CompletionProvider } from './providers/completion-provider';
import { DiagnosticProvider } from './providers/diagnostic-provider';
import { SignatureHelpProvider } from './providers/signature-help';

export function activate(context: vscode.ExtensionContext) {
    const config = vscode.workspace.getConfiguration('nkido');
    const cliPath = config.get<string>('cliPath') || 'nkido';
    const outputChannel = vscode.window.createOutputChannel('Nkido');
    
    const cliClient = new CliClient(cliPath, outputChannel);
    const serveModeManager = new ServeModeManager();
    const diagnosticCollection = vscode.languages.createDiagnosticCollection('akkado');
    
    // Register providers
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            'akkado', new CompletionProvider(), '"', '.'
        ),
        vscode.languages.registerSignatureHelpProvider(
            'akkado', new SignatureHelpProvider(), '(', ','
        )
    );
    
    // Register commands
    context.subscriptions.push(
        vscode.commands.registerCommand('nkido.evaluate', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) return;
            
            const doc = editor.document;
            if (doc.languageId !== 'akkado') return;
            
            await doc.save();
            const diagnostics = await cliClient.check(doc.uri.fsPath);
            
            if (diagnostics.length === 0) {
                serveModeManager.start();
                serveModeManager.send({ cmd: 'load', source: doc.getText(), uri: doc.uri.toString() });
                outputChannel.appendLine(`Playing: ${doc.uri.fsPath}`);
            } else {
                diagnosticCollection.set(doc.uri, diagnostics);
            }
        }),

        vscode.commands.registerCommand('nkido.stop', () => {
            serveModeManager.stop();
            outputChannel.appendLine('Playback stopped');
        })
    );
    
    context.subscriptions.push(outputChannel, diagnosticCollection);
}
```

---

## 7. Implementation Phases

### Phase 1: Foundation + Syntax Highlighting

**Goal:** Extension loads in VS Code. `.akkado` and `.akk` files get syntax highlighting.

1. Create `~/workspace/nkido-vscode` with `npm init` and VS Code extension scaffold
2. Port TextMate grammar from web IDE's `akkado-language.ts` tokenizer
3. Generate `syntaxes/akkado.tmLanguage.json`
4. Register language in `package.json` with file extensions `.akkado`, `.akk`
5. Drop human-delivered file icons into `icons/akkado-{light,dark}.svg` and reference them in the language contribution (see section 5.5)
6. Basic extension activation (logs to output channel)

**Files:**
| File | Change |
|------|--------|
| `package.json` | New - Extension manifest (incl. `contributes.languages.icon`) |
| `src/extension.ts` | New - Entry point |
| `syntaxes/akkado.tmLanguage.json` | New - TextMate grammar |
| `src/syntax/grammar-generator.ts` | New - Generates grammar from tokens |
| `icons/akkado-light.svg`, `icons/akkado-dark.svg` | New - File icons (human-delivered) |
| `language-configuration.json` | New - Brackets, comments, auto-closing |

**Verification:** Open `.akkado` file in VS Code, verify syntax colors match web IDE. Verify the custom file icon shows in the Explorer for both `.akkado` and `.akk` files in light and dark themes.

### Phase 2: Providers (Diagnostics + Completion)

**Goal:** Errors appear as squiggly underlines + Problems panel. Autocomplete works.

1. Implement `CompletionProvider` using VS Code Direct API
2. Implement `DiagnosticProvider` (parse `nkido check --json` output)
3. Implement `SignatureHelpProvider` for function parameter tooltips
4. Register providers in `extension.ts`
5. Test with sample `.akkado` file

**Files:**
| File | Change |
|------|--------|
| `src/providers/completion-provider.ts` | New |
| `src/providers/diagnostic-provider.ts` | New |
| `src/providers/signature-help.ts` | New |
| `src/extension.ts` | Modified (register providers) |

**Verification:** Type invalid code, see red squiggly lines. Type `l`, see autocomplete popup with function names.

### Phase 3: CLI Integration + Commands

**Goal:** Play/stop commands work. Audio plays from VS Code.

1. Implement `CliClient` (spawns `nkido` for `check`/`compile`/`play`)
2. Implement `ServeModeManager` (persistent `nkido serve` process, JSON over stdio — depends on the CLI prerequisite in section 11)
3. Register commands: `nkido.evaluate`, `nkido.stop`, `nkido.compile`
4. Configure keyboard shortcuts (default Ctrl+Enter, Escape)
5. Add settings: `nkido.cliPath`, `nkido.autoCompileOnSave`

**Files:**
| File | Change |
|------|--------|
| `src/cli/cli-client.ts` | New |
| `src/cli/serve-mode-manager.ts` | New |
| `src/commands/evaluate.ts` | New |
| `src/commands/stop.ts` | New |
| `package.json` | Modified (add commands, keybindings, settings) |

**Verification:** Open `.akkado` file, press Ctrl+Enter, hear audio. Press Escape, audio stops.

### Phase 4: Polish + Settings

**Goal:** Signature tooltips work. Output channel shows status. Settings are complete.

1. Verify signature help provider works (implemented in Phase 2)
2. Wire up Nkido output channel
3. Add all configurable settings
4. Test edge cases (missing CLI, invalid code, etc.)
5. Update README with installation/usage instructions

**Files:**
| File | Change |
|------|--------|
| `src/utils/config.ts` | New |
| `src/utils/logger.ts` | New |
| `README.md` | New |

**Verification:** Type `lp(`, see signature tooltip. Check output channel for status messages. Verify all settings work.

---

## 8. Edge Cases

**Empty file:** `nkido.evaluate` shows info message "No code to evaluate", no error.

**Missing nkido:** Extension shows warning "nkido not found. Configure path in settings." Output channel logs full error. Diagnostics cleared.

**Invalid code:** `nkido check` returns errors. Squiggly underlines appear at correct line/column. Problems panel populated. Audio stops if playing.

**CLI crashes:** `ServeModeManager` detects process exit, clears playback state. Output channel shows error. User can restart via `Nkido: Restart CLI`.

**Multiple files open:** Each file tracked independently. Diagnostics scoped to correct file. Only active editor's file is played on evaluate.

**Save before play:** If `autoCompileOnSave` is true, file is saved automatically before evaluate. Otherwise, user gets prompt "Save file before playing?"

**Large files:** Completions filtered to relevant items. No performance degradation for files < 1000 lines.

**Rapid evaluate presses:** Debounced to 500ms. Multiple rapid Ctrl+Enter presses queued, only latest executed.

---

## 9. Testing Strategy

### Manual Testing (v1)

1. **Syntax highlighting:** Open `.akkado` file, verify keywords, builtins, strings, comments are colored
2. **Autocomplete:** Type `l`, verify popup shows `lp`, `limiter`, `lfo`, etc.
3. **Diagnostics:** Type invalid code, verify squiggly underlines + Problems panel
4. **Signature help:** Type `lp(`, verify tooltip shows `lp(in, cut, q?)`
5. **Evaluate:** Press Ctrl+Enter, verify audio plays
6. **Stop:** Press Escape, verify audio stops
7. **Settings:** Change `nkido.cliPath`, verify extension uses new path
8. **Output channel:** Verify Nkido channel shows compilation/playback status

### Build Verification

```bash
cd ~/workspace/nkido-vscode
npm install
npm run compile         # Compile TypeScript
npm run package          # Create .vsix file
code --install-extension nkido-0.1.0.vsix
# Test in VS Code
```

---

## 10. Open Questions

1. **Serve-mode wire format**: JSON over stdio is the proposed transport, but the exact command/event vocabulary needs to be locked down with the CLI work in section 11. Concretely: how are diagnostics shaped (reuse `format_diagnostic_json`?), how is the audio backend selected, and how is `param()` state pushed?

2. **nkido path validation**: Should the extension verify `nkido` works on startup, or lazily on first use?

3. **Signature help trigger**: Using `(` and `,` as trigger characters. Should `Ctrl+Shift+Space` also manually trigger it (standard VS Code behavior)?

---

## 11. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| `nkido/cedar` | **Stays** | No modifications, used via `nkido` |
| `nkido/akkado` | **Stays** | No modifications, used via `nkido` |
| `nkido/tools/nkido` | **Modified (prerequisite)** | Adds new `serve` mode (headless, JSON-over-stdio) for hot-swap; `bytecode_loader.cpp` extended to recognize `.akk` alongside `.akkado`. Existing `ui` mode is unchanged. |
| `~/workspace/nkido-vscode` | **New** | Entire extension is new |
| File icon assets (`icons/akkado-light.svg`, `icons/akkado-dark.svg`) | **New (human-delivered)** | Provided by maintainer, see section 5.5 |
| Web IDE (`nkido/web`) | **Stays** | Independent, may share syntax definitions in future |

### 11.1 Prerequisite: `nkido serve` mode

Before extension v1 can ship, `nkido` needs:

1. A new `serve` subcommand that runs headless (no SDL2 window, audio backend stays).
2. A line-delimited JSON command/event protocol on stdin/stdout (`load`, `stop`, `set_param`, `quit` → `compiled`, `diagnostic`, `stopped`, `param_changed`).
3. Hot-swap: receiving a new `load` while playing should reuse Cedar's existing crossfade path.
4. `tools/nkido/bytecode_loader.cpp` extended to recognize `.akk` as a source extension (currently keys on `.akkado` only — see line ~with `".akkado"` literal). Without this, files saved with `.akk` will be misclassified as bytecode.

This is its own work item, tracked separately from the extension; the VS Code extension PRD assumes it lands first.

---

## 12. Future Work (post-v1)

- **Inline visualizations**: Waveforms, frequency responses (requires significant editor integration)
- **Parameter controls**: UI for `param()`, `toggle()`, `button()` bound to code
- **Debug panels**: State inspector, pattern debugger, instruction highlighting
- **Multi-file support**: Module imports, workspace-level compilation
- **WASM integration**: Embed `akkado.wasm` for offline compilation (no CLI needed)
- **Recording**: Record audio output to WAV file from VS Code
- **Snippets**: Code snippets for common patterns (oscillator, envelope, etc.)
- **Hover info**: Hover over function name shows documentation
- **Go to definition**: Jump to user-defined function definitions

---

## 13. References

- [VS Code Extension API](https://code.visualstudio.com/api)
- [TextMate Grammar Guide](https://macromates.com/manual/en/language_grammars)
- [Nkido Web IDE PRD](./prd-nkido-web-ide.md) - Source of truth for feature parity
- [Nkido Editor Autocomplete PRD](./prd-editor-autocomplete.md) - Completion logic reference
- [nkido source](../tools/nkido/) - CLI integration target
- [Web IDE Akkado Language](../web/src/lib/editor/akkado-language.ts) - Tokenizer to port
- [Web IDE Completions](../web/src/lib/editor/akkado-completions.ts) - Completion logic to reuse
