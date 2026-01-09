/**
 * Enkido WASM Bindings
 *
 * Provides a C-style API for Cedar VM and Akkado compiler
 * to be compiled with Emscripten for browser use.
 */

#include <cedar/vm/vm.hpp>
#include <cedar/vm/instruction.hpp>
#include <akkado/akkado.hpp>
#include <cstdint>
#include <cstring>
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
    if (!g_vm || !bytecode) return -1;

    // Each instruction is 16 bytes
    constexpr size_t INST_SIZE = sizeof(cedar::Instruction);
    if (byte_count % INST_SIZE != 0) return -2;

    size_t inst_count = byte_count / INST_SIZE;
    auto instructions = reinterpret_cast<const cedar::Instruction*>(bytecode);

    auto result = g_vm->load_program(std::span{instructions, inst_count});
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
// Akkado Compiler API
// ============================================================================

/**
 * Compile Akkado source code to Cedar bytecode
 * @param source Source code (null-terminated)
 * @param source_len Length of source string
 * @return 1 on success, 0 on error
 */
WASM_EXPORT int akkado_compile(const char* source, uint32_t source_len) {
    if (!source) return 0;

    std::string_view src{source, source_len};
    g_compile_result = akkado::compile(src, "<web>");

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
 */
WASM_EXPORT void akkado_clear_result() {
    g_compile_result = akkado::CompileResult{};
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
