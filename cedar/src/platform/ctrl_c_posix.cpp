#include "cedar/platform/ctrl_c.hpp"

#include <atomic>
#include <csignal>

namespace cedar::platform {

namespace {

std::atomic<CtrlCCallback> g_cb{nullptr};

void posix_signal_handler(int /*signum*/) {
    if (auto cb = g_cb.load(std::memory_order_acquire)) {
        cb();
    }
}

} // namespace

void install_ctrl_c_handler(CtrlCCallback cb) {
    g_cb.store(cb, std::memory_order_release);
    std::signal(SIGINT, posix_signal_handler);
    std::signal(SIGTERM, posix_signal_handler);
}

} // namespace cedar::platform
