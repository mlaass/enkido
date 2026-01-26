#include "akkado/pattern_eval.hpp"
#include "akkado/chord_parser.hpp"
#include <algorithm>

namespace akkado {

// PatternEventStream implementation

void PatternEventStream::sort_by_time() {
    std::sort(events.begin(), events.end(),
              [](const PatternEvent& a, const PatternEvent& b) {
                  return a.time < b.time;
              });
}

std::vector<const PatternEvent*>
PatternEventStream::events_in_range(float start, float end) const {
    std::vector<const PatternEvent*> result;
    for (const auto& event : events) {
        if (event.time >= start && event.time < end) {
            result.push_back(&event);
        }
    }
    return result;
}

void PatternEventStream::merge(const PatternEventStream& other) {
    events.insert(events.end(), other.events.begin(), other.events.end());
}

void PatternEventStream::scale_time(float factor) {
    for (auto& event : events) {
        event.time *= factor;
        event.duration *= factor;
    }
}

void PatternEventStream::offset_time(float offset) {
    for (auto& event : events) {
        event.time += offset;
    }
}

// PatternEvaluator implementation

PatternEvaluator::PatternEvaluator(const AstArena& arena)
    : arena_(arena)
    , rng_(std::random_device{}())
{}

PatternEventStream PatternEvaluator::evaluate(NodeIndex pattern_root,
                                               std::uint32_t cycle) {
    PatternEventStream stream;
    current_cycle_ = cycle;

    if (pattern_root == NULL_NODE) {
        return stream;
    }

    PatternEvalContext ctx{
        .start_time = 0.0f,
        .duration = 1.0f,
        .velocity = 1.0f,
        .chance = 1.0f
    };

    eval_node(pattern_root, ctx, stream);
    stream.sort_by_time();

    // Calculate cycle_span from maximum event time
    float max_time = 0.0f;
    for (const auto& event : stream.events) {
        max_time = std::max(max_time, event.time + event.duration);
    }
    stream.cycle_span = std::max(1.0f, max_time);  // At least 1 cycle

    return stream;
}

void PatternEvaluator::eval_node(NodeIndex node, const PatternEvalContext& ctx,
                                  PatternEventStream& stream) {
    if (node == NULL_NODE) return;

    const Node& n = arena_[node];

    switch (n.type) {
        case NodeType::MiniPattern:
            eval_pattern(node, ctx, stream);
            break;
        case NodeType::MiniAtom:
            eval_atom(node, ctx, stream);
            break;
        case NodeType::MiniGroup:
            eval_group(node, ctx, stream);
            break;
        case NodeType::MiniSequence:
            eval_sequence(node, ctx, stream);
            break;
        case NodeType::MiniPolyrhythm:
            eval_polyrhythm(node, ctx, stream);
            break;
        case NodeType::MiniPolymeter:
            eval_polymeter(node, ctx, stream);
            break;
        case NodeType::MiniChoice:
            eval_choice(node, ctx, stream);
            break;
        case NodeType::MiniEuclidean:
            eval_euclidean(node, ctx, stream);
            break;
        case NodeType::MiniModified:
            eval_modified(node, ctx, stream);
            break;
        default:
            // Unknown node type - skip
            break;
    }
}

void PatternEvaluator::eval_pattern(NodeIndex node, const PatternEvalContext& ctx,
                                     PatternEventStream& stream) {
    // MiniPattern is the root - its children are the top-level elements
    // They subdivide the cycle based on weights and repeat counts

    // First pass: calculate total weight (accounting for repeat counts)
    float total_weight = 0.0f;
    NodeIndex child = arena_[node].first_child;
    while (child != NULL_NODE) {
        auto [weight, count] = get_child_weight_and_count(child);
        total_weight += weight * static_cast<float>(count);
        child = arena_[child].next_sibling;
    }

    if (total_weight <= 0.0f) return;

    // Second pass: evaluate with weighted time allocation
    float accumulated = 0.0f;
    child = arena_[node].first_child;
    while (child != NULL_NODE) {
        auto [weight, count] = get_child_weight_and_count(child);

        // For !n, evaluate n times with sequential time slots
        for (int i = 0; i < count; ++i) {
            PatternEvalContext child_ctx = ctx.subdivide_weighted(accumulated, weight, total_weight);
            eval_node_unwrap(child, child_ctx, stream);
            accumulated += weight;
        }
        child = arena_[child].next_sibling;
    }
}

void PatternEvaluator::eval_atom(NodeIndex node, const PatternEvalContext& ctx,
                                  PatternEventStream& stream) {
    const Node& n = arena_[node];
    const auto& atom_data = n.as_mini_atom();

    PatternEvent event;
    event.time = ctx.start_time;
    event.duration = ctx.duration;
    event.velocity = ctx.velocity;
    event.chance = ctx.chance;

    switch (atom_data.kind) {
        case Node::MiniAtomKind::Pitch:
            event.type = PatternEventType::Pitch;
            event.midi_note = atom_data.midi_note;
            break;
        case Node::MiniAtomKind::Sample:
            // In chord mode, try to parse sample name as chord symbol
            if (chord_mode_) {
                auto chord = parse_chord_symbol(atom_data.sample_name);
                if (chord.has_value()) {
                    event.type = PatternEventType::Chord;
                    event.chord_data = ChordEventData{
                        .root = chord->root,
                        .quality = chord->quality,
                        .intervals = chord->intervals,
                        .root_midi = chord->root_midi
                    };
                } else {
                    // Invalid chord symbol - treat as rest
                    event.type = PatternEventType::Rest;
                }
            } else {
                event.type = PatternEventType::Sample;
                event.sample_name = atom_data.sample_name;
                event.sample_variant = atom_data.sample_variant;
            }
            break;
        case Node::MiniAtomKind::Rest:
            event.type = PatternEventType::Rest;
            break;
    }

    stream.add(std::move(event));
}

void PatternEvaluator::eval_group(NodeIndex node, const PatternEvalContext& ctx,
                                   PatternEventStream& stream) {
    // MiniGroup subdivides its time span based on weights and repeat counts

    // First pass: calculate total weight (accounting for repeat counts)
    float total_weight = 0.0f;
    NodeIndex child = arena_[node].first_child;
    while (child != NULL_NODE) {
        auto [weight, count] = get_child_weight_and_count(child);
        total_weight += weight * static_cast<float>(count);
        child = arena_[child].next_sibling;
    }

    if (total_weight <= 0.0f) return;

    // Second pass: evaluate with weighted time allocation
    float accumulated = 0.0f;
    child = arena_[node].first_child;
    while (child != NULL_NODE) {
        auto [weight, count] = get_child_weight_and_count(child);

        // For !n, evaluate n times with sequential time slots
        for (int i = 0; i < count; ++i) {
            PatternEvalContext child_ctx = ctx.subdivide_weighted(accumulated, weight, total_weight);
            eval_node_unwrap(child, child_ctx, stream);
            accumulated += weight;
        }
        child = arena_[child].next_sibling;
    }
}

void PatternEvaluator::eval_sequence(NodeIndex node, const PatternEvalContext& ctx,
                                      PatternEventStream& stream) {
    // MiniSequence plays one child per cycle, rotating
    std::size_t child_count = count_children(node);
    if (child_count == 0) return;

    std::size_t selected_idx = current_cycle_ % child_count;
    NodeIndex selected = get_child(node, selected_idx);

    if (selected != NULL_NODE) {
        eval_node(selected, ctx, stream);
    }
}

void PatternEvaluator::eval_polyrhythm(NodeIndex node, const PatternEvalContext& ctx,
                                        PatternEventStream& stream) {
    // MiniPolyrhythm plays all children simultaneously
    NodeIndex child = arena_[node].first_child;
    while (child != NULL_NODE) {
        eval_node(child, ctx.inherit(), stream);
        child = arena_[child].next_sibling;
    }
}

void PatternEvaluator::eval_polymeter(NodeIndex node, const PatternEvalContext& ctx,
                                       PatternEventStream& stream) {
    // MiniPolymeter divides parent duration into N steps
    // Unlike subdivision ([a b c]) which fits children into the parent duration,
    // polymeter plays each child at a step position, cycling through children
    // if step_count > child_count
    const Node& n = arena_[node];
    const auto& poly_data = n.as_mini_polymeter();

    std::size_t child_count = count_children(node);
    if (child_count == 0) return;

    // Determine number of steps:
    // - If %n specified, use n steps
    // - Otherwise, use child count (each child = 1 step)
    std::size_t steps = poly_data.step_count > 0
        ? static_cast<std::size_t>(poly_data.step_count)
        : child_count;

    // Each step gets an equal division of the parent duration
    float step_duration = ctx.duration / static_cast<float>(steps);

    for (std::size_t i = 0; i < steps; ++i) {
        PatternEvalContext step_ctx{
            .start_time = ctx.start_time + step_duration * static_cast<float>(i),
            .duration = step_duration,
            .velocity = ctx.velocity,
            .chance = ctx.chance
        };

        // Wrap around if more steps than children (for %n syntax)
        NodeIndex child = get_child(node, i % child_count);
        if (child != NULL_NODE) {
            eval_node(child, step_ctx, stream);
        }
    }
}

void PatternEvaluator::eval_choice(NodeIndex node, const PatternEvalContext& ctx,
                                    PatternEventStream& stream) {
    // MiniChoice randomly selects one child
    std::size_t child_count = count_children(node);
    if (child_count == 0) return;

    std::uniform_int_distribution<std::size_t> dist(0, child_count - 1);
    std::size_t selected_idx = dist(rng_);
    NodeIndex selected = get_child(node, selected_idx);

    if (selected != NULL_NODE) {
        eval_node(selected, ctx, stream);
    }
}

void PatternEvaluator::eval_euclidean(NodeIndex node, const PatternEvalContext& ctx,
                                       PatternEventStream& stream) {
    const Node& n = arena_[node];
    const auto& euclid_data = n.as_mini_euclidean();

    // Get the atom child
    NodeIndex atom = n.first_child;
    if (atom == NULL_NODE) return;

    // Generate euclidean pattern
    std::vector<bool> pattern = generate_euclidean(
        euclid_data.hits, euclid_data.steps, euclid_data.rotation);

    // Create events for each hit
    float step_duration = ctx.duration / static_cast<float>(euclid_data.steps);

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i]) {
            PatternEvalContext step_ctx{
                .start_time = ctx.start_time + step_duration * static_cast<float>(i),
                .duration = step_duration,
                .velocity = ctx.velocity,
                .chance = ctx.chance
            };
            eval_node(atom, step_ctx, stream);
        }
    }
}

