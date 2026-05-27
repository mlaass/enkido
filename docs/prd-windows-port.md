> **Status: NOT STARTED** — Bringing `nkido` and `akkado` to Windows (MSVC) with full runtime parity, CI on every PR, and downloadable release zips on every `v*` tag.

# PRD: Windows Port of Nkido Executables

**Status:** Draft
**Author:** Claude
**Date:** 2026-05-27
**Related:** [Cross-platform porting requirements](cross-platform-porting.md), [Netlify deployment](netlify_deployment.md)

## 1. Overview

### 1.1 Context

The Cedar and Akkado **libraries** already compile on Windows under MSVC — three blockers (`std::aligned_alloc`, `__builtin_ctz`, `std::from_chars<double>`) were fixed on 2026-04-22 and verified green via the `godot-nkido-addon` CI ([run 24777116197 attempt 2](https://github.com/mlaass/godot-nkido-addon/actions/runs/24777116197)) across `windows-latest / {Debug, Release}`. See `docs/cross-platform-porting.md` for that history.

What does **not** exist today:
- A Windows build of the `nkido` and `akkado` **executables** (the addon CI builds Cedar as a library only).
- Any Windows CI in this repo (`.github/workflows/deploy.yml` runs `ubuntu-latest` only and produces a WASM build).
- Any pre-built Windows binaries on the GitHub Releases page.

This PRD adds all three.

### 1.2 Proposed Solution

1. Fix the remaining Windows-blocking source code in `tools/nkido-cli/` (currently `<unistd.h>` is included unconditionally in two files).
2. Add a small platform-abstraction layer for the three things that aren't already abstracted: console Ctrl+C handler, stdin/stdout binary mode, UTF-8 process code page.
3. Bundle SDL2 on Windows by downloading the official SDL2 dev SDK in CI and installing `SDL2.dll` next to the executable.
4. Add a `windows-latest` job to a new `.github/workflows/ci.yml` that builds, tests, and (on `v*` tags) uploads release artifacts.
5. Ship a self-contained zip `nkido-windows-x64-vX.Y.Z.zip` containing the two executables, `SDL2.dll`, and the bundled sample kit / SoundFonts / TDM catalog, all nested under a `nkido-vX.Y.Z/` folder.

### 1.3 Goals

- **Full runtime parity** on Windows for `nkido` and `akkado`: real-time audio (SDL2 → WASAPI/DirectSound), live MIDI in (RtMidi → winmm), SDL visualization window, `serve` mode with stdin JSON protocol, all CLI subcommands (`compile`, `disasm`, `render`, `serve`, etc.).
- **Windows CI on every push/PR** that exercises (a) the full nkido build (cedar+akkado+tools), (b) the standalone `cedar-only` preset (protects the Godot addon's downstream build), and (c) the `cedar_tests` + `akkado_tests` test suites via `ctest`.
- **Self-contained release zip** auto-attached to each GitHub Release on `v*` tags, ready to extract and run with no further setup.
- **UTF-8 paths everywhere**, so `C:\Users\Joël\samples\kick.wav` works.
- **No regressions on Linux/macOS** — every platform abstraction must keep the existing POSIX behavior bit-identical.

### 1.4 Non-Goals

- **MinGW-w64 / MSYS2 / Clang-cl support.** MSVC only for v1. (Future PRD if demand surfaces.)
- **MSI installer / winget / Chocolatey package.** v1 ships zip-only; packaging belongs in a follow-up PRD.
- **Code signing / Authenticode.** Users will see SmartScreen warnings; document in `README.txt`. (Future, paid certificate.)
- **`midi2akk`** — that's a Python tool that runs on Windows today via `uv`. No code changes needed; we will add one paragraph to its README documenting Windows install.
- **`web/` SvelteKit app and WASM build.** Already cross-platform via Bun + Emscripten; out of scope.
- **`/WX` (warnings as errors) on MSVC.** Deferred to a follow-up "Windows warning cleanup" PRD so v1 can ship without first chasing every `/W4` conversion warning.
- **Win8.1 and pre-1903 Win10.** The UTF-8 manifest requires Windows 10 1903+ (May 2019). Older versions are unsupported.
- **32-bit (x86) builds.** x64 only.
- **ARM64 / Windows on ARM.** Future PRD.

---

## 2. Current State

### 2.1 What works today

| Item | Status | Notes |
|------|--------|-------|
| `cedar` library on MSVC | **Works** | Verified by godot-nkido-addon CI run 24777116197 attempt 2 |
| `akkado` library on MSVC | **Works** | Same run |
| `cedar/include/cedar/vm/audio_arena.hpp` MSVC branch | **Works** | `_aligned_malloc` / `_aligned_free` |
| `cedar/src/io/file_cache.cpp` Windows branch | **Works** | Uses `%LOCALAPPDATA%\nkido\cache` |
| `tools/nkido-cli/asset_loader.cpp::executable_dir()` Windows branch | **Works** | `GetModuleFileNameA` |
| SDL2 `WIN32`-gated `SDL2main` link | **Works** | Already in `tools/nkido-cli/CMakeLists.txt:60-62` |
| RtMidi Windows backend (winmm) | **Works** | Built by FetchContent unconditionally; rtmidi enables winmm on Windows automatically |

### 2.2 What's broken or missing

| Item | Problem | Where |
|------|---------|-------|
| `<unistd.h>` included unconditionally | MSVC has no `<unistd.h>`; build fails immediately | `tools/nkido-cli/asset_loader.cpp:20`, `tools/nkido-cli/tests/test_serve.cpp:27` |
| `signal(SIGINT)` is racy on Windows | MSVC delivers SIGINT on a separate thread; audio thread can race during shutdown | `tools/nkido-cli/serve_mode.cpp:18-26` |
| `std::cin` / `std::cout` in **text mode** | Windows CRT translates `\n` ↔ `\r\n` on stdin/stdout — corrupts the serve-mode JSON line protocol if any client sends `\r\n` line endings, and double-newlines our emitted JSON | `tools/nkido-cli/serve_mode.cpp:64, 1182` |
| UTF-8 paths | Default MSVC CRT treats `char*` paths as the system ANSI code page (CP1252 on most installs), so `C:\Users\Joël\…` breaks `std::ifstream` and `fopen` | All file I/O sites that take `std::string` paths |
| `test_serve.cpp` is POSIX-only | Uses `mkstemp`, `close()`, shell command strings with env prefixes — would not even compile on MSVC, let alone run | `tools/nkido-cli/tests/test_serve.cpp:43-60+` |
| No Windows CI | No `.github/workflows/*.yml` builds on `windows-latest` | `.github/workflows/` |
| No Windows release artifacts | `deploy.yml` only produces WASM | `.github/workflows/deploy.yml` |
| SDL2 not bundled | `find_package(SDL2)` works on dev boxes that pre-installed the SDK, but there's no story for downloading it in CI or shipping `SDL2.dll` to end users | `tools/nkido-cli/CMakeLists.txt:3` |

---

## 3. Architecture

### 3.1 Platform abstraction layer

Three thin headers under `cedar/include/cedar/platform/` (Cedar already has `cedar/io/`, so a sibling namespace is consistent). All abstractions are header-only or 1-file `.cpp`, with `#if defined(_WIN32)` branches at the bottom — same pattern as `cedar/include/cedar/vm/audio_arena.hpp` and `cedar/src/io/file_cache.cpp`.

```
cedar/include/cedar/platform/
├── ctrl_c.hpp        // install_ctrl_c_handler(callback)
├── stdio_binary.hpp  // set_stdio_binary_mode()  -- no-op on POSIX
└── utf8_init.hpp     // ensure_utf8_console()    -- no-op on POSIX
```

These live in `cedar` (not `tools/nkido`) because the same Ctrl+C / UTF-8 console abstractions would be useful for any future Cedar consumer (e.g. a Windows-only Cedar host). Cost: <100 LOC total.

### 3.2 Console Ctrl+C handler

**API:**
```cpp
namespace cedar::platform {
    using CtrlCCallback = void(*)();
    // Idempotent. Last-call-wins.
    void install_ctrl_c_handler(CtrlCCallback cb);
}
```

**POSIX branch** wraps `std::signal(SIGINT, …)` and `std::signal(SIGTERM, …)` (current behavior in `serve_mode.cpp:25-26`).

**Windows branch** uses `SetConsoleCtrlHandler`:
```cpp
static CtrlCCallback g_cb = nullptr;
static BOOL WINAPI handler(DWORD evt) {
    if (evt == CTRL_C_EVENT || evt == CTRL_BREAK_EVENT ||
        evt == CTRL_CLOSE_EVENT) {
        if (g_cb) g_cb();
        return TRUE;
    }
    return FALSE;
}
void install_ctrl_c_handler(CtrlCCallback cb) {
    g_cb = cb;
    SetConsoleCtrlHandler(handler, TRUE);
}
```

`serve_mode.cpp::install_signal_handlers()` becomes a single call to `cedar::platform::install_ctrl_c_handler(...)`.

### 3.3 stdin/stdout binary mode

**API:**
```cpp
namespace cedar::platform {
    void set_stdio_binary_mode();  // no-op on POSIX
}
```

**Windows branch:**
```cpp
#include <io.h>
#include <fcntl.h>
void set_stdio_binary_mode() {
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    // stderr left alone — it's diagnostic, CRLF is fine there.
}
```

Called once from `main()` of `nkido` (before serve mode reads stdin) and once from `main()` of `akkado` (defensive, since `--emit-json` writes structured output).

### 3.4 UTF-8 console + manifest

Two complementary mechanisms are needed; both are required, neither is sufficient on its own.

**(a) Per-executable manifest** — Windows reads it at process start and routes ANSI APIs (`fopen`, `CreateFileA`, `GetCommandLineA`) through UTF-8 instead of the system code page. Win10 1903+. Manifest file:

```xml
<!-- tools/nkido-cli/nkido.manifest -->
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application>
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    </windowsSettings>
  </application>
</assembly>
```

A matching `tools/akkado-cli/akkado.manifest`.

Embed via CMake (MSVC-only):
```cmake
if(WIN32)
    target_sources(nkido PRIVATE nkido.manifest)
    target_sources(akkado PRIVATE akkado.manifest)
endif()
```

**(b) Runtime console code page** — even with the manifest, the *console output* code page may still be CP437/CP1252. Set it at start of `main()`:

```cpp
// cedar/src/platform/utf8_init_win.cpp
#include <windows.h>
void ensure_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
```

POSIX version is a no-op. Called once at the top of each CLI's `main()`.

### 3.5 SDL2 acquisition (Windows)

Keep `find_package(SDL2)` in `tools/nkido-cli/CMakeLists.txt` unchanged. Add a CI step that downloads the official SDL2 dev SDK zip, extracts it, and points `SDL2_DIR` (or `CMAKE_PREFIX_PATH`) at the extracted folder. This matches Option C from the Round 2 decision — predictable, fast, no vcpkg bootstrap, no FetchContent build of SDL2 (which is large).

Pin SDL2 **2.30.x** (latest 2.x stable; SDL3 is a future migration). Specific version `2.30.10` (or whatever is current at implementation time) pinned in the CI step for reproducibility.

CI step (sketch):
```yaml
- name: Download SDL2 dev SDK
  shell: pwsh
  run: |
    $ver = "2.30.10"
    $url = "https://github.com/libsdl-org/SDL/releases/download/release-$ver/SDL2-devel-$ver-VC.zip"
    Invoke-WebRequest -Uri $url -OutFile sdl2.zip
    Expand-Archive sdl2.zip -DestinationPath sdl2
    "SDL2_DIR=$PWD/sdl2/SDL2-$ver/cmake" >> $env:GITHUB_ENV
```

`SDL2.dll` then needs to be:
1. Copied next to `nkido.exe` in the build directory (so CI can `ctest`).
2. Installed via CMake into the bin dir (so `cmake --install` lays it out for the zip).

Done with a small post-build step + an `install(FILES)` rule, both Windows-only.

### 3.6 Release zip packaging

CMake `install()` rules already lay out the install tree:
```
<prefix>/bin/nkido.exe
<prefix>/bin/akkado.exe
<prefix>/bin/SDL2.dll                              (new, WIN32-only)
<prefix>/share/nkido/default_kit/*.wav
<prefix>/share/nkido/default_kit/strudel.json
<prefix>/share/nkido/soundfonts/*.sf3
<prefix>/share/nkido/tidal-drum-machines/catalog.json
```

For the release zip we want a *nested* root folder so extracting doesn't spray files into `Downloads/`:
```
nkido-vX.Y.Z/
├── nkido.exe
├── akkado.exe
├── SDL2.dll
├── README.txt
├── LICENSE.txt
├── default_kit/
│   └── …
├── soundfonts/
│   └── …
└── tidal-drum-machines/
    └── …
```

Note the **flat layout** (no `bin/`/`share/`) — the user has expressed a preference for "extract and double-click" rather than the Linux-style split. We get this by:
1. Running `cmake --install build --prefix staging/nkido-vX.Y.Z`
2. Flattening `bin/` and `share/nkido/` into the root of `nkido-vX.Y.Z/` via a small PowerShell step
3. `Compress-Archive staging/nkido-vX.Y.Z nkido-windows-x64-vX.Y.Z.zip`

This is simpler than fighting CPack into a custom layout and runs entirely in the CI job.

### 3.7 CI matrix

New file `.github/workflows/ci.yml`:

| Job | Runner | Configure | Build | Test | Upload |
|-----|--------|-----------|-------|------|--------|
| `linux-debug` | `ubuntu-latest` | `--preset debug` | full | `ctest` | — |
| `windows-debug` | `windows-latest` | `--preset debug` + `-DSDL2_DIR=...` | full | `ctest` | — |
| `windows-release` | `windows-latest` | `--preset release` + `-DSDL2_DIR=...` | full | (skip — release strips tests) | `nkido-windows-x64-vX.Y.Z.zip` (tags only) |
| `windows-cedar-only` | `windows-latest` | `--preset cedar-only` | cedar | `ctest` (cedar only) | — |

The existing `deploy.yml` (WASM + Netlify deploy) stays separate so the web build path doesn't depend on the Windows build path. Windows release upload is **a separate step in `ci.yml`** that only runs on `startsWith(github.ref, 'refs/tags/v')` and uses `softprops/action-gh-release` to attach to the same Release page that `deploy.yml` creates.

To avoid a race (both `deploy.yml` and `ci.yml` trying to create the Release), we set the `softprops/action-gh-release` step's `fail_on_unmatched_files: false` and use `append_body: false` — both workflows simply attach to whatever Release exists. If neither has run yet, the first one creates it.

---

## 4. File-Level Changes

### 4.1 Code changes (existing files)

| File | Change |
|------|--------|
| `tools/nkido-cli/asset_loader.cpp` | Wrap line 20's `#include <unistd.h>` in `#if defined(__linux__)` (it's only used by the Linux `readlink("/proc/self/exe", …)` branch at line 591). |
| `tools/nkido-cli/serve_mode.cpp` | Replace `install_signal_handlers()` body (lines 18-26) with `cedar::platform::install_ctrl_c_handler([](){ g_signal_received.store(true, std::memory_order_release); });`. Call `cedar::platform::set_stdio_binary_mode()` early in `serve()`. |
| `tools/nkido-cli/main.cpp` | Call `cedar::platform::ensure_utf8_console()` as the first statement of `main()`. |
| `tools/akkado-cli/main.cpp` | Same: call `cedar::platform::ensure_utf8_console()` first. |
| `tools/nkido-cli/tests/test_serve.cpp` | Replace POSIX-specific subprocess/`mkstemp` machinery with a portable equivalent. Concrete plan in §6. |
| `tools/nkido-cli/CMakeLists.txt` | Add `WIN32`-gated `target_sources(... nkido.manifest)`, `install(FILES ${SDL2_RUNTIME_LIBRARY} DESTINATION ${CMAKE_INSTALL_BINDIR})`, and a `POST_BUILD` `copy_if_different` of `SDL2.dll` next to `nkido.exe` in the build tree (so `ctest` works in-place). |
| `tools/akkado-cli/CMakeLists.txt` | Add `WIN32`-gated `target_sources(... akkado.manifest)`. |
| `cmake/CompilerOptions.cmake` | No change in v1. (Existing MSVC `/W4 /permissive-` block is fine without `/WX`.) |
| `.github/workflows/deploy.yml` | Leave alone. (Windows lives in a new file so the WASM deploy stays isolated.) |
| `README.md` (top level) | Add a "Windows" subsection under build instructions: link to the Release page, note the SmartScreen warning, mention "Right-click → Properties → Unblock" if needed. |
| `tools/midi2akk/README.md` | One paragraph: "On Windows, use `uv` (or `pipx`) — same install command as Linux/macOS." |

### 4.2 New files

| File | Purpose |
|------|---------|
| `cedar/include/cedar/platform/ctrl_c.hpp` | Public API: `install_ctrl_c_handler(CtrlCCallback)`. |
| `cedar/src/platform/ctrl_c_posix.cpp` | POSIX `signal()`-based implementation. |
| `cedar/src/platform/ctrl_c_win.cpp` | `SetConsoleCtrlHandler`-based implementation. |
| `cedar/include/cedar/platform/stdio_binary.hpp` | Public API: `set_stdio_binary_mode()`. |
| `cedar/src/platform/stdio_binary_posix.cpp` | No-op. |
| `cedar/src/platform/stdio_binary_win.cpp` | `_setmode(_fileno(stdin/stdout), _O_BINARY)`. |
| `cedar/include/cedar/platform/utf8_init.hpp` | Public API: `ensure_utf8_console()`. |
| `cedar/src/platform/utf8_init_posix.cpp` | No-op. |
| `cedar/src/platform/utf8_init_win.cpp` | `SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);`. |
| `cedar/CMakeLists.txt` (modify) | Conditionally compile `*_win.cpp` on `WIN32`, `*_posix.cpp` otherwise. |
| `tools/nkido-cli/nkido.manifest` | UTF-8 `activeCodePage` manifest, embedded in the exe via `target_sources`. |
| `tools/akkado-cli/akkado.manifest` | Same for `akkado`. |
| `.github/workflows/ci.yml` | Linux + Windows + cedar-only matrix; Windows release-zip upload on `v*` tags. |
| `scripts/package-windows-zip.ps1` | PowerShell: take a `cmake --install` staging dir, flatten `bin/`+`share/nkido/` to root, copy `README.txt` / `LICENSE.txt`, `Compress-Archive` to `nkido-windows-x64-vX.Y.Z.zip`. Called from CI. |
| `tools/windows/README.txt.in` | CMake-configured template; ends up at zip root with version/git-sha/build-date. |

### 4.3 Files explicitly NOT changed

| File | Why |
|------|-----|
| `cedar/include/cedar/vm/audio_arena.hpp` | MSVC branch already correct (verified by addon CI). |
| `cedar/src/dsp/fft.cpp` | `std::countr_zero` already in place. |
| `akkado/src/lexer.cpp`, `akkado/src/mini_lexer.cpp` | `std::strtod` already in place. |
| `cedar/src/io/file_cache.cpp` | Windows branch already present and correct. |
| `tools/nkido-cli/asset_loader.cpp::executable_dir()` (line 588+) | `GetModuleFileNameA` branch already correct. |
| `tools/nkido-cli/CMakeLists.txt:60-62` | `SDL2main` link guard already correct. |
| `cmake/Dependencies.cmake` | rtmidi's FetchContent already handles winmm on Windows automatically. |
| `web/` | Out of scope. |
| `tools/midi2akk/src/` | Python; Windows-works today. README update only. |

---

## 5. Implementation Phases

### Phase 1 — Source-level Windows readiness (no CI yet)

**Goal:** A fresh MSVC `cmake --build` succeeds for `nkido`, `akkado`, `cedar_tests`, `akkado_tests` on a Windows developer machine.

**Files:** all changes in §4.1 + §4.2 *except* `.github/workflows/ci.yml`.

**Verification:** Manual — run `cmake --preset debug` and `cmake --build build/debug` on a Win11 + VS 2022 + SDL2-2.30.10 box. Confirm `build/debug/bin/nkido.exe` and `build/debug/bin/akkado.exe` exist. Run `nkido.exe --help`, `akkado.exe --help`, `nkido.exe render --help`. **Do not** run the audio path yet — that's Phase 3.

**Commit:** `feat(windows): source-level MSVC readiness for nkido and akkado`

### Phase 2 — Platform abstractions integrated

**Goal:** UTF-8 console works, Ctrl+C exits cleanly, serve mode stdin/stdout is binary-mode.

**Files:** `serve_mode.cpp` (handler swap + `set_stdio_binary_mode` call), `main.cpp` × 2 (`ensure_utf8_console` calls), the three `cedar/include/cedar/platform/*.hpp` plus their POSIX + Windows implementations, manifest files + CMake `target_sources` lines.

**Verification:**
- Linux: `cedar_tests`, `akkado_tests`, `nkido render` all pass (no behavior change — POSIX implementations are no-op / wrap existing `signal`).
- Windows: launch `nkido serve`, hit Ctrl+C → process exits within 1s with no stuck audio. Pass JSON over stdin and watch stdout for the parsed events. Confirm `nkido render examples/with-emoji-é.akk out.wav` works (UTF-8 path test).

**Commit:** `feat(windows): platform abstractions for ctrl-c, stdio binary mode, UTF-8 console`

### Phase 3 — Test suite ported

**Goal:** `ctest` runs green on Windows.

**Files:** primarily `tools/nkido-cli/tests/test_serve.cpp` rewrite — replace the `mkstemp` + shell-string + `popen`-style approach with either:
- **Option A (preferred):** `std::filesystem::temp_directory_path()` + `_popen`/`popen` behind a thin `tests/test_subprocess.hpp` helper.
- **Option B (fallback):** mark the test `[!mayfail]` on Windows and file a follow-up. Avoid if possible — the test exists for a reason.

Other tests audited: `cedar_tests` and `akkado_tests` use Catch2 + standard C++ only — expected to pass on Windows without changes, but Phase 1's manual run will surface any surprises.

**Verification:** `ctest` exit code 0 on both `windows-debug` and `linux-debug` configurations.

**Commit:** `test(windows): port test_serve.cpp to MSVC; verify full test suite on Windows`

### Phase 4 — CI on every push/PR

**Goal:** `.github/workflows/ci.yml` lights up green for Linux + Windows + Windows cedar-only on every push/PR.

**Files:** `.github/workflows/ci.yml` (new). Steps per Windows job:
1. `actions/checkout@v4`
2. Download SDL2-2.30.10-devel-VC.zip, extract, export `SDL2_DIR`.
3. `cmake --preset debug -B build`
4. `cmake --build build --config Debug`
5. `ctest --test-dir build --output-on-failure`

`windows-release` job uses `--preset release` and `--config Release`; skips `ctest` (release builds don't include tests).

**Verification:** Open a no-op PR (e.g. typo fix), confirm all four jobs green.

**Commit:** `ci(windows): build + test matrix on every push/PR`

### Phase 5 — Release zip on `v*` tags

**Goal:** Pushing a `v*` tag attaches `nkido-windows-x64-vX.Y.Z.zip` to the corresponding GitHub Release.

**Files:** `scripts/package-windows-zip.ps1` (new), `tools/windows/README.txt.in` (new), additional release-only step in `.github/workflows/ci.yml`.

**Verification:** Tag a test pre-release (e.g. `v0.0.0-windows-test`), wait for CI, download the produced zip on a clean Win11 VM (no Visual Studio installed), extract, double-click `nkido.exe` → sees `--help` output; run `nkido render examples/hello.akk out.wav` → produces a WAV; run `nkido serve` → emits the initial banner JSON. Delete the pre-release after.

**Commit:** `ci(windows): build self-contained release zip and attach to GitHub Releases on v* tags`

---

## 6. Edge Cases

### 6.1 Console code page when there is no console

`nkido` may be launched from `cmd.exe`, PowerShell, Windows Terminal, or detached (no console at all — e.g. spawned by a parent like VS Code's task runner). `SetConsoleOutputCP(CP_UTF8)` returns `FALSE` with `GetLastError() == ERROR_INVALID_HANDLE` when there's no attached console. **Expected behavior:** ignore the failure silently — there's no console to set the code page on, so it doesn't matter. The manifest already covers ANSI API calls.

### 6.2 stdin not a terminal (e.g. piped from a file)

`_setmode(_fileno(stdin), _O_BINARY)` works whether stdin is a console, a pipe, or a redirected file. No special handling needed.

### 6.3 `serve_mode` Ctrl+C during audio block

Windows `SetConsoleCtrlHandler` fires the handler on a **new thread**, just like POSIX `signal()` may interrupt arbitrarily. Our handler only does `g_signal_received.store(true, std::memory_order_release)`, which is async-signal-safe and atomic — no race with the audio thread that polls it.

### 6.4 SDL2.dll missing at runtime

If a user extracts the zip but accidentally moves `nkido.exe` to another folder without `SDL2.dll`, Windows shows a "SDL2.dll was not found" dialog before `main()` runs. We can't catch this in code; document the requirement in `README.txt`: "keep all files in the same folder".

### 6.5 SmartScreen / Windows Defender

Unsigned executables downloaded from GitHub trigger SmartScreen on first launch. Document in `README.txt`: "Right-click `nkido.exe` → Properties → check 'Unblock' → OK". Code signing is out of scope for v1 (needs a paid Authenticode cert).

### 6.6 Paths with backslashes inside `.akk` source files

Akkado source like `sample("samples\\kick.wav")` (Windows-style paths) — `std::filesystem::path` normalizes both `/` and `\` on Windows, so this works automatically. On Linux the same source file would be invalid (`\\k` is escape garbage), but that's the user's problem — Linux users write `/`. Document briefly in the `README.txt`.

### 6.7 `windows-latest` runner image bumps

GitHub may swap `windows-latest` from Windows Server 2022 → 2025 etc. To mitigate: pin to `windows-2022` initially. Document in `ci.yml` comment that this is a deliberate pin; review yearly.

### 6.8 Files inside `share/` with characters Compress-Archive dislikes

`Compress-Archive` (PowerShell 5.1, default on Windows runners) has historical issues with paths > 260 chars and certain UTF-8 names. Mitigation: use 7-zip via `choco install 7zip` (already on `windows-latest`) if `Compress-Archive` fails for any sample filename. Implement Compress-Archive first; switch to 7-zip only if it breaks.

### 6.9 `cedar-only` build path on Windows

The `cedar-only` preset disables akkado and tools — the only thing it builds is `cedar` (and its tests if enabled). No SDL2 dependency, no rtmidi, no platform abstractions are involved in the library itself today. This job should be the fastest and most boring of the matrix. If it ever fails, the bug is in cedar core, not the Windows port.

---

## 7. Testing & Verification Strategy

### 7.1 Per-phase verification (summarized from §5)

| Phase | Test |
|-------|------|
| 1 | Manual `cmake --build` on a Windows dev box; `--help` runs on both executables. |
| 2 | Round-trip Ctrl+C and UTF-8 path tests on Windows; existing Linux tests unchanged. |
| 3 | `ctest` exit 0 on Windows for `cedar_tests`, `akkado_tests`, `nkido_tests`. |
| 4 | All 4 CI jobs green on a no-op PR. |
| 5 | Pre-release tag → downloadable zip → smoke-test on clean Win11 VM. |

### 7.2 Acceptance criteria for v1

The PRD is **DONE** when all of the following are true on `master`:

1. `ci.yml` runs on every push/PR with `windows-debug`, `windows-release`, `windows-cedar-only`, and `linux-debug` jobs, all green.
2. `cedar_tests` + `akkado_tests` + `nkido_tests` pass on Windows via `ctest`.
3. Pushing a `v*` tag produces `nkido-windows-x64-vX.Y.Z.zip` attached to the Release page.
4. Manual smoke test on a clean Win10 1903+ machine (no Visual Studio, no Bun, no Python): download the zip, extract, run `nkido render examples/hello.akk out.wav`, listen to `out.wav` → audible. Run `nkido serve`, paste a `compile` command → get an `event_count` response. Plug in a USB MIDI keyboard, run `nkido serve --midi`, play notes → see `note_on` events emitted on stdout.
5. `docs/cross-platform-porting.md` updated with a "**Windows executables: SHIPPED**" section linking to this PRD and the first release tag that included Windows binaries.

### 7.3 Concrete UTF-8 path smoke test

Create `examples/utf8-é-é.akk` containing a trivial program. On Windows:
```cmd
nkido.exe compile examples\utf8-é-é.akk --emit-json
```
Expected: succeeds, prints valid JSON. Pre-port: fails with "file not found" because MSVC's `fopen("é")` lookup uses CP1252.

### 7.4 Concrete serve-mode binary-stdio test

The existing `test_serve.cpp::TEST_CASE("serve registers banks on demand")` (the regression test for the VS Code extension bug) is the gold-standard end-to-end check. After the Phase 3 port, it must run green on Windows. If we cannot port it cleanly within Phase 3, mark it `[!mayfail][windows]` and open a tracking issue — but do not drop the test entirely.

---

## 8. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| MSVC trips on existing `/W4` warnings that GCC/Clang don't flag | High | Medium | Don't enable `/WX` in v1 (already a decision); file follow-up PRD. |
| Some untested Akkado test sourcefile uses features that subtly differ on MSVC (e.g. `<charconv>` corner cases) | Medium | Medium | Phase 3's `ctest` run will surface this; fix per-case. |
| `Compress-Archive` mangles UTF-8 filenames in `share/` | Low | Low | Fallback to 7-zip (already on the runner). Tested in Phase 5. |
| SDL2 dev SDK 2.30.10 URL changes / 404s | Low | High | Pin a specific version; if SDL releases policy changes, vendor the zip into the repo as a one-time fallback. |
| `softprops/action-gh-release` race between `deploy.yml` and `ci.yml` on the same tag | Medium | Low | Both workflows use `append_body: false` and `fail_on_unmatched_files: false`; first one creates the Release, second attaches its assets. |
| Test ported via `_popen` deadlocks on Windows when the child blocks on stdout | Low | Medium | Use threaded reader (one thread per pipe) — well-known pattern. Catch2's `[!mayfail]` is the escape hatch if it doesn't pan out in time. |
| User reports broken non-ASCII paths despite the manifest | Low | Medium | The manifest only kicks in on Win10 1903+. Document the minimum version in `README.txt`. |
| Antivirus quarantines `nkido.exe` on first download | Medium | Low | Document the "Unblock" workaround; revisit code signing in a follow-up PRD. |

---

## 9. Open Questions

None at PRD time — all design decisions were resolved during the question rounds:

| Decision | Choice |
|----------|--------|
| v1 scope | CI + downloadable zips on `v*` tags |
| Tools in scope | `nkido`, `akkado` (midi2akk = doc-only) |
| Runtime parity | Full (audio + MIDI + viz + serve) |
| Toolchain | MSVC (VS 2022) only |
| Dep mgmt | Pre-downloaded SDL2 dev SDK + FetchContent rtmidi |
| Zip contents | Self-contained (binaries + DLL + share/) |
| Path encoding | UTF-8 throughout + Windows manifest |
| Ctrl+C | Console Ctrl Handler abstraction |
| Tests in CI | Yes, full matrix |
| Zip layout | Flat, nested under `nkido-vX.Y.Z/` |
| Warnings | `/WX` deferred to follow-up |
| Standalone cedar | Yes, second matrix entry |
| Release trigger | `v*` tags only |
| SDL2 version | 2.30.x (latest 2.x stable) |

---

## 10. Follow-up Work (out of scope for v1)

These deserve their own PRDs once v1 has shipped:

1. **MSVC `/W4 /WX` cleanup** — turn warnings into errors after the initial port stabilizes; surface and fix MSVC-only warnings systematically.
2. **MinGW-w64 / Clang-cl support** — if community demand surfaces.
3. **MSI installer / winget / Chocolatey** — once the zip distribution has a few releases under its belt and we understand what users want.
4. **Code signing (Authenticode)** — needs paid certificate + signing infrastructure; eliminates SmartScreen warnings.
5. **ARM64 (Windows on ARM)** — second runner, second zip artifact, separate compatibility audit.
6. **`midi2akk` Windows packaging** — currently Python via `uv`; could ship a PyInstaller-frozen `.exe` for non-Python users.
