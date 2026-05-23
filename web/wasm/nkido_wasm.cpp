/**
 * Nkido WASM Bindings
 *
 * Provides a C-style API for Cedar VM and Akkado compiler
 * to be compiled with Emscripten for browser use.
 */

#include <cedar/vm/vm.hpp>
#include <cedar/io/audio_decoder.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/audio/soundfont.hpp>
#include <cedar/generated/opcode_metadata.hpp>
#include <cedar/opcodes/sequencing.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/dsp_state.hpp>
#include <cedar/opcodes/midi.hpp>
#include <akkado/akkado.hpp>
#include <akkado/builtins.hpp>
#include <akkado/builtins_json.hpp>
#include <akkado/sample_registry.hpp>
#include <akkado/pattern_debug.hpp>
#include <akkado/shape_index.hpp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <sstream>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

// Global VM instance (single instance for audio processing)
static std::unique_ptr<cedar::VM> g_vm;

// Shared buffers for audio output
static float g_output_left[128];
static float g_output_right[128];

// Shared buffers for audio input — populated by the AudioWorklet each block
// before cedar_process_block(). When `g_input_active` is false, the VM is
// fed null pointers and the INPUT opcode emits silence.
static float g_input_left[128];
static float g_input_right[128];
static bool g_input_active = false;

// Source string requested by the most recent compile (e.g. "mic", "tab",
// "file:NAME"). Empty string means "use UI default". The host reads this
// after compile to switch input source.
static std::string g_input_source;

// User-selected default MIDI input device (PRD prd-midi-input §4.10). When
// a program contains a bare `midi()` call (MidiSourceKind::DefaultDevice),
// the JS host resolves the route against this name. WASM stores it purely
// as a label for diagnostics / inspection — the actual route-table build
// and event fan-out live on the main thread next to requestMIDIAccess().
static std::string g_default_midi_device;

// Compilation result storage
static akkado::CompileResult g_compile_result;