void PatternEvaluator::eval_modified(NodeIndex node, const PatternEvalContext& ctx,
                                      PatternEventStream& stream) {
    const Node& n = arena_[node];
    const auto& mod_data = n.as_mini_modifier();

    // Get the child being modified
    NodeIndex child = n.first_child;
    if (child == NULL_NODE) return;

    PatternEvalContext new_ctx = ctx;

    switch (mod_data.modifier_type) {
        case Node::MiniModifierType::Speed:
            // Speed up: reduce duration, potentially repeat
            new_ctx = ctx.with_speed(mod_data.value);
            break;

        case Node::MiniModifierType::Slow:
            // Slow down: increase duration
            new_ctx.duration = ctx.duration * mod_data.value;
            break;

        case Node::MiniModifierType::Duration:
            // Set explicit duration
            new_ctx.duration = ctx.duration * mod_data.value;
            break;

        case Node::MiniModifierType::Weight:
            // Weight affects temporal allocation in parent group/pattern
            // Just pass through to child - parent handles the time allocation
            break;

        case Node::MiniModifierType::Repeat:
            // Repeat extends sequence at parent level
            // Just pass through to child - parent handles the replication
            break;

        case Node::MiniModifierType::Chance:
            // Set probability
            new_ctx = ctx.with_chance(mod_data.value);
            break;
    }

    eval_node(child, new_ctx, stream);
}

