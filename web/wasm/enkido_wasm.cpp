/**
 * Enkido WASM Bindings
 *
 * Provides a C-style API for Cedar VM and Akkado compiler
 * to be compiled with Emscripten for browser use.
 */

#include <cedar/vm/vm.hpp>
#include <cedar/vm/instruction.hpp>
#include <akkado/akkado.hpp>
#include <akkado/sample_registry.hpp>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <memory>

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
    std::printf("[Cedar C++] cedar_load_program called: bytecode=%p, byte_count=%u\n",
                static_cast<const void*>(bytecode), byte_count);

    if (!g_vm || !bytecode) {
        std::printf("[Cedar C++] load_program FAILED: g_vm=%p, bytecode=%p\n",
                    static_cast<void*>(g_vm.get()), static_cast<const void*>(bytecode));
        return -1;
    }

    // Each instruction is 16 bytes
    constexpr size_t INST_SIZE = sizeof(cedar::Instruction);
    if (byte_count % INST_SIZE != 0) {
        std::printf("[Cedar C++] load_program FAILED: byte_count %u not multiple of %zu\n",
                    byte_count, INST_SIZE);
        return -2;
    }

    size_t inst_count = byte_count / INST_SIZE;
    auto instructions = reinterpret_cast<const cedar::Instruction*>(bytecode);

    std::printf("[Cedar C++] Loading %zu instructions\n", inst_count);

    auto result = g_vm->load_program(std::span{instructions, inst_count});

    std::printf("[Cedar C++] load_program result=%d, has_pending_swap=%d, swap_count=%u\n",
                static_cast<int>(result),
                g_vm->has_pending_swap() ? 1 : 0,
                g_vm->swap_count());

    return static_cast<int>(result);
}

/**
 * Process one block of audio (128 samples)
 * After calling, use cedar_get_output_left/right to get the audio data
 */
