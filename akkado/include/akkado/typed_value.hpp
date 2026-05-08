#pragma once

#include "required_sample.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace akkado {

/// Type tag for codegen values
enum class ValueType : std::uint8_t {
    Signal,    // Audio-rate buffer (oscillator, filter output, etc.)
    Number,    // Compile-time known numeric constant (still has buffer)
    Pattern,   // Mini-notation pattern with field buffers
    Record,    // Named field collection
    Array,     // Multi-element collection (compile-time unrolled)
    String,    // Compile-time string (no runtime buffer)
    Function,  // Function reference (no runtime buffer)
    StateCell, // Handle to a CellState slot (state(init) in userspace)
    Void       // No value (statements, directives)
};

/// Channel count for signal values. When Stereo, `right_buffer` holds the
/// right channel and must equal `buffer + 1` (adjacent-buffer invariant).
enum class ChannelCount : std::uint8_t {
    Mono   = 0,
    Stereo = 1,
};

constexpr const char* channel_count_name(ChannelCount c) {
    return c == ChannelCount::Stereo ? "Stereo" : "Mono";
}

struct TypedValue;

/// Pattern payload: field buffers + state metadata
struct PatternPayload {
    /// Fixed-position field buffers indexed by the FREQ..SAMPLE_ID constants
    /// declared at the bottom of this struct. 0xFFFF = field has no buffer
    /// allocated (e.g. `voice` slot reserved but not surfaced today).
    std::array<std::uint16_t, 11> fields = {
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF
    };

    /// Per-voice frequency buffers for polyphonic patterns (chord patterns and
    /// mini-notation `[c4,e4,g4]`). voice_freqs[0] mirrors fields[FREQ] for the
    /// mono/voice-0 fast path; voice_freqs[1..N-1] hold the upper chord voices
    /// emitted by SEQPAT_STEP with the matching voice index. Empty for
    /// monophonic patterns. Consumers that handle chord polyphony natively
    /// (e.g. `soundfont`) iterate this vector to emit one instruction per voice.
    std::vector<std::uint16_t> voice_freqs;

    /// Phase 2.1 PRD §11: custom property buffers populated by SEQPAT_PROP.
    /// Keyed by source name (e.g. "cutoff"); value is the buffer index.
    /// Resolved by `pattern_field()` after the fixed-field check fails.
    std::unordered_map<std::string, std::uint16_t> custom_fields;

    /// State ID for SEQPAT instructions (links poly() to upstream pattern).
    /// poly() reads its source events through the SequenceState directly via
    /// POLY_BEGIN — there is intentionally no per-voice buffer plumbing here.
    std::uint32_t state_id = 0;

    /// Cycle length in beats
    float cycle_length = 4.0f;

    /// PRD prd-patterns-as-scalar-values §5.3.
    /// Pattern→Signal coerce inspects these flags to decide whether the
    /// pattern can be implicitly cast to its primary value buffer (Signal).
    /// Sample patterns (`s"…"`) and polyphonic chord patterns (`c"…"`,
    /// `n"[c4,e4]"`) are rejected at coerce sites with E160; users must
    /// pick a field or wrap with poly() / sampler() explicitly.
    bool is_sample_pattern = false;
    std::uint8_t max_voices = 1;

    /// Samples this Pattern needs, with bank/variant context. Populated by
    /// every Pattern producer (mini-literal, transforms, bank/variant calls)
    /// from the local SequenceCompiler's per-event mappings. Transparent
    /// across transforms: `s"foo:0".bank("X").fast(2)` ends up with
    /// `{bank:"X", name:"foo", variant:0}` regardless of wrapping order.
    /// Published to CodeGenResult::required_samples_extended via
    /// CodeGenerator::publish_sample_refs() at the producer site.
    std::vector<RequiredSample> sample_refs;

    /// Field index constants
    static constexpr std::size_t FREQ      = 0;
    static constexpr std::size_t VEL       = 1;
    static constexpr std::size_t TRIG      = 2;
    static constexpr std::size_t GATE      = 3;
    static constexpr std::size_t TYPE      = 4;
    static constexpr std::size_t NOTE      = 5;
    static constexpr std::size_t DUR       = 6;
    static constexpr std::size_t CHANCE    = 7;
    static constexpr std::size_t TIME      = 8;
    static constexpr std::size_t PHASE     = 9;
    static constexpr std::size_t SAMPLE_ID = 10;
};

/// Build a human-readable list of available pattern field names: the five
/// fixed fields plus any custom keys registered on the payload. Used for
/// E136 diagnostics.
std::string available_fields(const PatternPayload& payload);

/// Record payload: named fields mapped to TypedValues
struct RecordPayload {
    std::unordered_map<std::string, TypedValue> fields;
};

/// Array payload: ordered collection of TypedValues
struct ArrayPayload {
    std::vector<TypedValue> elements;
};

/// A typed value produced by the code generator.
/// Wraps a buffer index with type information and optional compound payloads.
struct TypedValue {
    ValueType type = ValueType::Void;
    std::uint16_t buffer = 0xFFFF;
    bool error = false;