extern "C" {

// Forward declaration for use in akkado_compile
WASM_EXPORT void akkado_clear_result();

// ============================================================================
// Cedar VM API
// ============================================================================

/**
 * Initialize the Cedar VM
 * Must be called before any other VM functions
 */
WASM_EXPORT void cedar_init() {
    if (!g_vm) {
        g_vm = std::make_unique<cedar::VM>();
    }
}

/**
 * Destroy the Cedar VM
 */
WASM_EXPORT void cedar_destroy() {
    g_vm.reset();
}

/**
 * Set the sample rate
 * @param rate Sample rate in Hz (e.g., 48000)
 */
WASM_EXPORT void cedar_set_sample_rate(float rate) {
    if (g_vm) {
        g_vm->set_sample_rate(rate);
    }
}

/**
 * Set the tempo
 * @param bpm Beats per minute
 */
WASM_EXPORT void cedar_set_bpm(float bpm) {
    if (g_vm) {
        g_vm->set_bpm(bpm);
    }
}

/**
 * Set crossfade duration for hot-swapping
 * @param blocks Number of blocks (2-5, each block is 128 samples)
 */
WASM_EXPORT void cedar_set_crossfade_blocks(uint32_t blocks) {
    if (g_vm) {
        g_vm->set_crossfade_blocks(blocks);
    }
}

/**
 * Load a program (bytecode) into the VM
 * @param bytecode Pointer to bytecode array
 * @param byte_count Size in bytes (must be multiple of sizeof(Instruction) = 16)
 * @return 0 on success, non-zero on error
 */
WASM_EXPORT int cedar_load_program(const uint8_t* bytecode, uint32_t byte_count) {
    if (!g_vm || !bytecode) {
        return -1;
    }

    // Each instruction is 16 bytes
    constexpr size_t INST_SIZE = sizeof(cedar::Instruction);
    if (byte_count % INST_SIZE != 0) {
        return -2;
    }

    size_t inst_count = byte_count / INST_SIZE;
    auto instructions = reinterpret_cast<const cedar::Instruction*>(bytecode);

    // PRD prd-runtime-functions-control-flow L3: if the most recent compile
    // produced FOREACH_EVENT subprogram blocks, stage their table for this
    // load. The bytecode passed here is g_compile_result.bytecode, so the
    // table matches. Programs with no blocks leave the table empty.
    if (!g_compile_result.block_table.empty()) {
        g_vm->set_block_table(g_compile_result.block_table,
                              g_compile_result.main_instruction_count);
    }

    auto result = g_vm->load_program(std::span{instructions, inst_count});

    return static_cast<int>(result);
}

/**
 * Process one block of audio (128 samples)
 * After calling, use cedar_get_output_left/right to get the audio data
 */
WASM_EXPORT void cedar_process_block() {
    if (g_vm) {
        // Wire input pointers based on whether the host has marked input
        // active. set_input_buffers(nullptr, nullptr) makes INPUT emit silence.
        if (g_input_active) {
            g_vm->set_input_buffers(g_input_left, g_input_right);
        } else {
            g_vm->set_input_buffers(nullptr, nullptr);
        }
        g_vm->process_block(g_output_left, g_output_right);
    } else {
        // No VM - output silence
        std::memset(g_output_left, 0, sizeof(g_output_left));
        std::memset(g_output_right, 0, sizeof(g_output_right));
    }
}

/**
 * Get pointer to left channel output buffer (128 floats)
 */
WASM_EXPORT float* cedar_get_output_left() {
    return g_output_left;
}

/**
 * Get pointer to right channel output buffer (128 floats)
 */
WASM_EXPORT float* cedar_get_output_right() {
    return g_output_right;
}

/**
 * Get pointer to left channel input buffer (128 floats).
 * The AudioWorklet writes captured samples directly into this buffer each
 * block before calling cedar_process_block().
 */
WASM_EXPORT float* cedar_get_input_left() {
    return g_input_left;
}

/**
 * Get pointer to right channel input buffer (128 floats).
 */
WASM_EXPORT float* cedar_get_input_right() {
    return g_input_right;
}

/**
 * Mark input buffers as active (1) or inactive (0). When inactive, the
 * INPUT opcode reads null pointers and produces silence.
 */
WASM_EXPORT void cedar_set_input_active(int active) {
    g_input_active = (active != 0);
}

/**
 * Set the input source string requested by the most recent compile.
 * The host reads this on compile completion to switch its capture pipeline
 * (microphone, tab audio, uploaded file). Empty string means "use UI default".
 */
WASM_EXPORT void cedar_set_input_source(const char* source) {
    g_input_source = source ? source : "";
}

/**
 * Get the most recently requested input source string. Returns an empty
 * string when no override was specified.
 */
WASM_EXPORT const char* cedar_get_input_source() {
    return g_input_source.c_str();
}

// ============================================================================
// MIDI Input API (PRD prd-midi-input §4.10)
// ============================================================================
//
// The web host owns Web MIDI permission, device acquisition, and route
// fan-out on the main thread (where requestMIDIAccess() lives). When an
// onmidimessage fires:
//   1) the main thread walks the compile-time route table (built from the
//      `akkado_get_required_midi_source_*` metadata below),
//   2) applies the channel filter,
//   3) posts {type:'midi', state_id, status, d1, d2} to the AudioWorklet,
//   4) the worklet calls cedar_push_midi_event(state_id, ...).
//
// cedar_set_default_midi_device records the user's currently selected
// default device label. The main thread is the source of truth for routing,
// so this slot is informational (diagnostics, state inspection); it lets
// non-JS callers — Python bindings, future debug overlays — read back the
// currently active default device name.

/**
 * Push a channel-voice MIDI event into the queue for the given state_id.
 * Mirrors the RtMidi callback path on the CLI side. Returns 1 on success,
 * 0 on a missing state or full ring (drop-newest policy).
 *
 * @param state_id The MidiQueueState's state_id from the compile result.
 *                 (akkado_get_required_midi_source_state_id(i) for each i.)
 * @param status   MIDI status byte (channel-voice messages 0x80..0xEF).
 * @param d1       First data byte (note number, CC index, etc.).
 * @param d2       Second data byte (velocity, CC value, etc.).
 */
WASM_EXPORT int cedar_push_midi_event(uint32_t state_id,
                                      uint8_t status,
                                      uint8_t d1,
                                      uint8_t d2) {
    if (!g_vm) return 0;
    return g_vm->push_midi_event(state_id, status, d1, d2) ? 1 : 0;
}

// PRD prd-midi-input §7.4: drain file-CC events from a File-kind midi state
// into a caller-provided uint32 buffer (one event per 4 bytes: status, d1,
// d2, channel — packed little-endian). The worklet posts each event back to
// the main thread as a `setParam` so the existing CC route table on
// midi-input.svelte.ts can dispatch it.
//
// `out` must point to an aligned 4-byte-per-event buffer of `max_events`
// capacity; the function returns the number of events written (≤ max_events).
// If `max_events == 0` the function returns 0 without touching `out`.
//
// Channel is 0-indexed (matches MidiQueueState::PendingCc::channel); the
// caller adds +1 before matching against MidiCcRoute::channel_filter.
WASM_EXPORT int32_t cedar_drain_pending_file_ccs(uint32_t state_id,
                                                 uint8_t* out,
                                                 int32_t max_events) {
    if (!g_vm || !out || max_events <= 0) return 0;
    int32_t written = 0;
    g_vm->drain_pending_file_ccs(state_id,
        [&](uint8_t status, uint8_t d1, uint8_t d2, uint8_t channel) {
            if (written >= max_events) return;
            const std::size_t base = static_cast<std::size_t>(written) * 4u;
            out[base + 0] = status;
            out[base + 1] = d1;
            out[base + 2] = d2;
            out[base + 3] = channel;
            ++written;
        });
    return written;
}

/**
 * Record the user-selected default MIDI input device name. Bare `midi()`
 * calls in source resolve against this label on the JS side; WASM keeps it
 * as a queryable label only. Pass nullptr or "" to clear the selection.
 */
WASM_EXPORT void cedar_set_default_midi_device(const char* name) {
    g_default_midi_device = name ? name : "";
}

/**
 * Read back the currently recorded default MIDI device name. Empty string
 * means no device is selected.
 */
WASM_EXPORT const char* cedar_get_default_midi_device() {
    return g_default_midi_device.c_str();
}

/**
 * Reset the VM (clear all state)
 */
WASM_EXPORT void cedar_reset() {
    if (g_vm) {
        g_vm->reset();
    }
}

/**
 * Check if VM is currently crossfading
 * @return 1 if crossfading, 0 otherwise
 */
WASM_EXPORT int cedar_is_crossfading() {
    return g_vm && g_vm->is_crossfading() ? 1 : 0;
}

/**
 * Get crossfade position (0.0 to 1.0)
 */
WASM_EXPORT float cedar_crossfade_position() {
    return g_vm ? g_vm->crossfade_position() : 0.0f;
}

/**
 * Check if VM has a loaded program
 * @return 1 if has program, 0 otherwise
 */
WASM_EXPORT int cedar_has_program() {
    return g_vm && g_vm->has_program() ? 1 : 0;
}

// ============================================================================
// Diagnostic API (for debugging swap issues)
// ============================================================================

/**
 * Check if VM has a pending swap
 * @return 1 if has pending swap, 0 otherwise
 */
WASM_EXPORT int cedar_debug_has_pending_swap() {
    return g_vm && g_vm->has_pending_swap() ? 1 : 0;
}

/**
 * Get instruction count in current slot
 * @return Number of instructions, 0 if no slot
 */
WASM_EXPORT uint32_t cedar_debug_current_slot_instruction_count() {
    return g_vm ? g_vm->current_slot_instruction_count() : 0;
}

/**
 * Get instruction count in previous slot (during crossfade)
 * @return Number of instructions, 0 if no previous slot
 */
WASM_EXPORT uint32_t cedar_debug_previous_slot_instruction_count() {
    return g_vm ? g_vm->previous_slot_instruction_count() : 0;
}

/**
 * Get total number of swaps performed
 * @return Swap count
 */
WASM_EXPORT uint32_t cedar_debug_swap_count() {
    return g_vm ? g_vm->swap_count() : 0;
}

/**
 * Set an external parameter
 * @param name Parameter name (null-terminated)
 * @param value Parameter value
 * @return 1 on success, 0 on failure
 */
WASM_EXPORT int cedar_set_param(const char* name, float value) {
    if (g_vm && name) {
        return g_vm->set_param(name, value) ? 1 : 0;
    }
    return 0;
}

/**
 * Set an external parameter with slew
 * @param name Parameter name (null-terminated)
 * @param value Parameter value
 * @param slew_ms Slew time in milliseconds
 * @return 1 on success, 0 on failure
 */
WASM_EXPORT int cedar_set_param_slew(const char* name, float value, float slew_ms) {
    if (g_vm && name) {
        return g_vm->set_param(name, value, slew_ms) ? 1 : 0;
    }
    return 0;
}

// ============================================================================
// Sample Management API
// ============================================================================

/**
 * Load a sample from float audio data
 * @param name Sample name (null-terminated)
 * @param audio_data Pointer to interleaved float audio data
 * @param num_samples Total number of samples (frames * channels)
 * @param channels Number of channels (1=mono, 2=stereo)
 * @param sample_rate Sample rate in Hz
 * @return Sample ID (>=0) on success, -1 on failure
 */
WASM_EXPORT int32_t cedar_load_sample(const char* name,
                                      const float* audio_data,
                                      uint32_t num_samples,
                                      uint32_t channels,
                                      float sample_rate) {
    if (!g_vm || !name || !audio_data || channels == 0) {
        return -1;
    }

    const std::uint32_t id = g_vm->load_sample(name, audio_data, num_samples, channels, sample_rate);
    return id == 0 ? -1 : static_cast<int32_t>(id);
}

/**
 * Load a sample from audio data in any supported format (WAV, OGG, FLAC, MP3)
 * Auto-detects the format from magic bytes and decodes in C++.
 * @param name Sample name (null-terminated)
 * @param data Pointer to audio file data
 * @param size Size of data in bytes
 * @return Sample ID (>=0) on success, -1 on failure
 */
WASM_EXPORT int32_t cedar_load_audio_data(const char* name,
                                          const uint8_t* data,
                                          uint32_t size) {
    if (!g_vm || !name || !data || size == 0) {
        return -1;
    }

    cedar::MemoryView view(data, size);
    const std::uint32_t id = g_vm->sample_bank().load_audio_data(name, view);
    return id == 0 ? -1 : static_cast<int32_t>(id);
}

/**
 * Check if a sample exists
 * @param name Sample name (null-terminated)
 * @return 1 if sample exists, 0 otherwise
 */
WASM_EXPORT int cedar_has_sample(const char* name) {
    if (!g_vm || !name) {
        return 0;
    }
    
    return g_vm->sample_bank().has_sample(name) ? 1 : 0;
}

/**
 * Get sample ID by name
 * @param name Sample name (null-terminated)
 * @return Sample ID (>0) if found, 0 if not found
 */
WASM_EXPORT uint32_t cedar_get_sample_id(const char* name) {
    if (!g_vm || !name) {
        return 0;
    }
    
    return g_vm->sample_bank().get_sample_id(name);
}

/**
 * Clear all loaded samples
 */
WASM_EXPORT void cedar_clear_samples() {
    if (g_vm) {
        g_vm->sample_bank().clear();
    }
}

/**
 * Load a wavetable bank from WAV file data in memory and register it
 * under `name`. The bank is preprocessed (FFT mip pyramid, RMS-normalized,
 * fundamental-phase aligned) inside this call.
 *
 * Multi-bank: each call appends to the registry. To match the compiler's
 * sequential bank IDs, the host should call cedar_clear_wavetables() once
 * before loading the program's required_wavetables in source order.
 *
 * @param name Bank name (null-terminated). Must match the first arg of
 *             wt_load() in the Akkado source.
 * @param wav_data Pointer to WAV file data (mono, length multiple of 2048)
 * @param wav_size Size of WAV data in bytes
 * @return Assigned bank ID (>= 0) on success, -1 on failure
 */
WASM_EXPORT int32_t cedar_load_wavetable_wav(const char* name,
                                              const uint8_t* wav_data,
                                              uint32_t wav_size) {
    if (!g_vm || !name || !wav_data || wav_size == 0) {
        return -1;
    }
    std::string err;
    const int id = g_vm->wavetable_registry().load_from_memory(
        name, cedar::MemoryView(wav_data, wav_size), &err);
    if (id < 0) {
        std::printf("[Wavetable] Load failed for '%s': %s\n", name, err.c_str());
        return -1;
    }
    return static_cast<int32_t>(id);
}

/**
 * Drop all registered wavetable banks. Call this before loading a new
 * program's required wavetables to keep the runtime registry's IDs in
 * sync with the compiler's source-order assignments.
 */
WASM_EXPORT void cedar_clear_wavetables() {
    if (g_vm) {
        g_vm->wavetable_registry().clear();
    }
}

/**
 * Check if a wavetable bank with this name is registered.
 * @return 1 if present, 0 otherwise.
 */
WASM_EXPORT int cedar_has_wavetable(const char* name) {
    if (!g_vm || !name) return 0;
    return g_vm->wavetable_registry().has(name) ? 1 : 0;
}

/**
 * Look up the bank ID for `name`. Returns -1 if not registered.
 * Useful for sanity checks from the JS host.
 */
WASM_EXPORT int32_t cedar_wavetable_find_id(const char* name) {
    if (!g_vm || !name) return -1;
    return static_cast<int32_t>(g_vm->wavetable_registry().find_id(name));
}

/**
 * Number of currently registered wavetable banks.
 */
WASM_EXPORT uint32_t cedar_wavetable_count() {
    if (!g_vm) return 0;
    return static_cast<uint32_t>(g_vm->wavetable_registry().size());
}

/**
 * Get number of loaded samples
 * @return Number of samples in the bank
 */
WASM_EXPORT uint32_t cedar_get_sample_count() {
    if (!g_vm) {
        return 0;
    }
    
    return static_cast<uint32_t>(g_vm->sample_bank().size());
}

// ============================================================================
// Akkado Compiler API
// ============================================================================

/**
 * Compile Akkado source code to Cedar bytecode
 * Samples are resolved at runtime, not compile time.
 * @param source Source code (null-terminated)
 * @param source_len Length of source string
 * @return 1 on success, 0 on error
 */
WASM_EXPORT int akkado_compile(const char* source, uint32_t source_len) {
    if (!source) {
        return 0;
    }

    std::string_view src{source, source_len};
    akkado::CompileResult new_result = akkado::compile(src, "<web>", nullptr);
    std::swap(g_compile_result, new_result);

    return g_compile_result.success ? 1 : 0;
}

/**
 * Get the compiled bytecode pointer
 * Only valid after successful akkado_compile()
 */
WASM_EXPORT const uint8_t* akkado_get_bytecode() {
    return g_compile_result.bytecode.data();
}

/**
 * Get the compiled bytecode size in bytes
 */
WASM_EXPORT uint32_t akkado_get_bytecode_size() {
    return static_cast<uint32_t>(g_compile_result.bytecode.size());
}

/**
 * Get number of diagnostics (errors/warnings)
 */
WASM_EXPORT uint32_t akkado_get_diagnostic_count() {
    return static_cast<uint32_t>(g_compile_result.diagnostics.size());
}

/**
 * Get diagnostic severity (0=Info, 1=Warning, 2=Error)
 * @param index Diagnostic index
 */
WASM_EXPORT int akkado_get_diagnostic_severity(uint32_t index) {
    if (index >= g_compile_result.diagnostics.size()) return -1;
    return static_cast<int>(g_compile_result.diagnostics[index].severity);
}

/**
 * Get diagnostic message
 * @param index Diagnostic index
 * @return Pointer to null-terminated message string
 */
WASM_EXPORT const char* akkado_get_diagnostic_message(uint32_t index) {
    if (index >= g_compile_result.diagnostics.size()) return "";
    return g_compile_result.diagnostics[index].message.c_str();
}

/**
 * Get diagnostic line number (1-based)
 * @param index Diagnostic index
 */
WASM_EXPORT uint32_t akkado_get_diagnostic_line(uint32_t index) {
    if (index >= g_compile_result.diagnostics.size()) return 0;
    return static_cast<uint32_t>(g_compile_result.diagnostics[index].location.line);
}

/**
 * Get diagnostic column number (1-based)
 * @param index Diagnostic index
 */
WASM_EXPORT uint32_t akkado_get_diagnostic_column(uint32_t index) {
    if (index >= g_compile_result.diagnostics.size()) return 0;
    return static_cast<uint32_t>(g_compile_result.diagnostics[index].location.column);
}

/**
 * Clear compilation results (free memory)
 *
 * NOTE: This is now a no-op. The compile result is cleared automatically
 * via move assignment when akkado_compile() is called. Explicitly clearing
 * causes issues because:
 * 1. Heap operations in the audio thread can corrupt memory
 * 2. Double-clearing can trigger use-after-free
 * 3. The move assignment in akkado_compile handles cleanup properly
 */
WASM_EXPORT void akkado_clear_result() {
    // Intentionally empty - cleanup happens via move assignment in akkado_compile
}

// ============================================================================
// Required Samples API
// ============================================================================

/**
 * Get number of required samples from compile result
 * @return Number of unique sample names used in the compiled code
 */
WASM_EXPORT uint32_t akkado_get_required_samples_count() {
    return static_cast<uint32_t>(g_compile_result.required_samples.size());
}

/**
 * Get required sample name by index
 * @param index Sample index (0 to count-1)
 * @return Pointer to null-terminated sample name, or nullptr if index out of range
 */
WASM_EXPORT const char* akkado_get_required_sample(uint32_t index) {
    if (index >= g_compile_result.required_samples.size()) return nullptr;
    return g_compile_result.required_samples[index].c_str();
}

// ============================================================================
// Extended Sample Info API (for sample banks)
// ============================================================================

/**
 * Get number of required samples with extended info (bank/variant support)
 * @return Number of unique sample references used in the compiled code
 */
WASM_EXPORT uint32_t akkado_get_required_samples_extended_count() {
    return static_cast<uint32_t>(g_compile_result.required_samples_extended.size());
}

/**
 * Get required sample bank name by index
 * @param index Sample index (0 to count-1)
 * @return Pointer to null-terminated bank name, or nullptr if invalid/default bank
 */
WASM_EXPORT const char* akkado_get_required_sample_bank(uint32_t index) {
    if (index >= g_compile_result.required_samples_extended.size()) return nullptr;
    const auto& sample = g_compile_result.required_samples_extended[index];
    if (sample.bank.empty()) return nullptr;
    return sample.bank.c_str();
}

/**
 * Get required sample name by index (extended version)
 * @param index Sample index (0 to count-1)
 * @return Pointer to null-terminated sample name, or nullptr if invalid
 */
WASM_EXPORT const char* akkado_get_required_sample_name(uint32_t index) {
    if (index >= g_compile_result.required_samples_extended.size()) return nullptr;
    return g_compile_result.required_samples_extended[index].name.c_str();
}

/**
 * Get required sample variant by index
 * @param index Sample index (0 to count-1)
 * @return Variant index (0 = first variant), or -1 if invalid
 */
WASM_EXPORT int32_t akkado_get_required_sample_variant(uint32_t index) {
    if (index >= g_compile_result.required_samples_extended.size()) return -1;
    return g_compile_result.required_samples_extended[index].variant;
}

/**
 * Get qualified sample name for Cedar lookup (e.g., "TR808_bd_0")
 * This is the name that should be used when loading and looking up samples in Cedar.
 * @param index Sample index (0 to count-1)
 * @return Pointer to null-terminated qualified name, or nullptr if invalid
 */
static std::string g_qualified_name_buffer;  // Static buffer for returning string
WASM_EXPORT const char* akkado_get_required_sample_qualified(uint32_t index) {
    if (index >= g_compile_result.required_samples_extended.size()) return nullptr;
    g_qualified_name_buffer = g_compile_result.required_samples_extended[index].qualified_name();
    return g_qualified_name_buffer.c_str();
}

// ============================================================================
// Required SoundFonts API
// ============================================================================

/**
 * Get number of required SoundFont files from compile result
 * @return Number of unique SF2 files referenced in the compiled code
 */
WASM_EXPORT uint32_t akkado_get_required_soundfonts_count() {
    return static_cast<uint32_t>(g_compile_result.required_soundfonts.size());
}

/**
 * Get required SoundFont filename by index
 * @param index SoundFont index (0 to count-1)
 * @return Pointer to null-terminated filename, or nullptr if index out of range
 */
WASM_EXPORT const char* akkado_get_required_soundfont_filename(uint32_t index) {
    if (index >= g_compile_result.required_soundfonts.size()) return nullptr;
    return g_compile_result.required_soundfonts[index].filename.c_str();
}

/**
 * Get required SoundFont preset index by index
 * @param index SoundFont index (0 to count-1)
 * @return Preset index, or -1 if index out of range
 */
WASM_EXPORT int32_t akkado_get_required_soundfont_preset(uint32_t index) {
    if (index >= g_compile_result.required_soundfonts.size()) return -1;
    return g_compile_result.required_soundfonts[index].preset_index;
}

// ============================================================================
// Required Wavetables API
// ============================================================================
//
// The Akkado compiler records every wt_load("name", "path") call in the
// program's compile result. The JS host should iterate these after compile,
// fetch each file from its source (URL, IndexedDB, etc.), and feed the
// bytes back through cedar_load_wavetable_wav(name, ...). v1 keeps one
// active bank, so the LAST entry in this list determines the audible bank.

/**
 * Get number of wt_load() calls from the compile result.
 */
WASM_EXPORT uint32_t akkado_get_required_wavetables_count() {
    return static_cast<uint32_t>(g_compile_result.required_wavetables.size());
}

/**
 * Get the bank name for the wt_load() call at `index`.
 * @return Pointer to null-terminated name, or nullptr if index out of range.
 */
WASM_EXPORT const char* akkado_get_required_wavetable_name(uint32_t index) {
    if (index >= g_compile_result.required_wavetables.size()) return nullptr;
    return g_compile_result.required_wavetables[index].name.c_str();
}

/**
 * Get the file path for the wt_load() call at `index`.
 * The path is whatever the user wrote in source; the JS host is expected
 * to resolve it (relative paths, URLs, etc.).
 * @return Pointer to null-terminated path, or nullptr if index out of range.
 */
WASM_EXPORT const char* akkado_get_required_wavetable_path(uint32_t index) {
    if (index >= g_compile_result.required_wavetables.size()) return nullptr;
    return g_compile_result.required_wavetables[index].path.c_str();
}

// ============================================================================
// Required URIs API (akkado samples() builtin and friends)
// ============================================================================
//
// The Akkado compiler records every top-level URI directive (samples("..."),
// future soundfont("..."), etc.) in `required_uris`. The JS host iterates
// these after compile and dispatches each URI to the appropriate registry
// based on its `kind`:
//   0 = SampleBank   → bankRegistry.loadBank(uri)
//   1 = SoundFont    → audioEngine.loadAsset(uri, 'soundfont', ...)
//   2 = Wavetable    → audioEngine.loadAsset(uri, 'wavetable', ...)
//   3 = Sample       → audioEngine.loadAsset(uri, 'sample', ...)
// The bytecode swap is gated behind every URI resolving (the same drain
// pattern as required_samples / required_soundfonts / required_wavetables).

/**
 * Get the number of URI directives in the compile result.
 */
WASM_EXPORT uint32_t akkado_get_required_uri_count() {
    return static_cast<uint32_t>(g_compile_result.required_uris.size());
}

/**
 * Get the URI string at `index`.
 * @return Pointer to null-terminated URI, or nullptr if index out of range.
 */
WASM_EXPORT const char* akkado_get_required_uri(uint32_t index) {
    if (index >= g_compile_result.required_uris.size()) return nullptr;
    return g_compile_result.required_uris[index].uri.c_str();
}

/**
 * Get the kind tag (0..3) for the URI at `index`. Mirrors
 * `akkado::UriKind`.
 * @return UriKind enum value, or -1 if index out of range.
 */
WASM_EXPORT int32_t akkado_get_required_uri_kind(uint32_t index) {
    if (index >= g_compile_result.required_uris.size()) return -1;
    return static_cast<int32_t>(g_compile_result.required_uris[index].kind);
}

/**
 * Get number of in() calls in the most recent compile (each entry corresponds
 * to one in() call in source order). Returns 0 if the program does not use in().
 */
WASM_EXPORT uint32_t akkado_get_required_input_sources_count() {
    return static_cast<uint32_t>(g_compile_result.required_input_sources.size());
}

/**
 * Get the source string for the in() call at `index`. Empty string means
 * the call had no argument (host should fall back to its UI default).
 * Returns nullptr if index is out of range.
 */
WASM_EXPORT const char* akkado_get_required_input_source(uint32_t index) {
    if (index >= g_compile_result.required_input_sources.size()) return nullptr;
    return g_compile_result.required_input_sources[index].c_str();
}

// ============================================================================
// Required MIDI Sources API (PRD prd-midi-input §4.7)
// ============================================================================
//
// Each midi() call in source produces one entry in required_midi_sources
// (entries are NOT deduplicated — per-call channel filters and option
// overrides may differ). The web host reads these after compile to build
// a Web MIDI route table:
//   kind == DefaultDevice(0): resolve via cedar_set_default_midi_device label
//   kind == NamedDevice(1)  : substring match against navigator.requestMIDIAccess() input names
//   kind == File(2)         : skipped in Phase 4 (Phase 5 ships .mid playback)

/**
 * Number of midi() call sites in the most recent compile.
 */
WASM_EXPORT uint32_t akkado_get_required_midi_sources_count() {
    return static_cast<uint32_t>(g_compile_result.required_midi_sources.size());
}

/**
 * State ID for the i-th midi() call. Pass this to cedar_push_midi_event()
 * for every event the host routes to that call site.
 */
WASM_EXPORT uint32_t akkado_get_required_midi_source_state_id(uint32_t index) {
    if (index >= g_compile_result.required_midi_sources.size()) return 0;
    return g_compile_result.required_midi_sources[index].state_id;
}

/**
 * Source kind (cedar::MidiSourceKind) for the i-th midi() call.
 * 0 = DefaultDevice, 1 = NamedDevice, 2 = File. Returns -1 on bad index.
 */
WASM_EXPORT int32_t akkado_get_required_midi_source_kind(uint32_t index) {
    if (index >= g_compile_result.required_midi_sources.size()) return -1;
    return static_cast<int32_t>(g_compile_result.required_midi_sources[index].kind);
}

/**
 * Device name (NamedDevice) or file path (File) for the i-th midi() call.
 * Empty string for DefaultDevice. Returns nullptr on bad index.
 */
WASM_EXPORT const char* akkado_get_required_midi_source_name(uint32_t index) {
    if (index >= g_compile_result.required_midi_sources.size()) return nullptr;
    return g_compile_result.required_midi_sources[index].name_or_path.c_str();
}

/**
 * Channel filter for the i-th midi() call. 0 means accept any channel;
 * 1-16 restricts to that MIDI channel. Returns -1 on bad index.
 */
WASM_EXPORT int32_t akkado_get_required_midi_source_channel(uint32_t index) {
    if (index >= g_compile_result.required_midi_sources.size()) return -1;
    return static_cast<int32_t>(g_compile_result.required_midi_sources[index].channel_filter);
}

/**
 * Loop flag for the i-th midi() call (file mode only). Returns -1 on bad index.
 */
WASM_EXPORT int32_t akkado_get_required_midi_source_loop(uint32_t index) {
    if (index >= g_compile_result.required_midi_sources.size()) return -1;
    return g_compile_result.required_midi_sources[index].loop ? 1 : 0;
}

/**
 * Tempo mode for the i-th midi() call (file mode only).
 * 0 = Follow, 1 = File. Returns -1 on bad index.
 */
WASM_EXPORT int32_t akkado_get_required_midi_source_tempo(uint32_t index) {
    if (index >= g_compile_result.required_midi_sources.size()) return -1;
    return static_cast<int32_t>(g_compile_result.required_midi_sources[index].tempo_mode);
}

// ─────────────────────────────────────────────────────────────────────────────
// midi_cc() route accessors (PRD prd-midi-input §4.8)
//
// midi_cc("name", {cc|pb|at, ...}) is a compile-time directive that emits no
// bytecode. The host MIDI dispatcher (JS main thread) walks this list, matches
// each incoming CC/PB/AT against the routes, computes the scaled value, and
// posts a 'setParam' message to the worklet — which calls cedar_set_param_slew.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Number of midi_cc() routes in the most recent compile.
 */
WASM_EXPORT uint32_t akkado_get_required_midi_cc_routes_count() {
    return static_cast<uint32_t>(g_compile_result.required_midi_cc_routes.size());
}

/**
 * Param name string for the i-th midi_cc() route. Lives in g_compile_result;
 * stable until the next compile. Returns nullptr on bad index.
 */
WASM_EXPORT const char* akkado_get_required_midi_cc_route_name(uint32_t index) {
    if (index >= g_compile_result.required_midi_cc_routes.size()) return nullptr;
    return g_compile_result.required_midi_cc_routes[index].param_name.c_str();
}

/**
 * cc_num for the i-th route. 0..127 = MIDI CC number; -1 = pitch-bend;
 * -2 = channel aftertouch. Returns -128 on bad index (out-of-band sentinel).
 */
WASM_EXPORT int32_t akkado_get_required_midi_cc_route_cc(uint32_t index) {
    if (index >= g_compile_result.required_midi_cc_routes.size()) return -128;
    return static_cast<int32_t>(g_compile_result.required_midi_cc_routes[index].cc_num);
}

/**
 * Channel filter for the i-th route. 0 = any; 1..16 = specific channel.
 * Returns -1 on bad index.
 */
WASM_EXPORT int32_t akkado_get_required_midi_cc_route_channel(uint32_t index) {
    if (index >= g_compile_result.required_midi_cc_routes.size()) return -1;
    return static_cast<int32_t>(g_compile_result.required_midi_cc_routes[index].channel_filter);
}

/**
 * Scale factor for the i-th route (= max - min). Returns 0 on bad index.
 */
WASM_EXPORT float akkado_get_required_midi_cc_route_scale(uint32_t index) {
    if (index >= g_compile_result.required_midi_cc_routes.size()) return 0.0f;
    return g_compile_result.required_midi_cc_routes[index].scale;
}

/**
 * Bias for the i-th route (= min). Returns 0 on bad index.
 */
WASM_EXPORT float akkado_get_required_midi_cc_route_bias(uint32_t index) {
    if (index >= g_compile_result.required_midi_cc_routes.size()) return 0.0f;
    return g_compile_result.required_midi_cc_routes[index].bias;
}

/**
 * EnvMap slew in milliseconds for the i-th route. Returns 0 on bad index.
 */
WASM_EXPORT float akkado_get_required_midi_cc_route_slew_ms(uint32_t index) {
    if (index >= g_compile_result.required_midi_cc_routes.size()) return 0.0f;
    return g_compile_result.required_midi_cc_routes[index].slew_ms;
}

/**
 * Resolve sample IDs in state_inits using currently loaded samples.
 * Call this AFTER loading required samples, BEFORE cedar_apply_state_inits().
 * This maps sample names to IDs in the sample bank.
 *
 * For bank samples, the qualified name format is: "bank_name_variant" (e.g., "TR808_bd_0")
 * The TypeScript side must load samples with these qualified names.
 */
WASM_EXPORT void akkado_resolve_sample_ids() {
    if (!g_vm) return;

    for (auto& init : g_compile_result.state_inits) {
        // Handle SequenceProgram type (sample mappings)
        // Events are stored in sequence_events vectors
        for (const auto& mapping : init.sequence_sample_mappings) {
            if (mapping.seq_idx < init.sequence_events.size()) {
                auto& events = init.sequence_events[mapping.seq_idx];
                if (mapping.event_idx < events.size()) {
                    // Build qualified name for bank samples
                    std::string lookup_name;
                    if (mapping.bank.empty() || mapping.bank == "default") {
                        // Default bank - use simple name with variant suffix if non-zero
                        lookup_name = mapping.variant > 0
                            ? mapping.sample_name + ":" + std::to_string(mapping.variant)
                            : mapping.sample_name;
                    } else {
                        // Custom bank - use qualified name format: bank_name_variant
                        lookup_name = mapping.bank + "_" + mapping.sample_name + "_" + std::to_string(mapping.variant);
                    }
                    auto id = g_vm->sample_bank().get_sample_id(lookup_name);
                    auto& ev = events[mapping.event_idx];
                    std::uint8_t slot = mapping.value_slot;
                    if (slot < cedar::MAX_VALUES_PER_EVENT) {
                        ev.values[slot] = static_cast<float>(id);
                    }
                }
            }
        }
    }
}

/**
 * Patch sample-id placeholder constants in the bytecode buffer using the
 * currently loaded sample bank. Walks `g_compile_result.scalar_sample_mappings`
 * (populated by codegen for `sample(trig, pitch, "name")` calls), looks up each
 * sample's qualified name in the bank, and overwrites the recorded PUSH_CONST
 * instruction's `state_id` immediate (which is bit_cast'd to a float at run
 * time) with the resolved sample ID.
 *
 * Call AFTER the sample bank has been populated and BEFORE
 * `cedar_load_program(bytecode_ptr, ...)` so the loaded program reflects the
 * patches.
 *
 * `bytecode_ptr` and `byte_count` describe the in-WASM bytecode buffer that is
 * about to be passed to `cedar_load_program`. The buffer is interpreted as a
 * contiguous array of `cedar::Instruction`.
 *
 * Returns the number of instructions patched (or 0 if the VM is uninitialized
 * or the buffer is malformed).
 */
WASM_EXPORT uint32_t akkado_patch_sample_ids_in_bytecode(uint8_t* bytecode_ptr,
                                                          uint32_t byte_count) {
    if (!g_vm || !bytecode_ptr) return 0;
    if (byte_count == 0 || (byte_count % sizeof(cedar::Instruction)) != 0) return 0;

    auto* instructions = reinterpret_cast<cedar::Instruction*>(bytecode_ptr);
    uint32_t inst_count = byte_count / sizeof(cedar::Instruction);
    uint32_t patched = 0;

    for (const auto& m : g_compile_result.scalar_sample_mappings) {
        if (m.instruction_index >= inst_count) continue;
        cedar::Instruction& inst = instructions[m.instruction_index];
        if (inst.opcode != cedar::Opcode::PUSH_CONST) continue;

        std::string lookup;
        if (m.bank.empty() || m.bank == "default") {
            lookup = m.variant > 0
                ? m.name + ":" + std::to_string(m.variant)
                : m.name;
        } else {
            lookup = m.bank + "_" + m.name + "_" + std::to_string(m.variant);
        }

        std::uint32_t id = g_vm->sample_bank().get_sample_id(lookup);
        float as_float = static_cast<float>(id);
        std::memcpy(&inst.state_id, &as_float, sizeof(float));
        ++patched;
    }

    return patched;
}

// ============================================================================
// State Initialization API
// ============================================================================

/**
 * Get number of state initializations from compile result
 */
WASM_EXPORT uint32_t akkado_get_state_init_count() {
    return static_cast<uint32_t>(g_compile_result.state_inits.size());
}

/**
 * Get state_id for a state initialization
 * @param index State init index
 * @return state_id (32-bit FNV-1a hash)
 */
WASM_EXPORT uint32_t akkado_get_state_init_id(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return 0;
    return g_compile_result.state_inits[index].state_id;
}

/**
 * Get type for a state initialization (1=Timeline, 2=SequenceProgram)
 * @param index State init index
 * @return type
 */
WASM_EXPORT int akkado_get_state_init_type(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return -1;
    return static_cast<int>(g_compile_result.state_inits[index].type);
}

/**
 * Get cycle length for a state initialization
 * @param index State init index
 * @return Cycle length in beats (default 4.0)
 */
WASM_EXPORT float akkado_get_state_init_cycle_length(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return 4.0f;
    return g_compile_result.state_inits[index].cycle_length;
}

/**
 * Apply all state initializations from compile result to the VM
 * Should be called after cedar_load_program for correct pattern playback
 * @return Number of states initialized
 */
WASM_EXPORT uint32_t cedar_apply_state_inits() {
    if (!g_vm) return 0;

    uint32_t count = 0;
    for (const auto& init : g_compile_result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            // Set up sequence event pointers before passing to VM
            // The sequences need their event pointers to point to the sequence_events data
            std::vector<cedar::Sequence> seq_copy = init.sequences;
            for (std::size_t i = 0; i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                if (!init.sequence_events[i].empty()) {
                    seq_copy[i].events = const_cast<cedar::Event*>(init.sequence_events[i].data());
                    seq_copy[i].num_events = static_cast<std::uint32_t>(init.sequence_events[i].size());
                    seq_copy[i].capacity = static_cast<std::uint32_t>(init.sequence_events[i].size());
                }
            }

            // Initialize sequence-based pattern (arena allocates and copies)
            g_vm->init_sequence_program_state(
                init.state_id,
                seq_copy.data(),
                seq_copy.size(),
                init.cycle_length,
                init.is_sample_pattern,
                init.total_events
            );
            if (init.iter_n > 0) {
                g_vm->init_sequence_iter_state(init.state_id, init.iter_n, init.iter_dir);
            }
            count++;
        }
        else if (init.type == akkado::StateInitData::Type::PolyAlloc) {
            g_vm->init_poly_state(
                init.state_id,
                init.poly_seq_state_id,
                init.poly_max_voices,
                init.poly_mode,
                init.poly_steal_strategy,
                init.poly_release_seconds,
                init.poly_prop_count,
                init.poly_prop_defaults
            );
            count++;
        }
        else if (init.type == akkado::StateInitData::Type::ForeachAlloc) {
            // PRD L3: FOREACH_EVENT instance config (poly/each_voice/fold).
            g_vm->init_foreach_state(
                init.state_id,
                init.foreach_allocator_kind,
                init.foreach_block_id,
                init.foreach_event_src_state_id,
                init.foreach_max_iterations,
                init.poly_max_voices,
                init.poly_mode,
                init.poly_steal_strategy,
                init.poly_release_seconds,
                init.poly_prop_count,
                init.poly_prop_defaults
            );
            count++;
        }
#ifndef CEDAR_NO_SOUNDFONT
        else if (init.type == akkado::StateInitData::Type::SoundfontEvents) {
            g_vm->init_soundfont_voice_event_state(
                init.state_id, init.sf_seq_state_id, init.sf_preset_idx);
            count++;
        }
#endif
        else if (init.type == akkado::StateInitData::Type::ExtendedParams) {
            g_vm->init_extended_params(
                init.state_id,
                init.ext_constants.data(),
                init.ext_buffer_indices.data(),
                init.ext_count
            );
            count++;
        }
        else if (init.type == akkado::StateInitData::Type::Timeline) {
            auto& state = g_vm->states().get_or_create<cedar::TimelineState>(init.state_id);
            state.num_points = std::min(
                static_cast<std::uint32_t>(init.timeline_breakpoints.size()),
                static_cast<std::uint32_t>(cedar::TimelineState::MAX_BREAKPOINTS));
            for (std::uint32_t i = 0; i < state.num_points; ++i) {
                state.points[i] = init.timeline_breakpoints[i];
            }
            state.loop = init.timeline_loop;
            state.loop_length = init.timeline_loop_length;
            count++;
        }
        else if (init.type == akkado::StateInitData::Type::EventTransform) {
            // PRD prd-runtime-event-transforms Phase 1: transform-owned
            // SequenceState filled at runtime by EVENT_MAP / EVENT_FILTER.
            g_vm->init_event_transform_state(init.state_id, init.cycle_length,
                                             init.is_sample_pattern,
                                             init.total_events);
            count++;
        }
        else if (init.type == akkado::StateInitData::Type::RateScale) {
            // PRD prd-runtime-event-transforms Phase 3: EVENT_RATE_SCALE
            // beat-position integrator. No payload — just allocate + reset.
            g_vm->init_event_rate_scale_state(init.state_id);
            count++;
        }
    }
    return count;
}

