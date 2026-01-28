#pragma once

#include "ast.hpp"
#include "diagnostics.hpp"
#include "symbol_table.hpp"
#include "sample_registry.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace akkado {

// ============================================================================
// Parameter Exposure System
// ============================================================================

/// Type of exposed parameter for UI generation
enum class ParamType : std::uint8_t {
    Continuous = 0,  // Float value in range [min, max] - rendered as slider
    Button = 1,      // Momentary: 1 while pressed, 0 otherwise
    Toggle = 2,      // Boolean: 0 or 1, click to flip
    Select = 3       // Discrete: integer index into options array
};

/// Declaration of an exposed parameter extracted at compile time
/// Used for auto-generating UI controls and external binding (Godot, MIDI, etc.)
struct ParamDecl {
    std::string name;              // Display name and EnvMap key
    std::uint32_t name_hash = 0;   // FNV-1a hash for ENV_GET lookup
    ParamType type = ParamType::Continuous;
    float default_value = 0.0f;    // Initial value
    float min_value = 0.0f;        // Minimum (Continuous only)
    float max_value = 1.0f;        // Maximum (Continuous only)
    std::vector<std::string> options;  // Option names (Select only)

    // Source location for UI linking (click-to-source)
    std::uint32_t source_offset = 0;   // Byte offset in source
    std::uint32_t source_length = 0;   // Length in source
};

// ============================================================================
// State Initialization Data
// ============================================================================

/// Sample name mapping for SequenceProgram deferred resolution
/// Tracks which events in which sequences need sample ID resolution
struct SequenceSampleMapping {
    std::uint16_t seq_idx;    // Index into sequences vector
    std::uint16_t event_idx;  // Index into sequence's events array
    std::string sample_name;  // Sample name to resolve
};

/// State initialization data for SEQ_STEP, TIMELINE, and SEQPAT_QUERY opcodes
struct StateInitData {
    std::uint32_t state_id;  // Must match Instruction::state_id (32-bit FNV-1a hash)
    enum class Type : std::uint8_t {
        SeqStep,          // Initialize SeqStepState with timed events
        Timeline,         // Initialize TimelineState with breakpoints
        SequenceProgram   // Initialize SequenceState with compiled sequences
    } type;

    // For SeqStep: parallel arrays of event data
    std::vector<float> times;       // Event times in beats
    std::vector<float> values;      // Values (sample ID, pitch, etc.)
    std::vector<float> velocities;  // Velocity per event (0.0-1.0)
    std::vector<std::string> sample_names;  // Sample names (for deferred resolution)
    float cycle_length = 4.0f;      // Cycle length in beats

    // For Timeline: [time, value, curve, ...] triplets (existing usage)

    // For SequenceProgram: compiled sequences for lazy query
    // Note: The Sequence structs contain pointers to event vectors stored here
    std::vector<cedar::Sequence> sequences;  // Compiled sequence data (shallow copy)
    std::vector<std::vector<cedar::Event>> sequence_events;  // Actual event storage per sequence
    bool is_sample_pattern = false;  // Sample pattern vs pitch pattern

    // Size hints for arena allocation
    std::uint32_t total_events = 0;  // Total event count across all sequences

    // For SequenceProgram: sample name mappings for deferred resolution
    std::vector<SequenceSampleMapping> sequence_sample_mappings;

    // Pattern string location in document (for UI highlighting)
    SourceLocation pattern_location;  // Document offset of pattern string
};

/// Result of code generation
struct CodeGenResult {
    std::vector<cedar::Instruction> instructions;
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;  // State initialization data
    std::vector<std::string> required_samples;  // Unique sample names used
    std::vector<ParamDecl> param_decls;  // Declared parameters for UI generation
    bool success = false;
};

/// Buffer allocator for code generation
/// Simple linear allocation with no reuse (MVP)
class BufferAllocator {
public:
    static constexpr std::uint16_t MAX_BUFFERS = 256;
    static constexpr std::uint16_t BUFFER_UNUSED = 0xFFFF;
    // Buffer 255 is reserved for BUFFER_ZERO (always contains 0.0)
    static constexpr std::uint16_t MAX_ALLOCATABLE = 255;

    BufferAllocator() = default;

    /// Allocate a new buffer
    /// Returns BUFFER_UNUSED if pool exhausted
    [[nodiscard]] std::uint16_t allocate();

    /// Get current allocation count
    [[nodiscard]] std::uint16_t count() const { return next_; }

