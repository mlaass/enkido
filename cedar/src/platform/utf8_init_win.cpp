#include "cedar/platform/utf8_init.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace cedar::platform {

void ensure_utf8_console() {
    // Both calls return FALSE when no console is attached (e.g. detached
    // child process). The UTF-8 activeCodePage manifest baked into the
    // executable handles ANSI APIs regardless, so we ignore failures.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

} // namespace cedar::platform
