#include "akkado/pattern_eval.hpp"
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
    // They subdivide the cycle like a group
    std::size_t child_count = count_children(node);
    if (child_count == 0) return;

    std::size_t idx = 0;
    NodeIndex child = arena_[node].first_child;
    while (child != NULL_NODE) {
        PatternEvalContext child_ctx = ctx.subdivide(idx, child_count);
        eval_node(child, child_ctx, stream);
        child = arena_[child].next_sibling;
        idx++;
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
            event.type = PatternEventType::Sample;
            event.sample_name = atom_data.sample_name;
            event.sample_variant = atom_data.sample_variant;
            break;
        case Node::MiniAtomKind::Rest:
            event.type = PatternEventType::Rest;
            break;
    }

    stream.add(std::move(event));
}

void PatternEvaluator::eval_group(NodeIndex node, const PatternEvalContext& ctx,
                                   PatternEventStream& stream) {
    // MiniGroup subdivides its time span among children
    std::size_t child_count = count_children(node);
    if (child_count == 0) return;

    std::size_t idx = 0;
    NodeIndex child = arena_[node].first_child;
    while (child != NULL_NODE) {
        PatternEvalContext child_ctx = ctx.subdivide(idx, child_count);
        eval_node(child, child_ctx, stream);
        child = arena_[child].next_sibling;
        idx++;
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
            // Weight affects velocity
            new_ctx = ctx.with_velocity(mod_data.value);
            break;

        case Node::MiniModifierType::Repeat: {
            // Repeat the child N times
            int repeats = static_cast<int>(mod_data.value);
            float repeat_duration = ctx.duration / static_cast<float>(repeats);

            for (int i = 0; i < repeats; ++i) {
                PatternEvalContext repeat_ctx{
                    .start_time = ctx.start_time + repeat_duration * static_cast<float>(i),
                    .duration = repeat_duration,
                    .velocity = ctx.velocity,
                    .chance = ctx.chance
                };
                eval_node(child, repeat_ctx, stream);
            }
            return; // Already handled
        }

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

// Convenience function
PatternEventStream evaluate_pattern(NodeIndex pattern_root, const AstArena& arena,
                                     std::uint32_t cycle) {
    PatternEvaluator evaluator(arena);
    return evaluator.evaluate(pattern_root, cycle);
}

} // namespace akkado
