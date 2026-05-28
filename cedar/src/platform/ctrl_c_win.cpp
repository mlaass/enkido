#include "cedar/platform/ctrl_c.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

namespace cedar::platform {

namespace {

std::atomic<CtrlCCallback> g_cb{nullptr};

BOOL WINAPI win_console_handler(DWORD evt) {
    if (evt == CTRL_C_EVENT || evt == CTRL_BREAK_EVENT || evt == CTRL_CLOSE_EVENT) {
        if (auto cb = g_cb.load(std::memory_order_acquire)) {
            cb();
        }
        return TRUE;
    }
    return FALSE;
}

} // namespace

void install_ctrl_c_handler(CtrlCCallback cb) {
    g_cb.store(cb, std::memory_order_release);
    SetConsoleCtrlHandler(win_console_handler, TRUE);
}

} // namespace cedar::platform