std::vector<bool> PatternEvaluator::generate_euclidean(std::uint8_t hits,
                                                        std::uint8_t steps,
                                                        std::uint8_t rotation) {
    // Bjorklund algorithm for euclidean rhythm generation
    if (steps == 0) return {};
    if (hits >= steps) {
        return std::vector<bool>(steps, true);
    }
    if (hits == 0) {
        return std::vector<bool>(steps, false);
    }

    // Initialize pattern groups
    std::vector<std::vector<bool>> groups;
    for (std::uint8_t i = 0; i < steps; ++i) {
        groups.push_back({i < hits});
    }

    // Bjorklund iteration
    std::size_t group1_end = hits;
    std::size_t group2_start = hits;

    while (group2_start < groups.size() && groups.size() - group2_start > 1) {
        std::size_t num_to_distribute = std::min(group1_end, groups.size() - group2_start);

        for (std::size_t i = 0; i < num_to_distribute; ++i) {
            groups[i].insert(groups[i].end(),
                            groups[group2_start + i].begin(),
                            groups[group2_start + i].end());
        }

        // Remove distributed groups
        groups.erase(groups.begin() + static_cast<long>(group2_start),
                    groups.begin() + static_cast<long>(group2_start + num_to_distribute));

        group1_end = num_to_distribute;
        group2_start = num_to_distribute;
    }

    // Flatten groups into pattern
    std::vector<bool> pattern;
    for (const auto& group : groups) {
        pattern.insert(pattern.end(), group.begin(), group.end());
    }

    // Apply rotation
    if (rotation > 0 && rotation < pattern.size()) {
        std::rotate(pattern.begin(),
                   pattern.begin() + rotation,
                   pattern.end());
    }

    return pattern;
}

