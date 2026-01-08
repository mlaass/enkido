#include "cedar/cedar.hpp"

namespace cedar {

namespace {
    Config g_config{};
    bool g_initialized = false;
}

bool init(const Config& config) {
    if (g_initialized) {
        return false;
    }

    g_config = config;
    g_initialized = true;
    return true;
}

void shutdown() {
    g_initialized = false;
    g_config = {};
}

const Config& config() {
    return g_config;
}

} // namespace cedar
