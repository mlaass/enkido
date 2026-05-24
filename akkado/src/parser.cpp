#include "akkado/parser.hpp"
#include "akkado/mini_parser.hpp"
#include <stdexcept>
#include <set>

namespace akkado {

Parser::Parser(std::vector<Token> tokens, std::string_view source,
               std::string_view filename)
    : tokens_(std::move(tokens))
    , source_(source)
    , filename_(filename)
{}

Ast Parser::parse() {
    Ast ast;
    ast.root = parse_program();
    ast.arena = std::move(arena_);
    return ast;
}

// Token navigation

const Token& Parser::current() const {
    return tokens_[current_idx_];
}

const Token& Parser::previous() const {
    return tokens_[current_idx_ - 1];
}

bool Parser::is_at_end() const {
    return current().type == TokenType::Eof;
}

bool Parser::check(TokenType type) const {
    return current().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::advance() {
    if (!is_at_end()) {
        current_idx_++;
    }
    return previous();
}

const Token& Parser::consume(TokenType type, std::string_view message) {
    if (check(type)) {
        return advance();
    }
    error(message);
    return current();
}

// Error handling

void Parser::error(std::string_view message) {
    error_at(current(), message);
}

// Names reserved for state-cell built-ins (Phase 3 of userspace-state PRD).
// Users cannot bind these — silent shadowing would let `s.get()` resolve to
// a user closure that doesn't understand StateCell args, producing a
// confusing error far from the cause. (notes/freqs are intentionally NOT
// reserved — they are common variable names; they behave like len/map:
// special-cased by codegen but shadowable by a user binding.)
static bool is_reserved_state_name(std::string_view name) {
    return name == "state" || name == "get" || name == "set";
}

void Parser::error_at(const Token& token, std::string_view message) {
    if (panic_mode_) return;  // Suppress cascading errors
    panic_mode_ = true;

    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .code = "P001",
        .message = std::string(message),
        .filename = filename_,
        .location = token.location
    });
}

void Parser::error_with_code(const Token& token, std::string_view code,
                             std::string_view message) {
    if (panic_mode_) return;
    panic_mode_ = true;

    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .code = std::string(code),
        .message = std::string(message),
        .filename = filename_,
        .location = token.location
    });
}

void Parser::warn(const Token& token, std::string_view code,
                  std::string_view message) {
    // Warnings don't suppress on panic — they're additive lints, not errors.
    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Warning,
        .code = std::string(code),
        .message = std::string(message),
        .filename = filename_,
        .location = token.location
    });
}

namespace {

// Identifier-or-keyword tokens accepted as field names in the dotless
// hole-field shorthand (`@field` / `%field`). Excludes the string-prefix
// pattern tokens (Timeline, ValuePat, NotePat, SamplePat, ChordPat)
// because those only lex when followed by `"` and dragging them in would
// hijack the trailing string literal.
constexpr bool is_hole_field_token(TokenType t) {
    switch (t) {
        case TokenType::Identifier:
        case TokenType::True:
        case TokenType::False:
        case TokenType::Match:
        case TokenType::Fn:
        case TokenType::As:
        case TokenType::Const:
        case TokenType::Import:
            return true;
        default:
            return false;
    }
}

constexpr bool tokens_adjacent(const Token& a, const Token& b) {
    return a.location.offset + a.location.length == b.location.offset;
}

} // namespace

void Parser::synchronize() {
    panic_mode_ = false;

    while (!is_at_end()) {
        // Synchronize at statement boundaries
        switch (current().type) {
            case TokenType::Timeline:
            case TokenType::ValuePat:
            case TokenType::NotePat:
            case TokenType::SamplePat:
            case TokenType::ChordPat:
            case TokenType::Const:
                return;
            default:
                break;
        }

        // Check if previous token ends a statement
        if (previous().type == TokenType::RBrace) {
            return;
        }

        // Check if we hit an identifier that could start an assignment
        if (check(TokenType::Identifier)) {
            // Peek ahead for '=' to detect assignment
            if (current_idx_ + 1 < tokens_.size() &&
                tokens_[current_idx_ + 1].type == TokenType::Equals) {
                return;
            }
        }

        advance();
    }
}

// Precedence helpers

Precedence Parser::get_precedence(TokenType type) const {
    switch (type) {
        case TokenType::Pipe:         return Precedence::Pipe;
        case TokenType::OrOr:         return Precedence::Or;
        case TokenType::AndAnd:       return Precedence::And;
        case TokenType::EqualEqual:
        case TokenType::BangEqual:    return Precedence::Equality;
        case TokenType::Less:
        case TokenType::Greater:
        case TokenType::LessEqual:
        case TokenType::GreaterEqual: return Precedence::Comparison;
        case TokenType::Plus:
        case TokenType::Minus:        return Precedence::Addition;
        case TokenType::Star:
        case TokenType::Slash:        return Precedence::Multiplication;
        case TokenType::Caret:        return Precedence::Power;
        default:                      return Precedence::None;
    }
}

bool Parser::is_infix_operator(TokenType type) const {
    switch (type) {
        case TokenType::Pipe:
        case TokenType::OrOr:
        case TokenType::AndAnd:
        case TokenType::EqualEqual:
        case TokenType::BangEqual:
        case TokenType::Less:
        case TokenType::Greater:
        case TokenType::LessEqual:
        case TokenType::GreaterEqual:
        case TokenType::Plus:
        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Caret:
            return true;
        default:
            return false;
    }
}

// Node creation helpers

NodeIndex Parser::make_node(NodeType type) {
    return arena_.alloc(type, current().location);
}

NodeIndex Parser::make_node(NodeType type, const Token& token) {
    return arena_.alloc(type, token.location);
}

// Program parsing

NodeIndex Parser::parse_import_decl() {
    Token import_tok = previous();  // Import token already consumed by match()
    NodeIndex node = make_node(NodeType::ImportDecl, import_tok);

    // Expect string literal (path). Guard against the `str` keyword token
    // (which shares TokenType::String per PRD prd-parameter-type-annotations-phase-2 §4.4).
    if (!check(TokenType::String) ||
        !std::holds_alternative<std::string>(tokens_[current_idx_].value)) {
        error("Expected module path string after 'import'");
        return node;
    }
    Token path_tok = advance();
    std::string path = path_tok.as_string();

    // Optional: as <ident>
    std::string alias;
    if (match(TokenType::As)) {
        if (!check(TokenType::Identifier)) {
            error("Expected identifier after 'as'");
        } else {
            Token alias_tok = advance();
            alias = std::string(alias_tok.lexeme);
        }
    }

    arena_[node].data = Node::ImportDeclData{std::move(path), std::move(alias)};
    return node;
}

NodeIndex Parser::parse_program() {
    NodeIndex program = make_node(NodeType::Program);
    bool seen_code = false;

    while (!is_at_end()) {
        if (match(TokenType::Import)) {
            if (seen_code) {
                diagnostics_.push_back(Diagnostic{
                    .severity = Severity::Error,
                    .code = "E501",
                    .message = "Import statements must appear before other code",
                    .filename = filename_,
                    .location = previous().location
                });
            }
            NodeIndex imp = parse_import_decl();
            if (imp != NULL_NODE) {
                arena_.add_child(program, imp);
            }
        } else {
            seen_code = true;
            NodeIndex stmt = parse_statement();
            if (stmt != NULL_NODE) {
                arena_.add_child(program, stmt);
            }
        }
        if (panic_mode_) {
            synchronize();
        }
    }

    return program;
}

// Statement parsing

