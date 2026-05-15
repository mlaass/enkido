#include "audio_engine.hpp"
#include "midi_input.hpp"
#include <SDL2/SDL.h>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <utility>

namespace nkido {

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
    // Close MIDI ports BEFORE stop() so any in-flight MIDI callback can
    // finish before vm_ goes away. RtMidiIn::cancelCallback joins the
    // backend thread.
    midi_inputs_.clear();
    stop();
    if (initialized_.load()) {
        SDL_Quit();
    }
}

void AudioEngine::list_capture_devices(std::ostream& out) {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        out << "error: SDL_Init failed: " << SDL_GetError() << "\n";
        return;
    }

    int count = SDL_GetNumAudioDevices(/*iscapture=*/1);
    out << "Capture devices (" << count << "):\n";
    out << "  default: <system default>\n";
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(i, /*iscapture=*/1);
        out << "  " << i << ": " << (name ? name : "<unknown>") << "\n";
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioEngine::init_capture(const char* device_name) {
    if (!initialized_.load()) {
        std::cerr << "error: init_capture called before init()\n";
        return false;
    }

    SDL_AudioSpec want{};
    want.freq = static_cast<int>(config_.sample_rate);
    want.format = AUDIO_F32SYS;
    want.channels = 2;  // request stereo; SDL may downmix from mono devices
    want.samples = static_cast<std::uint16_t>(config_.buffer_size);
    want.callback = capture_callback;
    want.userdata = this;

    SDL_AudioSpec have{};
    capture_device_id_ = SDL_OpenAudioDevice(
        device_name, /*iscapture=*/1, &want, &have,
        SDL_AUDIO_ALLOW_CHANNELS_CHANGE);

    if (capture_device_id_ == 0) {
        std::cerr << "warning: capture device unavailable";
        if (device_name) std::cerr << " (\"" << device_name << "\")";
        std::cerr << ": " << SDL_GetError() << "\n"
                  << "  in() will return silence.\n";
        return false;
    }

    if (have.freq != want.freq) {
        std::cerr << "warning: capture sample rate is " << have.freq
                  << " Hz, requested " << want.freq << " Hz\n";
    }

    capture_channels_ = have.channels;
    if (capture_channels_ != 1 && capture_channels_ != 2) {
        std::cerr << "warning: capture device reports "
                  << static_cast<int>(capture_channels_)
                  << " channels; only 1 or 2 supported. Closing capture.\n";
        SDL_CloseAudioDevice(capture_device_id_);
        capture_device_id_ = 0;
        return false;
    }

    return true;
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

    // Diagnostic: log selected driver + negotiated format so silence bugs
    // (wrong sink, format mismatch, no driver) can be diagnosed from
    // serve's stderr stream.
    const char* driver = SDL_GetCurrentAudioDriver();
    std::fprintf(stderr,
                 "[audio] driver=%s device_id=%u freq=%d ch=%d samples=%u format=0x%04x\n",
                 driver ? driver : "(none)",
                 device_id_,
                 have.freq,
                 static_cast<int>(have.channels),
                 static_cast<unsigned>(have.samples),
                 static_cast<unsigned>(have.format));

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
    if (capture_device_id_ != 0) {
        SDL_PauseAudioDevice(capture_device_id_, 0);
    }

    return true;
}

void AudioEngine::pause() {
    if (device_id_ != 0) {
        SDL_PauseAudioDevice(device_id_, 1);
    }
    if (capture_device_id_ != 0) {
        SDL_PauseAudioDevice(capture_device_id_, 1);
    }
    running_.store(false, std::memory_order_release);
}

