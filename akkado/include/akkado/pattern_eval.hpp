#pragma once

#include "ast.hpp"
#include "pattern_event.hpp"
#include <cstdint>
#include <random>

namespace akkado {

/// Evaluates a mini-notation AST into a PatternEventStream
///
/// The evaluator traverses the parsed mini-notation AST and expands
/// all constructs (groups, sequences, modifiers, euclidean rhythms)
/// into a flat timeline of events for one cycle.
class PatternEvaluator {
public:
    /// Construct an evaluator with a reference to the AST arena
    explicit PatternEvaluator(const AstArena& arena);

    /// Enable chord mode - Sample tokens are interpreted as chord symbols
    void set_chord_mode(bool enabled) { chord_mode_ = enabled; }

    /// Evaluate a pattern AST into an event stream
    /// @param pattern_root Root node of the mini-notation AST (MiniPattern)
    /// @param cycle Current cycle number (for sequence rotation)
    /// @return Expanded event stream for one cycle
    [[nodiscard]] PatternEventStream evaluate(NodeIndex pattern_root,
                                               std::uint32_t cycle = 0);

    /// Count how many cycles a pattern spans
    ///
    /// This analyzes the AST to determine multi-cycle patterns:
    /// - MiniSequence (<a b c>) needs N cycles where N = number of elements
    /// - MiniGroup ([a b c]) needs max of children's cycle counts
    /// - Slow modifier (/n) multiplies cycle count by n
    /// - Speed modifier (*n) does not increase cycles (compresses time)
    /// - Atoms need 1 cycle
    ///
    /// @param node Root node to analyze
    /// @return Number of cycles this pattern spans
    [[nodiscard]] std::uint32_t count_cycles(NodeIndex node) const;

private:
    /// Evaluate a single node in the given context
    void eval_node(NodeIndex node, const PatternEvalContext& ctx,
                   PatternEventStream& stream);

    /// Evaluate a MiniPattern node (root of pattern)
    void eval_pattern(NodeIndex node, const PatternEvalContext& ctx,
                      PatternEventStream& stream);

    /// Evaluate a MiniAtom node (pitch, sample, rest)
    void eval_atom(NodeIndex node, const PatternEvalContext& ctx,
                   PatternEventStream& stream);

    /// Evaluate a MiniGroup node (subdivision)
    void eval_group(NodeIndex node, const PatternEvalContext& ctx,
                    PatternEventStream& stream);

    /// Evaluate a MiniSequence node (alternating)
    void eval_sequence(NodeIndex node, const PatternEvalContext& ctx,
                       PatternEventStream& stream);

    /// Evaluate a MiniPolyrhythm node (simultaneous)
    void eval_polyrhythm(NodeIndex node, const PatternEvalContext& ctx,
                         PatternEventStream& stream);

    /// Evaluate a MiniPolymeter node (LCM alignment)
    void eval_polymeter(NodeIndex node, const PatternEvalContext& ctx,
                        PatternEventStream& stream);

    /// Evaluate a MiniChoice node (random selection)
    void eval_choice(NodeIndex node, const PatternEvalContext& ctx,
                     PatternEventStream& stream);

    /// Evaluate a MiniEuclidean node
    void eval_euclidean(NodeIndex node, const PatternEvalContext& ctx,
                        PatternEventStream& stream);

    /// Evaluate a MiniModified node
    void eval_modified(NodeIndex node, const PatternEvalContext& ctx,
                       PatternEventStream& stream);

    /// Generate Euclidean rhythm pattern (Bjorklund algorithm)
    [[nodiscard]] std::vector<bool> generate_euclidean(std::uint8_t hits,
                                                        std::uint8_t steps,
                                                        std::uint8_t rotation);

    /// Count children of a node
    [[nodiscard]] std::size_t count_children(NodeIndex node) const;

    /// Get the Nth child of a node
    [[nodiscard]] NodeIndex get_child(NodeIndex node, std::size_t index) const;

    /// Get the weight and repeat count for a child node
    /// Weight (@n) affects time allocation in parent
    /// Repeat (!n) means this child counts as n children
    /// @return {weight, count} pair
    [[nodiscard]] std::pair<float, int> get_child_weight_and_count(NodeIndex child) const;

    /// Evaluate a node, unwrapping weight/repeat modifiers (parent handles them)
    /// This prevents the modifier from being applied twice
    void eval_node_unwrap(NodeIndex node, const PatternEvalContext& ctx,
                          PatternEventStream& stream);

    const AstArena& arena_;
    std::uint32_t current_cycle_ = 0;
    std::mt19937 rng_;  // For choice operator
    bool chord_mode_ = false;  // When true, interpret Sample atoms as chord symbols
};

/// Convenience function to evaluate a pattern
/// @param pattern_root Root of the mini-notation AST
/// @param arena The AST arena
/// @param cycle Current cycle number
/// @return Expanded event stream
[[nodiscard]] PatternEventStream
evaluate_pattern(NodeIndex pattern_root, const AstArena& arena,
                 std::uint32_t cycle = 0);

/// Convenience function to count cycles in a pattern
/// @param pattern_root Root of the mini-notation AST
/// @param arena The AST arena
/// @return Number of cycles the pattern spans
[[nodiscard]] std::uint32_t
count_pattern_cycles(NodeIndex pattern_root, const AstArena& arena);

/// Evaluate a pattern across all its cycles and combine into single stream
///
/// This handles multi-cycle patterns like <a b c> by:
/// 1. Determining cycle count via count_cycles()
/// 2. Evaluating each cycle
/// 3. Offsetting times by cycle number
/// 4. Combining into single stream with proper cycle_span
///
/// @param pattern_root Root of the mini-notation AST
/// @param arena The AST arena
/// @return Combined event stream spanning all cycles
[[nodiscard]] PatternEventStream
evaluate_pattern_multi_cycle(NodeIndex pattern_root, const AstArena& arena);

} // namespace akkado