/**
 * Initialize one MidiQueueState per required_midi_sources entry. The
 * CLI's `apply_midi_route_plan` (audio_engine.cpp) calls
 * `vm.init_midi_queue_state` directly; on web the WASM heap is private to
 * this module, so the JS host calls this single function instead. Must be
 * invoked AFTER cedar_load_program — initialization writes into the new
 * program's state pool. Phase 5 (file playback) will load `.mid` bytes
 * separately and patch the file pointer on each MidiQueueState; Phase 4
 * leaves File-kind entries silent.
 *
 * @return Number of MidiQueueStates initialized.
 */
WASM_EXPORT uint32_t cedar_apply_midi_sources() {
    if (!g_vm) return 0;
    uint32_t count = 0;
    for (const auto& src : g_compile_result.required_midi_sources) {
        g_vm->init_midi_queue_state(
            src.state_id,
            src.kind,
            src.name_or_path.empty() ? nullptr : src.name_or_path.c_str(),
            src.channel_filter,
            src.loop,
            src.tempo_mode);
        count++;
    }
    return count;
}

// ============================================================================
// Utility
// ============================================================================

/**
 * Get block size (128 samples)
 */
WASM_EXPORT uint32_t nkido_get_block_size() {
    return 128;
}

/**
 * Allocate memory in WASM heap (for passing data from JS)
 * @param size Size in bytes
 * @return Pointer to allocated memory, or nullptr on failure
 */