    /// Channel count for Signal values. Defaults to Mono; set to Stereo by
    /// stereo-producing handlers (stereo(), pan(), pingpong(), ...) and by
    /// auto-lifted DSP operations whose input was Stereo.
    ChannelCount channels = ChannelCount::Mono;

    /// Right-channel buffer index when `channels == Stereo`. Required to equal
    /// `buffer + 1` (adjacent-buffer invariant enforced by BufferAllocator).
    std::uint16_t right_buffer = 0xFFFF;

    // Compound type payloads (shared_ptr for cheap copies)
    std::shared_ptr<PatternPayload> pattern;
    std::shared_ptr<RecordPayload> record;
    std::shared_ptr<ArrayPayload> array;

    // String ID (FNV-1a hash) for ValueType::String
    std::uint32_t string_id = 0;

    // State pool slot ID for ValueType::StateCell — populated by state(init),
    // consumed by get(s) / set(s, v). Same FNV-1a path-hash scheme as every
    // other stateful builtin.
    std::uint32_t cell_state_id = 0;

    /// True when this value represents a stereo signal.
    [[nodiscard]] bool is_stereo() const { return channels == ChannelCount::Stereo; }

    // --- Factory helpers ---

    static TypedValue signal(std::uint16_t buf) {
        TypedValue tv;
        tv.type = ValueType::Signal;
        tv.buffer = buf;
        return tv;
    }

    static TypedValue stereo_signal(std::uint16_t left, std::uint16_t right) {
        TypedValue tv;
        tv.type = ValueType::Signal;
        tv.buffer = left;
        tv.right_buffer = right;
        tv.channels = ChannelCount::Stereo;
        return tv;
    }

    static TypedValue number(std::uint16_t buf) {
        TypedValue tv;
        tv.type = ValueType::Number;
        tv.buffer = buf;
        return tv;
    }

    static TypedValue void_val() {
        return TypedValue{};
    }

    static TypedValue error_val() {
        TypedValue tv;
        tv.error = true;
        return tv;
    }

    static TypedValue string_val(std::uint32_t id) {
        TypedValue tv;
        tv.type = ValueType::String;
        tv.string_id = id;
        return tv;
    }

    static TypedValue function_val() {
        TypedValue tv;
        tv.type = ValueType::Function;
        return tv;
    }

    static TypedValue state_cell(std::uint32_t state_id, std::uint16_t buf) {
        TypedValue tv;
        tv.type = ValueType::StateCell;
        tv.cell_state_id = state_id;
        tv.buffer = buf;
        return tv;
    }

    static TypedValue make_pattern(std::shared_ptr<PatternPayload> p, std::uint16_t primary_buf) {
        TypedValue tv;
        tv.type = ValueType::Pattern;
        tv.buffer = primary_buf;
        tv.pattern = std::move(p);
        return tv;
    }

    static TypedValue make_record(std::unordered_map<std::string, TypedValue> fields,
                                   std::uint16_t primary_buf) {
        TypedValue tv;
        tv.type = ValueType::Record;
        tv.buffer = primary_buf;
        tv.record = std::make_shared<RecordPayload>();
        tv.record->fields = std::move(fields);
        return tv;
    }

    static TypedValue make_array(std::vector<TypedValue> elements, std::uint16_t primary_buf) {
        TypedValue tv;
        tv.type = ValueType::Array;
        tv.buffer = primary_buf;
        tv.array = std::make_shared<ArrayPayload>();
        tv.array->elements = std::move(elements);
        return tv;
    }
};

/// Human-readable name for a ValueType (for error messages)
constexpr const char* value_type_name(ValueType type) {
    switch (type) {
        case ValueType::Signal:   return "Signal";
        case ValueType::Number:   return "Number";
        case ValueType::Pattern:  return "Pattern";
        case ValueType::Record:   return "Record";
        case ValueType::Array:    return "Array";
        case ValueType::String:   return "String";
        case ValueType::Function: return "Function";
        case ValueType::StateCell:return "StateCell";
        case ValueType::Void:     return "Void";
    }
    return "Unknown";
}

/// Extract all buffer indices from a typed value (for multi-buffer compat).
/// Signal/Number → single buffer. Array → all element buffers. Pattern → freq buf only.
std::vector<std::uint16_t> buffers_of(const TypedValue& tv);

/// Resolve a pattern field by canonical name.
/// Maps aliases: "freq"/"pitch"/"f" → FREQ, "vel"/"velocity"/"v" → VEL, etc.
/// Returns a Signal TypedValue for the field buffer, or error_val() if not found.
TypedValue pattern_field(const TypedValue& tv, const std::string& name);

/// Map canonical field name to PatternPayload index.
/// Returns -1 if name is not a known pattern field.
int pattern_field_index(const std::string& name);

/// One row in the pattern-field alias table.
/// `index` is the slot in `PatternPayload::fields` (FREQ..SAMPLE_ID).
/// `names[0]` is the canonical name; subsequent entries are aliases.
struct PatternFieldAlias {
    std::size_t index;
    std::vector<const char*> names;
};

/// Public read-only access to the pattern-field alias table. Used by
/// the shape-index serializer (Phase 2 of records-system-unification PRD)
/// and by E136 diagnostic helpers.
const std::vector<PatternFieldAlias>& pattern_field_aliases();

} // namespace akkado
