#include "cedar/platform/stdio_binary.hpp"

#include <cstdio>
#include <fcntl.h>
#include <io.h>

namespace cedar::platform {

void set_stdio_binary_mode() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    // stderr left in text mode — diagnostics, CRLF is fine.
}

} // namespace cedar::platform
