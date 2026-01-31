#pragma once

#include <vector>
#include <string_view>
#include "mini_token.hpp"
#include "ast.hpp"
#include "diagnostics.hpp"

namespace akkado {

/// Parser for mini-notation patterns inside pat(), seq(), etc.
///
/// Parses the token stream produced by MiniLexer into an AST subtree.
/// The resulting AST uses the mini-notation node types (MiniPattern, MiniAtom, etc.)
/// and is integrated into the main AST as a child of MiniLiteral nodes.
///
/// Grammar (simplified):
///   pattern    = { element }
///   element    = atom [ modifiers ] | group | sequence | polyrhythm | polymeter
///   atom       = pitch | sample | rest | euclidean
///   group      = "[" pattern "]"
///   sequence   = "<" pattern ">"
///   polyrhythm = "[" atom { "," atom } "]"
///   polymeter  = "{" pattern "}" [ "%" number ]
///   euclidean  = atom "(" number "," number [ "," number ] ")"
///   modifiers  = { "*" number | "/" number | ":" number | "@" number | "!" number | "?" number }
///   choice     = element { "|" element }
class MiniParser {
public:
    /// Construct a mini-parser from a token stream
    /// @param tokens Vector of tokens from MiniLexer (must end with Eof)
    /// @param arena Reference to the AST arena to add nodes to
    /// @param base_location Location of the pattern string for error reporting
    MiniParser(std::vector<MiniToken> tokens, AstArena& arena,
               SourceLocation base_location = {});

    /// Parse the pattern into an AST subtree
    /// @return Index of the root MiniPattern node, or NULL_NODE on failure
    [[nodiscard]] NodeIndex parse();

    /// Get diagnostics generated during parsing
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

    /// Check if any errors occurred
    [[nodiscard]] bool has_errors() const {
        return akkado::has_errors(diagnostics_);
    }

private:
    // Token navigation
    [[nodiscard]] const MiniToken& current() const;
    [[nodiscard]] const MiniToken& previous() const;
    [[nodiscard]] bool is_at_end() const;
    [[nodiscard]] bool check(MiniTokenType type) const;
    [[nodiscard]] bool match(MiniTokenType type);
    const MiniToken& advance();
    const MiniToken& consume(MiniTokenType type, std::string_view message);

    // Error handling
    void error(std::string_view message);
    void error_at(const MiniToken& token, std::string_view message);

    // Pattern parsing
    NodeIndex parse_pattern();      // Root pattern (sequence of elements)
    NodeIndex parse_choice();       // Element or choice of elements (a | b | c)
    NodeIndex parse_element();      // Single element with optional modifiers
    NodeIndex parse_atom();         // Pitch, sample, rest, or grouped pattern

    // Grouping parsers
    NodeIndex parse_group();        // [a b c] or [a, b, c]
    NodeIndex parse_sequence();     // <a b c>
    NodeIndex parse_polymeter();    // {a b c} or {a b}%n

    // Modifier parsing
    NodeIndex parse_modifiers(NodeIndex atom);
    NodeIndex parse_euclidean(NodeIndex atom);

    // Atom parsers
    NodeIndex parse_pitch_atom(const MiniToken& token);
    NodeIndex parse_sample_atom(const MiniToken& token);
    NodeIndex parse_chord_atom(const MiniToken& token);
    NodeIndex parse_rest();

    // Helper to check if current token starts an atom
    [[nodiscard]] bool is_atom_start() const;

    // Make nodes
    NodeIndex make_node(NodeType type, const MiniToken& token);
    NodeIndex make_node(NodeType type, SourceLocation loc);

    // Data
    std::vector<MiniToken> tokens_;
    AstArena& arena_;
    SourceLocation base_location_;
    std::vector<Diagnostic> diagnostics_;

    std::size_t current_idx_ = 0;
};

/// Convenience function to parse a mini-notation pattern
/// @param pattern The pattern string content
/// @param arena Reference to the AST arena
/// @param base_location Location for error reporting
/// @param sample_only When true, treat all alphanumeric sequences as samples (for chord patterns)
/// @return Pair of root node index and diagnostics
std::pair<NodeIndex, std::vector<Diagnostic>>
parse_mini(std::string_view pattern, AstArena& arena,
           SourceLocation base_location = {}, bool sample_only = false);

} // namespace akkado
