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

    /// Evaluate a pattern AST into an event stream
    /// @param pattern_root Root node of the mini-notation AST (MiniPattern)
    /// @param cycle Current cycle number (for sequence rotation)
    /// @return Expanded event stream for one cycle
    [[nodiscard]] PatternEventStream evaluate(NodeIndex pattern_root,
                                               std::uint32_t cycle = 0);

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

    const AstArena& arena_;
    std::uint32_t current_cycle_ = 0;
    std::mt19937 rng_;  // For choice operator
};

/// Convenience function to evaluate a pattern
/// @param pattern_root Root of the mini-notation AST
/// @param arena The AST arena
/// @param cycle Current cycle number
/// @return Expanded event stream
[[nodiscard]] PatternEventStream
evaluate_pattern(NodeIndex pattern_root, const AstArena& arena,
                 std::uint32_t cycle = 0);

} // namespace akkado
