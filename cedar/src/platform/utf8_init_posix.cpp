#include "cedar/platform/utf8_init.hpp"

namespace cedar::platform {

void ensure_utf8_console() {
    // POSIX terminals are UTF-8 by convention. Nothing to do.
}

} // namespace cedar::platform
