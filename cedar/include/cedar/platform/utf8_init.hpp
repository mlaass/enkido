#pragma once

namespace cedar::platform {

// Switch the attached console to UTF-8 input/output.
//
// POSIX: no-op — terminals are UTF-8 by convention.
// Windows: SetConsoleOutputCP(CP_UTF8) + SetConsoleCP(CP_UTF8).
//   Failures (e.g. when no console is attached) are ignored silently;
//   the executable's UTF-8 activeCodePage manifest already covers
//   ANSI-API paths regardless of console state.
//
// Call once at the very top of main() before any console I/O.
void ensure_utf8_console();

} // namespace cedar::platform