NodeIndex Parser::parse_statement() {
    // Check for directive
    if (check(TokenType::Directive)) {
        return parse_directive();
    }

    // Check for annotation: #inline fn ... (PRD prd-runtime-functions-control-flow L2).
    // Annotations are a `#` followed by an identifier at statement position,
    // attached to the next `fn` declaration. v1 defines `#inline` only.
    if (match(TokenType::Hash)) {
        Token hash_tok = previous();
        if (!check(TokenType::Identifier)) {
            error_with_code(current(), "E249",
                            "expected annotation name after '#'");
            return NULL_NODE;
        }
        Token ann_tok = advance();
        if (ann_tok.lexeme != "inline") {
            error_with_code(ann_tok, "E249",
                            "unknown annotation '#" + std::string(ann_tok.lexeme) +
                            "' — see docs for supported annotations");
            return NULL_NODE;
        }
        // #inline must precede a `fn` declaration (optionally `const fn`).
        if (match(TokenType::Const)) {
            if (match(TokenType::Fn)) {
                return parse_fn_def(/*is_const=*/true, /*is_inline=*/true);
            }
            error_with_code(previous(), "E246",
                            "#inline must precede a `fn` declaration");
            return NULL_NODE;
        }
        if (match(TokenType::Fn)) {
            return parse_fn_def(/*is_const=*/false, /*is_inline=*/true);
        }
        error_with_code(hash_tok, "E246",
                        "#inline must precede a `fn` declaration");
        return NULL_NODE;
    }

    // Check for const declaration: const fn ... or const x = ...
    if (match(TokenType::Const)) {
        if (match(TokenType::Fn)) {
            return parse_fn_def(/*is_const=*/true);
        }
        if (check(TokenType::Underscore)) {
            error("Cannot assign to reserved identifier '_'");
            return NULL_NODE;
        }
        if (check(TokenType::Identifier)) {
            Token name = advance();
            return parse_const_decl(name);
        }
        error("Expected 'fn' or identifier after 'const'");
        return NULL_NODE;
    }

    // Check for function definition
    if (match(TokenType::Fn)) {
        return parse_fn_def();
    }

    // Check for assignment: identifier = expr (block _ = ...)
    if (check(TokenType::Underscore) && current_idx_ + 1 < tokens_.size() &&
        tokens_[current_idx_ + 1].type == TokenType::Equals) {
        error("Cannot assign to reserved identifier '_'");
        advance();  // consume _
        advance();  // consume =
        parse_expression();  // consume RHS for error recovery
        return NULL_NODE;
    }
    if (check(TokenType::Identifier)) {
        if (current_idx_ + 1 < tokens_.size() &&
            tokens_[current_idx_ + 1].type == TokenType::Equals) {
            Token name = advance();  // consume identifier
            return parse_assignment(name);
        }
    }

    // Check for statement-level destructure assignment: { Ident [= default]
    // (, Ident [= default])* } = expr.
    // Disambiguated from a record-literal expression-statement by the absence
    // of `:` after the first identifier, AND the presence of `} =` at the
    // matching close brace. The lookahead tracks paren/bracket/brace depth so
    // default expressions like `{x = (a + b)}` don't confuse it.
    if (check(TokenType::LBrace)) {
        bool is_destructure_stmt = false;
        std::size_t j = current_idx_ + 1;  // first token after `{`
        if (j < tokens_.size() && tokens_[j].type == TokenType::Identifier) {
            // `{x:` → record literal, not destructure.
            bool starts_like_record = (j + 1 < tokens_.size()
                                       && tokens_[j + 1].type == TokenType::Colon);
            if (!starts_like_record) {
                int depth = 1;  // already inside the outer `{`
                std::size_t k = j;
                while (k < tokens_.size() && depth > 0) {
                    TokenType t = tokens_[k].type;
                    if (t == TokenType::LBrace || t == TokenType::LParen ||
                        t == TokenType::LBracket) {
                        ++depth;
                    } else if (t == TokenType::RBrace || t == TokenType::RParen ||
                               t == TokenType::RBracket) {
                        --depth;
                        if (depth == 0) break;
                    }
                    ++k;
                }
                if (depth == 0 && k + 1 < tokens_.size() &&
                    tokens_[k + 1].type == TokenType::Equals) {
                    is_destructure_stmt = true;
                }
            }
        }

        if (is_destructure_stmt) {
            Token brace_tok = current();  // `{` — used for node location
            std::vector<DestructureField> fields = parse_destructure_fields(true);
            consume(TokenType::Equals, "Expected '=' after destructure pattern");

            NodeIndex node = make_node(NodeType::DestructureAssignment, brace_tok);
            arena_[node].data = Node::DestructureAssignmentData{std::move(fields)};

            NodeIndex value = parse_expression();
            if (value != NULL_NODE) {
                arena_.add_child(node, value);
            }
            return node;
        }
    }

    // Otherwise it's an expression statement.
    NodeIndex expr = parse_expression();

    // Diamond operator (bus-routing Phase 3): `expr <>` / `expr <>(N)` is
    // statement-trailing sugar for `expr |> out(@)` / `expr |> bus(N, @)`.
    // Consumed only here, after the full expression (including the entire
    // `|>` chain), so `<>` naturally binds looser than `|>`.
    if (expr != NULL_NODE && check(TokenType::Diamond)) {
        Token diamond_tok = advance();  // consume '<>'

        // Optional `(N)` bus-index argument. The index expression is wired
        // in verbatim — `handle_bus_call` validates it (E260), so `<>(x)`
        // and `bus(x, @)` produce the identical diagnostic.
        NodeIndex index_expr = NULL_NODE;
        if (match(TokenType::LParen)) {
            index_expr = parse_expression();
            consume(TokenType::RParen,
                    "Expected ')' after bus index in `<>(N)`");
        }

        // `@` hole — the signal injected by the pipe.
        NodeIndex hole = arena_.alloc(NodeType::Hole, diamond_tok.location);
        arena_[hole].data = Node::HoleData{std::nullopt};

        // Sink call: `out(@)` for bare `<>`, `bus(N, @)` for `<>(N)`.
        NodeIndex call = make_node(NodeType::Call, diamond_tok);
        arena_[call].data =
            Node::IdentifierData{index_expr == NULL_NODE ? "out" : "bus"};

        if (index_expr != NULL_NODE) {
            NodeIndex idx_arg =
                arena_.alloc(NodeType::Argument, arena_[index_expr].location);
            arena_[idx_arg].data = Node::ArgumentData{std::nullopt};
            arena_.add_child(idx_arg, index_expr);
            arena_.add_child(call, idx_arg);
        }

        NodeIndex hole_arg =
            arena_.alloc(NodeType::Argument, diamond_tok.location);
        arena_[hole_arg].data = Node::ArgumentData{std::nullopt};
        arena_.add_child(hole_arg, hole);
        arena_.add_child(call, hole_arg);

        // Wrap as `expr |> <call>`.
        NodeIndex pipe = make_node(NodeType::Pipe, diamond_tok);
        arena_.add_child(pipe, expr);
        arena_.add_child(pipe, call);
        return pipe;
    }

    // Phase 4b: field-assignment sugar over record-valued state cells.
    //   `receiver.field = value` parses as `FieldAccess(receiver, field)`
    //   followed by a stray `=`. Transform into a FieldAssignment statement.
    //
    //   `expr |> receiver.field = value` parses as a Pipe whose RHS is the
    //   FieldAccess; that case is rejected with E205 because field assignment
    //   is a statement, not an expression — the user must wrap in `{ ... }`.
    if (expr != NULL_NODE && check(TokenType::Equals)) {
        const Node& en = arena_[expr];

        if (en.type == NodeType::FieldAccess) {
            // Steal the FieldAccess's field name and receiver child, build a
            // FieldAssignment in their place.
            std::string field_name = en.as_field_access().field_name;
            NodeIndex receiver = en.first_child;
            SourceLocation loc = en.location;

            advance();  // consume '='
            NodeIndex value = parse_expression();

            NodeIndex node = arena_.alloc(NodeType::FieldAssignment, loc);
            arena_[node].data = Node::FieldAssignmentData{std::move(field_name)};
            arena_.add_child(node, receiver);
            if (value != NULL_NODE) {
                arena_.add_child(node, value);
            }
            return node;
        }

        if (en.type == NodeType::Pipe) {
            // The pipe's RHS is the second child. If it's a FieldAccess, the
            // user wrote `expr |> cell.field = value` outside a block.
            NodeIndex first = en.first_child;
            NodeIndex pipe_rhs = (first != NULL_NODE)
                ? arena_[first].next_sibling : NULL_NODE;
            if (pipe_rhs != NULL_NODE &&
                arena_[pipe_rhs].type == NodeType::FieldAccess) {
                Token eq_tok = current();
                diagnostics_.push_back(Diagnostic{
                    .severity = Severity::Error,
                    .code = "E205",
                    .message = "Field assignment is a statement, not an "
                               "expression — it cannot be the right-hand "
                               "side of a pipe. Move the `cell.field = expr` "
                               "to a top-level statement.",
                    .filename = filename_,
                    .location = eq_tok.location
                });
                advance();             // consume '='
                (void)parse_expression();  // recover: discard value
                return NULL_NODE;
            }
        }
        // Some other LHS — fall through; the leftover '=' will become a
        // generic parse error at the top-level loop.
    }

    return expr;
}

NodeIndex Parser::parse_assignment(const Token& name_token) {
    if (is_reserved_state_name(name_token.lexeme)) {
        error_at(name_token,
                 "'" + std::string(name_token.lexeme) +
                 "' is a reserved built-in identifier and cannot be redefined");
    }
    consume(TokenType::Equals, "Expected '=' after identifier");

    NodeIndex node = make_node(NodeType::Assignment, name_token);
    arena_[node].data = Node::IdentifierData{std::string(name_token.lexeme)};

    NodeIndex value = parse_expression();
    if (value != NULL_NODE) {
        arena_.add_child(node, value);
    }

    return node;
}

// Expression parsing (Pratt parser)

NodeIndex Parser::parse_expression() {
    return parse_precedence(Precedence::Pipe);
}

// Check if a node can be followed by an index operation `[...]`.
// Returns false for expression types that shouldn't be indexed (match, blocks).
// This prevents `match(...){...}` at end of function body from consuming
// subsequent array literals as index operations.
bool Parser::is_indexable(NodeIndex node) const {
    if (node == NULL_NODE) return false;
    const Node& n = arena_[node];
    switch (n.type) {
        case NodeType::Identifier:
        case NodeType::Call:
        case NodeType::MethodCall:
        case NodeType::Index:
        case NodeType::FieldAccess:  // Allow e.notes[i] / record.field[i]
        case NodeType::ArrayLit:
        case NodeType::StringLit:  // Allow "str"[0] for string indexing
            return true;
        default:
            return false;
    }
}

