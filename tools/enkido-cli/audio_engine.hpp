#pragma once

#include "cedar/vm/vm.hpp"
#include "cedar/dsp/constants.hpp"
#include <atomic>
#include <array>
#include <cstdint>

namespace enkido {

// SDL2-based audio engine for real-time playback
class AudioEngine {
public:
    struct Config {
        std::uint32_t sample_rate = 48000;
        std::uint32_t buffer_size = cedar::BLOCK_SIZE;
        std::uint32_t channels = 2;
    };

    AudioEngine();
    ~AudioEngine();

    // Non-copyable
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Initialize SDL2 audio with given config
    bool init(const Config& config);

    // Start/stop playback
    bool start();
    void pause();  // Pause playback (can be resumed with start())
    void stop();   // Full stop (closes device, requires re-init)

    // Request graceful shutdown (thread-safe)
    void request_shutdown();

    // Check if shutdown was requested
    [[nodiscard]] bool should_shutdown() const;

    // Access VM for loading programs
    [[nodiscard]] cedar::VM& vm() { return vm_; }
    [[nodiscard]] const cedar::VM& vm() const { return vm_; }

    // Wait for shutdown signal (blocks until request_shutdown called or signal)
    void wait_for_shutdown();

    // Get current config
    [[nodiscard]] const Config& config() const { return config_; }

    // Waveform visualization (thread-safe)
    static constexpr std::size_t WAVEFORM_SIZE = 512;  // ~10ms at 48kHz
    void get_waveform(float* out, std::size_t count) const;

private:
    // SDL2 audio callback (static to match C callback signature)
    static void audio_callback(void* userdata, std::uint8_t* stream, int len);

    cedar::VM vm_;
    Config config_;

    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> initialized_{false};

    // SDL2 device ID (0 = not initialized)
    std::uint32_t device_id_ = 0;

    // Waveform ring buffer for visualization
    std::array<float, WAVEFORM_SIZE> waveform_buffer_{};
    std::atomic<std::size_t> waveform_write_pos_{0};
};

// Global signal flag for Ctrl+C handling
extern std::atomic<bool> g_signal_received;

// Install signal handlers (SIGINT, SIGTERM)
void install_signal_handlers();

}  // namespace enkido
