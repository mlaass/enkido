> **Status: IN PROGRESS — Phases 1, 2, 3 (test infra), 3.5a (test_types UAF) & 3.5b (snapshot CRLF) DONE on Windows dev box; 3.5c–d (SoundFont URI, file:// drive-letter) pending** — Phase 1 source-level readiness in `1412a0b`; Phase 2 platform abstractions end-to-end-verified (cedar_tests green, render + serve JSON + UTF-8 paths). Phase 3 (2026-06-13, `b11fff8`) made every test binary build, link, and run: cedar_tests 331/331, akkado_tests 1145/1167, nkido_cli_tests 19/23. Phase 3.5a (2026-06-13, `345371a`) restored the `[stereo-native]` 70-case group via a test-side UAF fix (predicted struct-packing hypothesis was wrong). Phase 3.5b (2026-06-19) added `.gitattributes` pinning snapshot/fixture line endings to LF — akkado_tests now **1167/1167 cases, 321 401 assertions all pass on Windows**. The remaining failures live in nkido_cli_tests (3.5c, 3.5d). Phases 4 (CI) and 5 (release zip) remain.

# PRD: Windows Port of Nkido Executables

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

1. Fix the remaining Windows-blocking source code in `tools/nkido/` (currently `<unistd.h>` is included unconditionally in two files).
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
| `tools/nkido/asset_loader.cpp::executable_dir()` Windows branch | **Works** | `GetModuleFileNameA` |
| SDL2 `WIN32`-gated `SDL2main` link | **Works** | Already in `tools/nkido/CMakeLists.txt:67-70` |
| RtMidi Windows backend (winmm) | **Works** | Built by FetchContent unconditionally; rtmidi enables winmm on Windows automatically |

### 2.2 What's broken or missing

| Item | Problem | Where |
|------|---------|-------|
| `<unistd.h>` included unconditionally | MSVC has no `<unistd.h>`; build fails immediately | `tools/nkido/tests/test_serve.cpp:27` (note: `tools/nkido/asset_loader.cpp:19-25` already correctly guards `<unistd.h>` behind `#if defined(__linux__)`) |
| `signal(SIGINT)` is racy on Windows | MSVC delivers SIGINT on a separate thread; audio thread can race during shutdown | `tools/nkido/audio_engine.cpp:24-27` (declared `audio_engine.hpp:161`; called from `main.cpp:643` and `serve_mode.cpp:1133`) |
| `std::cin` / `std::cout` in **text mode** | Windows CRT translates `\n` ↔ `\r\n` on stdin/stdout — corrupts the serve-mode JSON line protocol if any client sends `\r\n` line endings, and double-newlines our emitted JSON | `tools/nkido/serve_mode.cpp:64, 1182` |
| UTF-8 paths | Default MSVC CRT treats `char*` paths as the system ANSI code page (CP1252 on most installs), so `C:\Users\Joël\…` breaks `std::ifstream` and `fopen` | All file I/O sites that take `std::string` paths |
| `test_serve.cpp` is POSIX-only | Uses `mkstemp`, `close()`, shell command strings with env prefixes — would not even compile on MSVC, let alone run | `tools/nkido/tests/test_serve.cpp:43-60+` |
| No Windows CI | No `.github/workflows/*.yml` builds on `windows-latest` | `.github/workflows/` |
| No Windows release artifacts | `deploy.yml` only produces WASM | `.github/workflows/deploy.yml` |
| SDL2 not bundled | `find_package(SDL2)` works on dev boxes that pre-installed the SDK, but there's no story for downloading it in CI or shipping `SDL2.dll` to end users | `tools/nkido/CMakeLists.txt:3` |

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

**POSIX branch** wraps `std::signal(SIGINT, …)` and `std::signal(SIGTERM, …)` (current behavior in `tools/nkido/audio_engine.cpp:24-27`).

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

**Approach (A):** keep the existing `nkido::install_signal_handlers()` wrapper in `audio_engine.cpp` — both call sites (`main.cpp:643`, `serve_mode.cpp:1133`) stay unchanged — but rewrite its body to delegate to `cedar::platform::install_ctrl_c_handler([]{ g_signal_received.store(true, std::memory_order_release); })`. Minimal call-site churn; preserves the `nkido::` public API in `audio_engine.hpp:161`.

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
<!-- tools/nkido/nkido.manifest -->
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application>
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    </windowsSettings>
  </application>
</assembly>
```

A matching `tools/akkado/akkado.manifest`.

Embed via CMake (MSVC-only):
```cmake
if(WIN32)
    target_sources(nkido_cli PRIVATE nkido.manifest)
    target_sources(akkado_cli PRIVATE akkado.manifest)
endif()
```

CMake auto-embeds `.manifest` sources for MSVC targets since **CMake 3.4**. The project's top-level `CMakeLists.txt` already requires a newer CMake, so no version bump is needed. Both the Ninja generator (used by our CI matrix — see §3.7) and the Visual Studio generator handle manifest embedding identically via the linker's `/MANIFEST` flag. No `/MANIFESTINPUT` fallback required.

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

Keep `find_package(SDL2)` in `tools/nkido/CMakeLists.txt` unchanged. Add a CI step that downloads the official SDL2 dev SDK zip, extracts it, and points `SDL2_DIR` (or `CMAKE_PREFIX_PATH`) at the extracted folder. This matches Option C from the Round 2 decision — predictable, fast, no vcpkg bootstrap, no FetchContent build of SDL2 (which is large).

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

The root `LICENSE` file is also staged via a top-level CMake `install(FILES)` rule that renames it during install so the zip carries the conventional `.txt` extension:

```cmake
# Top-level CMakeLists.txt (Windows-only — keeps the install tree
# tidy for the release zip without leaking into Linux/macOS layouts).
if(WIN32)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/nkido
        RENAME LICENSE.txt)
