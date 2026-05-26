#pragma once

#include <vector>
#include <string_view>
#include "token.hpp"
#include "ast.hpp"
#include "diagnostics.hpp"
#include "string_interner.hpp"

namespace akkado {

/// Precedence levels for Pratt parser (lower = binds looser)
enum class Precedence : std::uint8_t {
    None = 0,
    Pipe,           // |>
    Or,             // ||
    And,            // &&
    Equality,       // == !=
    Comparison,     // < > <= >=
    Addition,       // + -
    Multiplication, // * /
    Power,          // ^
    Unary,          // ! (prefix)
    Method,         // .method()
    Call,           // f()
    Primary,        // literals, identifiers
};

/// Parsed closure parameter with optional default
struct ParsedParam {
    std::string name;
    std::optional<double> default_value;
    std::optional<std::string> default_string;  // String default
    NodeIndex default_node = NULL_NODE;          // AST node for default literal
    bool is_rest = false;  // true for ...param (variadic rest)
    // Phase 3b: function-parameter destructure (`fn f({x, y [= default]})`).
    // When true, `name` is a synthetic placeholder (`__destr_param_<N>`) and
    // `destructure_fields` carries the actual field-binding spec.
    bool is_destructure = false;
    std::vector<DestructureField> destructure_fields;
    // PRD prd-parameter-type-annotations §4.4: parser-side intermediate for
    // `name: stream` / `name: signal` annotations on `fn` parameters.
    // Defaults to Any (un-annotated). Destructure and rest params reject
    // annotations with E104 in Phase 1, so this field stays Any for those.
    ParamValueType annotated_type = ParamValueType::Any;
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
    /// @param interner Per-compile StringInterner (Phase 5 — used to
    ///        intern identifier strings synthesized by the parser
    ///        itself, e.g. `"_"`, `"bnot"`, `"out"`, `"bus"`).
    /// @param filename Filename for error reporting
    Parser(std::vector<Token> tokens, std::string_view source,
           StringInterner& interner,
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

    /// Enable opt-in lint warnings (currently: W201 dotted hole-field).
    /// Off by default — must be set before calling parse().
    void set_lint_strict(bool strict) { lint_strict_ = strict; }

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
    void error_with_code(const Token& token, std::string_view code,
                         std::string_view message);
    void warn(const Token& token, std::string_view code,
              std::string_view message);
    void synchronize();

    // Expression parsing (Pratt parser)
    NodeIndex parse_expression();
    NodeIndex parse_precedence(Precedence prec);
    NodeIndex parse_prefix();
    NodeIndex parse_infix(NodeIndex left, const Token& op);

    // Specific expression parsers
    NodeIndex parse_number();
    NodeIndex parse_pitch();
    NodeIndex parse_bool();
    NodeIndex parse_string();
    NodeIndex parse_identifier_or_call();
    NodeIndex parse_hole();
    NodeIndex parse_unary_not();
    NodeIndex parse_array();
    NodeIndex parse_grouping();
    NodeIndex parse_closure();
    NodeIndex parse_mini_literal();
    NodeIndex parse_match_expr();
    NodeIndex parse_record_literal();
    NodeIndex parse_field_access(NodeIndex left);
    NodeIndex parse_field_access_with_name(NodeIndex left, const Token& name_tok);

    // Infix parsers
    NodeIndex parse_binary(NodeIndex left, const Token& op);
    NodeIndex parse_pipe(NodeIndex left);
    NodeIndex parse_diamond_infix(NodeIndex left);
    NodeIndex parse_method_call(NodeIndex left);
    NodeIndex parse_index(NodeIndex left);

    // Function call helpers
    NodeIndex parse_call(const Token& name_token);
    NodeIndex parse_loop_expr(const Token& loop_token);
    /// True when the token stream at the current '(' is `( ... ) {` — i.e. the
    /// `loop` contextual keyword's block form rather than an ordinary call.
    [[nodiscard]] bool loop_form_ahead() const;
    NodeIndex parse_argument();
    std::vector<NodeIndex> parse_argument_list();

    // Closure helpers
    // `allow_destructure` permits `fn f({x, y})`-style destructure slots; only
    // fn-defs (Phase 3b) accept them. Closures `(x) -> …` pass false.
    std::vector<ParsedParam> parse_param_list(bool allow_destructure = false);
    NodeIndex parse_closure_body();
    NodeIndex parse_block();

    // Statement parsing
    NodeIndex parse_statement();
    NodeIndex parse_assignment(const Token& name_token);
    NodeIndex parse_fn_def(bool is_const = false, bool is_inline = false);
    NodeIndex parse_const_decl(const Token& name_token);
    NodeIndex parse_directive();

    // Destructuring helpers
    // Parses `{ Ident [= default] (, Ident [= default])* }`. The caller has
    // already confirmed the leading `{`; this helper consumes that `{` AND
    // the closing `}`. Emits E188 on duplicate field names.
    //
    // `allow_defaults` gates the `Ident = expr` form. Statement-level
    // destructure (`{x = 1} = r`) and fn-param destructure
    // (`fn f({x = 1})`) pass `true`. Pipe-binding (`as {x, y}`) and match-arm
    // (`{x, y}:`) destructures pass `false` — defaults in those positions
    // are deferred and emit a clear parse error.
    std::vector<DestructureField> parse_destructure_fields(bool allow_defaults);

    // Import parsing
    NodeIndex parse_import_decl();

    // Program parsing
    NodeIndex parse_program();

    // Precedence helpers
    [[nodiscard]] Precedence get_precedence(TokenType type) const;
    [[nodiscard]] bool is_infix_operator(TokenType type) const;
    [[nodiscard]] bool is_indexable(NodeIndex node) const;

    // Make nodes
    NodeIndex make_node(NodeType type);
    NodeIndex make_node(NodeType type, const Token& token);

    // Data
    std::vector<Token> tokens_;
    std::string_view source_;
    StringInterner* interner_ = nullptr;  // PRD Phase 5
    std::string filename_;
    std::vector<Diagnostic> diagnostics_;
    AstArena arena_;

    std::size_t current_idx_ = 0;
    bool panic_mode_ = false;
    bool lint_strict_ = false;  // emit opt-in lint warnings (W201 etc.)
    std::size_t destr_counter_ = 0;  // unique counter for destructuring temp bindings
};

/// Convenience function to parse source code
/// @param tokens Tokens from lexer
/// @param source Original source code
/// @param interner Per-compile StringInterner
/// @param filename Filename for error reporting
/// @param lint_strict Emit opt-in lint warnings (W201 etc.). Off by default.
/// @return Pair of AST and diagnostics
std::pair<Ast, std::vector<Diagnostic>>
parse(std::vector<Token> tokens, std::string_view source,
      StringInterner& interner,
      std::string_view filename = "<input>",
      bool lint_strict = false);

} // namespace akkado
