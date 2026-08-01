#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include "diagnostics.hpp"
#include "string_interner.hpp"  // for SymbolId (used in IdentifierData)
#include "typed_value.hpp"  // for ParamValueType (used in ClosureParamData)

namespace akkado {

class AstArena;  // forward declaration for MiniLiteralData

/// Index into the AST arena (0xFFFFFFFF = null/invalid)
using NodeIndex = std::uint32_t;
constexpr NodeIndex NULL_NODE = 0xFFFFFFFF;

/// One field in a destructure pattern (`{x}`, `{x = 1}`, etc.), as stored on
/// DestructureAssignment / DestructureParam node data. Used by statement-level
/// (`{x, y} = r`) and fn-param (`fn f({x, y})`) destructure forms. The field's
/// default expression (if any) lives in the owning Node's `extra_children[i]`
/// (same index as the field) — see `destructure_bindings()` in
/// destructure_field.hpp for the zipped (name, default) view.
struct DestructureField {
    std::string name;
};

/// AST node types
enum class NodeType : std::uint8_t {
    // Literals
    NumberLit,      // Numeric literal
    BoolLit,        // true/false
    StringLit,      // "..." or '...' or `...`
    PitchLit,       // 'c4', 'f#3', 'Bb5' (MIDI note)
    ArrayLit,       // [a, b, c] - array literal

    // Identifiers
    Identifier,     // Variable or function name
    Hole,           // % (pipe input reference)

    // Expressions
    Call,           // Function call: f(a, b, c)
    MethodCall,     // Method call: x.f(a, b)
    Index,          // Array indexing: arr[i]
    Pipe,           // a |> b (let-binding rewrite)
    Closure,        // (params) -> body

    // Arguments
    Argument,       // Named or positional argument

    // Patterns (top-level pattern constructs)
    MiniLiteral,    // pat("..."), seq("...", closure), etc.

    // Mini-notation AST (parsed pattern content)
    MiniPattern,    // Root of parsed mini-notation pattern
    MiniAtom,       // Single pitch, sample, or rest
    MiniGroup,      // [a b c] - subdivision (elements share parent time span)
    MiniSequence,   // <a b c> - alternating sequence (one per cycle, rotating)
    MiniPolyrhythm, // [a, b, c] - polyrhythm (all elements simultaneously)
    MiniPolymeter,  // {a b c} or {a b}%n - polymeter (LCM alignment)
    MiniChoice,     // a | b | c - random choice each cycle
    MiniEuclidean,  // x(k,n,r) - euclidean rhythm
    MiniModified,   // Atom with modifier (speed, weight, etc.)

    // Statements
    Assignment,     // x = expr
    ConstDecl,      // const x = expr
    Block,          // { statements... expr }
    FunctionDef,    // fn name(params) -> body
    DestructureAssignment,  // {x, y} = expr - statement-level record destructure
    DestructureParam,       // {x, y [= default]} as a function parameter slot

    // Expressions (advanced)
    MatchExpr,      // match(expr) { arm, arm, ... }
    MatchArm,       // pattern: body
    LoopExpr,       // loop(count) { body } - bounded static iteration.
                    // Children: [count, body, seed?]. seed is appended by the
                    // analyzer when the loop is a pipe RHS. Body keeps its `@`
                    // holes (the running accumulator), resolved at codegen.

    // Records
    RecordLit,      // {field: value, ...} - record literal
    FieldAccess,    // expr.field - field access on record
    FieldAssignment,// receiver.field = expr - record-state-cell field write (Phase 4b)
    PipeBinding,    // expr as name - named binding in pipe chain

    // Imports
    ImportDecl,     // import "path" [as alias]

    // Directives
    Directive,      // $name(args) - compiler directive

