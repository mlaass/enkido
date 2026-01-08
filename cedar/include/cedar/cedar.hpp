#pragma once

#include <cstdint>
#include <string_view>

namespace cedar {

/// Cedar version information
struct Version {
    static constexpr int major = 0;
    static constexpr int minor = 1;
    static constexpr int patch = 0;

    static constexpr std::string_view string() { return "0.1.0"; }
};

/// Default audio configuration
struct Config {
    std::uint32_t sample_rate = 48000;
    std::uint32_t block_size = 128;
    std::uint32_t channels = 2;
};

/// Initialize Cedar with the given configuration
/// Returns true on success
bool init(const Config& config = {});

/// Shutdown Cedar and release resources
void shutdown();

/// Get the current configuration
const Config& config();

} // namespace cedar