endif()
```

`scripts/package-windows-zip.ps1` then flattens `share/nkido/LICENSE.txt` into the zip root alongside `README.txt`.

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

New file `.github/workflows/ci.yml`. All Windows jobs pin to `windows-2022` (see §6.7 for rationale) and use the **Ninja** generator — `bin/nkido.exe` lands at a predictable `${CMAKE_BINARY_DIR}/bin/` path (matching Linux), avoiding the `Debug/`/`Release/` config subdir the VS generator inserts. The runner image already includes Ninja.

| Job | Runner | Configure | Build | Test | Upload |
|-----|--------|-----------|-------|------|--------|
| `linux-debug` | `ubuntu-latest` | `--preset debug` | full | `ctest` | — |
| `windows-debug` | `windows-2022` | `--preset debug -G Ninja` + `-DSDL2_DIR=...` | full | `ctest` | — |
| `windows-release` | `windows-2022` | `--preset release -G Ninja` + `-DSDL2_DIR=...` | full | (skip — release strips tests) | `nkido-windows-x64-vX.Y.Z.zip` (tags only) |
| `windows-cedar-only` | `windows-2022` | `--preset cedar-only -G Ninja` | cedar | `ctest` (cedar only) | — |

The existing `deploy.yml` (WASM + Netlify deploy) stays separate so the web build path doesn't depend on the Windows build path. Windows release upload is **a separate step in `ci.yml`** that only runs on `startsWith(github.ref, 'refs/tags/v')` and uses `softprops/action-gh-release` to attach to the same Release page that `deploy.yml` creates.

To avoid a race (both `deploy.yml` and `ci.yml` trying to create the Release), we set the `softprops/action-gh-release` step's `fail_on_unmatched_files: false` and use `append_body: false` — both workflows simply attach to whatever Release exists. If neither has run yet, the first one creates it.

---

## 4. File-Level Changes

### 4.1 Code changes (existing files)

| File | Change |
|------|--------|
| `tools/nkido/asset_loader.cpp` | **No change.** Lines 19-25 already guard `<unistd.h>` behind `#if defined(__linux__)` (verified against current code). Listed here only so future readers don't re-flag it. |
| `tools/nkido/audio_engine.cpp` | Rewrite `install_signal_handlers()` body (lines 24-27) to delegate to `cedar::platform::install_ctrl_c_handler([](){ g_signal_received.store(true, std::memory_order_release); });`. Keeps the `nkido::` wrapper so callers in `main.cpp:643` and `serve_mode.cpp:1133` stay unchanged. |
| `tools/nkido/serve_mode.cpp` | Call `cedar::platform::set_stdio_binary_mode()` early in `run_serve_mode()` (before the existing `install_signal_handlers()` invocation at line 1133). |
| `tools/nkido/main.cpp` | Call `cedar::platform::ensure_utf8_console()` as the first statement of `main()`. |
| `tools/akkado/main.cpp` | Same: call `cedar::platform::ensure_utf8_console()` first. |
| `tools/nkido/tests/test_serve.cpp` | Replace POSIX-specific subprocess/`mkstemp` machinery with a portable equivalent. Concrete plan in §5 Phase 3 (Option A: `std::filesystem::temp_directory_path()` + `_popen`/`popen` behind `tests/test_subprocess.hpp`; Option B fallback: `[!mayfail][windows]`). |
| `tools/nkido/CMakeLists.txt` | Add `WIN32`-gated `target_sources(... nkido.manifest)`, `install(FILES ${SDL2_RUNTIME_LIBRARY} DESTINATION ${CMAKE_INSTALL_BINDIR})`, and a `POST_BUILD` `copy_if_different` of `SDL2.dll` next to `nkido.exe` in the build tree (so `ctest` works in-place). |
| `tools/akkado/CMakeLists.txt` | Add `WIN32`-gated `target_sources(... akkado.manifest)`. |
| `CMakeLists.txt` (top-level) | Add `WIN32`-gated `install(FILES LICENSE DESTINATION share/nkido RENAME LICENSE.txt)` so the release zip carries the conventional `.txt` extension (see §3.6). |
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
| `tools/nkido/nkido.manifest` | UTF-8 `activeCodePage` manifest, embedded in the exe via `target_sources`. |
| `tools/akkado/akkado.manifest` | Same for `akkado`. |
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
| `tools/nkido/asset_loader.cpp::executable_dir()` (line 588+) | `GetModuleFileNameA` branch already correct. |
| `tools/nkido/CMakeLists.txt:67-70` | `SDL2main` link guard already correct. |
| `cmake/Dependencies.cmake` | rtmidi's FetchContent already handles winmm on Windows automatically. |
| `web/` | Out of scope. |
| `tools/midi2akk/src/` | Python; Windows-works today. README update only. |