NodeIndex Parser::parse_precedence(Precedence prec) {
    NodeIndex left = parse_prefix();
    if (left == NULL_NODE) {
        return NULL_NODE;
    }

    // Handle postfix operations (highest precedence, can chain)
    // Method calls (.method()) work at any precedence level.
    // Indexing ([expr]) only after "indexable" expressions to avoid
    // consuming array literals at statement level after match/block expressions.
    while (prec <= Precedence::Method) {
        if (check(TokenType::Dot)) {
            advance();  // consume '.'
            left = parse_method_call(left);
        } else if (check(TokenType::LBracket) && is_indexable(left)) {
            // Only allow indexing after identifiers, calls, other indexes, arrays, etc.
            // Not after match expressions or blocks
            left = parse_index(left);
        } else {
            break;
        }
    }

    // Handle binary operators
    while (!is_at_end()) {
        // Check for 'as' binding before pipe operator: expr as name |> ...
        // The 'as' binds the current expression to a name for use in pipe chain
        if (check(TokenType::As) && prec <= Precedence::Pipe) {
            Token as_tok = advance();  // consume 'as'

            if (check(TokenType::LBrace)) {
                // as {field1, field2, ...} — destructuring binding.
                // Defaults (`as {x = 1}`) are deferred — the helper rejects
                // them when allow_defaults is false.
                std::vector<DestructureField> destr_fields =
                    parse_destructure_fields(false);
                std::vector<std::string> destr_names;
                destr_names.reserve(destr_fields.size());
                for (auto& f : destr_fields) {
                    destr_names.push_back(std::move(f.name));
                }

                std::string temp_name = "__destr_" + std::to_string(destr_counter_++);
                NodeIndex binding = make_node(NodeType::PipeBinding, as_tok);
                arena_[binding].data = Node::PipeBindingData{temp_name, std::move(destr_names)};
                arena_.add_child(binding, left);
                left = binding;
            } else if (check(TokenType::Identifier)) {
                Token name_tok = advance();

                // Wrap 'left' in a PipeBinding node
                NodeIndex binding = make_node(NodeType::PipeBinding, as_tok);
                arena_[binding].data = Node::PipeBindingData{std::string(name_tok.lexeme)};
                arena_.add_child(binding, left);

                left = binding;
            } else {
                error("Expected identifier or '{' after 'as'");
            }
            continue;  // Continue to check for |> or other operators
        }

        if (!is_infix_operator(current().type)) {
            break;
        }

        Precedence op_prec = get_precedence(current().type);
        if (op_prec < prec) {
            break;
        }

        Token op = advance();
        left = parse_infix(left, op);

        // After binary op, check for postfix ops again
        while (prec <= Precedence::Method) {
            if (check(TokenType::Dot)) {
                advance();  // consume '.'
                left = parse_method_call(left);
            } else if (check(TokenType::LBracket) && is_indexable(left)) {
                left = parse_index(left);
            } else {
                break;
            }
        }
    }

    return left;
}

NodeIndex Parser::parse_prefix() {
    switch (current().type) {
        case TokenType::Number:
            return parse_number();
        case TokenType::PitchLit:
            return parse_pitch();
        case TokenType::True:
        case TokenType::False:
            return parse_bool();
        case TokenType::String:
            return parse_string();
        case TokenType::Identifier:
            return parse_identifier_or_call();
        case TokenType::Hole:
        case TokenType::At:
            return parse_hole();
        case TokenType::LParen:
            return parse_grouping();
        case TokenType::LBracket:
            return parse_array();
        case TokenType::LBrace:
            return parse_record_literal();
        case TokenType::Timeline:
        case TokenType::ValuePat:
        case TokenType::NotePat:
        case TokenType::SamplePat:
        case TokenType::ChordPat:
            return parse_mini_literal();
        case TokenType::Match:
            return parse_match_expr();
        case TokenType::Bang:
            return parse_unary_not();
        case TokenType::Diamond:
            // The diamond `<>` is a statement terminator producing void; it
            // is valid only trailing a complete statement (handled in
            // parse_statement). Reaching it where an expression is expected
            // means it was misplaced — sub-expression position, an
            // assignment RHS, a closure/fn body, etc. (A leftover `<>` after
            // a non-expression statement also surfaces here, since it
            // becomes the leading token of the next statement parse.)
            error_with_code(current(), "E263",
                            "the diamond operator `<>` is a statement "
                            "terminator and cannot appear in expression "
                            "position — it must trail a complete statement, "
                            "e.g. `osc(\"saw\", 220) <>`");
            advance();  // consume '<>' so parsing makes progress
            return NULL_NODE;
        case TokenType::Underscore: {
            // Placeholder for partial application: add(3, _)
            Token tok = advance();
            NodeIndex node = make_node(NodeType::Identifier, tok);
            arena_[node].data = Node::IdentifierData{"_"};
            return node;
        }
        default:
            // Pre-Phase-2 latent bug: the default case must advance to make
            // progress, otherwise parse_program's `while (!is_at_end())` loop
            // spins forever on any unhandled token (e.g. a reserved type-name
            // keyword like `sig` / `arr` reached in expression position).
            error("Expected expression");
            advance();
            return NULL_NODE;
    }
}

NodeIndex Parser::parse_infix(NodeIndex left, const Token& op) {
    switch (op.type) {
        case TokenType::Pipe:
            return parse_pipe(left);
        case TokenType::Plus:
        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Caret:
        case TokenType::OrOr:
        case TokenType::AndAnd:
        case TokenType::EqualEqual:
        case TokenType::BangEqual:
        case TokenType::Less:
        case TokenType::Greater:
        case TokenType::LessEqual:
        case TokenType::GreaterEqual:
            return parse_binary(left, op);
        default:
            error("Unknown infix operator");
            return left;
    }
}

// Literal parsers

NodeIndex Parser::parse_number() {
    Token tok = advance();
    NodeIndex node = make_node(NodeType::NumberLit, tok);

    // PRD prd-parameter-type-annotations-phase-2 §4.4: the lexer maps the
    // `num` keyword to TokenType::Number with an empty value variant (so
    // parse_optional_annotation can disambiguate by lexeme). If such a
    // keyword-token leaks into expression position, emit a clean E185
    // instead of crashing on std::bad_variant_access below.
    if (!std::holds_alternative<NumericValue>(tok.value)) {
        error_with_code(tok, "E185",
            "Reserved type-name keyword '" + std::string(tok.lexeme) +
            "' cannot appear in expression position.");
        arena_[node].data = Node::NumberData{0.0, true};
        return node;
    }

    auto& num_val = std::get<NumericValue>(tok.value);
    arena_[node].data = Node::NumberData{num_val.value, num_val.is_integer};

    return node;
}

NodeIndex Parser::parse_pitch() {
    Token tok = advance();
    NodeIndex node = make_node(NodeType::PitchLit, tok);
    arena_[node].data = Node::PitchData{tok.as_pitch()};
    return node;
}

NodeIndex Parser::parse_bool() {
    Token tok = advance();
    NodeIndex node = make_node(NodeType::BoolLit, tok);
    arena_[node].data = Node::BoolData{tok.type == TokenType::True};
    return node;
}

NodeIndex Parser::parse_string() {
    Token tok = advance();
    NodeIndex node = make_node(NodeType::StringLit, tok);

    // PRD prd-parameter-type-annotations-phase-2 §4.4: the lexer maps the
    // `str` keyword to TokenType::String with an empty value variant. If
    // such a keyword-token leaks into expression position, emit a clean
    // E185 instead of crashing on std::bad_variant_access in as_string().
    if (!std::holds_alternative<std::string>(tok.value)) {
        error_with_code(tok, "E185",
            "Reserved type-name keyword '" + std::string(tok.lexeme) +
            "' cannot appear in expression position.");
        arena_[node].data = Node::StringData{""};
        return node;
    }

    arena_[node].data = Node::StringData{tok.as_string()};
    return node;
}

NodeIndex Parser::parse_hole() {
    Token tok = advance();  // consume '%' or '@'
    NodeIndex node = make_node(NodeType::Hole, tok);

    // Dotless shorthand: `@field` / `%field` when an identifier or
    // word-keyword token is immediately adjacent to the hole. Produces the
    // same HoleData{field_name} AST as the dotted form.
    if (is_hole_field_token(current().type) && tokens_adjacent(tok, current())) {
        // `@ident(` is a method-call attempt and not permitted (E108).
        // Recover by treating `ident` as a field name so downstream
        // diagnostics stay localized.
        if (current_idx_ + 1 < tokens_.size() &&
            tokens_[current_idx_ + 1].type == TokenType::LParen) {
            std::string hint_msg = "Method calls on holes require a dot: write `";
            hint_msg += tok.lexeme;
            hint_msg += '.';
            hint_msg += current().lexeme;
            hint_msg += "(...)` instead of `";
            hint_msg += tok.lexeme;
            hint_msg += current().lexeme;
            hint_msg += "(...)`.";
            error_with_code(current(), "E108", hint_msg);
        }
        Token field_tok = advance();
        arena_[node].data = Node::HoleData{std::string(field_tok.lexeme)};
        return node;
    }

    // Dotted form: `@.field` / `%.field` — kept for back-compat. Under
    // `--strict` we suggest migrating to the dotless form (W201).
    // Method calls (`@.method()`) restore position and let postfix handling
    // pick up the dot.
    if (check(TokenType::Dot)) {
        std::size_t save_pos = current_idx_;

        advance();  // tentatively consume '.'

        if (check(TokenType::Identifier)) {
            Token field_tok = advance();  // tentatively consume identifier

            if (check(TokenType::LParen)) {
                // This is a method call: @.method() — restore and defer.
                current_idx_ = save_pos;
                arena_[node].data = Node::HoleData{std::nullopt};
            } else {
                arena_[node].data = Node::HoleData{std::string(field_tok.lexeme)};
                if (lint_strict_) {
                    std::string msg = "Dotted hole-field access is deprecated; write `";
                    msg += tok.lexeme;
                    msg += field_tok.lexeme;
                    msg += "` instead of `";
                    msg += tok.lexeme;
                    msg += '.';
                    msg += field_tok.lexeme;
                    msg += "`.";
                    warn(tok, "W201", msg);
                }
            }
        } else {
            error("Expected field name after hole '.' access");
            arena_[node].data = Node::HoleData{std::nullopt};
        }
    } else {
        arena_[node].data = Node::HoleData{std::nullopt};
    }

    return node;
}