    /// Check if any buffers available
    [[nodiscard]] bool has_available() const { return next_ < MAX_ALLOCATABLE; }

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

    /// Warning helper - emits a warning diagnostic but does not stop compilation
    void warn(const std::string& code, const std::string& message, SourceLocation loc);

    /// Check if a match expression can be resolved at compile time
    /// Returns true if scrutinee (if present) is const and all patterns/guards are const
    [[nodiscard]] bool is_compile_time_match(NodeIndex node, const Node& n) const;

    /// Handle compile-time match - evaluate patterns and guards, emit only winning branch
    std::uint16_t handle_compile_time_match(NodeIndex node, const Node& n);

    /// Handle runtime match - emit all branches and build nested select chain
    std::uint16_t handle_runtime_match(NodeIndex node, const Node& n);

    /// FM Detection: Automatically upgrade oscillators to 4x when FM is detected
    /// @param freq_buffer The buffer index containing the frequency input
    /// @return true if the frequency input traces back to an audio-rate source
    /// Note: is_audio_rate_producer, is_upgradeable_oscillator, upgrade_for_fm
    /// are inline helpers in akkado/codegen/fm_detection.hpp
    [[nodiscard]] bool is_fm_modulated(std::uint16_t freq_buffer) const;

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

    /// Handle FunctionValue calls - inline expansion of lambda assigned to variable
    /// @param node The Call node
    /// @param n The Node reference
    /// @param func The function reference from symbol table
    /// @return Output buffer index
    std::uint16_t handle_function_value_call(NodeIndex node, const Node& n,
                                              const FunctionRef& func);

    /// Handle Closure nodes - allocate buffers for parameters and generate body
    /// @param node The Closure node
    /// @param n The Node reference
    /// @return Output buffer index
    std::uint16_t handle_closure(NodeIndex node, const Node& n);

    /// Handle MatchExpr nodes - compile-time match resolution
    /// @param node The MatchExpr node
    /// @param n The Node reference
    /// @return Output buffer index
    std::uint16_t handle_match_expr(NodeIndex node, const Node& n);

    /// Handle pattern variable reference
    /// Emits SEQ_STEP code for the stored pattern.
    /// @param name The pattern variable name (for path tracking)
    /// @param pattern_node The MiniLiteral node index
    /// @param loc Source location for error reporting
    /// @return Output buffer index (pitch or sample_id)
    std::uint16_t handle_pattern_reference(const std::string& name, NodeIndex pattern_node,
                                           SourceLocation loc);

    /// Handle chord() function calls - Strudel-compatible chord expansion
    /// chord("Am") -> array of MIDI notes
    /// chord("Am C7 F") -> pattern of chord arrays
    /// @param node The Call node
    /// @param n The Node reference
    /// @return Output buffer index
    std::uint16_t handle_chord_call(NodeIndex node, const Node& n);

    /// Handle MiniLiteral (pattern) nodes - pat("c4 e4 g4"), etc.
    std::uint16_t handle_mini_literal(NodeIndex node, const Node& n);

    /// Handle sample patterns (bd, sn, etc.)
    std::uint16_t handle_sample_pattern(NodeIndex node, const Node& n,
                                        const struct PatternEventStream& events,
                                        std::uint32_t state_id);

    /// Handle pitch patterns (c4 e4 g4, etc.)
    std::uint16_t handle_pitch_pattern(NodeIndex node, const Node& n,
                                        const struct PatternEventStream& events,
                                        std::uint32_t state_id,
                                        NodeIndex closure_node);

    /// Handle single chord expansion
    std::uint16_t handle_single_chord(NodeIndex node, const Node& n,
                                      const struct ChordInfo& chord,
                                      const std::string& chord_str);

    /// Handle chord progression (multiple chords)
    std::uint16_t handle_chord_progression(NodeIndex node, const Node& n,
                                           const std::vector<struct ChordInfo>& chords,
                                           const std::string& chord_str);

    /// Handle chord progression using mini-notation event timing
    std::uint16_t handle_chord_progression_events(NodeIndex node, const Node& n,
                                                  const struct PatternEventStream& events,
                                                  const std::string& chord_str);

    // ============================================================================
    // Parameter exposure handlers
    // ============================================================================

    /// Handle param(name, default, min?, max?) - continuous slider parameter
    std::uint16_t handle_param_call(NodeIndex node, const Node& n);