WASM_EXPORT void* nkido_malloc(uint32_t size) {
    return std::malloc(size);
}

/**
 * Free memory allocated with nkido_malloc
 * @param ptr Pointer to free
 */
WASM_EXPORT void nkido_free(void* ptr) {
    std::free(ptr);
}

// ============================================================================
// Akkado Builtins Metadata API
// ============================================================================

// Static buffer for JSON output (avoids allocation issues)
static std::string g_builtins_json;

/**
 * Get all builtin function metadata as JSON string
 * Returns pointer to null-terminated JSON string
 *
 * JSON format:
 * {
 *   "functions": {
 *     "lp": {
 *       "params": [
 *         {"name": "in", "required": true},
 *         {"name": "cut", "required": true},
 *         {"name": "q", "required": false, "default": 0.707}
 *       ],
 *       "description": "State-variable lowpass filter"
 *     },
 *     ...
 *   },
 *   "aliases": {
 *     "lowpass": "lp",
 *     ...
 *   },
 *   "keywords": ["fn", "pat", "seq", "timeline", "note", "true", "false", "match", "post"]
 * }
 */
WASM_EXPORT const char* akkado_get_builtins_json() {
    // Build JSON only once (lazy initialization). Body is implemented in
    // akkado/src/builtins_json.cpp so akkado-layer unit tests can call it
    // without going through the WASM boundary.
    if (g_builtins_json.empty()) {
        g_builtins_json = akkado::serialize_builtins_json();
    }
    return g_builtins_json.c_str();
}