NodeIndex Parser::parse_unary_not() {
    Token bang_tok = advance();  // consume '!'

    // Parse operand at Unary precedence (high, to bind tightly)
    NodeIndex operand = parse_precedence(Precedence::Unary);

    // Desugar to bnot(operand)
    NodeIndex node = make_node(NodeType::Call, bang_tok);
    arena_[node].data = Node::IdentifierData{"bnot"};

    // Add operand as argument
    NodeIndex arg = arena_.alloc(NodeType::Argument, arena_[operand].location);
    arena_[arg].data = Node::ArgumentData{std::nullopt};  // positional arg
    arena_.add_child(arg, operand);
    arena_.add_child(node, arg);

    return node;
}

NodeIndex Parser::parse_array() {
    Token bracket = advance();  // consume '['
    NodeIndex node = make_node(NodeType::ArrayLit, bracket);

    // Empty array
    if (check(TokenType::RBracket)) {
        advance();  // consume ']'
        return node;
    }

    // Parse elements (with optional ..spread)
    do {
        if (check(TokenType::DotDot)) {
            Token dot_tok = advance();  // consume '..'
            NodeIndex spread_expr = parse_expression();
            if (spread_expr != NULL_NODE) {
                NodeIndex wrapper = arena_.alloc(NodeType::Argument, dot_tok.location);
                arena_[wrapper].data = Node::ArgumentData{std::nullopt, spread_expr};
                arena_.add_child(node, wrapper);
            }
        } else {
            NodeIndex elem = parse_expression();
            if (elem != NULL_NODE) {
                arena_.add_child(node, elem);
            }
        }
    } while (match(TokenType::Comma));

    consume(TokenType::RBracket, "Expected ']' after array elements");
    return node;
}

bool Parser::loop_form_ahead() const {
    // The current token is the '('. Walk to its matching ')' and check
    // whether a '{' follows — that distinguishes `loop(N) { body }` from an
    // ordinary call to a user function that happens to be named `loop`.
    int depth = 0;
    for (std::size_t i = current_idx_; i < tokens_.size(); ++i) {
        TokenType t = tokens_[i].type;
        if (t == TokenType::LParen) {
            ++depth;
        } else if (t == TokenType::RParen) {
            --depth;
            if (depth == 0) {
                return i + 1 < tokens_.size() &&
                       tokens_[i + 1].type == TokenType::LBrace;
            }
        } else if (t == TokenType::Eof) {
            break;
        }
    }
    return false;
}

NodeIndex Parser::parse_loop_expr(const Token& loop_token) {
    // `loop` already consumed; current token is '('.
    NodeIndex node = make_node(NodeType::LoopExpr, loop_token);

    consume(TokenType::LParen, "Expected '(' after 'loop'");
    NodeIndex count = parse_expression();
    if (count != NULL_NODE) {
        arena_.add_child(node, count);
    }
    consume(TokenType::RParen, "Expected ')' after loop count");

    // Body is a brace-delimited block expression; its value is the final
    // expression so it composes with `|>`.
    NodeIndex body = parse_block();
    if (body != NULL_NODE) {
        arena_.add_child(node, body);
    }
    return node;
}

NodeIndex Parser::parse_identifier_or_call() {
    Token name_tok = advance();

    // A `(` only extends this identifier into a call when it sits on the same
    // source line. The grammar is newline-insensitive and has no statement
    // terminator, so without this guard a bare trailing identifier swallows
    // the next line: `fn f(x) -> a * b` followed by `(g(1) + g(2)) |> out(@)`
    // would parse as `f`'s body = `b((g(1)+g(2)) |> out(@))`, merging the
    // whole program into the function body (and tripping false E240
    // recursion). A `(` on a later line begins a new parenthesized statement.
    const bool lparen_same_line =
        check(TokenType::LParen) &&
        current().location.line == name_tok.location.line;

    // Contextual keyword: loop(N) { body }. Only the `( ... ) {` shape is the
    // loop form — a bare `loop(x)` stays an ordinary identifier/call.
    if (name_tok.lexeme == "loop" && lparen_same_line && loop_form_ahead()) {
        return parse_loop_expr(name_tok);
    }

    // Check for function call
    if (lparen_same_line) {
        return parse_call(name_tok);
    }

    // Check for method call
    if (check(TokenType::Colon)) {
        // Could be named arg or method - but we're not in arg context
        // So this might be an error or future syntax
    }

    // Plain identifier
    NodeIndex node = make_node(NodeType::Identifier, name_tok);
    arena_[node].data = Node::IdentifierData{std::string(name_tok.lexeme)};
    return node;
}

NodeIndex Parser::parse_grouping() {
    advance();  // consume '('

    // Check for closure: () -> or (params) ->
    // We need to look ahead to see if this is a closure
    bool is_closure = false;

    // Empty parens followed by arrow is closure
    if (check(TokenType::RParen)) {
        std::size_t saved = current_idx_;
        advance();  // consume ')'
        if (check(TokenType::Arrow)) {
            current_idx_ = saved - 1;  // go back before '('
            advance();  // consume '('
            return parse_closure();
        }
        current_idx_ = saved;
    }
    // Check if it's identifier(s) / `{...}` destructure params (with optional
    // defaults and rest params) followed by ) ->
    else if (check(TokenType::Identifier) || check(TokenType::DotDotDot) ||
             check(TokenType::LBrace)) {
        std::size_t saved = current_idx_;
        bool looks_like_params = true;

        while (!is_at_end() && looks_like_params) {
            // A `{...}` destructure param — skip the balanced brace group.
            if (check(TokenType::LBrace)) {
                int brace_depth = 0;
                bool brace_closed = false;
                while (!is_at_end()) {
                    if (check(TokenType::LBrace)) {
                        brace_depth++;
                        advance();
                    } else if (check(TokenType::RBrace)) {
                        brace_depth--;
                        advance();
                        if (brace_depth == 0) { brace_closed = true; break; }
                    } else {
                        advance();
                    }
                }
                if (!brace_closed) {
                    looks_like_params = false;
                    break;
                }
                if (check(TokenType::Comma)) {
                    advance();
                    continue;
                } else if (check(TokenType::RParen)) {
                    advance();
                    if (check(TokenType::Arrow)) {
                        is_closure = true;
                    }
                    break;
                } else {
                    looks_like_params = false;
                    break;
                }
            }

            // Allow ... prefix for rest params
            if (check(TokenType::DotDotDot)) {
                advance();
            }

            if (!check(TokenType::Identifier)) {
                looks_like_params = false;
                break;
            }
            advance();

            // Skip optional default value: = <expression>
            if (check(TokenType::Equals)) {
                advance();  // consume '='
                // Skip an arbitrary expression by balancing parens/brackets/braces
                // and stopping at comma or closing paren at depth 0
                int depth = 0;
                bool valid = false;
                while (!is_at_end()) {
                    if (check(TokenType::LParen) || check(TokenType::LBracket) || check(TokenType::LBrace)) {
                        depth++;
                        advance();
                    } else if (check(TokenType::RParen) || check(TokenType::RBracket) || check(TokenType::RBrace)) {
                        if (depth == 0) {
                            valid = true;
                            break;  // This RParen closes the param list
                        }
                        depth--;
                        advance();
                    } else if (check(TokenType::Comma) && depth == 0) {
                        valid = true;
                        break;  // Next parameter
                    } else {
                        advance();
                    }
                }
                if (!valid) {
                    looks_like_params = false;
                    break;
                }
            }

            if (check(TokenType::Comma)) {
                advance();
            } else if (check(TokenType::RParen)) {
                advance();
                if (check(TokenType::Arrow)) {
                    is_closure = true;
                }
                break;
            } else {
                looks_like_params = false;
            }
        }

        current_idx_ = saved;

        if (is_closure) {
            current_idx_--;  // go back before '('
            advance();  // re-consume '('
            return parse_closure();
        }
    }

    // Not a closure, parse as grouped expression
    NodeIndex expr = parse_expression();
    consume(TokenType::RParen, "Expected ')' after expression");
    return expr;
}

// Closure parsing

