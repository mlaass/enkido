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
// confusing error far from the cause.
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

void Parser::synchronize() {
    panic_mode_ = false;

    while (!is_at_end()) {
        // Synchronize at statement boundaries
        switch (current().type) {
            case TokenType::Pat:
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

    // Expect string literal (path)
    if (!check(TokenType::String)) {
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

    // Check for statement-level destructure assignment: { Ident (, Ident)* } = expr
    // Disambiguated from a record-literal expression-statement by the absence
    // of `:` after the first identifier and the presence of `} =` after the field list.
    if (check(TokenType::LBrace)) {
        std::size_t i = current_idx_ + 1;
        bool is_destructure_stmt = false;
        if (i < tokens_.size() && tokens_[i].type == TokenType::Identifier) {
            ++i;
            // `{x:` → record literal, not destructure
            if (i < tokens_.size() && tokens_[i].type != TokenType::Colon) {
                while (i < tokens_.size() && tokens_[i].type == TokenType::Comma) {
                    ++i;
                    if (i < tokens_.size() && tokens_[i].type == TokenType::Identifier) {
                        ++i;
                    } else {
                        break;
                    }
                }
                if (i < tokens_.size() && tokens_[i].type == TokenType::RBrace
                    && i + 1 < tokens_.size() && tokens_[i + 1].type == TokenType::Equals) {
                    is_destructure_stmt = true;
                }
            }
        }

        if (is_destructure_stmt) {
            Token brace_tok = advance();  // consume '{'
            std::vector<std::string> fields = parse_destructure_fields();
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

    // Otherwise it's an expression statement
    return parse_expression();
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
                // as {field1, field2, ...} — destructuring binding
                advance();  // consume '{'
                std::vector<std::string> destr_fields = parse_destructure_fields();

                std::string temp_name = "__destr_" + std::to_string(destr_counter_++);
                NodeIndex binding = make_node(NodeType::PipeBinding, as_tok);
                arena_[binding].data = Node::PipeBindingData{temp_name, std::move(destr_fields)};
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
        case TokenType::ChordLit:
            return parse_chord();
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
        case TokenType::Pat:
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
        case TokenType::Underscore: {
            // Placeholder for partial application: add(3, _)
            Token tok = advance();
            NodeIndex node = make_node(NodeType::Identifier, tok);
            arena_[node].data = Node::IdentifierData{"_"};
            return node;
        }
        default:
            error("Expected expression");
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

NodeIndex Parser::parse_chord() {
    Token tok = advance();
    NodeIndex node = make_node(NodeType::ChordLit, tok);
    const auto& chord = tok.as_chord();
    arena_[node].data = Node::ChordData{chord.root_midi, chord.intervals};
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
    arena_[node].data = Node::StringData{tok.as_string()};
    return node;
}

NodeIndex Parser::parse_hole() {
    Token tok = advance();  // consume '%' or '@'
    NodeIndex node = make_node(NodeType::Hole, tok);

    // Check for field access: %.field
    // But NOT if followed by '(' which indicates a method call: %.method()
    if (check(TokenType::Dot)) {
        // Peek ahead: if it's identifier followed by '(', it's a method call
        // In that case, don't consume the dot - let postfix handling do it
        std::size_t save_pos = current_idx_;

        advance();  // tentatively consume '.'

        if (check(TokenType::Identifier)) {
            Token field_tok = advance();  // tentatively consume identifier

            if (check(TokenType::LParen)) {
                // This is a method call: %.method()
                // Restore position and let postfix handling deal with it
                current_idx_ = save_pos;
                arena_[node].data = Node::HoleData{std::nullopt};
            } else {
                // This is field access: %.field
                arena_[node].data = Node::HoleData{std::string(field_tok.lexeme)};
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

NodeIndex Parser::parse_identifier_or_call() {
    Token name_tok = advance();

    // Check for function call
    if (check(TokenType::LParen)) {
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
    // Check if it's identifier(s) (with optional defaults and rest params) followed by ) ->
    else if (check(TokenType::Identifier) || check(TokenType::DotDotDot)) {
        std::size_t saved = current_idx_;
        bool looks_like_params = true;

        while (!is_at_end() && looks_like_params) {
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

    // Parse parameter list
    std::vector<ParsedParam> params = parse_param_list();

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
            NodeIndex param_node = arena_.alloc(NodeType::Identifier, start_tok.location);

            if (param.default_value.has_value() || param.default_string.has_value() ||
                param.is_rest || param.default_node != NULL_NODE) {
                // Parameter with default value, string default, rest flag, or expression default
                arena_[param_node].data = Node::ClosureParamData{
                    param.name, param.default_value, param.default_string, param.is_rest};
            } else {
                // Simple parameter - use IdentifierData
                arena_[param_node].data = Node::IdentifierData{param.name};
            }

            // Attach expression default as child of param node
            if (param.default_node != NULL_NODE && !param.default_value.has_value() &&
                !param.default_string.has_value()) {
                arena_[param_node].first_child = param.default_node;
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

std::vector<ParsedParam> Parser::parse_param_list() {
    std::vector<ParsedParam> params;

    if (check(TokenType::RParen)) {
        return params;  // Empty params
    }

    bool seen_default = false;

    do {
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
            params.push_back(ParsedParam{std::move(name), std::nullopt, std::nullopt, NULL_NODE, true});
            // Rest param must be last - break out of loop
            break;
        }

        NodeIndex default_node_idx = NULL_NODE;
        if (match(TokenType::Equals)) {
            // Fast path: simple number literal (not followed by an operator)
            // Uses advance() instead of parse_expression() to avoid orphaning arena nodes
            if (check(TokenType::Number) &&
                current_idx_ + 1 < tokens_.size() &&
                (tokens_[current_idx_ + 1].type == TokenType::Comma ||
                 tokens_[current_idx_ + 1].type == TokenType::RParen)) {
                Token num_tok = advance();
                default_value = std::get<NumericValue>(num_tok.value).value;
                seen_default = true;
            }
            // Fast path: simple string literal (not followed by an operator)
            else if (check(TokenType::String) &&
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

        params.push_back(ParsedParam{std::move(name), default_value, default_string, default_node_idx, false});
    } while (match(TokenType::Comma));

    return params;
}

NodeIndex Parser::parse_closure_body() {
    // Closure body is greedy - captures everything including pipes
    // Can be a block { ... } or an expression (pipe_expr)
    if (check(TokenType::LBrace)) {
        return parse_block();
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
    bool is_pat_call = (kw_tok.type == TokenType::Pat);

    // Map token type to MiniParseMode for the prefix forms.
    MiniParseMode mode = MiniParseMode::Auto;
    std::string mode_marker;
    switch (kw_tok.type) {
        case TokenType::Pat:       mode = MiniParseMode::Auto;   mode_marker = "pat";    break;
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

    // Only `pat()` accepts the function-call-with-closure form. The typed
    // prefix and timeline forms are bare string literals.
    bool has_parens = false;
    if (is_pat_call && check(TokenType::LParen)) {
        has_parens = true;
        advance(); // consume '('
    }

    // First argument: the mini-notation string
    if (!check(TokenType::String)) {
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

    // Tag the MiniLiteral so codegen knows the parse mode.
    // "pat" is the default and uses no marker; everything else gets a tag.
    if (mode != MiniParseMode::Auto) {
        arena_[node].data = Node::StringData{.value = mode_marker};
    }

    if (has_parens) {
        // Optional second argument: closure (only in function-call form)
        if (match(TokenType::Comma)) {
            if (check(TokenType::LParen)) {
                advance();  // consume '('
                NodeIndex closure = parse_closure();
                arena_.add_child(node, closure);
            } else {
                error("Expected closure after comma in pattern");
            }
        }

        consume(TokenType::RParen, "Expected ')' after pattern arguments");
    }

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
                advance();  // consume '{'
                is_destructure = true;
                destructure_fields = parse_destructure_fields();
                // Create a placeholder pattern node
                pattern = make_node(NodeType::Identifier, arm_tok);
                arena_[pattern].data = Node::IdentifierData{"_destructure"};
            } else if (check(TokenType::String)) {
                pattern = parse_string();
            } else if (check(TokenType::Number) || check(TokenType::Minus)) {
                // Parse optionally-negative number literal
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
NodeIndex Parser::parse_fn_def(bool is_const) {
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

    // Parse parameter list (reuse closure param parsing)
    std::vector<ParsedParam> params = parse_param_list();

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
        is_const
    };

    // Add parameters as Identifier children (using ClosureParamData for those with defaults/rest)
    for (const auto& param : params) {
        NodeIndex param_node = arena_.alloc(NodeType::Identifier, name_tok.location);

        if (param.default_value.has_value() || param.default_string.has_value() ||
            param.is_rest || param.default_node != NULL_NODE) {
            arena_[param_node].data = Node::ClosureParamData{
                param.name, param.default_value, param.default_string, param.is_rest};
        } else {
            arena_[param_node].data = Node::IdentifierData{param.name};
        }

        // Attach expression default as child of param node
        if (param.default_node != NULL_NODE && !param.default_value.has_value() &&
            !param.default_string.has_value()) {
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
std::vector<std::string> Parser::parse_destructure_fields() {
    std::vector<std::string> fields;
    std::set<std::string> seen;

    while (!check(TokenType::RBrace) && !is_at_end()) {
        if (!check(TokenType::Identifier)) {
            error("Expected field name in destructuring pattern");
            break;
        }
        Token name_tok = current();
        std::string name = std::string(name_tok.lexeme);

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
            fields.push_back(std::move(name));
        }
        advance();

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
      std::string_view filename) {
    Parser parser(std::move(tokens), source, filename);
    Ast ast = parser.parse();
    return {std::move(ast), parser.diagnostics()};
}

} // namespace akkado