    // Program
    Program,        // Root node containing statements
};

/// Convert node type to string for debugging
constexpr const char* node_type_name(NodeType type) {
    switch (type) {
        case NodeType::NumberLit:   return "NumberLit";
        case NodeType::BoolLit:     return "BoolLit";
        case NodeType::StringLit:   return "StringLit";
        case NodeType::PitchLit:    return "PitchLit";
        case NodeType::ArrayLit:    return "ArrayLit";
        case NodeType::Identifier:  return "Identifier";
        case NodeType::Hole:        return "Hole";
        case NodeType::Call:        return "Call";
        case NodeType::MethodCall:  return "MethodCall";
        case NodeType::Index:       return "Index";
        case NodeType::Pipe:        return "Pipe";
        case NodeType::Closure:     return "Closure";
        case NodeType::Argument:    return "Argument";
        case NodeType::MiniLiteral:    return "MiniLiteral";
        case NodeType::MiniPattern:    return "MiniPattern";
        case NodeType::MiniAtom:       return "MiniAtom";
        case NodeType::MiniGroup:      return "MiniGroup";
        case NodeType::MiniSequence:   return "MiniSequence";
        case NodeType::MiniPolyrhythm: return "MiniPolyrhythm";
        case NodeType::MiniPolymeter:  return "MiniPolymeter";
        case NodeType::MiniChoice:     return "MiniChoice";
        case NodeType::MiniEuclidean:  return "MiniEuclidean";
        case NodeType::MiniModified:   return "MiniModified";
        case NodeType::Assignment:     return "Assignment";
        case NodeType::ConstDecl:      return "ConstDecl";
        case NodeType::Block:       return "Block";
        case NodeType::FunctionDef: return "FunctionDef";
        case NodeType::DestructureAssignment: return "DestructureAssignment";
        case NodeType::DestructureParam: return "DestructureParam";
        case NodeType::MatchExpr:   return "MatchExpr";
        case NodeType::MatchArm:    return "MatchArm";
        case NodeType::LoopExpr:    return "LoopExpr";
        case NodeType::RecordLit:   return "RecordLit";
        case NodeType::FieldAccess: return "FieldAccess";
        case NodeType::FieldAssignment: return "FieldAssignment";
        case NodeType::PipeBinding: return "PipeBinding";
        case NodeType::ImportDecl:  return "ImportDecl";
        case NodeType::Directive:   return "Directive";
        case NodeType::Program:     return "Program";
    }
    return "Unknown";
}

/// AST Node - stored in contiguous arena
/// Uses indices instead of pointers for cache efficiency
struct Node {
    NodeType type;
    SourceLocation location;

    // Child links (indices into arena)
    NodeIndex first_child = NULL_NODE;
    NodeIndex next_sibling = NULL_NODE;

    /// Auxiliary AST-node children that live outside the first_child /
    /// next_sibling list (match-arm guards, spread sources, destructure
    /// defaults). Slot meaning is fixed per NodeType — see
    /// extra_child_kinds(). Generic traversals (clone, substitute, hash)
    /// visit these AFTER the linked-list children. Entries may be
    /// NULL_NODE (e.g. a destructure field without a default).
    std::vector<NodeIndex> extra_children;

    /// extra_children[i], or NULL_NODE when the slot is absent.
    [[nodiscard]] NodeIndex extra_child(std::size_t i) const {
        return i < extra_children.size() ? extra_children[i] : NULL_NODE;
    }

    // Node-specific data (union-like via variant)
    struct NumberData { double value; bool is_integer; };
    struct BoolData { bool value; };
    struct StringData { std::string value; };
    // PRD prd-parser-codegen-correctness.md Phase 5 (F12): identifier
    // names are per-compile interned SymbolIds. Resolve to a string
    // view via the compile's `StringInterner::view(id)` when text is
    // needed (diagnostics, JSON serialization). Equality compare via
    // `id == id` — no string compare, no rehash.
    struct IdentifierData { SymbolId name; };
    struct ArgumentData {
        std::optional<std::string> name;  // Named arg
        // Spread (`..expr`): the spread expression lives in the node's
        // extra_children[0], not as a linked-list child.
    };
    struct PitchData { std::uint8_t midi_note; };
    struct ClosureParamData {
        std::string name;
        std::optional<double> default_value;
        std::optional<std::string> default_string;  // String default for match dispatch
        bool is_rest = false;  // true for ...param (variadic rest parameter)
        // PRD prd-parameter-type-annotations §4.4: parameter type annotation
        // (`name: stream` / `name: signal`). Defaults to Any (un-annotated).
        // Carried on closure params so it survives the parser → analyzer →
        // codegen pipeline; closures themselves don't enforce annotations
        // (they inline), but fn-defs share the same AST node shape.
        ParamValueType annotated_type = ParamValueType::Any;
    };  // Closure param with optional default

    // Mini-notation atom types
    enum class MiniAtomKind : std::uint8_t {
        Pitch,      // Note pitch (MIDI note number)
        Sample,     // Sample name with optional variant
        Rest,       // Rest/silence (~)
        Elongate,   // _ - extend previous note's duration (Tidal-compatible)
        Chord,      // Chord symbol (Am, C7, Fmaj7, etc.)
        CurveLevel, // Curve value level (_, ., -, ^, ')
        CurveRamp,  // Curve ramp (/, \)
        Value,      // Raw numeric scalar (for v"…" patterns)
    };

