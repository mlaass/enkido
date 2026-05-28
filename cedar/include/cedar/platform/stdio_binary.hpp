#pragma once

namespace cedar::platform {

// Switch stdin/stdout to binary mode (no CRLF translation).
//
// POSIX: no-op — stdio is binary by default.
// Windows: _setmode(_fileno(stdin), _O_BINARY) and the same for stdout.
//   stderr is left in text mode (diagnostic output, CRLF is fine).
//
// Call once before any read/write on cin/cout if the program speaks a
// strict line protocol (e.g. nkido serve's JSON line stream). Idempotent.
void set_stdio_binary_mode();

} // namespace cedar::platform
