#pragma once

#include "cedar/vm/vm.hpp"
#include "cedar/dsp/constants.hpp"
#include "akkado/codegen.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace nkido {

class MidiInput;

// SDL2-based audio engine for real-time playback
class AudioEngine {
public:
    struct Config {
        std::uint32_t sample_rate = 48000;
        std::uint32_t buffer_size = cedar::BLOCK_SIZE;
        std::uint32_t channels = 2;
    };

    AudioEngine();
    ~AudioEngine();  // out-of-line so std::unique_ptr<MidiInput> can use a forward decl

    // Non-copyable
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Initialize SDL2 audio with given config
    bool init(const Config& config);

    // Optionally open a capture device (microphone / line-in) feeding the
    // INPUT opcode. Pass nullptr for the system default; otherwise pass an
    // exact device name as reported by `--list-devices`. Returns true if a
    // device was successfully opened. On failure (no device, name not found,
    // permission denied), this returns false and the VM continues with no
    // input — `in()` then returns silence per the audio-input PRD contract.
    // Must be called after init() and before start().
    bool init_capture(const char* device_name);

    // Print a numbered list of available capture devices (and default).
    static void list_capture_devices(std::ostream& out);

    // True when a capture device was successfully opened.
    [[nodiscard]] bool has_input() const { return capture_device_id_ != 0; }

    // Record the preferred MIDI input device name for resolving
    // MidiSourceKind::DefaultDevice entries (bare `midi()`). Empty / nullptr
    // means "use the first available port." Should be called once before
    // start(); apply_midi_route_plan() reads this value on every reload.
    // Per docs/prd-midi-input.md §4.9 (Phase 3).
    void set_preferred_midi_device(const char* preferred_name);

    // Print every available MIDI input port to `out`. Mirrors
    // list_capture_devices.
    static void list_midi_devices(std::ostream& out);

    // Reconcile the open MIDI input ports with `required` from the latest
    // bytecode load. For every entry: call vm.init_midi_queue_state(...) so
    // its SPSC ring is allocated, then group by resolved device name, open
    // any newly-referenced devices, and atomically swap each open device's
    // route table. Existing devices that disappear from the program are
    // kept open with an empty route table (cheap, avoids open/close thrash
    // when a midi() call flickers in/out across hot-swaps).
    void apply_midi_route_plan(const std::vector<akkado::RequiredMidiSource>& required,
                               const std::vector<akkado::RequiredMidiCcRoute>& cc_routes = {});

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

    // Master output gain applied in the audio callback. Thread-safe.
    // Clamped to [0, 2] so a runaway slider can't blow up downstream gear.
    void set_master_volume(float v);
    [[nodiscard]] float master_volume() const {
        return master_volume_.load(std::memory_order_relaxed);
    }

    // Waveform visualization (thread-safe). Size is enough for the serve-mode
    // oscilloscope to find a trigger point and still display ~1024 samples.
    static constexpr std::size_t WAVEFORM_SIZE = 2048;
    void get_waveform(float* out, std::size_t count) const;

private:
    // SDL2 audio callback (static to match C callback signature)
    static void audio_callback(void* userdata, std::uint8_t* stream, int len);

    // SDL2 capture callback — writes captured stereo frames into capture_ring_.
    static void capture_callback(void* userdata, std::uint8_t* stream, int len);

    cedar::VM vm_;
    Config config_;

    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> initialized_{false};

    // SDL2 device IDs (0 = not initialized)
    std::uint32_t device_id_ = 0;
    std::uint32_t capture_device_id_ = 0;
    std::uint8_t capture_channels_ = 0;     // Number of channels SDL gave us (1 or 2)

    // Waveform ring buffer for visualization
    std::array<float, WAVEFORM_SIZE> waveform_buffer_{};
    std::atomic<std::size_t> waveform_write_pos_{0};

    std::atomic<float> master_volume_{1.0f};

    // Lock-free SPSC ring buffer for captured audio (stereo interleaved L,R,L,R...).
    // Sized for ~170ms at 48kHz so a slow consumer never starves the producer in
    // practice. The capture callback writes; the playback callback reads.
    static constexpr std::size_t CAPTURE_RING_FRAMES = 8192;
    std::array<float, CAPTURE_RING_FRAMES * 2> capture_ring_{};
    std::atomic<std::uint64_t> capture_write_pos_{0};  // Total frames written
    std::atomic<std::uint64_t> capture_read_pos_{0};   // Total frames read

    // Live MIDI inputs (one per physical device). Ports are opened lazily
    // by apply_midi_route_plan() and kept across hot-swaps. The destructor
    // tears down each MidiInput's RtMidiIn before the VM goes away.
    std::vector<std::unique_ptr<MidiInput>> midi_inputs_;
    std::string preferred_midi_device_name_;  // matches MidiSourceKind::DefaultDevice; "" = first available
};

// Global signal flag for Ctrl+C handling
extern std::atomic<bool> g_signal_received;

// Install signal handlers (SIGINT, SIGTERM)
void install_signal_handlers();

}  // namespace nkido