NodeIndex Parser::parse_closure() {
    // Already consumed '('
    Token start_tok = previous();
    NodeIndex node = make_node(NodeType::Closure, start_tok);

    // Parse parameter list. Closures accept `{x, y}` destructure params just
    // like `fn` definitions (prd-poly-callback-event-record).
    std::vector<ParsedParam> params = parse_param_list(/*allow_destructure=*/true);

    consume(TokenType::RParen, "Expected ')' after parameters");
    consume(TokenType::Arrow, "Expected '->' after closure parameters");

    // Parse body
    NodeIndex body = parse_closure_body();
    if (body != NULL_NODE) {
        arena_.add_child(node, body);
    }

    // Store params as children before body
    // Use ClosureParamData for params with defaults/rest, IdentifierData for simple params
    if (!params.empty()) {
        NodeIndex first_param = NULL_NODE;
        NodeIndex prev_param = NULL_NODE;

        for (const auto& param : params) {
            NodeIndex param_node;
            if (param.is_destructure) {
                // `{x, y}` destructure param — its own AST node type, same as
                // the `fn` definition path.
                param_node = arena_.alloc(NodeType::DestructureParam,
                                          start_tok.location);
                arena_[param_node].data =
                    Node::DestructureParamData{param.destructure_fields};
            } else {
                param_node = arena_.alloc(NodeType::Identifier, start_tok.location);

                // PRD prd-parameter-type-annotations: an annotation alone
                // (without default / rest) also forces ClosureParamData so the
                // annotated_type survives into the AST.
                if (param.default_value.has_value() || param.default_string.has_value() ||
                    param.is_rest || param.default_node != NULL_NODE ||
                    param.annotated_type != ParamValueType::Any) {
                    // Parameter with default value, string default, rest flag,
                    // expression default, or type annotation.
                    arena_[param_node].data = Node::ClosureParamData{
                        param.name, param.default_value, param.default_string,
                        param.is_rest, param.annotated_type};
                } else {
                    // Simple parameter - use IdentifierData
                    arena_[param_node].data = Node::IdentifierData{param.name};
                }

                // Attach expression/numeric default as child of param node. String
                // defaults stay carried only in ClosureParamData (their node is
                // synthetic and routed through match string-dispatch instead).
                if (param.default_node != NULL_NODE && !param.default_string.has_value()) {
                    arena_[param_node].first_child = param.default_node;
                }
            }

            if (first_param == NULL_NODE) {
                first_param = param_node;
            } else {
                arena_[prev_param].next_sibling = param_node;
            }
            prev_param = param_node;
        }

        // Link params before body
        if (prev_param != NULL_NODE && body != NULL_NODE) {
            arena_[prev_param].next_sibling = body;
        }

        // Replace first_child
        arena_[node].first_child = first_param;
    }

    return node;
}

std::vector<ParsedParam> Parser::parse_param_list(bool allow_destructure) {
    std::vector<ParsedParam> params;

    if (check(TokenType::RParen)) {
        return params;  // Empty params
    }

    bool seen_default = false;
    std::size_t destr_param_idx = 0;

    // PRD prd-parameter-type-annotations-phase-2 §3.1: parse the optional
    // annotation after a parameter name. Returns Any when no annotation is
    // present. Emits E185 for an unknown type name (e.g. `: bogustype`) and
    // recovers by skipping one token.
    //
    // Token strategy (P2-Q12): `num`/`str` reuse TokenType::Number/String
    // (literal token types). The lexer keyword path produces these tokens
    // with `tok.value == std::monostate{}` and `tok.lexeme == "num"`/`"str"`,
    // so we disambiguate via the lexeme. `fn` reuses TokenType::Fn (the
    // declaration keyword) — contexts are disjoint, no conflict.
    auto parse_optional_annotation = [&]() -> ParamValueType {
        if (!match(TokenType::Colon)) {
            return ParamValueType::Any;
        }
        Token tok = advance();
        if (tok.type == TokenType::Evs)    return ParamValueType::Stream;
        if (tok.type == TokenType::Signal) return ParamValueType::Signal;
        if (tok.type == TokenType::Record) return ParamValueType::Record;
        if (tok.type == TokenType::Array)  return ParamValueType::Array;
        if (tok.type == TokenType::Fn)     return ParamValueType::Function;
        if (tok.type == TokenType::Number && tok.lexeme == "num")
                                           return ParamValueType::Number;
        if (tok.type == TokenType::String && tok.lexeme == "str")
                                           return ParamValueType::String;
        error_with_code(tok, "E185",
            "Unknown type name '" + std::string(tok.lexeme) +
            "' in parameter annotation; expected `evs`, `sig`/`signal`, "
            "`num`, `rec`, `arr`, `str`, or `fn`.");
        return ParamValueType::Any;
    };

    // Consume the tokens of an annotation that we are *rejecting* (destructure
    // / rest contexts). Keeps the parser from cascading "Expected ',' or ')'"
    // errors after E104. The caller has already verified `check(TokenType::Colon)`.
    auto skip_rejected_annotation = [&]() {
        advance();  // consume ':'
        if (check(TokenType::Evs) || check(TokenType::Signal) ||
            check(TokenType::Record) || check(TokenType::Array) ||
            check(TokenType::Fn) ||
            check(TokenType::Number) || check(TokenType::String) ||
            check(TokenType::Identifier)) {
            advance();
        }
    };

    do {
        // Phase 3b: function-parameter destructure `fn f({x, y [= default]})`.
        // Only enabled in fn-def context (allow_destructure=true). Closures
        // currently reject destructure slots — this scope decision is
        // deliberate; revisit if user demand emerges.
        if (check(TokenType::LBrace)) {
            if (!allow_destructure) {
                error("Destructuring parameters are only allowed in `fn` definitions");
                break;
            }
            std::vector<DestructureField> fields = parse_destructure_fields(true);
            ParsedParam dp;
            dp.name = "__destr_param_" + std::to_string(destr_param_idx++);
            dp.is_destructure = true;
            dp.destructure_fields = std::move(fields);
            // PRD §8.2: annotations on destructure params are out of scope
            // in Phase 1. Reject with E104, consume the tokens to avoid
            // cascading "Expected ',' or ')'" errors.
            if (check(TokenType::Colon)) {
                error_with_code(current(), "E104",
                    "Parameter type annotations are not allowed on destructure "
                    "params in Phase 1 of prd-parameter-type-annotations.");
                skip_rejected_annotation();
            }
            params.push_back(std::move(dp));
            continue;
        }

        // Check for variadic rest parameter: ...name
        bool is_rest = match(TokenType::DotDotDot);

        if (!check(TokenType::Identifier)) {
            error("Expected parameter name");
            break;
        }
        Token name_tok = advance();
        std::string name = std::string(name_tok.lexeme);

        std::optional<double> default_value;
        std::optional<std::string> default_string;

        if (is_rest) {
            // Rest parameter: no default allowed, must be last
            if (seen_default) {
                // Rest after default is OK - rest collects remaining args
            }
            // PRD §8.2: annotations on rest params are out of scope in Phase 1.
            if (check(TokenType::Colon)) {
                error_with_code(current(), "E104",
                    "Parameter type annotations are not allowed on rest params "
                    "(`...name`) in Phase 1 of prd-parameter-type-annotations.");
                skip_rejected_annotation();
            }
            ParsedParam rp;
            rp.name = std::move(name);
            rp.is_rest = true;
            params.push_back(std::move(rp));
            // Rest param must be last - break out of loop
            break;
        }

        // PRD §3.1: parse optional `name: type` annotation before the default.
        ParamValueType annotated = parse_optional_annotation();

        NodeIndex default_node_idx = NULL_NODE;
        if (match(TokenType::Equals)) {
            // Fast path: simple number literal (not followed by an operator)
            // Uses advance() instead of parse_expression() to avoid orphaning arena nodes.
            // The std::holds_alternative guard rejects the `num` keyword token
            // (PRD prd-parameter-type-annotations-phase-2 §4.4); such a token
            // falls through to the expression-default path where parse_number()
            // surfaces a clean E185.
            if (check(TokenType::Number) &&
                std::holds_alternative<NumericValue>(tokens_[current_idx_].value) &&
                current_idx_ + 1 < tokens_.size() &&
                (tokens_[current_idx_ + 1].type == TokenType::Comma ||
                 tokens_[current_idx_ + 1].type == TokenType::RParen)) {
                Token num_tok = advance();
                auto& num_val = std::get<NumericValue>(num_tok.value);
                default_value = num_val.value;
                seen_default = true;
                // Also materialise a NumberLit node so the default is
                // discoverable as a compile-time literal (param_literals_):
                // lets `match(param)` const-fold and builtins like linspace
                // see the literal when the param is left at its default.
                default_node_idx = arena_.alloc(NodeType::NumberLit, num_tok.location);
                arena_[default_node_idx].data =
                    Node::NumberData{num_val.value, num_val.is_integer};
            }
            // Fast path: simple string literal (not followed by an operator).
            // Same guard as above for the `str` keyword token.
            else if (check(TokenType::String) &&
                     std::holds_alternative<std::string>(tokens_[current_idx_].value) &&
                     current_idx_ + 1 < tokens_.size() &&
                     (tokens_[current_idx_ + 1].type == TokenType::Comma ||
                      tokens_[current_idx_ + 1].type == TokenType::RParen)) {
                Token str_tok = advance();
                default_string = str_tok.as_string();
                seen_default = true;
                default_node_idx = arena_.alloc(NodeType::StringLit, str_tok.location);
                arena_[default_node_idx].data = Node::StringData{*default_string};
            }
            // Expression default: parse full expression (e.g. 440 * 2, mtof(60))
            else {
                default_node_idx = parse_expression();
                seen_default = true;
            }
        } else if (seen_default) {
            // Required param after optional - error
            error("Required parameter cannot follow optional parameter");
            break;
        }

        ParsedParam pp;
        pp.name = std::move(name);
        pp.default_value = default_value;
        pp.default_string = default_string;
        pp.default_node = default_node_idx;
        pp.is_rest = false;
        pp.annotated_type = annotated;
        params.push_back(std::move(pp));
    } while (match(TokenType::Comma));

    return params;
}