    // Mini-notation modifier types
    enum class MiniModifierType : std::uint8_t {
        Speed,      // *n - speed up by factor n
        Slow,       // /n - slow down by factor n
        Weight,     // @n - probability weight
        Repeat,     // !n - repeat n times
        Chance,     // ?n - probability of playing (0-1)
    };

    // Data for mini-notation atoms
    struct MiniAtomData {
        MiniAtomKind kind;
        std::uint8_t midi_note;     // For Pitch kind
        std::int8_t micro_offset = 0;  // Microtonal step offset
        float velocity = 1.0f;      // 0.0-1.0, from :vel suffix
        std::string sample_name;    // For Sample kind
        std::uint8_t sample_variant; // For Sample kind (e.g., bd:2)
        std::string sample_bank;    // For Sample kind - bank name (empty = default)
        // Chord data (for Chord kind)
        std::string chord_root;             // Root note: "A", "C#", "Bb"
        std::string chord_quality;          // Quality: "", "m", "7", "maj7", etc.
        std::uint8_t chord_root_midi;       // MIDI of root (octave 4)
        std::vector<std::int8_t> chord_intervals;  // Semitone intervals
        float curve_value = 0.0f;    // For CurveLevel: 0.0, 0.25, 0.5, 0.75, 1.0
        bool curve_smooth = false;   // For CurveLevel: true if preceded by ~ modifier
        float scalar_value = 0.0f;   // For Value (v"…"): raw numeric, no mtof
        // Phase 2 PRD: record-suffix properties from `c4{vel:0.8, bend:0.2}`.
        // Recognized short-form keys (vel/bend/aftertouch/dur) populate fixed
        // PatternEvent fields; unrecognized keys are kept here for the §5.5a
        // pipe-binding pattern-property accessor.
        std::vector<std::pair<std::string, float>> properties;
    };

    // Data for mini-notation euclidean patterns
    struct MiniEuclideanData {
        std::uint8_t hits;      // Number of hits
        std::uint8_t steps;     // Number of steps
        std::uint8_t rotation;  // Rotation offset
    };

    // Data for mini-notation modifiers
    struct MiniModifierData {
        MiniModifierType modifier_type;
        float value;
    };

    // Data for mini-notation polymeter
    struct MiniPolymeterData {
        std::uint8_t step_count;  // 0 means use child count
    };

    // Data attached to a top-level MiniLiteral node (pat/seq/note/sample/chord
    // /timeline). The parsed mini-notation AST lives in a sub-arena referenced
    // by this node (Phase 1b: parse-at-parse-time). Diagnostics buffered here
    // are merged into the main parser's diagnostic list when the surrounding
    // Parser::parse() returns. Shared-ownership pointer so analyzer node
    // clones (analyzer.cpp clone_node copies the data variant) share the
    // sub-arena without a deep-copy path; the sub-arena is read-only after
    // parsing.
    struct MiniLiteralData {
        std::string mode_marker;                    // "pat"/"seq"/"note"/"sample"/"chord"/"timeline"/"value"
        std::shared_ptr<AstArena> mini_arena;       // shared sub-AST owner (nullptr if parse failed)
        NodeIndex mini_root = NULL_NODE;            // root index inside *mini_arena
        std::vector<Diagnostic> mini_diagnostics;   // pre-collected by parser
    };

    // Data for function definitions (fn name(params) -> body)
    struct FunctionDefData {
        std::string name;
        std::size_t param_count;  // Number of Identifier children before body
        bool has_rest_param = false;  // true if last param is ...rest
        bool is_const = false;  // true for const fn
        bool is_inline = false;  // true for #inline fn (per-site inlining)
    };

    // Data for match arms (pattern: body, or pattern && guard: body).
    // The guard expression (when has_guard) lives in the node's
    // extra_children[0].
    struct MatchArmData {
        bool is_wildcard;      // true for `_` pattern
        bool has_guard;        // true if `&&` guard follows pattern
        bool is_range = false;       // true for range pattern (low..high)
        double range_low = 0.0;      // Lower bound (inclusive)
        double range_high = 0.0;     // Upper bound (exclusive)
        bool is_destructure = false;                    // true for {field, ...} pattern
        std::vector<std::string> destructure_fields;    // field names to bind
    };

    // Data for match expressions (track scrutinee vs guard-only form)
    struct MatchExprData {
        bool has_scrutinee;  // false for guard-only `match { ... }`
    };

    // Data for record field (used in RecordLit children)
    struct RecordFieldData {
        std::string name;        // Field name
        bool is_shorthand;       // true for {x} shorthand (name taken from identifier)
    };