---

## 5. Implementation Phases

### Phase 1 — Source-level Windows readiness (no CI yet) — **DONE (2026-05-29, `1412a0b`)**

**Goal:** A fresh MSVC `cmake --build` succeeds for `nkido`, `akkado`, `cedar_tests`, `akkado_tests` on a Windows developer machine. `nkido_tests` (which depends on the still-POSIX `test_serve.cpp`) is **explicitly excluded** from Phase 1 via a new `-DNKIDO_BUILD_NKIDO_TESTS=OFF` flag (default = `NKIDO_BUILD_TESTS`). Phase 3 flips it back on once `test_serve.cpp` has been ported.

**Files:** all changes in §4.1 + §4.2 *except* `.github/workflows/ci.yml`.

**Verification (achieved on this box):** `cmake --preset debug -G Ninja` + `cmake --build build/debug` produces `nkido.exe` (12 MB) and `akkado.exe` (8 MB) under MSVC 14.44 / Ninja / SDL2 2.30.10 / OpenSSL 3.5 (vcpkg). `nkido --help`, `akkado --help`, `nkido render --help` all exit 0.

**Pre-existing MSVC blockers also fixed in the same commit** (necessary for the build):
- `scale_quantize.ak` (94 KB) > MSVC string-literal limit (error C2026) — `cmake/generate_stdlib_files.cmake` now chunks long `.ak` files into ≤12 KB raw-string segments at line boundaries.
- `__builtin_ctz` → `std::countr_zero` in `akkado/src/codegen_patterns.cpp:1074`.
- 4 SDL include sites: `<SDL2/SDL.h>` → `<SDL.h>` (works on both layouts).
- `bitmap_font.hpp`: missing `<cstddef>` for `std::size_t`.
- `/STACK:8388608` (8 MB) on MSVC linker — `cedar::VM` static init overflowed the default 1 MB Windows stack.

