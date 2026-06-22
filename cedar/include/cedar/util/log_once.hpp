#pragma once

#include <atomic>
#include <cstdio>

// One-shot diagnostic latch. CEDAR_LOG_ONCE(fmt, ...) prints to stderr at most
// once per call site for the life of the process (thread-safe). Used for
// audio-arena exhaustion / free-list overflow so a pathological program logs
// once instead of spamming every 2.67 ms block. Mirrors the existing
// "[CEDAR BUG]" fprintf style; allocates nothing in steady state (the static
// flag is the only state).
#define CEDAR_LOG_ONCE(...)                                                  \
    do {                                                                     \
        static std::atomic<bool> _cedar_logged_once{false};                 \
        if (!_cedar_logged_once.exchange(true)) {                           \
            std::fprintf(stderr, __VA_ARGS__);                               \
        }                                                                    \
    } while (0)