    // Data for field access
    struct FieldAccessData {
        std::string field_name;  // The field being accessed
    };

    // Data for field assignment (Phase 4b: bidirectional sugar over record-
    // valued state cells). `receiver.field = value` lowers to STATE_OP rate=2
    // on the per-field sub-cell. Children: [receiver, value]. The receiver
    // must analyze to a StateCell holding a Record; value records and
    // FieldAccess receivers (nested writes) are rejected at codegen time.
    struct FieldAssignmentData {
        std::string field_name;
    };

    // RecordLit nodes carry no data variant arm; a `{..expr, ...}` spread
    // source lives in the node's extra_children[0].

    // Data for pipe binding (expr as name, or expr as {field1, field2})
    struct PipeBindingData {
        std::string binding_name;  // The name bound by 'as'
        std::vector<std::string> destructure_fields;  // empty for normal binding
    };

    // Data for statement-level destructure assignment ({x, y} = expr).
    // RHS expression is the node's first_child. Per-field default-expression
    // nodes live in extra_children (index-aligned with fields).
    struct DestructureAssignmentData {
        std::vector<DestructureField> fields;  // Field names
    };

    // Data for a destructuring function parameter (fn f({x, y [= default]})).
    // Lives as a child of FunctionDef in place of the usual Identifier child.
    // Per-field default-expression nodes live in extra_children.
    struct DestructureParamData {
        std::vector<DestructureField> fields;
    };

    // Data for hole with optional field access (%.field)
    struct HoleData {
        std::optional<std::string> field_name;  // Field name if %.field, nullopt for bare %
    };

    // Data for import declarations (import "path" [as alias])
    struct ImportDeclData {
        std::string path;    // Import path string
        std::string alias;   // Empty for direct injection, non-empty for "as X"
    };

    // Data for directives ($name(args))
    struct DirectiveData {
        std::string name;  // Directive name (e.g., "polyphony")
    };

    std::variant<
        std::monostate,
        NumberData,
        BoolData,
        StringData,
        IdentifierData,
        ArgumentData,
        PitchData,
        ClosureParamData,
        MiniAtomData,
        MiniEuclideanData,
        MiniModifierData,
        MiniPolymeterData,
        MiniLiteralData,
        FunctionDefData,
        MatchArmData,
        MatchExprData,
        RecordFieldData,
        FieldAccessData,
        FieldAssignmentData,
        PipeBindingData,
        HoleData,
        ImportDeclData,
        DirectiveData,
        DestructureAssignmentData,
        DestructureParamData
    > data;

    // Type-safe accessors
    [[nodiscard]] double as_number() const {
        return std::get<NumberData>(data).value;
    }

    [[nodiscard]] bool as_bool() const {
        return std::get<BoolData>(data).value;
    }

    [[nodiscard]] const std::string& as_string() const {
        return std::get<StringData>(data).value;
    }

    /// Phase 5 (F12): returns the interned identifier's SymbolId.
    /// Resolve to a string_view via `StringInterner::view(id)` when
    /// you need the text (diagnostics, error messages, serialization).
    [[nodiscard]] SymbolId as_identifier() const {
        return std::get<IdentifierData>(data).name;
    }


    [[nodiscard]] const std::optional<std::string>& as_arg_name() const {
        return std::get<ArgumentData>(data).name;
    }

    [[nodiscard]] const ArgumentData& as_argument() const {
        return std::get<ArgumentData>(data);
    }

    [[nodiscard]] std::uint8_t as_pitch() const {
        return std::get<PitchData>(data).midi_note;
    }

    [[nodiscard]] const ClosureParamData& as_closure_param() const {
        return std::get<ClosureParamData>(data);
    }

    [[nodiscard]] const MiniAtomData& as_mini_atom() const {
        return std::get<MiniAtomData>(data);
    }

    [[nodiscard]] const MiniEuclideanData& as_mini_euclidean() const {
        return std::get<MiniEuclideanData>(data);
    }

    [[nodiscard]] const MiniModifierData& as_mini_modifier() const {
        return std::get<MiniModifierData>(data);
    }

    [[nodiscard]] const MiniPolymeterData& as_mini_polymeter() const {
        return std::get<MiniPolymeterData>(data);
    }

    [[nodiscard]] const MiniLiteralData& as_mini_literal() const {
        return std::get<MiniLiteralData>(data);
    }

    [[nodiscard]] MiniLiteralData& as_mini_literal() {
        return std::get<MiniLiteralData>(data);
    }

