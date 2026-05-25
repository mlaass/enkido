#include <akkado/ast_hash.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace akkado {
namespace {

constexpr std::uint64_t FNV_OFFSET_BASIS = 1469598103934665603ULL;
constexpr std::uint64_t FNV_PRIME        = 1099511628211ULL;

inline void mix_byte(std::uint64_t& h, std::uint8_t b) {
    h ^= static_cast<std::uint64_t>(b);
    h *= FNV_PRIME;
}

inline void mix_bytes(std::uint64_t& h, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) mix_byte(h, p[i]);
}

inline void mix_u32(std::uint64_t& h, std::uint32_t v) { mix_bytes(h, &v, sizeof(v)); }
inline void mix_u64(std::uint64_t& h, std::uint64_t v) { mix_bytes(h, &v, sizeof(v)); }
inline void mix_i32(std::uint64_t& h, std::int32_t v)  { mix_bytes(h, &v, sizeof(v)); }
inline void mix_f64(std::uint64_t& h, double v)        { mix_bytes(h, &v, sizeof(v)); }
inline void mix_f32(std::uint64_t& h, float v)         { mix_bytes(h, &v, sizeof(v)); }

inline void mix_str(std::uint64_t& h, std::string_view s) {
    mix_u64(h, s.size());
    mix_bytes(h, s.data(), s.size());
}

inline void mix_opt_str(std::uint64_t& h, const std::optional<std::string>& s) {
    mix_byte(h, s.has_value() ? 1 : 0);
    if (s) mix_str(h, *s);
}

inline void mix_destructure_fields(std::uint64_t& h,
                                   const std::vector<DestructureField>& fs) {
    mix_u64(h, fs.size());
    for (const auto& f : fs) {
        mix_str(h, f.name);
        mix_u32(h, f.default_node);
    }
}