void AudioEngine::stop() {
    if (device_id_ != 0) {
        SDL_PauseAudioDevice(device_id_, 1);
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    if (capture_device_id_ != 0) {
        SDL_PauseAudioDevice(capture_device_id_, 1);
        SDL_CloseAudioDevice(capture_device_id_);
        capture_device_id_ = 0;
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

void AudioEngine::capture_callback(void* userdata, std::uint8_t* stream, int len) {
    auto* engine = static_cast<AudioEngine*>(userdata);
    const auto* input = reinterpret_cast<const float*>(stream);

    const std::size_t bytes = static_cast<std::size_t>(len);
    const std::size_t total_floats = bytes / sizeof(float);
    const std::uint8_t channels = engine->capture_channels_;
    if (channels == 0) return;
    const std::size_t num_frames = total_floats / channels;

    std::uint64_t write_pos = engine->capture_write_pos_.load(std::memory_order_relaxed);
    const std::uint64_t read_pos = engine->capture_read_pos_.load(std::memory_order_acquire);
    constexpr std::size_t cap = CAPTURE_RING_FRAMES;

    for (std::size_t i = 0; i < num_frames; ++i) {
        // Drop frames if the consumer is too far behind to avoid overruns
        // (silently — the consumer's ring offset will catch up next block).
        if (write_pos - read_pos >= cap) break;

        std::size_t slot = (write_pos % cap) * 2u;
        if (channels == 1) {
            float s = input[i];
            engine->capture_ring_[slot]     = s;
            engine->capture_ring_[slot + 1] = s;
        } else {
            engine->capture_ring_[slot]     = input[i * 2];
            engine->capture_ring_[slot + 1] = input[i * 2 + 1];
        }
        ++write_pos;
    }
    engine->capture_write_pos_.store(write_pos, std::memory_order_release);
}

void AudioEngine::audio_callback(void* userdata, std::uint8_t* stream, int len) {
    auto* engine = static_cast<AudioEngine*>(userdata);
    auto* output = reinterpret_cast<float*>(stream);

    // One-shot diagnostic: confirm the SDL audio thread actually invokes us.
    static std::atomic<bool> first_call{true};
    bool expected = true;
    if (first_call.compare_exchange_strong(expected, false)) {
        std::fprintf(stderr, "[audio] callback first-fire len=%d\n", len);
    }

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

        // Drain captured input into per-block L/R buffers (when capture is active).
        // If the producer hasn't filled BLOCK_SIZE frames yet, the unfilled tail
        // is silence — preferred over blocking the audio thread.
        alignas(32) float input_l[cedar::BLOCK_SIZE]{};
        alignas(32) float input_r[cedar::BLOCK_SIZE]{};
        if (engine->capture_device_id_ != 0) {
            std::uint64_t read_pos = engine->capture_read_pos_.load(std::memory_order_relaxed);
            const std::uint64_t write_pos = engine->capture_write_pos_.load(std::memory_order_acquire);
            constexpr std::size_t cap = CAPTURE_RING_FRAMES;
            for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
                if (read_pos < write_pos) {
                    std::size_t slot = (read_pos % cap) * 2u;
                    input_l[i] = engine->capture_ring_[slot];
                    input_r[i] = engine->capture_ring_[slot + 1];
                    ++read_pos;
                } else {
                    // Underrun — leave remaining samples zero.
                    break;
                }
            }
            engine->capture_read_pos_.store(read_pos, std::memory_order_release);
            engine->vm_.set_input_buffers(input_l, input_r);
        } else {
            engine->vm_.set_input_buffers(nullptr, nullptr);
        }

        // VM outputs to separate L/R buffers
        alignas(32) float left[cedar::BLOCK_SIZE]{};
        alignas(32) float right[cedar::BLOCK_SIZE]{};

        engine->vm_.process_block(left, right);

        const float gain = engine->master_volume_.load(std::memory_order_relaxed);

        // Interleave to output buffer
        for (std::size_t i = 0; i < chunk; ++i) {
            output[(offset + i) * 2]     = left[i] * gain;
            output[(offset + i) * 2 + 1] = right[i] * gain;
        }

        // Capture waveform for visualization (mix to mono, post-gain so the
        // on-screen meter matches what the user hears).
        std::size_t write_pos = engine->waveform_write_pos_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < chunk; ++i) {
            float mono = (left[i] + right[i]) * 0.5f * gain;
            engine->waveform_buffer_[write_pos % WAVEFORM_SIZE] = mono;
            write_pos++;
        }
        engine->waveform_write_pos_.store(write_pos % WAVEFORM_SIZE, std::memory_order_release);

        offset += chunk;
    }
}