NodeIndex Parser::parse_closure_body() {
    // Closure body is greedy - captures everything including pipes.
    // Can be a block `{ ... }`, a record literal `{ k: v }`, or an
    // expression (pipe_expr).
    //
    // A `{ … }` body defaults to a block, but a record-literal body
    // (`(e) -> {note: e.note + 7}`, `(e) -> {}`) is disambiguated by shape:
    // an empty `{}`, a `{..spread}`, or a leading `{ ident :` field opener
    // is a record literal — none of those can begin a block statement.
    // Used by event_map / event_filter closures (PRD prd-runtime-event-
    // transforms), which return a field-overlay record.
    if (check(TokenType::LBrace)) {
        const std::size_t j = current_idx_ + 1;  // first token after `{`
        bool is_record = false;
        if (j < tokens_.size()) {
            const TokenType t1 = tokens_[j].type;
            if (t1 == TokenType::RBrace || t1 == TokenType::DotDot) {
                is_record = true;  // `{}` or `{..spread}`
            } else if (t1 == TokenType::Identifier &&
                       j + 1 < tokens_.size() &&
                       tokens_[j + 1].type == TokenType::Colon) {
                is_record = true;  // `{ field: … }`
            }
        }
        return is_record ? parse_record_literal() : parse_block();
    }

    // Parse pipe expression (greedy)
    return parse_expression();
}

NodeIndex Parser::parse_block() {
    Token brace = advance();  // consume '{'
    NodeIndex node = make_node(NodeType::Block, brace);

    while (!check(TokenType::RBrace) && !is_at_end()) {
        NodeIndex stmt = parse_statement();
        if (stmt != NULL_NODE) {
            arena_.add_child(node, stmt);
        }
        if (panic_mode_) {
            synchronize();
        }
    }

    consume(TokenType::RBrace, "Expected '}' after block");
    return node;
}

// Binary operator parsing

NodeIndex Parser::parse_binary(NodeIndex left, const Token& op) {
    // Determine the function name for the operator
    const char* func_name = nullptr;
    switch (op.type) {
        case TokenType::Plus:         func_name = "add"; break;
        case TokenType::Minus:        func_name = "sub"; break;
        case TokenType::Star:         func_name = "mul"; break;
        case TokenType::Slash:        func_name = "div"; break;
        case TokenType::Caret:        func_name = "pow"; break;
        case TokenType::OrOr:         func_name = "bor"; break;
        case TokenType::AndAnd:       func_name = "band"; break;
        case TokenType::EqualEqual:   func_name = "eq"; break;
        case TokenType::BangEqual:    func_name = "neq"; break;
        case TokenType::Less:         func_name = "lt"; break;
        case TokenType::Greater:      func_name = "gt"; break;
        case TokenType::LessEqual:    func_name = "lte"; break;
        case TokenType::GreaterEqual: func_name = "gte"; break;
        default:
            error("Unknown binary operator");
            return left;
    }

    // Get precedence for right-hand side
    // For left-associative operators, use same precedence
    // For right-associative (^), use lower precedence
    Precedence next_prec = get_precedence(op.type);
    if (op.type == TokenType::Caret) {
        // Power is right-associative
        next_prec = static_cast<Precedence>(static_cast<int>(next_prec));
    } else {
        // Left-associative: increment to bind tighter on right
        next_prec = static_cast<Precedence>(static_cast<int>(next_prec) + 1);
    }

    NodeIndex right = parse_precedence(next_prec);

    // Create binary op node desugared to Call
    NodeIndex node = make_node(NodeType::Call, op);

    // Set function name based on operator
    arena_[node].data = Node::IdentifierData{func_name};

    // Add left and right as arguments
    NodeIndex left_arg = arena_.alloc(NodeType::Argument, arena_[left].location);
    arena_[left_arg].data = Node::ArgumentData{std::nullopt};  // positional arg
    arena_.add_child(left_arg, left);
    arena_.add_child(node, left_arg);

    if (right != NULL_NODE) {
        NodeIndex right_arg = arena_.alloc(NodeType::Argument, arena_[right].location);
        arena_[right_arg].data = Node::ArgumentData{std::nullopt};  // positional arg
        arena_.add_child(right_arg, right);
        arena_.add_child(node, right_arg);
    }

    return node;
}

// Pipe parsing
// The 'as' binding is handled in parse_precedence before the pipe operator

NodeIndex Parser::parse_pipe(NodeIndex left) {
    Token pipe_tok = previous();
    NodeIndex node = make_node(NodeType::Pipe, pipe_tok);

    // Parse right side at same precedence (pipe is left-associative within itself)
    // But we need to parse at Addition level to not capture more pipes
    NodeIndex right = parse_precedence(Precedence::Addition);

    arena_.add_child(node, left);
    if (right != NULL_NODE) {
        arena_.add_child(node, right);
    }

    return node;
}

// Method call or field access parsing (for x.method() or x.field syntax)
NodeIndex Parser::parse_method_call(NodeIndex left) {
    Token dot_tok = previous();

    if (!check(TokenType::Identifier)) {
        error("Expected method or field name after '.'");
        return left;
    }

    Token name_tok = advance();

    // Check if this is a method call (has parens) or field access (no parens)
    if (check(TokenType::LParen)) {
        // Method call: x.method(args)
        NodeIndex node = make_node(NodeType::MethodCall, dot_tok);
        arena_[node].data = Node::IdentifierData{std::string(name_tok.lexeme)};

        // Add receiver as first child
        arena_.add_child(node, left);

        // Parse arguments
        advance();  // consume '('

        if (!check(TokenType::RParen)) {
            auto args = parse_argument_list();
            for (NodeIndex arg : args) {
                arena_.add_child(node, arg);
            }
        }

        consume(TokenType::RParen, "Expected ')' after arguments");

        return node;
    } else {
        // Field access: x.field
        return parse_field_access_with_name(left, name_tok);
    }
}

// Field access parsing (for x.field syntax, name already consumed)
NodeIndex Parser::parse_field_access_with_name(NodeIndex left, const Token& name_tok) {
    NodeIndex node = make_node(NodeType::FieldAccess, name_tok);
    arena_[node].data = Node::FieldAccessData{std::string(name_tok.lexeme)};

    // Add the expression being accessed as first child
    arena_.add_child(node, left);

    return node;
}

// Field access helper (for parse_infix if needed)
NodeIndex Parser::parse_field_access(NodeIndex left) {
    Token name_tok = advance();  // consume field name
    return parse_field_access_with_name(left, name_tok);
}

// Index parsing (for arr[i] syntax)
NodeIndex Parser::parse_index(NodeIndex left) {
    Token bracket = advance();  // consume '['
    NodeIndex node = make_node(NodeType::Index, bracket);

    // Add the array/receiver as first child
    arena_.add_child(node, left);

    // Parse the index expression
    NodeIndex index_expr = parse_expression();
    if (index_expr != NULL_NODE) {
        arena_.add_child(node, index_expr);
    }

    consume(TokenType::RBracket, "Expected ']' after index");

    return node;
}

// Function call parsing

NodeIndex Parser::parse_call(const Token& name_token) {
    NodeIndex node = make_node(NodeType::Call, name_token);
    arena_[node].data = Node::IdentifierData{std::string(name_token.lexeme)};

    consume(TokenType::LParen, "Expected '(' after function name");

    if (!check(TokenType::RParen)) {
        auto args = parse_argument_list();
        for (NodeIndex arg : args) {
            arena_.add_child(node, arg);
        }
    }

    consume(TokenType::RParen, "Expected ')' after arguments");
    return node;
}

std::vector<NodeIndex> Parser::parse_argument_list() {
    std::vector<NodeIndex> args;

    do {
        NodeIndex arg = parse_argument();
        if (arg != NULL_NODE) {
            args.push_back(arg);
        }
    } while (match(TokenType::Comma));

    return args;
}

NodeIndex Parser::parse_argument() {
    Token start = current();
    NodeIndex node = make_node(NodeType::Argument, start);

    // Check for spread argument: ..expr
    if (check(TokenType::DotDot)) {
        advance();  // consume '..'
        NodeIndex spread_expr = parse_expression();
        arena_[node].data = Node::ArgumentData{std::nullopt, spread_expr};
        return node;
    }

    // Check for named argument: identifier ':'
    if (check(TokenType::Identifier)) {
        std::size_t saved = current_idx_;
        Token name = advance();

        if (check(TokenType::Colon)) {
            advance();  // consume ':'
            arena_[node].data = Node::ArgumentData{std::string(name.lexeme), NULL_NODE};
            NodeIndex value = parse_expression();
            if (value != NULL_NODE) {
                arena_.add_child(node, value);
            }
            return node;
        }

        // Not a named arg, restore
        current_idx_ = saved;
    }

    // Positional argument
    arena_[node].data = Node::ArgumentData{std::nullopt, NULL_NODE};
    NodeIndex value = parse_expression();
    if (value != NULL_NODE) {
        arena_.add_child(node, value);
    }
    return node;
}