    [[nodiscard]] const FunctionDefData& as_function_def() const {
        return std::get<FunctionDefData>(data);
    }

    [[nodiscard]] const MatchArmData& as_match_arm() const {
        return std::get<MatchArmData>(data);
    }

    [[nodiscard]] const MatchExprData& as_match_expr() const {
        return std::get<MatchExprData>(data);
    }

    [[nodiscard]] const RecordFieldData& as_record_field() const {
        return std::get<RecordFieldData>(data);
    }

    [[nodiscard]] const FieldAccessData& as_field_access() const {
        return std::get<FieldAccessData>(data);
    }

    [[nodiscard]] const FieldAssignmentData& as_field_assignment() const {
        return std::get<FieldAssignmentData>(data);
    }

    [[nodiscard]] const PipeBindingData& as_pipe_binding() const {
        return std::get<PipeBindingData>(data);
    }

    [[nodiscard]] const DestructureAssignmentData& as_destructure_assignment() const {
        return std::get<DestructureAssignmentData>(data);
    }

    [[nodiscard]] const DestructureParamData& as_destructure_param() const {
        return std::get<DestructureParamData>(data);
    }

    [[nodiscard]] const HoleData& as_hole() const {
        return std::get<HoleData>(data);
    }

    [[nodiscard]] const DirectiveData& as_directive() const {
        return std::get<DirectiveData>(data);
    }
};

/// Names of the extra_children slots for a node type (debugging /
/// pattern-debug serialization). Index-aligned with extra_children; for
/// Destructure nodes the single "default" kind repeats per field.
inline std::span<const char* const> extra_child_kinds(NodeType t) {
    static constexpr const char* kGuard[]   = {"guard"};
    static constexpr const char* kSpread[]  = {"spread"};
    static constexpr const char* kDefault[] = {"default"};
    switch (t) {
        case NodeType::MatchArm:               return kGuard;
        case NodeType::Argument:               return kSpread;
        case NodeType::RecordLit:              return kSpread;
        case NodeType::DestructureAssignment:  return kDefault;
        case NodeType::DestructureParam:       return kDefault;
        default:                               return {};
    }
}

/// Arena-based AST storage
class AstArena {
public:
    AstArena() {
        nodes_.reserve(256);  // Pre-allocate for typical program size
    }

    /// Allocate a new node, returns its index
    NodeIndex alloc(NodeType type, SourceLocation loc) {
        NodeIndex idx = static_cast<NodeIndex>(nodes_.size());
        nodes_.push_back(Node{
            .type = type,
            .location = loc,
            .first_child = NULL_NODE,
            .next_sibling = NULL_NODE,
            .data = std::monostate{}
        });
        return idx;
    }

    /// Get node by index
    [[nodiscard]] Node& operator[](NodeIndex idx) {
        return nodes_[idx];
    }

    [[nodiscard]] const Node& operator[](NodeIndex idx) const {
        return nodes_[idx];
    }

    /// Get number of nodes
    [[nodiscard]] std::size_t size() const {
        return nodes_.size();
    }

    /// Check if index is valid
    [[nodiscard]] bool valid(NodeIndex idx) const {
        return idx != NULL_NODE && idx < nodes_.size();
    }

    /// Add child to parent (appends to end of child list)
    void add_child(NodeIndex parent, NodeIndex child) {
        if (nodes_[parent].first_child == NULL_NODE) {
            nodes_[parent].first_child = child;
        } else {
            // Find last sibling
            NodeIndex curr = nodes_[parent].first_child;
            while (nodes_[curr].next_sibling != NULL_NODE) {
                curr = nodes_[curr].next_sibling;
            }
            nodes_[curr].next_sibling = child;
        }
    }

    /// Count children of a node
    [[nodiscard]] std::size_t child_count(NodeIndex parent) const {
        std::size_t count = 0;
        NodeIndex curr = nodes_[parent].first_child;
        while (curr != NULL_NODE) {
            count++;
            curr = nodes_[curr].next_sibling;
        }
        return count;
    }

    /// Iterate children
    template<typename F>
    void for_each_child(NodeIndex parent, F&& func) const {
        NodeIndex curr = nodes_[parent].first_child;
        while (curr != NULL_NODE) {
            func(curr, nodes_[curr]);
            curr = nodes_[curr].next_sibling;
        }
    }

private:
    std::vector<Node> nodes_;
};

/// Parsed AST with root node
struct Ast {
    AstArena arena;
    NodeIndex root = NULL_NODE;

    [[nodiscard]] bool valid() const {
        return root != NULL_NODE;
    }
};

} // namespace akkado