void AudioEngine::set_master_volume(float v) {
    if (!(v == v)) v = 1.0f;            // NaN guard
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    master_volume_.store(v, std::memory_order_relaxed);
}

void AudioEngine::get_waveform(float* out, std::size_t count) const {
    if (count > WAVEFORM_SIZE) count = WAVEFORM_SIZE;
    std::size_t pos = waveform_write_pos_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t idx = (pos + WAVEFORM_SIZE - count + i) % WAVEFORM_SIZE;
        out[i] = waveform_buffer_[idx];
    }
}

// ---------------------------------------------------------------------------
// MIDI input plumbing (PRD docs/prd-midi-input.md §4.9, Phase 3).
// ---------------------------------------------------------------------------

void AudioEngine::set_preferred_midi_device(const char* preferred_name) {
    preferred_midi_device_name_ = preferred_name ? preferred_name : "";
}

void AudioEngine::list_midi_devices(std::ostream& out) {
    MidiInput::list_ports(out);
}

void AudioEngine::apply_midi_route_plan(
    const std::vector<akkado::RequiredMidiSource>& required) {

    // Phase 1: ensure every state has its SPSC ring + OutputEvents buffer.
    // Idempotent on hot-swap (same state_id → same ring; held notes survive).
    for (const auto& req : required) {
        vm_.init_midi_queue_state(
            req.state_id,
            req.kind,
            req.name_or_path.empty() ? nullptr : req.name_or_path.c_str(),
            req.channel_filter,
            req.loop,
            req.tempo_mode);
    }

    // Phase 2: group routes by resolved device-name. DefaultDevice entries
    // collapse to preferred_midi_device_name_ (empty string = "first
    // available", which open("") handles).
    std::unordered_map<std::string, MidiRouteTable> wanted;
    for (const auto& req : required) {
        std::string device;
        if (req.kind == cedar::MidiSourceKind::File) {
            // .mid file playback ships in Phase 5; ignore for routing.
            continue;
        }
        device = (req.kind == cedar::MidiSourceKind::DefaultDevice)
                     ? preferred_midi_device_name_
                     : req.name_or_path;
        wanted[device].push_back({req.state_id, req.channel_filter});
    }

    // Phase 3: install routes. For each wanted device, find or open a
    // MidiInput and swap its route table.
    for (auto& [device, routes] : wanted) {
        MidiInput* found = nullptr;
        for (auto& in : midi_inputs_) {
            if (in->is_open() && in->match_substr() == device) {
                found = in.get();
                break;
            }
        }
        auto table = std::make_shared<const MidiRouteTable>(std::move(routes));
        if (found) {
            found->set_route_table(table);
            continue;
        }
        auto in = std::make_unique<MidiInput>(vm_);
        if (in->open(device)) {
            std::cerr << "[midi] opened '" << in->port_name() << "'"
                      << " for " << table->size() << " route(s)\n";
            in->set_route_table(table);
            midi_inputs_.push_back(std::move(in));
        } else {
            std::cerr << "warning: midi: no input device matching \""
                      << device << "\" — events for "
                      << table->size() << " route(s) will be silent\n";
            // Don't keep a closed MidiInput around — its destructor would
            // run a no-op close(), and a future hot-swap that adds a real
            // device for the same name will open one then.
        }
    }

    // Phase 4: drain devices no longer referenced by clearing their route
    // tables (callbacks become no-ops). Keep ports open across hot-swaps to
    // avoid open/close thrash when a midi() call temporarily disappears.
    for (auto& in : midi_inputs_) {
        if (!in->is_open()) continue;
        if (wanted.find(in->match_substr()) == wanted.end()) {
            in->set_route_table(std::make_shared<const MidiRouteTable>());
        }
    }
}

}  // namespace nkido