// Mini-notation literal parsing

NodeIndex Parser::parse_mini_literal() {
    Token kw_tok = advance();

    bool is_timeline = (kw_tok.type == TokenType::Timeline);

    // Map token type to MiniParseMode for the prefix forms.
    MiniParseMode mode = MiniParseMode::Note;
    std::string mode_marker;
    switch (kw_tok.type) {
        case TokenType::Timeline:  mode = MiniParseMode::Curve;  mode_marker = "timeline"; break;
        case TokenType::ValuePat:  mode = MiniParseMode::Value;  mode_marker = "value";  break;
        case TokenType::NotePat:   mode = MiniParseMode::Note;   mode_marker = "note";   break;
        case TokenType::SamplePat: mode = MiniParseMode::Sample; mode_marker = "sample"; break;
        case TokenType::ChordPat:  mode = MiniParseMode::Chord;  mode_marker = "chord";  break;
        default:
            error("Expected pattern prefix");
            return NULL_NODE;
    }

    NodeIndex node = make_node(NodeType::MiniLiteral, kw_tok);

    // First argument: the mini-notation string. Guard against the `str` keyword
    // token (PRD prd-parameter-type-annotations-phase-2 §4.4).
    if (!check(TokenType::String) ||
        !std::holds_alternative<std::string>(tokens_[current_idx_].value)) {
        error("Expected string for mini-notation pattern");
        return node;
    }

    Token pattern_tok = advance();
    const std::string& pattern_str = pattern_tok.as_string();

    // Adjust location to point to content start (skip opening quote)
    // This ensures source offsets in mini-notation AST are relative to content
    SourceLocation content_loc = pattern_tok.location;
    content_loc.offset += 1;
    content_loc.column += 1;
    content_loc.length = pattern_str.length();  // Content length without quotes

    auto [pattern_ast, mini_diags] = parse_mini(pattern_str, arena_, content_loc, mode);

    // Add mini-notation diagnostics to our diagnostics
    for (auto& diag : mini_diags) {
        diag.filename = filename_;
        diagnostics_.push_back(std::move(diag));
    }

    // Add the parsed pattern as a child
    if (pattern_ast != NULL_NODE) {
        arena_.add_child(node, pattern_ast);
    }

    // Tag the MiniLiteral so codegen knows the parse mode. Every remaining
    // token type has an explicit mode marker.
    arena_[node].data = Node::StringData{.value = mode_marker};

    return node;
}

// Match expression parsing
// Two forms supported:
//   match(expr) { pattern: body, pattern && guard: body, ... }  -- scrutinee form
//   match { guard: body, guard: body, ... }                     -- guard-only form
NodeIndex Parser::parse_match_expr() {
    Token match_tok = advance();  // consume 'match' token

    NodeIndex node = make_node(NodeType::MatchExpr, match_tok);
    bool has_scrutinee = false;

    // Detect form: match(expr) vs match { }
    if (check(TokenType::LParen)) {
        has_scrutinee = true;
        advance();  // consume '('

        // Parse the scrutinee expression
        NodeIndex scrutinee = parse_expression();
        if (scrutinee == NULL_NODE) {
            error("Expected expression in match");
            return NULL_NODE;
        }

        arena_.add_child(node, scrutinee);
        consume(TokenType::RParen, "Expected ')' after match expression");
    }

    arena_[node].data = Node::MatchExprData{has_scrutinee};
    consume(TokenType::LBrace, "Expected '{' after match");

    // Parse match arms
    while (!check(TokenType::RBrace) && !is_at_end()) {
        Token arm_tok = current();

        NodeIndex pattern = NULL_NODE;
        NodeIndex guard = NULL_NODE;
        bool is_wildcard = false;
        bool has_guard = false;

        bool is_range = false;
        double range_low = 0.0;
        double range_high = 0.0;
        bool is_destructure = false;
        std::vector<std::string> destructure_fields;

        if (has_scrutinee) {
            // Scrutinee form: parse pattern, then optional && guard
            // Pattern: string, number, -number, number..number, bool, {field,...}, or _ (wildcard)
            if (check(TokenType::LBrace)) {
                // Destructuring pattern: { ident, ident, ... }
                // Defaults (`{x = 1}:`) are deferred — `allow_defaults=false`.
                is_destructure = true;
                std::vector<DestructureField> destr_arm_fields =
                    parse_destructure_fields(false);
                destructure_fields.reserve(destr_arm_fields.size());
                for (auto& f : destr_arm_fields) {
                    destructure_fields.push_back(std::move(f.name));
                }
                // Create a placeholder pattern node
                pattern = make_node(NodeType::Identifier, arm_tok);
                arena_[pattern].data = Node::IdentifierData{"_destructure"};
            } else if (check(TokenType::String) &&
                       std::holds_alternative<std::string>(tokens_[current_idx_].value)) {
                pattern = parse_string();
            } else if ((check(TokenType::Number) &&
                        std::holds_alternative<NumericValue>(tokens_[current_idx_].value)) ||
                       check(TokenType::Minus)) {
                // Parse optionally-negative number literal. The holds_alternative
                // guard rejects the `num` keyword token (PRD prd-parameter-type-annotations-phase-2 §4.4).
                bool neg = check(TokenType::Minus);
                if (neg) advance();  // consume '-'
                double num_val = current().as_number();
                pattern = parse_number();
                if (neg) {
                    num_val = -num_val;
                    arena_[pattern].data = Node::NumberData{num_val, false};
                }

                // Check for range pattern: number..number
                if (check(TokenType::DotDot)) {
                    advance();  // consume '..'
                    is_range = true;
                    range_low = num_val;

                    // Parse upper bound (optionally negative)
                    bool neg_high = check(TokenType::Minus);
                    if (neg_high) advance();
                    double high_val = current().as_number();
                    parse_number();  // consume upper bound (node discarded)
                    if (neg_high) high_val = -high_val;
                    range_high = high_val;
                }
            } else if (check(TokenType::True) || check(TokenType::False)) {
                pattern = parse_bool();
            } else if (check(TokenType::Underscore)) {
                advance();  // consume '_'
                is_wildcard = true;
                // Create an Identifier node with "_" as a placeholder
                pattern = make_node(NodeType::Identifier, arm_tok);
                arena_[pattern].data = Node::IdentifierData{"_"};
            } else {
                error("Expected pattern (string, number, bool, or '_')");
                synchronize();
                continue;
            }

            // Check for optional guard: pattern && guard
            if (check(TokenType::AndAnd)) {
                advance();  // consume '&&'
                has_guard = true;
                // Parse guard at higher precedence than && to avoid consuming subsequent &&
                guard = parse_precedence(Precedence::Or);
                if (guard == NULL_NODE) {
                    error("Expected guard expression after '&&'");
                }
            }
        } else {
            // Guard-only form: parse condition expression directly
            // Wildcard is just '_'
            if (check(TokenType::Underscore)) {
                advance();  // consume '_'
                is_wildcard = true;
                pattern = make_node(NodeType::Identifier, arm_tok);
                arena_[pattern].data = Node::IdentifierData{"_"};
            } else {
                // The "pattern" is actually the guard expression
                has_guard = true;
                guard = parse_precedence(Precedence::Or);
                if (guard == NULL_NODE) {
                    error("Expected condition expression in guard-only match");
                    synchronize();
                    continue;
                }
                // Create a placeholder pattern (true) since this is guard-only
                pattern = make_node(NodeType::BoolLit, arm_tok);
                arena_[pattern].data = Node::BoolData{true};
            }
        }

        consume(TokenType::Colon, "Expected ':' after pattern/guard");

        // Parse arm body - can be a block or an expression
        NodeIndex body = NULL_NODE;
        if (check(TokenType::LBrace)) {
            body = parse_block();
        } else {
            body = parse_expression();
        }

        // Create MatchArm node
        NodeIndex arm = make_node(NodeType::MatchArm, arm_tok);
        arena_[arm].data = Node::MatchArmData{is_wildcard, has_guard, guard, is_range, range_low, range_high, is_destructure, std::move(destructure_fields)};
        arena_.add_child(arm, pattern);
        if (body != NULL_NODE) {
            arena_.add_child(arm, body);
        }

        arena_.add_child(node, arm);

        // Allow optional comma between arms (but not required)
        match(TokenType::Comma);
    }

    consume(TokenType::RBrace, "Expected '}' after match arms");

    return node;
}

