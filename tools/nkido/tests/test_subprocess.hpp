#pragma once

// Tiny portable RAII helpers for integration tests that need to spawn the
// nkido binary with controlled env vars and capture its stdout/stderr.
// Used by test_serve.cpp; sufficient for any future test that wants the
// same pattern.
//
// Design choices:
//   * Subprocess invocation stays on `std::system` with shell-redirection
//     syntax (`< in > out 2>&1`) which `sh` and `cmd.exe` both parse
//     identically. No pipes, no process-spawning library.
//   * Env vars are set in the parent via setenv / _putenv_s; an RAII guard
//     restores the prior value on destruction so test cases don't leak
//     state into one another.
//   * Temp files live under std::filesystem::temp_directory_path() and use
//     a PID + atomic counter for uniqueness. Tests are sequential so a
//     plain counter is enough; no mkstemp race-avoidance needed.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#  include <process.h>  // _getpid
#else
#  include <unistd.h>   // getpid
#endif

namespace nkido::test {

inline int current_pid() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

inline std::optional<std::string> get_env(const char* name) {
#if defined(_WIN32)
    // _dupenv_s avoids the deprecated-getenv warning under MSVC and
    // returns a heap-allocated copy we own.
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) {
        return std::nullopt;
    }
    std::string value(buf);
    std::free(buf);
    return value;
#else
    const char* v = std::getenv(name);
    if (v == nullptr) return std::nullopt;
    return std::string(v);
#endif
}

inline bool set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
    return _putenv_s(name, value.c_str()) == 0;
#else
    return ::setenv(name, value.c_str(), 1) == 0;
#endif
}

inline bool unset_env(const char* name) {
#if defined(_WIN32)
    // On Windows, _putenv_s with an empty value removes the variable.
    return _putenv_s(name, "") == 0;
#else
    return ::unsetenv(name) == 0;
#endif
}

// Sets `name = value` on construction; restores the prior state (whether
// previously set to a specific value or unset entirely) on destruction.
// Empty `value` means "unset" — matches the legacy test pattern that
// passed e.g. `NKIDO_DEFAULT_KIT=` to mean "clear it".
// Movable but not copyable so the helper composes with std::vector when
// a caller wants to set several env vars in one go.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value)
        : name_(name), prior_(get_env(name)) {
        if (value.empty()) {
            unset_env(name_);
        } else {
            set_env(name_, value);
        }
    }
    ~ScopedEnv() { restore(); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&& other) noexcept
        : name_(other.name_), prior_(std::move(other.prior_)) {
        other.name_ = nullptr;  // disarm moved-from instance
    }
    ScopedEnv& operator=(ScopedEnv&& other) noexcept {
        if (this != &other) {
            restore();
            name_ = other.name_;
            prior_ = std::move(other.prior_);
            other.name_ = nullptr;
        }
        return *this;
    }

private:
    void restore() {
        if (name_ == nullptr) return;
        if (prior_.has_value()) {
            set_env(name_, *prior_);
        } else {
            unset_env(name_);
        }
    }

    const char* name_;
    std::optional<std::string> prior_;
};

// Reserves a unique path under temp_directory_path(); the file is not
// created here — caller writes to it (or lets a subprocess do so).
// On destruction, std::filesystem::remove silently no-ops if the file
// doesn't exist, matching std::remove's tolerance.
class ScopedTempFile {
public:
    explicit ScopedTempFile(const std::string& prefix) {
        static std::atomic<uint64_t> counter{0};
        const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
        std::error_code ec;
        auto dir = std::filesystem::temp_directory_path(ec);
        // If temp_directory_path() fails we fall back to the CWD — both
        // shells understand a bare filename.
        path_ = (ec ? std::filesystem::path(".") : dir) /
                (prefix + "_" + std::to_string(current_pid()) +
                 "_" + std::to_string(n));
    }
    ~ScopedTempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ScopedTempFile(ScopedTempFile&&) = delete;
    ScopedTempFile& operator=(ScopedTempFile&&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// Wraps a path in double quotes for use in a shell command line. Both
// cmd.exe and POSIX sh accept `"..."` quoting; the temp paths we
// generate never contain `"` or shell-meta characters, so no escaping
// is required.
inline std::string quote_for_shell(const std::filesystem::path& p) {
    return std::string("\"") + p.string() + "\"";
}

}  // namespace nkido::test