void mix_node_data(std::uint64_t& h, const Node& n) {
    mix_byte(h, static_cast<std::uint8_t>(n.data.index()));

    std::visit([&](const auto& d) {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            // nothing
        } else if constexpr (std::is_same_v<T, Node::NumberData>) {
            mix_f64(h, d.value);
            mix_byte(h, d.is_integer ? 1 : 0);
        } else if constexpr (std::is_same_v<T, Node::BoolData>) {
            mix_byte(h, d.value ? 1 : 0);
        } else if constexpr (std::is_same_v<T, Node::StringData>) {
            mix_str(h, d.value);
        } else if constexpr (std::is_same_v<T, Node::IdentifierData>) {
            mix_str(h, d.name);
        } else if constexpr (std::is_same_v<T, Node::BinaryOpData>) {
            mix_byte(h, static_cast<std::uint8_t>(d.op));
        } else if constexpr (std::is_same_v<T, Node::ArgumentData>) {
            mix_opt_str(h, d.name);
            mix_u32(h, d.spread_source);
        } else if constexpr (std::is_same_v<T, Node::PitchData>) {
            mix_byte(h, d.midi_note);
        } else if constexpr (std::is_same_v<T, Node::ClosureParamData>) {
            mix_str(h, d.name);
            mix_byte(h, d.default_value.has_value() ? 1 : 0);
            if (d.default_value) mix_f64(h, *d.default_value);
            mix_opt_str(h, d.default_string);
            mix_byte(h, d.is_rest ? 1 : 0);
            mix_byte(h, static_cast<std::uint8_t>(d.annotated_type));
        } else if constexpr (std::is_same_v<T, Node::MiniAtomData>) {
            mix_byte(h, static_cast<std::uint8_t>(d.kind));
            mix_byte(h, d.midi_note);
            mix_bytes(h, &d.micro_offset, sizeof(d.micro_offset));
            mix_f32(h, d.velocity);
            mix_str(h, d.sample_name);
            mix_byte(h, d.sample_variant);
            mix_str(h, d.sample_bank);
            mix_str(h, d.chord_root);
            mix_str(h, d.chord_quality);
            mix_byte(h, d.chord_root_midi);
            mix_u64(h, d.chord_intervals.size());
            for (auto iv : d.chord_intervals) mix_bytes(h, &iv, sizeof(iv));
            mix_f32(h, d.curve_value);
            mix_byte(h, d.curve_smooth ? 1 : 0);
            mix_f32(h, d.scalar_value);
            mix_u64(h, d.properties.size());
            for (const auto& [k, v] : d.properties) {
                mix_str(h, k);
                mix_f32(h, v);
            }
        } else if constexpr (std::is_same_v<T, Node::MiniEuclideanData>) {
            mix_byte(h, d.hits);
            mix_byte(h, d.steps);
            mix_byte(h, d.rotation);
        } else if constexpr (std::is_same_v<T, Node::MiniModifierData>) {
            mix_byte(h, static_cast<std::uint8_t>(d.modifier_type));
            mix_f32(h, d.value);
        } else if constexpr (std::is_same_v<T, Node::MiniPolymeterData>) {
            mix_byte(h, d.step_count);
        } else if constexpr (std::is_same_v<T, Node::FunctionDefData>) {
            mix_str(h, d.name);
            mix_u64(h, d.param_count);
            mix_byte(h, d.has_rest_param ? 1 : 0);
            mix_byte(h, d.is_const ? 1 : 0);
            mix_byte(h, d.is_inline ? 1 : 0);
        } else if constexpr (std::is_same_v<T, Node::MatchArmData>) {
            mix_byte(h, d.is_wildcard ? 1 : 0);
            mix_byte(h, d.has_guard ? 1 : 0);
            mix_u32(h, d.guard_node);
            mix_byte(h, d.is_range ? 1 : 0);
            mix_f64(h, d.range_low);
            mix_f64(h, d.range_high);
            mix_byte(h, d.is_destructure ? 1 : 0);
            mix_u64(h, d.destructure_fields.size());
            for (const auto& f : d.destructure_fields) mix_str(h, f);
        } else if constexpr (std::is_same_v<T, Node::MatchExprData>) {
            mix_byte(h, d.has_scrutinee ? 1 : 0);
        } else if constexpr (std::is_same_v<T, Node::RecordFieldData>) {
            mix_str(h, d.name);
            mix_byte(h, d.is_shorthand ? 1 : 0);
        } else if constexpr (std::is_same_v<T, Node::RecordLitData>) {
            mix_u32(h, d.spread_source);
        } else if constexpr (std::is_same_v<T, Node::FieldAccessData>) {
            mix_str(h, d.field_name);
        } else if constexpr (std::is_same_v<T, Node::FieldAssignmentData>) {
            mix_str(h, d.field_name);
        } else if constexpr (std::is_same_v<T, Node::PipeBindingData>) {
            mix_str(h, d.binding_name);
            mix_u64(h, d.destructure_fields.size());
            for (const auto& f : d.destructure_fields) mix_str(h, f);
        } else if constexpr (std::is_same_v<T, Node::HoleData>) {
            mix_opt_str(h, d.field_name);
        } else if constexpr (std::is_same_v<T, Node::ImportDeclData>) {
            mix_str(h, d.path);
            mix_str(h, d.alias);
        } else if constexpr (std::is_same_v<T, Node::DirectiveData>) {
            mix_str(h, d.name);
        } else if constexpr (std::is_same_v<T, Node::DestructureAssignmentData>) {
            mix_destructure_fields(h, d.fields);
        } else if constexpr (std::is_same_v<T, Node::DestructureParamData>) {
            mix_destructure_fields(h, d.fields);
        }
    }, n.data);
}

}  // namespace

std::uint64_t arena_structural_hash(const AstArena& arena) {
    std::uint64_t h = FNV_OFFSET_BASIS;
    mix_u64(h, arena.size());
    for (std::size_t i = 0; i < arena.size(); ++i) {
        const Node& n = arena[static_cast<NodeIndex>(i)];
        mix_byte(h, static_cast<std::uint8_t>(n.type));
        mix_u32(h, n.first_child);
        mix_u32(h, n.next_sibling);
        mix_node_data(h, n);
    }
    return h;
}

}  // namespace akkado
