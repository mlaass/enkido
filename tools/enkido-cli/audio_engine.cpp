#include "audio_engine.hpp"
#include <SDL2/SDL.h>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>

namespace enkido {

// Global signal flag
std::atomic<bool> g_signal_received{false};

static void signal_handler(int signal) {
    (void)signal;
    g_signal_received.store(true, std::memory_order_release);
}

void install_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    stop();
    if (initialized_.load()) {
        SDL_Quit();
    }
}

bool AudioEngine::init(const Config& config) {
    config_ = config;

    // Initialize SDL2 audio subsystem
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "error: SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }
    initialized_.store(true);

    // Configure audio spec
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(config_.sample_rate);
    want.format = AUDIO_F32SYS;  // 32-bit float, system byte order
    want.channels = static_cast<std::uint8_t>(config_.channels);
    want.samples = static_cast<std::uint16_t>(config_.buffer_size);
    want.callback = audio_callback;
    want.userdata = this;

    SDL_AudioSpec have{};
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);

    if (device_id_ == 0) {
        std::cerr << "error: SDL_OpenAudioDevice failed: " << SDL_GetError() << "\n";
        return false;
    }

    // Verify we got what we asked for
    if (have.freq != want.freq || have.format != want.format ||
        have.channels != want.channels) {
        std::cerr << "warning: audio format differs from requested\n";
        std::cerr << "  requested: " << want.freq << " Hz, "
                  << static_cast<int>(want.channels) << " channels\n";
        std::cerr << "  got: " << have.freq << " Hz, "
                  << static_cast<int>(have.channels) << " channels\n";
    }

    // Configure VM
    vm_.set_sample_rate(static_cast<float>(have.freq));

    return true;
}

bool AudioEngine::start() {
    if (device_id_ == 0) {
        std::cerr << "error: audio device not initialized\n";
        return false;
    }

    running_.store(true, std::memory_order_release);
    shutdown_requested_.store(false, std::memory_order_release);

    // Unpause audio device to start playback
    SDL_PauseAudioDevice(device_id_, 0);

    return true;
}

void AudioEngine::pause() {
    if (device_id_ != 0) {
        SDL_PauseAudioDevice(device_id_, 1);
    }
    running_.store(false, std::memory_order_release);
}

void AudioEngine::stop() {
    if (device_id_ != 0) {
        SDL_PauseAudioDevice(device_id_, 1);
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    running_.store(false, std::memory_order_release);
}

void AudioEngine::request_shutdown() {
    shutdown_requested_.store(true, std::memory_order_release);
}

bool AudioEngine::should_shutdown() const {
    return shutdown_requested_.load(std::memory_order_acquire);
}

void AudioEngine::wait_for_shutdown() {
    while (running_.load(std::memory_order_acquire)) {
        // Check for shutdown request or signal
        if (shutdown_requested_.load(std::memory_order_acquire) ||
            g_signal_received.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void AudioEngine::audio_callback(void* userdata, std::uint8_t* stream, int len) {
    auto* engine = static_cast<AudioEngine*>(userdata);
    auto* output = reinterpret_cast<float*>(stream);

    // Check for shutdown request
    if (engine->shutdown_requested_.load(std::memory_order_relaxed) ||
        g_signal_received.load(std::memory_order_relaxed)) {
        // Fill with silence
        std::memset(stream, 0, static_cast<std::size_t>(len));
        engine->running_.store(false, std::memory_order_release);
        return;
    }

    // Calculate number of samples (stereo interleaved)
    std::size_t total_samples = static_cast<std::size_t>(len) / sizeof(float);
    std::size_t num_frames = total_samples / 2;  // Stereo

    // Process in BLOCK_SIZE chunks
    std::size_t offset = 0;
    while (offset < num_frames) {
        std::size_t chunk = std::min(num_frames - offset, cedar::BLOCK_SIZE);

        // VM outputs to separate L/R buffers
        alignas(32) float left[cedar::BLOCK_SIZE]{};
        alignas(32) float right[cedar::BLOCK_SIZE]{};

        engine->vm_.process_block(left, right);

        // Interleave to output buffer
        for (std::size_t i = 0; i < chunk; ++i) {
            output[(offset + i) * 2]     = left[i];
            output[(offset + i) * 2 + 1] = right[i];
        }

        offset += chunk;
    }
}

}  // namespace enkido
