#pragma once

#include <vector>
#include <string_view>
#include "token.hpp"
#include "ast.hpp"
#include "diagnostics.hpp"

namespace akkado {

/// Precedence levels for Pratt parser (lower = binds looser)
enum class Precedence : std::uint8_t {
    None = 0,
    Pipe,           // |>
    Addition,       // + -
    Multiplication, // * /
    Power,          // ^
    Method,         // .method()
    Call,           // f()
    Primary,        // literals, identifiers
};

/// Parser for the Akkado language
///
/// Uses Pratt parsing (precedence climbing) to handle operator precedence.
/// Produces an arena-allocated AST.
class Parser {
public:
    /// Construct a parser from a token stream
    /// @param tokens Vector of tokens from lexer (must end with Eof)
    /// @param source Original source for error messages
    /// @param filename Filename for error reporting
    Parser(std::vector<Token> tokens, std::string_view source,
           std::string_view filename = "<input>");

    /// Parse the entire program
    /// @return Parsed AST (check diagnostics for errors)
    [[nodiscard]] Ast parse();

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
    [[nodiscard]] const Token& current() const;
    [[nodiscard]] const Token& previous() const;
    [[nodiscard]] bool is_at_end() const;
    [[nodiscard]] bool check(TokenType type) const;
    [[nodiscard]] bool match(TokenType type);
    const Token& advance();
    const Token& consume(TokenType type, std::string_view message);

    // Error handling
    void error(std::string_view message);
    void error_at(const Token& token, std::string_view message);
    void synchronize();

    // Expression parsing (Pratt parser)
    NodeIndex parse_expression();
    NodeIndex parse_precedence(Precedence prec);
    NodeIndex parse_prefix();
    NodeIndex parse_infix(NodeIndex left, const Token& op);

    // Specific expression parsers
    NodeIndex parse_number();
    NodeIndex parse_bool();
    NodeIndex parse_string();
    NodeIndex parse_identifier_or_call();
    NodeIndex parse_hole();
    NodeIndex parse_grouping();
    NodeIndex parse_closure();
    NodeIndex parse_mini_literal();

    // Infix parsers
    NodeIndex parse_binary(NodeIndex left, const Token& op);
    NodeIndex parse_pipe(NodeIndex left);
    NodeIndex parse_method_call(NodeIndex left);

    // Function call helpers
    NodeIndex parse_call(const Token& name_token);
    NodeIndex parse_argument();
    std::vector<NodeIndex> parse_argument_list();

    // Closure helpers
    std::vector<std::string> parse_param_list();
    NodeIndex parse_closure_body();
    NodeIndex parse_block();

    // Statement parsing
    NodeIndex parse_statement();
    NodeIndex parse_assignment(const Token& name_token);
    NodeIndex parse_post_stmt();

    // Program parsing
    NodeIndex parse_program();

    // Precedence helpers
    [[nodiscard]] Precedence get_precedence(TokenType type) const;
    [[nodiscard]] bool is_infix_operator(TokenType type) const;

    // Make nodes
    NodeIndex make_node(NodeType type);
    NodeIndex make_node(NodeType type, const Token& token);

    // Data
    std::vector<Token> tokens_;
    std::string_view source_;
    std::string filename_;
    std::vector<Diagnostic> diagnostics_;
    AstArena arena_;

    std::size_t current_idx_ = 0;
    bool panic_mode_ = false;
};

/// Convenience function to parse source code
/// @param tokens Tokens from lexer
/// @param source Original source code
/// @param filename Filename for error reporting
/// @return Pair of AST and diagnostics
std::pair<Ast, std::vector<Diagnostic>>
parse(std::vector<Token> tokens, std::string_view source,
      std::string_view filename = "<input>");

} // namespace akkado