// Static buffer for shape-index JSON. Re-built on every call (debounced
// 300 ms by the editor) — caching is on the JS side keyed by source hash.
static std::string g_shape_index_json;

/**
 * Get the analyzer-driven shape index for editor autocomplete.
 *
 * `source` / `source_len` describe the current editor buffer (UTF-8 bytes,
 * not null-terminated — pass the byte length explicitly). `cursor_offset`
 * is the UTF-8 byte offset of the editor caret in `source`; pass UINT32_MAX
 * (or any value past end-of-source) to skip patternHole resolution.
 *
 * Returns a pointer to a null-terminated JSON string with the shape index.
 * The returned pointer is valid until the next call to this function.
 *
 * Phase 2 of records-system-unification PRD; see `docs/prd-records-system-
 * unification.md` §5.2 for the JSON schema.
 */
WASM_EXPORT const char* akkado_get_shape_index(const char* source,
                                                std::uint32_t source_len,
                                                std::uint32_t cursor_offset) {
    g_shape_index_json = akkado::shape_index_json(
        std::string_view(source, source_len), cursor_offset);
    return g_shape_index_json.c_str();
}

// ============================================================================
// Pattern Highlighting API
// ============================================================================

// Preview query result buffers (for getting events one at a time)
// These are static to avoid allocation during preview queries
static constexpr std::size_t PREVIEW_MAX_SEQUENCES = 64;
static constexpr std::size_t PREVIEW_MAX_EVENTS_PER_SEQ = 256;
static constexpr std::size_t PREVIEW_MAX_OUTPUT_EVENTS = 256;
static cedar::Sequence g_preview_sequences[PREVIEW_MAX_SEQUENCES];
static cedar::Event g_preview_events[PREVIEW_MAX_SEQUENCES][PREVIEW_MAX_EVENTS_PER_SEQ];
static cedar::OutputEvents::OutputEvent g_preview_output_events[PREVIEW_MAX_OUTPUT_EVENTS];
static cedar::OutputEvents g_preview_output;

/**
 * Get number of SequenceProgram state inits (for UI highlighting)
 * @return Number of pattern state inits
 */
WASM_EXPORT uint32_t akkado_get_pattern_init_count() {
    uint32_t count = 0;
    for (const auto& init : g_compile_result.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            count++;
        }
    }
    return count;
}

/**
 * Get the Nth SequenceProgram state init index (maps pattern index to state_inits index)
 * @param pattern_index Pattern index (0 to pattern_count-1)
 * @return Index into state_inits array, or UINT32_MAX if not found
 */
static uint32_t get_pattern_init_index(uint32_t pattern_index) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_compile_result.state_inits.size(); ++i) {
        if (g_compile_result.state_inits[i].type == akkado::StateInitData::Type::SequenceProgram) {
            if (count == pattern_index) return i;
            count++;
        }
    }
    return UINT32_MAX;
}

