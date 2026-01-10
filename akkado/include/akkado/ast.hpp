#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include "diagnostics.hpp"

namespace akkado {

/// Index into the AST arena (0xFFFFFFFF = null/invalid)
using NodeIndex = std::uint32_t;
constexpr NodeIndex NULL_NODE = 0xFFFFFFFF;

/// AST node types
enum class NodeType : std::uint8_t {
    // Literals
    NumberLit,      // Numeric literal
    BoolLit,        // true/false
    StringLit,      // "..." or '...' or `...`
    PitchLit,       // 'c4', 'f#3', 'Bb5' (MIDI note)
    ChordLit,       // 'c4:maj', 'a3:min7' (chord)

    // Identifiers
    Identifier,     // Variable or function name
    Hole,           // % (pipe input reference)

    // Expressions
    BinaryOp,       // Desugared to Call (add, sub, mul, div, pow)
    Call,           // Function call: f(a, b, c)
    MethodCall,     // Method call: x.f(a, b)
    Pipe,           // a |> b (let-binding rewrite)
    Closure,        // (params) -> body

    // Arguments
    Argument,       // Named or positional argument

    // Patterns
    MiniLiteral,    // pat("..."), seq("...", closure), etc.

    // Statements
    Assignment,     // x = expr
    PostStmt,       // post(closure)
    Block,          // { statements... expr }

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
        case NodeType::ChordLit:    return "ChordLit";
        case NodeType::Identifier:  return "Identifier";
        case NodeType::Hole:        return "Hole";
        case NodeType::BinaryOp:    return "BinaryOp";
        case NodeType::Call:        return "Call";
        case NodeType::MethodCall:  return "MethodCall";
        case NodeType::Pipe:        return "Pipe";
        case NodeType::Closure:     return "Closure";
        case NodeType::Argument:    return "Argument";
        case NodeType::MiniLiteral: return "MiniLiteral";
        case NodeType::Assignment:  return "Assignment";
        case NodeType::PostStmt:    return "PostStmt";
        case NodeType::Block:       return "Block";
        case NodeType::Program:     return "Program";
    }
    return "Unknown";
}

/// Binary operator type (before desugaring to Call)
enum class BinOp : std::uint8_t {
    Add,    // +  -> add(a, b)
    Sub,    // -  -> sub(a, b)
    Mul,    // *  -> mul(a, b)
    Div,    // /  -> div(a, b)
    Pow,    // ^  -> pow(a, b)
};

/// Get the function name for a binary operator
constexpr const char* binop_function_name(BinOp op) {
    switch (op) {
        case BinOp::Add: return "add";
        case BinOp::Sub: return "sub";
        case BinOp::Mul: return "mul";
        case BinOp::Div: return "div";
        case BinOp::Pow: return "pow";
    }
    return "unknown";
}

/// Pattern keyword type
enum class PatternType : std::uint8_t {
    Pat,
    Seq,
    Timeline,
    Note,
};

/// AST Node - stored in contiguous arena
/// Uses indices instead of pointers for cache efficiency
struct Node {
    NodeType type;
    SourceLocation location;

    // Child links (indices into arena)
    NodeIndex first_child = NULL_NODE;
    NodeIndex next_sibling = NULL_NODE;

    // Node-specific data (union-like via variant)
    struct NumberData { double value; bool is_integer; };
    struct BoolData { bool value; };
    struct StringData { std::string value; };
    struct IdentifierData { std::string name; };
    struct BinaryOpData { BinOp op; };
    struct ArgumentData { std::optional<std::string> name; };  // Named arg
    struct PatternData { PatternType pattern_type; };
    struct PitchData { std::uint8_t midi_note; };
    struct ChordData { std::uint8_t root_midi; std::vector<std::int8_t> intervals; };
    struct ClosureParamData { std::string name; std::optional<double> default_value; };  // Closure param with optional default

    std::variant<
        std::monostate,
        NumberData,
        BoolData,
        StringData,
        IdentifierData,
        BinaryOpData,
        ArgumentData,
        PatternData,
        PitchData,
        ChordData,
        ClosureParamData
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

    [[nodiscard]] const std::string& as_identifier() const {
        return std::get<IdentifierData>(data).name;
    }

    [[nodiscard]] BinOp as_binop() const {
        return std::get<BinaryOpData>(data).op;
    }

    [[nodiscard]] const std::optional<std::string>& as_arg_name() const {
        return std::get<ArgumentData>(data).name;
    }

    [[nodiscard]] PatternType as_pattern_type() const {
        return std::get<PatternData>(data).pattern_type;
    }

    [[nodiscard]] std::uint8_t as_pitch() const {
        return std::get<PitchData>(data).midi_note;
    }

    [[nodiscard]] const ChordData& as_chord() const {
        return std::get<ChordData>(data);
    }

    [[nodiscard]] const ClosureParamData& as_closure_param() const {
        return std::get<ClosureParamData>(data);
    }
};

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