    /// Handle button(name) - momentary button parameter
    std::uint16_t handle_button_call(NodeIndex node, const Node& n);

    /// Handle toggle(name, default?) - boolean toggle parameter
    std::uint16_t handle_toggle_call(NodeIndex node, const Node& n);

    /// Handle select(name, opt1, opt2, ...) - selection dropdown parameter
    std::uint16_t handle_select_call(NodeIndex node, const Node& n);

    // Context
    const Ast* ast_ = nullptr;
    SymbolTable* symbols_ = nullptr;
    SampleRegistry* sample_registry_ = nullptr;
    BufferAllocator buffers_;
    std::vector<cedar::Instruction> instructions_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<StateInitData> state_inits_;  // State initialization data
    std::vector<ParamDecl> param_decls_;      // Declared parameters
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

    // ============================================================================
    // Multi-buffer support for polyphonic arrays (map/sum)
    // ============================================================================
    // Track nodes that produce multiple buffers (arrays/chords for polyphony)
    std::unordered_map<NodeIndex, std::vector<std::uint16_t>> multi_buffers_;

    /// Register a node as producing multiple buffers
    /// @return First buffer index (for compatibility with single-buffer code)
    std::uint16_t register_multi_buffer(NodeIndex node, std::vector<std::uint16_t> buffers);

    /// Check if a node produces multiple buffers
    [[nodiscard]] bool is_multi_buffer(NodeIndex node) const;

    /// Get all buffers produced by a multi-buffer node
    [[nodiscard]] std::vector<std::uint16_t> get_multi_buffers(NodeIndex node) const;

    /// Apply a lambda expression to a single buffer value
    /// Creates a temporary scope, binds the parameter, generates body code
    /// @param lambda_node The Closure node containing parameter and body
    /// @param arg_buf The buffer to bind to the lambda parameter
    /// @return Output buffer from the lambda body
    std::uint16_t apply_lambda(NodeIndex lambda_node, std::uint16_t arg_buf);

    /// Resolve a function argument (can be inline lambda, lambda variable, or fn name)
    /// @param func_node The node that should resolve to a function
    /// @return FunctionRef if valid, nullopt otherwise
    std::optional<FunctionRef> resolve_function_arg(NodeIndex func_node);

    /// Apply a resolved function reference with captures
    /// @param ref The function reference containing closure/body info and captures
    /// @param arg_buf The buffer to bind to the function's first parameter
    /// @param loc Source location for error reporting
    /// @return Output buffer from the function body
    std::uint16_t apply_function_ref(const FunctionRef& ref, std::uint16_t arg_buf,
                                      SourceLocation loc);

    /// Handle map(array, fn) call - apply function to each element
    std::uint16_t handle_map_call(NodeIndex node, const Node& n);

    /// Handle sum(array) call - reduce array by addition
    std::uint16_t handle_sum_call(NodeIndex node, const Node& n);

    /// Handle fold(array, fn, init) call - reduce array with binary function
    std::uint16_t handle_fold_call(NodeIndex node, const Node& n);

    /// Handle zipWith(a, b, fn) call - combine arrays element-wise with function
    std::uint16_t handle_zipWith_call(NodeIndex node, const Node& n);

    /// Handle zip(a, b) call - interleave two arrays
    std::uint16_t handle_zip_call(NodeIndex node, const Node& n);

    /// Handle take(n, array) call - take first n elements
    std::uint16_t handle_take_call(NodeIndex node, const Node& n);

    /// Handle drop(n, array) call - drop first n elements
    std::uint16_t handle_drop_call(NodeIndex node, const Node& n);

    /// Handle reverse(array) call - reverse array order
    std::uint16_t handle_reverse_call(NodeIndex node, const Node& n);

    /// Handle range(start, end) call - generate integer array
    std::uint16_t handle_range_call(NodeIndex node, const Node& n);

    /// Handle repeat(value, n) call - repeat value n times
    std::uint16_t handle_repeat_call(NodeIndex node, const Node& n);

    /// Apply a binary function reference to two buffer arguments
    /// @param ref The function reference containing closure/body info
    /// @param arg_buf1 The first argument buffer
    /// @param arg_buf2 The second argument buffer
    /// @param loc Source location for error reporting
    /// @return Output buffer from the function body
    std::uint16_t apply_binary_function_ref(const FunctionRef& ref,
                                            std::uint16_t arg_buf1,
                                            std::uint16_t arg_buf2,
                                            SourceLocation loc);
};

} // namespace akkado