/**
 * Get state_id for a pattern
 * @param pattern_index Pattern index (0 to pattern_count-1)
 * @return state_id (32-bit FNV-1a hash), or 0 if invalid
 */
WASM_EXPORT uint32_t akkado_get_pattern_state_id(uint32_t pattern_index) {
    uint32_t idx = get_pattern_init_index(pattern_index);
    if (idx == UINT32_MAX) return 0;
    return g_compile_result.state_inits[idx].state_id;
}

/**
 * Get document offset where pattern string starts
 * @param pattern_index Pattern index
 * @return Document offset (0-based byte offset), or 0 if invalid
 */
WASM_EXPORT uint32_t akkado_get_pattern_doc_offset(uint32_t pattern_index) {
    uint32_t idx = get_pattern_init_index(pattern_index);
    if (idx == UINT32_MAX) return 0;
    return g_compile_result.state_inits[idx].pattern_location.offset;
}

/**
 * Get pattern string length in document
 * @param pattern_index Pattern index
 * @return Length in characters, or 0 if invalid
 */
WASM_EXPORT uint32_t akkado_get_pattern_doc_length(uint32_t pattern_index) {
    uint32_t idx = get_pattern_init_index(pattern_index);
    if (idx == UINT32_MAX) return 0;
    return g_compile_result.state_inits[idx].pattern_location.length;
}

/**
 * Get cycle length for a pattern
 * @param pattern_index Pattern index
 * @return Cycle length in beats, or 4.0 if invalid
 */
WASM_EXPORT float akkado_get_pattern_cycle_length(uint32_t pattern_index) {
    uint32_t idx = get_pattern_init_index(pattern_index);
    if (idx == UINT32_MAX) return 4.0f;
    return g_compile_result.state_inits[idx].cycle_length;
}

/**
 * Query pattern for preview events (fills internal buffer)
 * @param pattern_index Pattern index
 * @param start_beat Query window start (in beats)
 * @param end_beat Query window end (in beats)
 * @return Number of events found
 */
WASM_EXPORT uint32_t akkado_query_pattern_preview(uint32_t pattern_index, float start_beat, float end_beat) {
    g_preview_output.clear();

    uint32_t idx = get_pattern_init_index(pattern_index);
    if (idx == UINT32_MAX) return 0;

    const auto& init = g_compile_result.state_inits[idx];
    if (init.sequences.empty()) return 0;

    // Set up static preview buffers
    std::size_t num_seqs = std::min(init.sequences.size(), PREVIEW_MAX_SEQUENCES);

    // Copy sequences and set up event pointers to static buffers
    for (std::size_t i = 0; i < num_seqs; ++i) {
        g_preview_sequences[i] = init.sequences[i];

        // Copy events from sequence_events if available
        if (i < init.sequence_events.size() && !init.sequence_events[i].empty()) {
            std::size_t num_events = std::min(init.sequence_events[i].size(), PREVIEW_MAX_EVENTS_PER_SEQ);
            for (std::size_t j = 0; j < num_events; ++j) {
                g_preview_events[i][j] = init.sequence_events[i][j];
            }
            g_preview_sequences[i].events = g_preview_events[i];
            g_preview_sequences[i].num_events = static_cast<std::uint32_t>(num_events);
            g_preview_sequences[i].capacity = static_cast<std::uint32_t>(PREVIEW_MAX_EVENTS_PER_SEQ);
        } else {
            g_preview_sequences[i].events = nullptr;
            g_preview_sequences[i].num_events = 0;
            g_preview_sequences[i].capacity = 0;
        }
    }

    // Create a temporary sequence state for querying using static buffers
    cedar::SequenceState temp_state;
    temp_state.sequences = g_preview_sequences;
    temp_state.num_sequences = static_cast<std::uint32_t>(num_seqs);
    temp_state.seq_capacity = static_cast<std::uint32_t>(PREVIEW_MAX_SEQUENCES);
    temp_state.output.events = g_preview_output_events;
    temp_state.output.num_events = 0;
    temp_state.output.capacity = static_cast<std::uint32_t>(PREVIEW_MAX_OUTPUT_EVENTS);
    temp_state.cycle_length = init.cycle_length;
    temp_state.pattern_seed = init.state_id;  // Use state_id as seed

    // Determine which cycle to query
    uint64_t cycle = static_cast<uint64_t>(start_beat / init.cycle_length);

    // Query the pattern for the full cycle
    cedar::query_pattern(temp_state, cycle, init.cycle_length);

    // Copy results to preview buffer, filtering out rest events (num_values == 0)
    g_preview_output.events = g_preview_output_events;
    g_preview_output.capacity = static_cast<std::uint32_t>(PREVIEW_MAX_OUTPUT_EVENTS);
    g_preview_output.num_events = 0;
    for (uint32_t i = 0; i < temp_state.output.num_events; ++i) {
        if (temp_state.output.events[i].num_values > 0) {
            g_preview_output.events[g_preview_output.num_events++] = temp_state.output.events[i];
        }
    }

    return g_preview_output.num_events;
}

/**
 * Get preview event time
 * @param event_index Event index (0 to event_count-1)
 * @return Event time in beats within cycle
 */
WASM_EXPORT float akkado_get_preview_event_time(uint32_t event_index) {
    if (event_index >= g_preview_output.num_events) return 0.0f;
    return g_preview_output.events[event_index].time;
}

/**
 * Get preview event duration
 * @param event_index Event index
 * @return Event duration in beats
 */
WASM_EXPORT float akkado_get_preview_event_duration(uint32_t event_index) {
    if (event_index >= g_preview_output.num_events) return 0.0f;
    return g_preview_output.events[event_index].duration;
}

/**
 * Get preview event value (frequency or sample ID)
 * @param event_index Event index
 * @return Event value
 */
WASM_EXPORT float akkado_get_preview_event_value(uint32_t event_index) {
    if (event_index >= g_preview_output.num_events) return 0.0f;
    // Return first value (OutputEvent can have multiple values for chords)
    return g_preview_output.events[event_index].values[0];
}

/**
 * Get preview event source offset (char offset within pattern string)
 * @param event_index Event index
 * @return Source offset, or 0 if invalid
 */
WASM_EXPORT uint32_t akkado_get_preview_event_source_offset(uint32_t event_index) {
    if (event_index >= g_preview_output.num_events) return 0;
    return g_preview_output.events[event_index].source_offset;
}

/**
 * Get preview event source length
 * @param event_index Event index
 * @return Source length in characters, or 0 if invalid
 */
WASM_EXPORT uint32_t akkado_get_preview_event_source_length(uint32_t event_index) {
    if (event_index >= g_preview_output.num_events) return 0;
    return g_preview_output.events[event_index].source_length;
}

/**
 * Get current beat position (for scrolling preview)
 * @return Current beat position (0-based, wraps at cycle boundary)
 */
WASM_EXPORT float cedar_get_current_beat_position() {
    if (!g_vm) return 0.0f;
    const auto& ctx = g_vm->context();
    float spb = ctx.samples_per_beat();
    return static_cast<float>(ctx.global_sample_counter) / spb;
}

/**
 * Get active step source offset for a pattern (by state_id)
 * @param state_id Pattern state ID (32-bit FNV-1a hash)
 * @return Source offset of currently active step, or 0 if not found
 */
WASM_EXPORT uint32_t cedar_get_pattern_active_offset(uint32_t state_id) {
    if (!g_vm) return 0;

    auto& states = g_vm->states();
    if (!states.exists(state_id)) return 0;

    auto& state = states.get<cedar::SequenceState>(state_id);
    return state.active_source_offset;
}

/**
 * Get active step source length for a pattern (by state_id)
 * @param state_id Pattern state ID
 * @return Source length of currently active step, or 0 if not found
 */
WASM_EXPORT uint32_t cedar_get_pattern_active_length(uint32_t state_id) {
    if (!g_vm) return 0;

    auto& states = g_vm->states();
    if (!states.exists(state_id)) return 0;

    auto& state = states.get<cedar::SequenceState>(state_id);
    return state.active_source_length;
}

// ============================================================================
// Pattern Debug API
// ============================================================================

// Static buffer for pattern debug JSON
static std::string g_pattern_debug_json;

/**
 * Get detailed pattern debug info as JSON
 * Includes: AST structure, compiled sequences, and flattened events
 *
 * JSON format:
 * {
 *   "ast": { ... AST tree ... },
 *   "sequences": [
 *     { "id": 0, "mode": "NORMAL", "duration": 4.0, "events": [...] },
 *     ...
 *   ],
 *   "cycleLength": 4.0,
 *   "isSamplePattern": false
 * }
 *
 * @param pattern_index Pattern index (0 to pattern_count-1)
 * @return Pointer to null-terminated JSON string
 */
WASM_EXPORT const char* akkado_get_pattern_debug_json(uint32_t pattern_index) {
    uint32_t idx = get_pattern_init_index(pattern_index);
    if (idx == UINT32_MAX) {
        g_pattern_debug_json = "{}";
        return g_pattern_debug_json.c_str();
    }

    const auto& init = g_compile_result.state_inits[idx];

    // Serialize sequences to JSON - returns {"sequences":[...]}
    std::string sequences_json = akkado::serialize_sequences_json(
        init.sequences, init.sequence_events);

    // Extract just the array part (after "sequences":)
    std::string sequences_array = "[]";
    auto pos = sequences_json.find('[');
    if (pos != std::string::npos) {
        // Find the last ] and extract the array
        auto end_pos = sequences_json.rfind(']');
        if (end_pos != std::string::npos && end_pos > pos) {
            sequences_array = sequences_json.substr(pos, end_pos - pos + 1);
        }
    }

    std::ostringstream json;
    json << "{";
    json << "\"ast\":" << (init.ast_json.empty() ? "null" : init.ast_json);
    json << ",\"sequences\":" << sequences_array;
    json << ",\"cycleLength\":" << init.cycle_length;
    json << ",\"isSamplePattern\":" << (init.is_sample_pattern ? "true" : "false");
    json << "}";

    g_pattern_debug_json = json.str();
    return g_pattern_debug_json.c_str();
}