**Commit:** `1412a0b` — `feat(windows): source-level MSVC readiness for nkido and akkado` (24 files, +282/-17).

### Phase 2 — Platform abstractions integrated — **DONE (2026-06-05, verified on Windows dev box)**

**Goal:** UTF-8 console works, Ctrl+C exits cleanly, serve mode stdin/stdout is binary-mode.

**Files:** Wiring landed together with the abstractions in Phase 1 commit `1412a0b` — `serve_mode.cpp` calls `cedar::platform::set_stdio_binary_mode()` before `install_signal_handlers()`; both CLI `main()`s call `cedar::platform::ensure_utf8_console()` first; `install_signal_handlers()` in `audio_engine.cpp:19-23` delegates to `cedar::platform::install_ctrl_c_handler`. Phase 2 itself = end-to-end verification.

**Verification (this box, 2026-06-05):**
- **cedar_tests:** 330 test cases / 341,522 assertions all pass (5 SoundFont-fixture tests skipped — fixture not committed).
- **akkado_tests:** crashes with `STACK_OVERFLOW` (`0xC00000FD`) even on `--help` — Catch2 static-init exceeds the 8 MB linker stack. Confirmed as the **known Phase 3 issue**.
- **nkido render:** `hello-sine.akk` → `test-hello.wav` (384,044 B = 2 s stereo float at 48 kHz, exact). `-v` reports `Rendered 1s (375 blocks, 48000 samples)`.
- **UTF-8 path test:** Created `build\debug\utf8-é-é.akk`, rendered to `build\debug\utf8-out-é.wav` (192,044 B) — UTF-8 manifest is routing ANSI APIs through CP_UTF8.
- **serve JSON round-trip:** Piped `{"cmd":"load",...}\n{"cmd":"quit"}\n` through `cmd /c "... < in.txt > out.txt"`. Got `{"event":"ready"}\n{"event":"compiled","ok":true}\n` back. **Stdout bytes confirmed LF-only (no `0x0D` anywhere)** — `set_stdio_binary_mode()` is suppressing CRLF translation as designed.
- **Ctrl+C handler:** Manually verified by user in an interactive PowerShell: launched `nkido serve`, pasted the load command, heard the 440 Hz tone (audio callback first-fired at `len=1024`), hit `Ctrl+C` in the console — process exited promptly back to the prompt, no stuck audio. (Note: Ctrl+C while the SDL window has foreground focus does nothing — expected, since the console handler only fires when the console has focus.) Automated test from Claude session not possible — every `GenerateConsoleCtrlEvent` harness variant (CREATE_NEW_PROCESS_GROUP, AttachConsole/FreeConsole) killed the parent shell. EOF-on-stdin proxy independently confirmed: `nkido serve < NUL` exits cleanly with code 0 in **273 ms**, exercising the same `g_signal_received` atomic check in the main loop (`serve_mode.cpp:1218`).
- **Linux:** No behavior change expected — POSIX implementations are `signal()` no-op wrappers. Not re-run here.

**Commit:** No new commit required — wiring already in `1412a0b`. Phase 2 = verification of that commit. Next commit lands with Phase 3.

**Carried forward to Phase 3:** (a) port `tools/nkido/tests/test_serve.cpp` off POSIX, (b) fix the `akkado_tests` Catch2 static-init stack overflow (raise `/STACK` further, defer test registration, or split the binary).

### Phase 3 — Test infra ported — **DONE (test runners) (2026-06-13)** / **PARTIAL (functional pass-rate)**

**Goal as shipped:** every test binary on Windows now builds, links, loads, and runs to completion. `ctest` exit-0 is held back by ~26 pre-existing Windows compat bugs in non-test code (URI parsing, struct layout, snapshot CRLF) that were hidden until Phase 3 unblocked the runners — tracked separately as Phase 3.5 below.