// Const variable declaration: const x = expr
NodeIndex Parser::parse_const_decl(const Token& name_token) {
    if (is_reserved_state_name(name_token.lexeme)) {
        error_at(name_token,
                 "'" + std::string(name_token.lexeme) +
                 "' is a reserved built-in identifier and cannot be redefined");
    }
    consume(TokenType::Equals, "Expected '=' after const identifier");

    NodeIndex node = make_node(NodeType::ConstDecl, name_token);
    arena_[node].data = Node::IdentifierData{std::string(name_token.lexeme)};

    NodeIndex value = parse_expression();
    if (value != NULL_NODE) {
        arena_.add_child(node, value);
    }

    return node;
}

// Function definition parsing
// fn name(params) -> body
// fn name(params) -> { block }
NodeIndex Parser::parse_fn_def(bool is_const, bool is_inline) {
    Token fn_tok = previous();  // 'fn' was already consumed

    if (check(TokenType::Underscore)) {
        error("Cannot use '_' as a function name");
        return NULL_NODE;
    }
    if (!check(TokenType::Identifier)) {
        error("Expected function name after 'fn'");
        return NULL_NODE;
    }

    Token name_tok = advance();

    if (is_reserved_state_name(name_tok.lexeme)) {
        error_at(name_tok,
                 "'" + std::string(name_tok.lexeme) +
                 "' is a reserved built-in identifier and cannot be redefined");
    }

    consume(TokenType::LParen, "Expected '(' after function name");

    // Parse parameter list (reuse closure param parsing).
    // Pass allow_destructure=true so `fn f({x, y})` is accepted (Phase 3b).
    std::vector<ParsedParam> params = parse_param_list(true);

    consume(TokenType::RParen, "Expected ')' after parameters");
    consume(TokenType::Arrow, "Expected '->' after function parameters");

    // Parse body
    NodeIndex body = NULL_NODE;
    if (check(TokenType::LBrace)) {
        body = parse_block();
    } else {
        body = parse_expression();
    }

    // Check if last param is rest
    bool has_rest = !params.empty() && params.back().is_rest;

    // Create FunctionDef node
    NodeIndex node = make_node(NodeType::FunctionDef, fn_tok);
    arena_[node].data = Node::FunctionDefData{
        std::string(name_tok.lexeme),
        params.size(),
        has_rest,
        is_const,
        is_inline
    };

    // Add parameters as Identifier children (using ClosureParamData for those with defaults/rest).
    // Destructure params (`fn f({x, y [= default]})`) get their own AST node type.
    for (const auto& param : params) {
        if (param.is_destructure) {
            NodeIndex dp_node = arena_.alloc(NodeType::DestructureParam, name_tok.location);
            arena_[dp_node].data = Node::DestructureParamData{param.destructure_fields};
            arena_.add_child(node, dp_node);
            continue;
        }

        NodeIndex param_node = arena_.alloc(NodeType::Identifier, name_tok.location);

        // PRD prd-parameter-type-annotations: promote to ClosureParamData so
        // annotated_type survives into the AST even for simple `name: stream`
        // parameters that have no default / rest flag.
        if (param.default_value.has_value() || param.default_string.has_value() ||
            param.is_rest || param.default_node != NULL_NODE ||
            param.annotated_type != ParamValueType::Any) {
            arena_[param_node].data = Node::ClosureParamData{
                param.name, param.default_value, param.default_string,
                param.is_rest, param.annotated_type};
        } else {
            arena_[param_node].data = Node::IdentifierData{param.name};
        }

        // Attach expression/numeric default as child of param node. String
        // defaults stay carried only in ClosureParamData.
        if (param.default_node != NULL_NODE && !param.default_string.has_value()) {
            arena_[param_node].first_child = param.default_node;
        }

        arena_.add_child(node, param_node);
    }

    // Add body as last child
    if (body != NULL_NODE) {
        arena_.add_child(node, body);
    }

    return node;
}

// Directive parsing: $name(args)
NodeIndex Parser::parse_directive() {
    Token dir_tok = advance();  // consume Directive token
    NodeIndex node = make_node(NodeType::Directive, dir_tok);

    // Get directive name from token value
    const std::string& dir_name = dir_tok.as_string();
    arena_[node].data = Node::DirectiveData{dir_name};

    // Parse arguments if present
    if (match(TokenType::LParen)) {
        if (!check(TokenType::RParen)) {
            // Parse arguments (as children)
            do {
                NodeIndex arg = parse_expression();
                if (arg != NULL_NODE) {
                    arena_.add_child(node, arg);
                }
            } while (match(TokenType::Comma));
        }
        consume(TokenType::RParen, "Expected ')' after directive arguments");
    }

    return node;
}

// Record literal parsing: {field: value, ...} or {field, ...} or {..spread, field: value}
NodeIndex Parser::parse_record_literal() {
    Token brace_tok = advance();  // consume '{'
    NodeIndex node = make_node(NodeType::RecordLit, brace_tok);

    // Empty record
    if (check(TokenType::RBrace)) {
        advance();  // consume '}'
        return node;
    }

    // Check for spread: {..expr, ...}
    NodeIndex spread_source = NULL_NODE;
    if (check(TokenType::DotDot)) {
        advance();  // consume '..'
        spread_source = parse_expression();
        // Consume comma after spread if present (before explicit fields)
        match(TokenType::Comma);
    }

    // Store spread info on the RecordLit node
    arena_[node].data = Node::RecordLitData{spread_source};

    // Parse fields (if any remain after spread)
    std::set<std::string> seen_fields;  // Track duplicates

    if (!check(TokenType::RBrace)) {
        do {
            if (!check(TokenType::Identifier)) {
                error("Expected field name in record literal");
                break;
            }

            Token field_tok = advance();
            std::string field_name = std::string(field_tok.lexeme);

            // Check for duplicate field names
            if (seen_fields.count(field_name)) {
                error_at(field_tok, "Duplicate field '" + field_name + "' in record literal");
            }
            seen_fields.insert(field_name);

            // Create a node to hold the field (using Argument node type with RecordFieldData)
            NodeIndex field_node = arena_.alloc(NodeType::Argument, field_tok.location);

            // Check for shorthand {field} vs full {field: value}
            if (check(TokenType::Colon)) {
                advance();  // consume ':'

                // Full form: field: value
                arena_[field_node].data = Node::RecordFieldData{field_name, false};

                // Parse the value expression
                NodeIndex value = parse_expression();
                if (value != NULL_NODE) {
                    arena_.add_child(field_node, value);
                }
            } else {
                // Shorthand form: {field} - value is the identifier with same name
                arena_[field_node].data = Node::RecordFieldData{field_name, true};

                // Create identifier node for the value
                NodeIndex ident = arena_.alloc(NodeType::Identifier, field_tok.location);
                arena_[ident].data = Node::IdentifierData{field_name};
                arena_.add_child(field_node, ident);
            }

            arena_.add_child(node, field_node);

        } while (match(TokenType::Comma) && !check(TokenType::RBrace));
    }

    consume(TokenType::RBrace, "Expected '}' after record fields");
    return node;
}

// Shared destructure-pattern field-list parser.
// Caller has just consumed '{'. Reads `Ident (, Ident)*`, consumes '}',
// emits E188 on duplicate names. Used by pipe-binding `as {x, y}`,
// match-arm `{x, y}` patterns, and statement-level `{x, y} = expr`.
std::vector<DestructureField> Parser::parse_destructure_fields(bool allow_defaults) {
    consume(TokenType::LBrace, "Expected '{' to begin destructuring pattern");

    std::vector<DestructureField> fields;
    std::set<std::string> seen;

    while (!check(TokenType::RBrace) && !is_at_end()) {
        if (!check(TokenType::Identifier)) {
            error("Expected field name in destructuring pattern");
            break;
        }
        Token name_tok = current();
        std::string name = std::string(name_tok.lexeme);
        advance();

        NodeIndex default_idx = NULL_NODE;
        if (check(TokenType::Equals)) {
            if (!allow_defaults) {
                // Defaults in pipe-binding (`as {x = 1}`) and match-arm
                // (`{x = 1}:`) destructures are deferred — this PRD only
                // ships them in statement-level and fn-param contexts.
                error("Default values are not allowed in this destructure context");
                // Recover: skip the default expression so we can keep parsing.
                advance();  // consume `=`
                NodeIndex throwaway = parse_expression();
                (void)throwaway;
            } else {
                advance();  // consume `=`
                default_idx = parse_expression();
            }
        }

        if (seen.count(name)) {
            // Push E188 directly so we don't trip panic_mode and keep
            // parsing the rest of the field list for recovery.
            diagnostics_.push_back(Diagnostic{
                .severity = Severity::Error,
                .code = "E188",
                .message = "Duplicate field '" + name + "' in destructure pattern",
                .filename = filename_,
                .location = name_tok.location
            });
        } else {
            seen.insert(name);
            fields.push_back(DestructureField{std::move(name), default_idx});
        }

        if (!check(TokenType::RBrace)) {
            consume(TokenType::Comma, "Expected ',' between destructuring fields");
        }
    }
    consume(TokenType::RBrace, "Expected '}' after destructuring fields");
    return fields;
}

// Convenience function

std::pair<Ast, std::vector<Diagnostic>>
parse(std::vector<Token> tokens, std::string_view source,
      std::string_view filename, bool lint_strict) {
    Parser parser(std::move(tokens), source, filename);
    parser.set_lint_strict(lint_strict);
    Ast ast = parser.parse();
    return {std::move(ast), parser.diagnostics()};
}

} // namespace akkado