// ============================================================================
// State Inspection API
// ============================================================================

// Static buffer for state inspection JSON
static std::string g_state_inspection_json;

/**
 * Inspect state by ID, returning JSON representation of state fields.
 * @param state_id State ID (32-bit FNV-1a hash)
 * @return Pointer to null-terminated JSON string, or empty string if not found
 */
WASM_EXPORT const char* cedar_inspect_state(uint32_t state_id) {
    if (!g_vm) {
        g_state_inspection_json = "";
        return g_state_inspection_json.c_str();
    }

    g_state_inspection_json = g_vm->states().inspect_state_json(state_id);
    return g_state_inspection_json.c_str();
}

// ============================================================================
// Parameter Declaration API
// ============================================================================

/**
 * Get number of parameter declarations from compile result
 * @return Number of declared parameters
 */
WASM_EXPORT uint32_t akkado_get_param_decl_count() {
    return static_cast<uint32_t>(g_compile_result.param_decls.size());
}

/**
 * Get parameter name by index
 * @param index Parameter index (0 to count-1)
 * @return Pointer to null-terminated name string, or nullptr if invalid
 */
WASM_EXPORT const char* akkado_get_param_name(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return nullptr;
    return g_compile_result.param_decls[index].name.c_str();
}

/**
 * Get parameter type by index
 * @param index Parameter index
 * @return Type: 0=Continuous, 1=Button, 2=Toggle, 3=Select, or -1 if invalid
 */
WASM_EXPORT int akkado_get_param_type(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return -1;
    return static_cast<int>(g_compile_result.param_decls[index].type);
}

/**
 * Get parameter default value
 * @param index Parameter index
 * @return Default value
 */
WASM_EXPORT float akkado_get_param_default(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0.0f;
    return g_compile_result.param_decls[index].default_value;
}

/**
 * Get parameter minimum value (Continuous only)
 * @param index Parameter index
 * @return Minimum value
 */
WASM_EXPORT float akkado_get_param_min(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0.0f;
    return g_compile_result.param_decls[index].min_value;
}

/**
 * Get parameter maximum value (Continuous only)
 * @param index Parameter index
 * @return Maximum value
 */
WASM_EXPORT float akkado_get_param_max(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 1.0f;
    return g_compile_result.param_decls[index].max_value;
}

/**
 * Get number of options for a Select parameter
 * @param index Parameter index
 * @return Number of options, or 0 if not Select type or invalid
 */
WASM_EXPORT uint32_t akkado_get_param_option_count(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0;
    return static_cast<uint32_t>(g_compile_result.param_decls[index].options.size());
}

/**
 * Get option name for a Select parameter
 * @param index Parameter index
 * @param opt_index Option index within the parameter
 * @return Pointer to null-terminated option string, or nullptr if invalid
 */
WASM_EXPORT const char* akkado_get_param_option(uint32_t index, uint32_t opt_index) {
    if (index >= g_compile_result.param_decls.size()) return nullptr;
    const auto& param = g_compile_result.param_decls[index];
    if (opt_index >= param.options.size()) return nullptr;
    return param.options[opt_index].c_str();
}

/**
 * Get source offset for a parameter declaration
 * @param index Parameter index
 * @return Source offset in bytes
 */
WASM_EXPORT uint32_t akkado_get_param_source_offset(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0;
    return g_compile_result.param_decls[index].source_offset;
}

/**
 * Get source length for a parameter declaration
 * @param index Parameter index
 * @return Source length in characters
 */
WASM_EXPORT uint32_t akkado_get_param_source_length(uint32_t index) {
    if (index >= g_compile_result.param_decls.size()) return 0;
    return g_compile_result.param_decls[index].source_length;
}

// ============================================================================
// Builtin Variable Override API
// ============================================================================

/**
 * Get number of builtin variable overrides from compile result
 * @return Number of overrides
 */
WASM_EXPORT uint32_t akkado_get_builtin_var_override_count() {
    return static_cast<uint32_t>(g_compile_result.builtin_var_overrides.size());
}

/**
 * Get builtin variable override name by index
 * @param index Override index (0 to count-1)
 * @return Pointer to null-terminated name string, or nullptr if invalid
 */
WASM_EXPORT const char* akkado_get_builtin_var_override_name(uint32_t index) {
    if (index >= g_compile_result.builtin_var_overrides.size()) return nullptr;
    return g_compile_result.builtin_var_overrides[index].name.c_str();
}

/**
 * Get builtin variable override value by index
 * @param index Override index
 * @return Override value as float
 */
WASM_EXPORT float akkado_get_builtin_var_override_value(uint32_t index) {
    if (index >= g_compile_result.builtin_var_overrides.size()) return 0.0f;
    return g_compile_result.builtin_var_overrides[index].value;
}

// ============================================================================
// Visualization Declaration API
// ============================================================================

/**
 * Get number of visualization declarations from compile result
 * @return Number of declared visualizations
 */
WASM_EXPORT uint32_t akkado_get_viz_count() {
    return static_cast<uint32_t>(g_compile_result.viz_decls.size());
}

/**
 * Get visualization name by index
 * @param index Visualization index (0 to count-1)
 * @return Pointer to null-terminated name string, or nullptr if invalid
 */
WASM_EXPORT const char* akkado_get_viz_name(uint32_t index) {
    if (index >= g_compile_result.viz_decls.size()) return nullptr;
    return g_compile_result.viz_decls[index].name.c_str();
}

/**
 * Get visualization type by index
 * @param index Visualization index
 * @return Type: 0=PianoRoll, 1=Oscilloscope, 2=Waveform, 3=Spectrum, or -1 if invalid
 */
WASM_EXPORT int akkado_get_viz_type(uint32_t index) {
    if (index >= g_compile_result.viz_decls.size()) return -1;
    return static_cast<int>(g_compile_result.viz_decls[index].type);
}

/**
 * Get visualization state_id (for probe-based visualizations)
 * @param index Visualization index
 * @return State ID for probe buffer lookup, or 0 if invalid
 */
WASM_EXPORT uint32_t akkado_get_viz_state_id(uint32_t index) {
    if (index >= g_compile_result.viz_decls.size()) return 0;
    return g_compile_result.viz_decls[index].state_id;
}

/**
 * Get visualization options JSON string
 * @param index Visualization index
 * @return Pointer to JSON options string, or nullptr if invalid
 */
WASM_EXPORT const char* akkado_get_viz_options(uint32_t index) {
    if (index >= g_compile_result.viz_decls.size()) return nullptr;
    const auto& viz = g_compile_result.viz_decls[index];
    if (viz.options_json.empty()) return nullptr;
    return viz.options_json.c_str();
}

/**
 * Get source offset for a visualization declaration
 * @param index Visualization index
 * @return Source offset in bytes
 */
WASM_EXPORT uint32_t akkado_get_viz_source_offset(uint32_t index) {
    if (index >= g_compile_result.viz_decls.size()) return 0;
    return g_compile_result.viz_decls[index].source_offset;
}

/**
 * Get source length for a visualization declaration
 * @param index Visualization index
 * @return Source length in characters
 */
WASM_EXPORT uint32_t akkado_get_viz_source_length(uint32_t index) {
    if (index >= g_compile_result.viz_decls.size()) return 0;
    return g_compile_result.viz_decls[index].source_length;
}

/**
 * Get pattern state_init index for a piano roll visualization
 * @param index Visualization index
 * @return Index into state_inits array, or -1 if not a piano roll or no pattern linked
 */
WASM_EXPORT int32_t akkado_get_viz_pattern_index(uint32_t index) {
    if (index >= g_compile_result.viz_decls.size()) return -1;
    return g_compile_result.viz_decls[index].pattern_state_init_index;
}

// ============================================================================
// Probe Data API (for oscilloscope/waveform/spectrum visualizations)
// ============================================================================

// Static buffer for probe data copy (avoid concurrent access issues)
static float g_probe_data_buffer[1024];

/**
 * Get the number of samples available in a probe buffer
 * @param state_id The state_id of the probe (from viz decl)
 * @return Number of samples (0-1024), or 0 if not found
 */
WASM_EXPORT uint32_t cedar_get_probe_sample_count(uint32_t state_id) {
    if (!g_vm) return 0;

    auto& states = g_vm->states();
    if (!states.exists(state_id)) return 0;

    auto* probe = states.get_if<cedar::ProbeState>(state_id);
    if (!probe) return 0;

    return static_cast<uint32_t>(probe->sample_count());
}

/**
 * Get probe data as a contiguous array (linearized from ring buffer)
 * @param state_id The state_id of the probe
 * @return Pointer to float array with samples, or nullptr if not found
 *
 * The returned data is ordered from oldest to newest sample.
 * Call cedar_get_probe_sample_count first to get the count.
 */
WASM_EXPORT const float* cedar_get_probe_data(uint32_t state_id) {
    if (!g_vm) return nullptr;

    auto& states = g_vm->states();
    if (!states.exists(state_id)) return nullptr;

    auto* probe = states.get_if<cedar::ProbeState>(state_id);
    if (!probe || !probe->initialized) return nullptr;

    // Linearize ring buffer: copy from write_pos to end, then from start to write_pos
    std::size_t size = cedar::ProbeState::PROBE_BUFFER_SIZE;
    std::size_t wp = probe->write_pos;

    // Copy oldest samples first (from write_pos to end)
    std::size_t first_chunk = size - wp;
    std::memcpy(g_probe_data_buffer, probe->buffer + wp, first_chunk * sizeof(float));

    // Copy newest samples (from start to write_pos)
    if (wp > 0) {
        std::memcpy(g_probe_data_buffer + first_chunk, probe->buffer, wp * sizeof(float));
    }

    return g_probe_data_buffer;
}

/**
 * Get the write position in the probe ring buffer
 * Useful for efficient partial updates
 * @param state_id The state_id of the probe
 * @return Current write position (0-1023)
 */