**Files shipped:**
- `tools/nkido/tests/test_subprocess.hpp` (new) — `ScopedEnv` (movable RAII env-var guard), `ScopedTempFile` (PID + atomic-counter naming under `std::filesystem::temp_directory_path()`), `quote_for_shell`. Header-only, ~110 LOC, `#if defined(_WIN32)` branches around `_putenv_s`/`setenv` and `_dupenv_s`/`getenv`.
- `tools/nkido/tests/test_serve.cpp` — `<unistd.h>` / `mkstemp` / shell `VAR=val cmd` prefix all gone; the 4 call sites that used to pass an `env_prefix` string now pass `std::vector<std::pair<std::string,std::string>>`. Test logic unchanged.
- `akkado/tests/akkado_test_main.cpp` (new) — custom Catch2 main that runs `session.run()` on a worker thread for stack uniformity. Note: Catch2 v3.5.2's `TEST_CASE` macros still register via static-init `AutoReg` on the main thread regardless, so this is hygiene rather than a stack-overflow fix.
- `cmake/CompilerOptions.cmake` — MSVC `/STACK:8388608` → `/STACK:33554432` (32 MB). This is what actually unblocks `akkado_tests` static-init.
- `akkado/tests/CMakeLists.txt` — `akkado_test_main.cpp` first in source list; link `Catch2::Catch2` instead of `Catch2WithMain`; `WIN32` POST_BUILD copy of `$<TARGET_FILE:Catch2::Catch2>` because vcpkg's `BUILD_SHARED_LIBS=ON` builds Catch2 as a DLL and `applocal.ps1` doesn't see in-build targets.
- `cedar/tests/CMakeLists.txt` + `tools/nkido/tests/CMakeLists.txt` — same `WIN32` POST_BUILD copy of `Catch2::Catch2` **and** `Catch2::Catch2WithMain` DLLs. Both binaries link `Catch2WithMain` which depends on the Catch2 DLL.
- `akkado/tests/test_hot_swap_determinism.cpp` — `#define _USE_MATH_DEFINES` at the top of the TU (must precede any `<cmath>` include, including transitive ones from Catch2). Pre-existing MSVC gap; only became visible at the link stage.