std::size_t PatternEvaluator::count_children(NodeIndex node) const {
    return arena_.child_count(node);
}

NodeIndex PatternEvaluator::get_child(NodeIndex node, std::size_t index) const {
    NodeIndex child = arena_[node].first_child;
    std::size_t i = 0;
    while (child != NULL_NODE && i < index) {
        child = arena_[child].next_sibling;
        i++;
    }
    return child;
}

std::pair<float, int> PatternEvaluator::get_child_weight_and_count(NodeIndex child) const {
    if (child == NULL_NODE) return {1.0f, 1};

    float weight = 1.0f;
    int count = 1;

    // Walk through modifier chain to find Weight and Repeat
    NodeIndex current = child;
    while (arena_[current].type == NodeType::MiniModified) {
        const auto& mod = arena_[current].as_mini_modifier();
        if (mod.modifier_type == Node::MiniModifierType::Repeat) {
            count = static_cast<int>(mod.value);
        } else if (mod.modifier_type == Node::MiniModifierType::Weight) {
            weight = mod.value;
        }
        current = arena_[current].first_child;
        if (current == NULL_NODE) break;
    }

    return {weight, count};
}

void PatternEvaluator::eval_node_unwrap(NodeIndex node, const PatternEvalContext& ctx,
                                         PatternEventStream& stream) {
    if (node == NULL_NODE) return;

    // If this is a Weight or Repeat modifier, skip it and eval inner content
    // (the parent already handled the weight/repeat semantics)
    NodeIndex current = node;
    PatternEvalContext current_ctx = ctx;

    while (arena_[current].type == NodeType::MiniModified) {
        const auto& mod = arena_[current].as_mini_modifier();
        if (mod.modifier_type == Node::MiniModifierType::Weight ||
            mod.modifier_type == Node::MiniModifierType::Repeat) {
            // Skip this modifier - parent handled it
            current = arena_[current].first_child;
            if (current == NULL_NODE) return;
        } else {
            // Other modifier - let eval_node handle it
            break;
        }
    }

    eval_node(current, current_ctx, stream);
}

