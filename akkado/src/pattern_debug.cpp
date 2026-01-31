#include <akkado/pattern_debug.hpp>
#include <sstream>
#include <iomanip>

namespace akkado {

namespace {

/// Escape a string for JSON
std::string escape_json(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (char c : sv) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

/// Get modifier type as string
const char* modifier_type_str(Node::MiniModifierType type) {
    switch (type) {
        case Node::MiniModifierType::Speed: return "Speed";
        case Node::MiniModifierType::Slow: return "Slow";
        case Node::MiniModifierType::Duration: return "Duration";
        case Node::MiniModifierType::Weight: return "Weight";
        case Node::MiniModifierType::Repeat: return "Repeat";
        case Node::MiniModifierType::Chance: return "Chance";
    }
    return "Unknown";
}

/// Get atom kind as string
const char* atom_kind_str(Node::MiniAtomKind kind) {
    switch (kind) {
        case Node::MiniAtomKind::Pitch: return "Pitch";
        case Node::MiniAtomKind::Sample: return "Sample";
        case Node::MiniAtomKind::Rest: return "Rest";
        case Node::MiniAtomKind::Chord: return "Chord";
    }
    return "Unknown";
}

/// Get sequence mode as string
const char* sequence_mode_str(cedar::SequenceMode mode) {
    switch (mode) {
        case cedar::SequenceMode::NORMAL: return "NORMAL";
        case cedar::SequenceMode::ALTERNATE: return "ALTERNATE";
        case cedar::SequenceMode::RANDOM: return "RANDOM";
    }
    return "UNKNOWN";
}

/// Serialize a single AST node to JSON (recursive)
void serialize_node(std::ostringstream& json, NodeIndex idx, const AstArena& arena) {
    if (idx == NULL_NODE || !arena.valid(idx)) {
        json << "null";
        return;
    }

    const Node& node = arena[idx];
    json << "{";
    json << "\"type\":\"" << node_type_name(node.type) << "\"";

    // Add source location
    json << ",\"location\":{\"offset\":" << node.location.offset
         << ",\"length\":" << node.location.length << "}";

    // Add node-specific data
    switch (node.type) {
        case NodeType::MiniAtom: {
            const auto& data = node.as_mini_atom();
            json << ",\"kind\":\"" << atom_kind_str(data.kind) << "\"";
            if (data.kind == Node::MiniAtomKind::Pitch) {
                json << ",\"midi\":" << static_cast<int>(data.midi_note);
            } else if (data.kind == Node::MiniAtomKind::Sample) {
                json << ",\"sampleName\":\"" << escape_json(data.sample_name) << "\"";
                if (data.sample_variant > 0) {
                    json << ",\"variant\":" << static_cast<int>(data.sample_variant);
                }
            }
            break;
        }

        case NodeType::MiniEuclidean: {
            const auto& data = node.as_mini_euclidean();
            json << ",\"hits\":" << static_cast<int>(data.hits);
            json << ",\"steps\":" << static_cast<int>(data.steps);
            json << ",\"rotation\":" << static_cast<int>(data.rotation);
            break;
        }

        case NodeType::MiniModified: {
            const auto& data = node.as_mini_modifier();
            json << ",\"modifier\":\"" << modifier_type_str(data.modifier_type) << "\"";
            json << ",\"value\":" << data.value;
            break;
        }

        case NodeType::MiniPolymeter: {
            const auto& data = node.as_mini_polymeter();
            json << ",\"stepCount\":" << static_cast<int>(data.step_count);
            break;
        }

        default:
            // Other node types don't have extra data in mini-notation context
            break;
    }

    // Add children
    if (node.first_child != NULL_NODE) {
        json << ",\"children\":[";
        bool first = true;
        NodeIndex child = node.first_child;
        while (child != NULL_NODE && arena.valid(child)) {
            if (!first) json << ",";
            first = false;
            serialize_node(json, child, arena);
            child = arena[child].next_sibling;
        }
        json << "]";
    }

    json << "}";
}

} // anonymous namespace

std::string serialize_mini_ast_json(NodeIndex root, const AstArena& arena) {
    std::ostringstream json;
    serialize_node(json, root, arena);
    return json.str();
}

std::string serialize_sequences_json(
    const std::vector<cedar::Sequence>& sequences,
    const std::vector<std::vector<cedar::Event>>& sequence_events) {

    std::ostringstream json;
    json << "{\"sequences\":[";

    for (std::size_t i = 0; i < sequences.size(); ++i) {
        if (i > 0) json << ",";

        const auto& seq = sequences[i];
        json << "{";
        json << "\"id\":" << i;
        json << ",\"mode\":\"" << sequence_mode_str(seq.mode) << "\"";
        json << ",\"duration\":" << seq.duration;
        json << ",\"step\":" << seq.step;
        json << ",\"events\":[";

        // Use sequence_events if available, otherwise fall back to seq.events pointer
        const cedar::Event* events = nullptr;
        std::uint32_t num_events = 0;

        if (i < sequence_events.size() && !sequence_events[i].empty()) {
            events = sequence_events[i].data();
            num_events = static_cast<std::uint32_t>(sequence_events[i].size());
        } else if (seq.events != nullptr) {
            events = seq.events;
            num_events = seq.num_events;
        }

        for (std::uint32_t j = 0; j < num_events; ++j) {
            if (j > 0) json << ",";
            const auto& e = events[j];
            json << "{";
            json << "\"type\":\"" << (e.type == cedar::EventType::DATA ? "DATA" : "SUB_SEQ") << "\"";
            json << ",\"time\":" << e.time;
            json << ",\"duration\":" << e.duration;
            json << ",\"chance\":" << e.chance;
            json << ",\"sourceOffset\":" << e.source_offset;
            json << ",\"sourceLength\":" << e.source_length;

            if (e.type == cedar::EventType::DATA) {
                json << ",\"numValues\":" << static_cast<int>(e.num_values);
                json << ",\"values\":[";
                for (std::uint8_t k = 0; k < e.num_values; ++k) {
                    if (k > 0) json << ",";
                    json << e.values[k];
                }
                json << "]";
            } else {
                json << ",\"seqId\":" << e.seq_id;
            }
            json << "}";
        }

        json << "]}";
    }

    json << "]}";
    return json.str();
}

} // namespace akkado