**Verification on Windows dev box (2026-06-13):**
- `akkado_tests --help` exits 0 (previously 0xC00000FD on static-init). **Static-init crash fixed.**
- `cedar_tests`: **331/331 pass** (5 SoundFont-fixture tests skipped — fixture not committed). 357 452 assertions, ~206 s.
- `akkado_tests`: 1167 cases / 321 401 assertions discovered. **1145 / 22 fail-by-case, 321 349 / 52 fail-by-assertion.** Test runner works end-to-end; failures are in newly-exposed Windows compat bugs (see §10).
- `nkido_cli_tests`: 23 cases discovered (previously didn't compile). **19 / 4 fail-by-case, 100 / 5 fail-by-assertion.** Test harness (subprocess + env-var + temp-file plumbing) verified working — failures are downstream URI/SoundFont resolution bugs.
- `NKIDO_BUILD_NKIDO_TESTS` flag stays as a downstream opt-out (default = `${NKIDO_BUILD_TESTS}`).

**Not done in Phase 3:** Linux regression smoke test of the new RAII helpers. `ScopedEnv`/`ScopedTempFile` POSIX paths are straightforward (`setenv`/`unsetenv`, `temp_directory_path()`) but should be eyeballed on Linux before the Phase 4 CI lights up.

**Commit:** `test(windows): port test_serve.cpp to MSVC; bump /STACK to 32 MB; copy Catch2 DLLs alongside test exes`

### Phase 3.5 — Windows compat bugs newly exposed by Phase 3

Phase 3 unblocked the runners and surfaced ~26 functional Windows bugs that were hidden because `akkado_tests` crashed before any test could execute, and `nkido_cli_tests` couldn't compile. None of these are Phase 3 regressions; they are pre-existing gaps in cedar/akkado that the Phase 1+2 source-level port didn't cover. Splitting them out keeps Phase 3 closeable and lets Phase 4 (CI) light up green-or-explicit-allowlist on schedule.

**3.5a — `akkado/tests/test_types.cpp` Instruction flags + rate (21 cases, 44 asserts) — DONE (2026-06-13, `345371a`)**

Original observation: `op->flags & cedar::InstructionFlag::STEREO_OUTPUT != 0` → `0 != 0`, `STEREO_INPUT == 0` → `1 == 0`, and `op->rate == 1` → `'\x01' == 1` displaying as `'?'`. Affected the 70 `[stereo-native]` cases.

**Actual root cause** (the predicted struct-packing / bitfield hypothesis was wrong): **use-after-free in the test helpers** at `akkado/tests/test_types.cpp`. The pattern `find_instruction(get_instructions(result), op)` returned a pointer into a `std::vector<cedar::Instruction>` *temporary* that was destroyed at end of the full expression. Linux GCC's release allocator returned the freed bytes to a free-list without overwriting, so the CHECKs read the still-valid pattern. MSVC's debug allocator immediately reused/stomped the freed region, so `op->flags` and `op->rate` read garbage. Diagnostic printfs confirmed codegen wrote `flags=0x0002` and the on-disk `.cedar` bytecode had `02 00` at offset 14 — the bug was on the read side, not the write side.

Fix (test-side only, +79/-36 in `akkado/tests/test_types.cpp`):
- Added a deleted rvalue overload `static const cedar::Instruction* find_instruction(std::vector<cedar::Instruction>&&, cedar::Opcode) = delete;` so any future temporary-vector use is a compile-time error.
- Rewrote 36 call sites to the safe two-line `auto insts = get_instructions(result); auto* op = find_instruction(insts, …);` pattern (33 other call sites already used this).

Result on Windows MSVC: `[stereo-native]` 70 cases / 402 assertions all pass (previously 21 cases / 44 asserts failing). No production code touched; no Linux risk (the safe pattern was already the majority).

**3.5b — `akkado/tests/test_bytecode_snapshot.cpp` snapshot mismatches (8 fixtures) — DONE (2026-06-19)**

Original observation: `expected == snapshot` fails for `01_basic_osc.ak` through `08_multiline_mini.ak`. Snapshot files were authored on Linux.

**Actual root cause**: CRLF line endings. The repo had no `.gitattributes` file, so Windows checkouts with `core.autocrlf=true` (which the dev box uses globally) converted every `.disasm` file's `\n` to `\r\n` on checkout. The test reads files in `std::ios::binary` mode (`test_bytecode_snapshot.cpp:27-33`) and compares raw bytes against `render_snapshot`'s LF-only output, so every line mismatched. Path separators were a red herring — `render_snapshot` only embeds bare filenames via `fixture.filename().string()`, no `/` or `\` in the snapshot body.

Fix (one new file + tree refresh):
- Added `.gitattributes` at repo root pinning `akkado/tests/snapshots/*.disasm` and `akkado/tests/fixtures/*.ak` to `text eol=lf`. Fixtures are pinned too so any future codegen change that observes source line endings (e.g. via a string literal) stays deterministic across platforms.
- Re-checked-out the 8 snapshots + 8 fixtures on this dev box to flush the CRLF working copies (`rm … && git checkout HEAD -- …`).

Verification on Windows MSVC (2026-06-19):
- `akkado_tests "[snapshot]"` → all 34 assertions in 1 test case pass.
- Full akkado_tests suite → **1167/1167 cases, 321 401/321 401 assertions all pass** (was 1145/1167 post-Phase-3).
- `[stereo-native]` re-run → still 70/70, 402/402 (3.5a regression check).
- Git working tree clean after re-checkout — no spurious LF-conversion diffs leaked into other files.

No production code touched. Zero Linux risk (Linux checkouts were already LF; the `.gitattributes` rule simply formalizes the invariant).

**3.5c — `tools/nkido/tests/test_render.cpp` SoundFont URI resolution (3 cases)**

`run_cli`/`run_cli_env` calls fail at lines 231, 276, 303 with `error: SoundFont fetch 'gm' failed: File not found: gm`. `tools/nkido/asset_loader.cpp::resolve_soundfont_alias()` searches a path layout that exists on the dev box but the resolution isn't kicking in inside the test invocation — possibly because the test's working directory doesn't include `share/nkido/soundfonts/` relative to the CLI exe path on Windows.

**3.5d — `tools/nkido/tests/test_serve.cpp` `file://C:/…` URI parsing (1 case)**

The "bare sample name resolves via built-in default kit" test (line 174–176) fails because `file:///C:/Users/moritz/…/default_kit_minimal.strudel.json` gets reduced to `/C:/Users/moritz/…` by the URI handler, then `fopen("/C:/…")` returns ENOENT. Windows `file://` URLs need the host-relative `/` stripped when the next character is a drive letter. Bug lives in cedar's URI handler (likely `cedar/src/io/handlers/file_handler.cpp` or wherever `file://` schemes are normalized). My Phase 3 subprocess harness is verified clean here — the test framework round-trip works end-to-end.

**Verification target for 3.5:** `ctest --output-on-failure` exit 0 on Windows. Estimated scope: 3.5a is the biggest unknown (struct layout audit + possible codegen fix); 3.5b and 3.5d are <50 LOC each; 3.5c is one path-resolution branch.

**Commits:** one per sub-bullet (3.5a–d), each with its own root-cause analysis.

### Phase 4 — CI on every push/PR

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

### 6.3a Console close-button (`CTRL_CLOSE_EVENT`)

When the user clicks the console window's red ❌, Windows fires `CTRL_CLOSE_EVENT` and (because our handler returns `TRUE`) gives the process a **~5 second grace period** to self-exit before it is forcibly killed. The POSIX path has no analog — SIGINT/SIGTERM simply set the atomic flag and the main loop notices on its next iteration.

Expected behavior on Windows: handler sets `g_signal_received`, the main loop's poll picks it up within one audio block (≤2.67 ms at 48 kHz / 128-sample blocks), serve_mode tears down SDL audio and MIDI, then returns from `run_serve_mode()`. Total shutdown latency budget: well under the 5 s grace window.

Risk: if some opcode's destructor blocks (e.g. a buggy IO handler), Windows force-kills the process and leaves `SDL2` audio device handles in the kernel. Mitigation: keep destructors short; rely on the OS to reclaim audio devices on process termination either way.

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

| Phase | Test | Status |
|-------|------|--------|
| 1 | Manual `cmake --build` on a Windows dev box; `--help` runs on both executables. | ✅ done 2026-05-29 (`1412a0b`) |
| 2 | Round-trip Ctrl+C and UTF-8 path tests on Windows; existing Linux tests unchanged. | ✅ done 2026-06-05 (incl. manual interactive Ctrl+C confirmation in PowerShell — exits promptly, no stuck audio) |
| 3 | `ctest` exit 0 on Windows for `cedar_tests`, `akkado_tests`, `nkido_cli_tests`. | ⏳ runners ship 2026-06-13 (cedar_tests 331/331, akkado_tests 1145/1167, nkido_cli_tests 19/23) but ctest exit-0 awaits Phase 3.5 functional fixes |
| 3.5a | `test_types.cpp` `[stereo-native]` 70 cases green on Windows. | ✅ done 2026-06-13 (`345371a`) — fix was test-side UAF, not the predicted struct-packing |
| 3.5b | `test_bytecode_snapshot.cpp` 8 fixtures green on Windows. | ✅ done 2026-06-19 — `.gitattributes` pins `.disasm`/`.ak` to `eol=lf`; akkado_tests now 1167/1167 |
| 3.5c–d | SoundFont URI + `file://` drive-letter fixes → nkido_cli_tests 100% green on Windows. | ⏳ pending |
| 4 | All 4 CI jobs green on a no-op PR. | ⏳ pending |
| 5 | Pre-release tag → downloadable zip → smoke-test on clean Win11 VM. | ⏳ pending |

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