WASM_EXPORT void cedar_process_block() {
    if (g_vm) {
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
 * @return Sample ID (>0) on success, 0 on failure
 */
WASM_EXPORT uint32_t cedar_load_sample(const char* name,
                                        const float* audio_data,
                                        uint32_t num_samples,
                                        uint32_t channels,
                                        float sample_rate) {
    if (!g_vm || !name || !audio_data || channels == 0) {
        return 0;
    }
    
    return g_vm->load_sample(name, audio_data, num_samples, channels, sample_rate);
}

/**
 * Load a sample from WAV file data in memory
 * @param name Sample name (null-terminated)
 * @param wav_data Pointer to WAV file data
 * @param wav_size Size of WAV data in bytes
 * @return Sample ID (>0) on success, 0 on failure
 */
WASM_EXPORT uint32_t cedar_load_sample_wav(const char* name,
                                            const uint8_t* wav_data,
                                            uint32_t wav_size) {
    if (!g_vm || !name || !wav_data || wav_size == 0) {
        return 0;
    }
    
    return g_vm->sample_bank().load_wav_memory(name, wav_data, wav_size);
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
    std::printf("[akkado_compile] ENTRY: source=%p, len=%u\n",
                static_cast<const void*>(source), source_len);

    if (!source) {
        std::printf("[akkado_compile] null source, returning 0\n");
        return 0;
    }

    std::printf("[akkado_compile] Creating string_view...\n");
    std::string_view src{source, source_len};
    std::printf("[akkado_compile] string_view created, calling akkado::compile...\n");

    // Compile to a fresh local result first
    akkado::CompileResult new_result = akkado::compile(src, "<web>", nullptr);
    std::printf("[akkado_compile] akkado::compile returned, success=%d\n", new_result.success ? 1 : 0);

    // Now swap - if the old result is corrupted, the crash happens here
    std::printf("[akkado_compile] About to swap with old result...\n");
    std::swap(g_compile_result, new_result);
    std::printf("[akkado_compile] Swap complete, about to destroy old result...\n");
    // new_result (now containing old data) is destroyed here

    std::printf("[akkado_compile] Returning success=%d\n", g_compile_result.success ? 1 : 0);
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

/**
 * Resolve sample IDs in state_inits using currently loaded samples.
 * Call this AFTER loading required samples, BEFORE cedar_apply_state_inits().
 * This maps sample names to IDs in the sample bank.
 */
WASM_EXPORT void akkado_resolve_sample_ids() {
    if (!g_vm) return;

    for (auto& init : g_compile_result.state_inits) {
        for (size_t i = 0; i < init.sample_names.size(); ++i) {
            const auto& name = init.sample_names[i];
            if (!name.empty()) {
                auto id = g_vm->sample_bank().get_sample_id(name);
                init.values[i] = static_cast<float>(id);
            }
        }
    }
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
 * Get type for a state initialization (0=SeqStep, 1=Timeline)
 * @param index State init index
 * @return type
 */
WASM_EXPORT int akkado_get_state_init_type(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return -1;
    return static_cast<int>(g_compile_result.state_inits[index].type);
}

/**
 * Get values count for a state initialization
 * @param index State init index
 * @return Number of values
 */
WASM_EXPORT uint32_t akkado_get_state_init_values_count(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return 0;
    return static_cast<uint32_t>(g_compile_result.state_inits[index].values.size());
}

/**
 * Get values pointer for a state initialization
 * @param index State init index
 * @return Pointer to float array of values
 */
WASM_EXPORT const float* akkado_get_state_init_values(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return nullptr;
    return g_compile_result.state_inits[index].values.data();
}

/**
 * Get times pointer for a state initialization
 * @param index State init index
 * @return Pointer to float array of times
 */
WASM_EXPORT const float* akkado_get_state_init_times(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return nullptr;
    return g_compile_result.state_inits[index].times.data();
}

/**
 * Get velocities pointer for a state initialization
 * @param index State init index
 * @return Pointer to float array of velocities
 */
WASM_EXPORT const float* akkado_get_state_init_velocities(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return nullptr;
    return g_compile_result.state_inits[index].velocities.data();
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
 * Get number of sample names for a state initialization
 * @param index State init index
 * @return Number of sample names
 */
WASM_EXPORT uint32_t akkado_get_state_init_sample_names_count(uint32_t index) {
    if (index >= g_compile_result.state_inits.size()) return 0;
    return static_cast<uint32_t>(g_compile_result.state_inits[index].sample_names.size());
}

/**
 * Get sample name by index for a state initialization
 * @param index State init index
 * @param value_index Sample name index within the state init
 * @return Pointer to null-terminated sample name, or nullptr if empty/invalid
 */
WASM_EXPORT const char* akkado_get_state_init_sample_name(uint32_t index, uint32_t value_index) {
    if (index >= g_compile_result.state_inits.size()) return nullptr;
    const auto& init = g_compile_result.state_inits[index];
    if (value_index >= init.sample_names.size()) return nullptr;
    if (init.sample_names[value_index].empty()) return nullptr;
    return init.sample_names[value_index].c_str();
}

/**
 * Apply a state initialization to the VM
 * @param state_id State ID to initialize (32-bit FNV-1a hash)
 * @param times Pointer to float array of event times (in beats)
 * @param values Pointer to float array of values
 * @param velocities Pointer to float array of velocities
 * @param count Number of events
 * @param cycle_length Cycle length in beats
 * @return 1 on success, 0 on failure
 */
WASM_EXPORT int cedar_init_seq_step_state(uint32_t state_id,
                                           const float* times,
                                           const float* values,
                                           const float* velocities,
                                           uint32_t count,
                                           float cycle_length) {
    if (!g_vm || !times || !values || !velocities) return 0;
    g_vm->init_seq_step_state(state_id, times, values, velocities, count, cycle_length);
    return 1;
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
        if (init.type == akkado::StateInitData::Type::SeqStep) {
            g_vm->init_seq_step_state(
                init.state_id,
                init.times.data(),
                init.values.data(),
                init.velocities.data(),
                init.values.size(),
                init.cycle_length
            );
            count++;
        }
        // Timeline state init would go here if needed
    }
    return count;
}

// ============================================================================
// Utility
// ============================================================================

/**
 * Get block size (128 samples)
 */
WASM_EXPORT uint32_t enkido_get_block_size() {
    return 128;
}

/**
 * Allocate memory in WASM heap (for passing data from JS)
 * @param size Size in bytes
 * @return Pointer to allocated memory, or nullptr on failure
 */
WASM_EXPORT void* enkido_malloc(uint32_t size) {
    return std::malloc(size);
}

/**
 * Free memory allocated with enkido_malloc
 * @param ptr Pointer to free
 */
WASM_EXPORT void enkido_free(void* ptr) {
    std::free(ptr);
}

} // extern "C"
