#pragma once

namespace cedar::platform {

using CtrlCCallback = void(*)();

// Install a process-wide Ctrl+C / shutdown handler.
//
// POSIX: wraps std::signal(SIGINT, ...) and std::signal(SIGTERM, ...).
// Windows: registers a SetConsoleCtrlHandler that fires on
//   CTRL_C_EVENT, CTRL_BREAK_EVENT, and CTRL_CLOSE_EVENT.
//
// Idempotent: calling multiple times replaces the previously-installed
// callback (last-call-wins). The callback may be invoked from a
// different thread than the caller and must be async-signal-safe.
void install_ctrl_c_handler(CtrlCCallback cb);

} // namespace cedar::platform
