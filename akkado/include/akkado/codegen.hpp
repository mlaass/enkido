#pragma once

#include "ast.hpp"
#include "diagnostics.hpp"
#include "symbol_table.hpp"
#include "sample_registry.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace akkado {

/// State initialization data for SEQ_STEP and TIMELINE opcodes
struct StateInitData {
    std::uint32_t state_id;  // Must match Instruction::state_id (32-bit FNV-1a hash)
    enum class Type : std::uint8_t {
        SeqStep,   // Initialize SeqStepState with timed events
        Timeline   // Initialize TimelineState with breakpoints
    } type;

    // For SeqStep: parallel arrays of event data
    std::vector<float> times;       // Event times in beats
    std::vector<float> values;      // Values (sample ID, pitch, etc.)
    std::vector<float> velocities;  // Velocity per event (0.0-1.0)
    std::vector<std::string> sample_names;  // Sample names (for deferred resolution)
    float cycle_length = 4.0f;      // Cycle length in beats

    // For Timeline: [time, value, curve, ...] triplets (existing usage)
};

/// Result of code generation
struct CodeGenResult {
    std::vector<cedar::Instruction> instructions;
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;  // State initialization data
    std::vector<std::string> required_samples;  // Unique sample names used
    bool success = false;
};

/// Buffer allocator for code generation
/// Simple linear allocation with no reuse (MVP)
class BufferAllocator {
public:
    static constexpr std::uint16_t MAX_BUFFERS = 256;
    static constexpr std::uint16_t BUFFER_UNUSED = 0xFFFF;

    BufferAllocator() = default;

    /// Allocate a new buffer
    /// Returns BUFFER_UNUSED if pool exhausted
    [[nodiscard]] std::uint16_t allocate();

    /// Get current allocation count
    [[nodiscard]] std::uint16_t count() const { return next_; }

    /// Check if any buffers available
    [[nodiscard]] bool has_available() const { return next_ < MAX_BUFFERS; }

private:
    std::uint16_t next_ = 0;
};

/// Code generator: converts analyzed AST to Cedar bytecode
class CodeGenerator {
public:
    /// Generate bytecode from analyzed AST
    /// @param ast The transformed AST (after pipe rewriting)
    /// @param symbols Symbol table from semantic analysis
    /// @param filename Filename for error reporting
    /// @param sample_registry Optional sample registry for resolving sample names to IDs
    CodeGenResult generate(const Ast& ast, SymbolTable& symbols,
                          std::string_view filename = "<input>",
                          SampleRegistry* sample_registry = nullptr);

private:
    /// Visit AST node and emit instructions
    /// Returns the buffer index containing this node's result
    std::uint16_t visit(NodeIndex node);

    /// Emit a single instruction
    void emit(const cedar::Instruction& inst);

    /// Generate semantic ID from path
    [[nodiscard]] std::uint32_t compute_state_id() const;

    /// Push/pop semantic path for nested structures
    void push_path(std::string_view segment);
    void pop_path();

    /// Error helpers
    void error(const std::string& code, const std::string& message, SourceLocation loc);

    /// FM Detection: Automatically upgrade oscillators to 4x when FM is detected
    /// @param freq_buffer The buffer index containing the frequency input
    /// @return true if the frequency input traces back to an audio-rate source
    [[nodiscard]] bool is_fm_modulated(std::uint16_t freq_buffer) const;

    /// Check if opcode produces an audio-rate signal (oscillators, noise)
    [[nodiscard]] static bool is_audio_rate_producer(cedar::Opcode op);

    /// Check if opcode is a basic oscillator that can be upgraded to 4x
    [[nodiscard]] static bool is_upgradeable_oscillator(cedar::Opcode op);

    /// Upgrade basic oscillator opcode to 4x oversampled variant
    [[nodiscard]] static cedar::Opcode upgrade_for_fm(cedar::Opcode op);

    /// Handle osc() function calls - Strudel-style oscillator selection
    /// Resolves string type parameter at compile time to the appropriate opcode.
    /// @param node The Call node
    /// @param n The Node reference
    /// @return Output buffer index
    std::uint16_t handle_osc_call(NodeIndex node, const Node& n);

    /// Handle len() function calls - compile-time array length
    /// Returns the number of elements in an array literal.
    /// @param node The Call node
    /// @param n The Node reference
    /// @return Output buffer index with constant length value
    std::uint16_t handle_len_call(NodeIndex node, const Node& n);

    /// Handle user-defined function calls - inline expansion
    /// @param node The Call node
    /// @param n The Node reference
    /// @param func The user function info from symbol table
    /// @return Output buffer index
    std::uint16_t handle_user_function_call(NodeIndex node, const Node& n,
                                            const UserFunctionInfo& func);

    // Context
    const Ast* ast_ = nullptr;
    SymbolTable* symbols_ = nullptr;
    SampleRegistry* sample_registry_ = nullptr;
    BufferAllocator buffers_;
    std::vector<cedar::Instruction> instructions_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<StateInitData> state_inits_;  // State initialization data
    std::string filename_;

    // Semantic path tracking for state_id generation
    std::vector<std::string> path_stack_;
    std::uint32_t anonymous_counter_ = 0;

    // Track call counts per stateful function for unique state_ids
    std::unordered_map<std::string, std::uint32_t> call_counters_;

    // Track unique sample names used (for runtime loading)
    std::set<std::string> required_samples_;

    // Map from AST node index to output buffer index
    std::unordered_map<NodeIndex, std::uint16_t> node_buffers_;

    // Map from parameter name hash to literal AST node (for inline match resolution)
    // Only populated during user function calls when the argument is a literal
    std::unordered_map<std::uint32_t, NodeIndex> param_literals_;
};

} // namespace akkado