std::uint32_t PatternEvaluator::count_cycles(NodeIndex node) const {
    if (node == NULL_NODE) return 1;

    const Node& n = arena_[node];

    switch (n.type) {
        case NodeType::MiniPattern: {
            // Root pattern: max of all children's cycle counts
            std::uint32_t max_cycles = 1;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                max_cycles = std::max(max_cycles, count_cycles(child));
                child = arena_[child].next_sibling;
            }
            return max_cycles;
        }

        case NodeType::MiniSequence: {
            // Alternating sequence <a b c> needs N cycles where N = child count
            // Each element plays once per N cycles
            std::size_t child_count = count_children(node);
            if (child_count == 0) return 1;

            // Also check if any child needs multiple cycles
            std::uint32_t child_max = 1;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                child_max = std::max(child_max, count_cycles(child));
                child = arena_[child].next_sibling;
            }

            // Total cycles = number of elements * max child cycles
            return static_cast<std::uint32_t>(child_count) * child_max;
        }

        case NodeType::MiniGroup:
        case NodeType::MiniPolyrhythm:
        case NodeType::MiniPolymeter: {
            // Group/polyrhythm: max of children's cycle counts
            std::uint32_t max_cycles = 1;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                max_cycles = std::max(max_cycles, count_cycles(child));
                child = arena_[child].next_sibling;
            }
            return max_cycles;
        }

        case NodeType::MiniModified: {
            // Slow modifier /n stretches TIME within a single evaluation,
            // it doesn't require additional cycle evaluations.
            // The time stretching is handled by eval_modified which increases
            // the duration, resulting in events with times > 1.0.
            // cycle_span is then calculated from max event times.
            //
            // Speed, repeat, etc. also don't increase cycle count.
            return count_cycles(n.first_child);
        }

        case NodeType::MiniChoice: {
            // Random choice: max of children (all options could potentially play)
            std::uint32_t max_cycles = 1;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                max_cycles = std::max(max_cycles, count_cycles(child));
                child = arena_[child].next_sibling;
            }
            return max_cycles;
        }

        case NodeType::MiniEuclidean:
            // Euclidean rhythm: check its atom child
            return count_cycles(n.first_child);

        case NodeType::MiniAtom:
        default:
            // Atoms and other types need 1 cycle
            return 1;
    }
}

// Convenience function
PatternEventStream evaluate_pattern(NodeIndex pattern_root, const AstArena& arena,
                                     std::uint32_t cycle) {
    PatternEvaluator evaluator(arena);
    return evaluator.evaluate(pattern_root, cycle);
}

std::uint32_t count_pattern_cycles(NodeIndex pattern_root, const AstArena& arena) {
    PatternEvaluator evaluator(arena);
    return evaluator.count_cycles(pattern_root);
}

PatternEventStream evaluate_pattern_multi_cycle(NodeIndex pattern_root,
                                                 const AstArena& arena) {
    PatternEvaluator evaluator(arena);

    // Determine how many cycles this pattern spans
    std::uint32_t num_cycles = evaluator.count_cycles(pattern_root);

    if (num_cycles <= 1) {
        // Single cycle - use standard evaluation
        return evaluator.evaluate(pattern_root, 0);
    }

    // Multi-cycle evaluation
    PatternEventStream combined;

    for (std::uint32_t cycle = 0; cycle < num_cycles; cycle++) {
        PatternEventStream cycle_events = evaluator.evaluate(pattern_root, cycle);

        // Offset times by cycle number
        for (auto& event : cycle_events.events) {
            event.time += static_cast<float>(cycle);
        }

        // Append to combined stream
        combined.events.insert(combined.events.end(),
                               cycle_events.events.begin(),
                               cycle_events.events.end());
    }

    combined.cycle_span = static_cast<float>(num_cycles);
    combined.sort_by_time();

    return combined;
}

} // namespace akkado