WASM_EXPORT uint32_t cedar_get_probe_write_pos(uint32_t state_id) {
    if (!g_vm) return 0;

    auto& states = g_vm->states();
    if (!states.exists(state_id)) return 0;

    auto* probe = states.get_if<cedar::ProbeState>(state_id);
    if (!probe) return 0;

    return static_cast<uint32_t>(probe->write_pos);
}

// ============================================================================
// FFT Probe Data API (for waterfall/spectrum visualizations)
// ============================================================================

// Static buffer for FFT magnitude copy
static float g_fft_magnitude_buffer[1025];  // MAX_BINS

/**
 * Get number of frequency bins for this FFT probe
 * @param state_id The state_id of the FFT probe
 * @return Number of bins (nfft/2+1), or 0 if not found
 */
WASM_EXPORT uint32_t cedar_get_fft_bin_count(uint32_t state_id) {
    if (!g_vm) return 0;

    auto& states = g_vm->states();
    if (!states.exists(state_id)) return 0;

    auto* fft_probe = states.get_if<cedar::FFTProbeState>(state_id);
    if (!fft_probe) return 0;

    return static_cast<uint32_t>(fft_probe->fft_size / 2 + 1);
}

/**
 * Get magnitude spectrum in dB
 * @param state_id The state_id of the FFT probe
 * @return Pointer to static buffer with magnitudes, or nullptr if not found
 */
WASM_EXPORT const float* cedar_get_fft_magnitudes(uint32_t state_id) {
    if (!g_vm) return nullptr;

    auto& states = g_vm->states();
    if (!states.exists(state_id)) return nullptr;

    auto* fft_probe = states.get_if<cedar::FFTProbeState>(state_id);
    if (!fft_probe || !fft_probe->initialized) return nullptr;

    std::size_t bin_count = fft_probe->fft_size / 2 + 1;
    std::memcpy(g_fft_magnitude_buffer, fft_probe->magnitudes_db, bin_count * sizeof(float));
    return g_fft_magnitude_buffer;
}

/**
 * Get frame counter to detect new FFT frames without redundant data copies
 * @param state_id The state_id of the FFT probe
 * @return Frame counter value, or 0 if not found
 */
WASM_EXPORT uint32_t cedar_get_fft_frame_counter(uint32_t state_id) {
    if (!g_vm) return 0;

    auto& states = g_vm->states();
    if (!states.exists(state_id)) return 0;

    auto* fft_probe = states.get_if<cedar::FFTProbeState>(state_id);
    if (!fft_probe) return 0;

    return fft_probe->frame_counter;
}

// ============================================================================
// Debug/Disassembly API
// ============================================================================

// Static buffer for disassembly JSON
static std::string g_disassembly_json;


/**
 * Get disassembly of compiled bytecode as JSON
 * Returns a JSON array of instruction objects
 *
 * Format:
 * {
 *   "instructions": [
 *     {
 *       "index": 0,
 *       "opcode": "OSC_SIN",
 *       "opcodeNum": 20,
 *       "out": 0,
 *       "inputs": [1, 65535, 65535, 65535, 65535],
 *       "stateId": 1234567890,
 *       "rate": 0,
 *       "stateful": true,
 *       "source": {"line": 5, "column": 3, "offset": 42, "length": 12}
 *     },
 *     ...
 *   ],
 *   "summary": {
 *     "totalInstructions": 10,
 *     "statefulCount": 3,
 *     "uniqueStateIds": 3,
 *     "stateIds": [123, 456, 789]
 *   }
 * }
 */
WASM_EXPORT const char* akkado_get_disassembly() {
    g_disassembly_json.clear();

    if (g_compile_result.bytecode.empty()) {
        g_disassembly_json = "{\"instructions\":[],\"summary\":{\"totalInstructions\":0,\"statefulCount\":0,\"uniqueStateIds\":0,\"stateIds\":[]}}";
        return g_disassembly_json.c_str();
    }

    constexpr size_t INST_SIZE = sizeof(cedar::Instruction);
    size_t inst_count = g_compile_result.bytecode.size() / INST_SIZE;
    auto instructions = reinterpret_cast<const cedar::Instruction*>(g_compile_result.bytecode.data());

    // Source locations parallel array (may be empty for older compiles)
    const auto& source_locations = g_compile_result.source_locations;
    bool has_source_info = source_locations.size() == inst_count;

    std::ostringstream json;
    json << "{\"instructions\":[";

    std::vector<uint32_t> state_ids;
    int stateful_count = 0;

    for (size_t i = 0; i < inst_count; ++i) {
        const auto& inst = instructions[i];
        if (i > 0) json << ",";

        json << "{\"index\":" << i;
        json << ",\"opcode\":\"" << cedar::opcode_to_string(inst.opcode) << "\"";
        json << ",\"opcodeNum\":" << static_cast<int>(inst.opcode);
        json << ",\"out\":" << inst.out_buffer;
        json << ",\"inputs\":[" << inst.inputs[0] << "," << inst.inputs[1] << ","
             << inst.inputs[2] << "," << inst.inputs[3] << "," << inst.inputs[4] << "]";
        json << ",\"stateId\":" << inst.state_id;
        json << ",\"rate\":" << static_cast<int>(inst.rate);

        bool is_stateful = cedar::opcode_is_stateful(inst.opcode);
        json << ",\"stateful\":" << (is_stateful ? "true" : "false");

        // Add source location if available
        if (has_source_info) {
            const auto& loc = source_locations[i];
            json << ",\"source\":{\"line\":" << loc.line
                 << ",\"column\":" << loc.column
                 << ",\"offset\":" << loc.offset
                 << ",\"length\":" << loc.length << "}";
        }

        json << "}";

        if (is_stateful && inst.state_id != 0) {
            stateful_count++;
            if (std::find(state_ids.begin(), state_ids.end(), inst.state_id) == state_ids.end()) {
                state_ids.push_back(inst.state_id);
            }
        }
    }

    json << "],\"summary\":{";
    json << "\"totalInstructions\":" << inst_count;
    json << ",\"statefulCount\":" << stateful_count;
    json << ",\"uniqueStateIds\":" << state_ids.size();
    json << ",\"stateIds\":[";
    for (size_t i = 0; i < state_ids.size(); ++i) {
        if (i > 0) json << ",";
        json << state_ids[i];
    }
    json << "]}}";

    g_disassembly_json = json.str();
    return g_disassembly_json.c_str();
}

/**
 * Get the number of unique state IDs in compiled bytecode
 * Useful for quick debugging without full disassembly
 */
WASM_EXPORT uint32_t akkado_get_unique_state_count() {
    if (g_compile_result.bytecode.empty()) return 0;

    constexpr size_t INST_SIZE = sizeof(cedar::Instruction);
    size_t inst_count = g_compile_result.bytecode.size() / INST_SIZE;
    auto instructions = reinterpret_cast<const cedar::Instruction*>(g_compile_result.bytecode.data());

    std::vector<uint32_t> state_ids;
    for (size_t i = 0; i < inst_count; ++i) {
        const auto& inst = instructions[i];
        if (cedar::opcode_is_stateful(inst.opcode) && inst.state_id != 0) {
            if (std::find(state_ids.begin(), state_ids.end(), inst.state_id) == state_ids.end()) {
                state_ids.push_back(inst.state_id);
            }
        }
    }
    return static_cast<uint32_t>(state_ids.size());
}

// ============================================================================
// SoundFont API
// ============================================================================

// Static buffer for SoundFont preset JSON
static std::string g_soundfont_presets_json;

/**
 * Load a SoundFont from memory
 * @param name Display name (null-terminated)
 * @param data Pointer to SF2 file data
 * @param size Size of data in bytes
 * @return SoundFont ID (>=0) on success, -1 on failure
 */
WASM_EXPORT int32_t cedar_load_soundfont(const char* name, const uint8_t* data, int32_t size) {
    if (!g_vm || !name || !data || size <= 0) {
        return -1;
    }

    return static_cast<int32_t>(g_vm->soundfont_registry().load_from_memory(
        cedar::MemoryView(data, static_cast<std::size_t>(size)),
        name, g_vm->sample_bank()));
}

/**
 * Load a `.mid` file from memory into the VM's name-keyed midi sequence
 * registry. Subsequent cedar_apply_midi_sources() calls will attach the
 * parsed sequence to any MidiQueueState whose RequiredMidiSource.name_or_path
 * matches `name`. Mirrors cedar_load_soundfont's contract.
 *
 * @param name Display name / lookup key (must match the akkado source
 *             reference, typically the file basename)
 * @param data Pointer to .mid file bytes
 * @param size Size of `data` in bytes
 * @return 0 on success, negative on parse failure or invalid arguments
 */
WASM_EXPORT int32_t cedar_load_midi_file(const char* name, const uint8_t* data, int32_t size) {
    if (!g_vm || !name || !data || size <= 0) {
        return -1;
    }
    return g_vm->load_midi_file(name, data, static_cast<std::size_t>(size));
}

/**
 * Get number of presets in a loaded SoundFont
 * @param sf_id SoundFont ID
 * @return Number of presets, or 0 if invalid
 */
WASM_EXPORT uint32_t cedar_soundfont_preset_count(int sf_id) {
    if (!g_vm) return 0;

    const auto* bank = g_vm->soundfont_registry().get(sf_id);
    if (!bank) return 0;
    return static_cast<uint32_t>(bank->presets.size());
}

/**
 * Get preset list as JSON for a loaded SoundFont
 * @param sf_id SoundFont ID
 * @return Pointer to null-terminated JSON string
 */
WASM_EXPORT const char* cedar_soundfont_presets_json(int sf_id) {
    if (!g_vm) {
        g_soundfont_presets_json = "[]";
        return g_soundfont_presets_json.c_str();
    }

    g_soundfont_presets_json = g_vm->soundfont_registry().get_presets_json(sf_id);
    return g_soundfont_presets_json.c_str();
}

/**
 * Get number of loaded SoundFonts
 * @return Count of loaded SoundFonts
 */
WASM_EXPORT uint32_t cedar_soundfont_count() {
    if (!g_vm) return 0;
    return static_cast<uint32_t>(g_vm->soundfont_registry().size());
}

} // extern "C"
