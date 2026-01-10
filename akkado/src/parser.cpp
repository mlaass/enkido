#include "akkado/parser.hpp"
#include <stdexcept>

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
            case TokenType::Post:
            case TokenType::Pat:
            case TokenType::Seq:
            case TokenType::Timeline:
            case TokenType::Note:
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
        case TokenType::Pipe:     return Precedence::Pipe;
        case TokenType::Plus:
        case TokenType::Minus:    return Precedence::Addition;
        case TokenType::Star:
        case TokenType::Slash:    return Precedence::Multiplication;
        case TokenType::Caret:    return Precedence::Power;
        default:                  return Precedence::None;
    }
}

bool Parser::is_infix_operator(TokenType type) const {
    switch (type) {
        case TokenType::Pipe:
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

NodeIndex Parser::parse_program() {
    NodeIndex program = make_node(NodeType::Program);

    while (!is_at_end()) {
        NodeIndex stmt = parse_statement();
        if (stmt != NULL_NODE) {
            arena_.add_child(program, stmt);
        }
        if (panic_mode_) {
            synchronize();
        }
    }

    return program;
}

// Statement parsing

NodeIndex Parser::parse_statement() {
    // Check for post statement
    if (match(TokenType::Post)) {
        return parse_post_stmt();
    }

    // Check for assignment: identifier = expr
    if (check(TokenType::Identifier)) {
        if (current_idx_ + 1 < tokens_.size() &&
            tokens_[current_idx_ + 1].type == TokenType::Equals) {
            Token name = advance();  // consume identifier
            return parse_assignment(name);
        }
    }

    // Otherwise it's an expression statement
    return parse_expression();
}

NodeIndex Parser::parse_assignment(const Token& name_token) {
    consume(TokenType::Equals, "Expected '=' after identifier");

    NodeIndex node = make_node(NodeType::Assignment, name_token);
    arena_[node].data = Node::IdentifierData{std::string(name_token.lexeme)};

    NodeIndex value = parse_expression();
    if (value != NULL_NODE) {
        arena_.add_child(node, value);
    }

    return node;
}

NodeIndex Parser::parse_post_stmt() {
    Token post_token = previous();
    consume(TokenType::LParen, "Expected '(' after 'post'");

    NodeIndex node = make_node(NodeType::PostStmt, post_token);

    // Expect a closure: (params) -> body
    if (!check(TokenType::LParen)) {
        error("Expected closure in post()");
        return node;
    }

    advance();  // consume '(' of the closure
    NodeIndex closure = parse_closure();
    if (closure != NULL_NODE) {
        arena_.add_child(node, closure);
    }

    consume(TokenType::RParen, "Expected ')' after post closure");
    return node;
}

// Expression parsing (Pratt parser)

NodeIndex Parser::parse_expression() {
    return parse_precedence(Precedence::Pipe);
}

NodeIndex Parser::parse_precedence(Precedence prec) {
    NodeIndex left = parse_prefix();
    if (left == NULL_NODE) {
        return NULL_NODE;
    }

    // Handle method calls (highest precedence, can chain)
    while (check(TokenType::Dot) && prec <= Precedence::Method) {
        advance();  // consume '.'
        left = parse_method_call(left);
    }

    // Handle binary operators
    while (!is_at_end()) {
        if (!is_infix_operator(current().type)) {
            break;
        }

        Precedence op_prec = get_precedence(current().type);
        if (op_prec < prec) {
            break;
        }

        Token op = advance();
        left = parse_infix(left, op);

        // After binary op, check for method calls again
        while (check(TokenType::Dot) && prec <= Precedence::Method) {
            advance();  // consume '.'
            left = parse_method_call(left);
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
            return parse_hole();
        case TokenType::LParen:
            return parse_grouping();
        case TokenType::Pat:
        case TokenType::Seq:
        case TokenType::Timeline:
        case TokenType::Note:
            return parse_mini_literal();
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
    Token tok = advance();
    return make_node(NodeType::Hole, tok);
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
    // Check if it's identifier(s) followed by ) ->
    else if (check(TokenType::Identifier)) {
        std::size_t saved = current_idx_;
        bool looks_like_params = true;

        while (!is_at_end() && looks_like_params) {
            if (!check(TokenType::Identifier)) {
                looks_like_params = false;
                break;
            }
            advance();

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
    // Use Identifier for simple params, ClosureParamData for params with defaults
    if (!params.empty()) {
        NodeIndex first_param = NULL_NODE;
        NodeIndex prev_param = NULL_NODE;

        for (const auto& param : params) {
            NodeIndex param_node = arena_.alloc(NodeType::Identifier, start_tok.location);

            if (param.default_value.has_value()) {
                // Parameter with default value - use ClosureParamData
                arena_[param_node].data = Node::ClosureParamData{param.name, param.default_value};
            } else {
                // Simple parameter - use IdentifierData
                arena_[param_node].data = Node::IdentifierData{param.name};
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
        if (!check(TokenType::Identifier)) {
            error("Expected parameter name");
            break;
        }
        Token name_tok = advance();
        std::string name = std::string(name_tok.lexeme);

        std::optional<double> default_value;
        if (match(TokenType::Equals)) {
            // Parse default value (must be a number literal)
            if (!check(TokenType::Number)) {
                error("Default parameter value must be a number literal");
                break;
            }
            Token num_tok = advance();
            default_value = std::get<NumericValue>(num_tok.value).value;
            seen_default = true;
        } else if (seen_default) {
            // Required param after optional - error
            error("Required parameter cannot follow optional parameter");
            break;
        }

        params.push_back(ParsedParam{std::move(name), default_value});
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
    // Determine the operator
    BinOp binop;
    switch (op.type) {
        case TokenType::Plus:  binop = BinOp::Add; break;
        case TokenType::Minus: binop = BinOp::Sub; break;
        case TokenType::Star:  binop = BinOp::Mul; break;
        case TokenType::Slash: binop = BinOp::Div; break;
        case TokenType::Caret: binop = BinOp::Pow; break;
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

    // Create binary op node (will be desugared to Call in later phase)
    // Or we can desugar now to Call node
    NodeIndex node = make_node(NodeType::Call, op);

    // Set function name based on operator
    arena_[node].data = Node::IdentifierData{binop_function_name(binop)};

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

// Method call parsing (for x.method() syntax)
NodeIndex Parser::parse_method_call(NodeIndex left) {
    Token dot_tok = previous();

    if (!check(TokenType::Identifier)) {
        error("Expected method name after '.'");
        return left;
    }

    Token method_name = advance();
    NodeIndex node = make_node(NodeType::MethodCall, dot_tok);
    arena_[node].data = Node::IdentifierData{std::string(method_name.lexeme)};

    // Add receiver as first child
    arena_.add_child(node, left);

    // Parse arguments
    consume(TokenType::LParen, "Expected '(' after method name");

    if (!check(TokenType::RParen)) {
        auto args = parse_argument_list();
        for (NodeIndex arg : args) {
            arena_.add_child(node, arg);
        }
    }

    consume(TokenType::RParen, "Expected ')' after arguments");

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

    // Check for named argument: identifier ':'
    if (check(TokenType::Identifier)) {
        std::size_t saved = current_idx_;
        Token name = advance();

        if (check(TokenType::Colon)) {
            advance();  // consume ':'
            arena_[node].data = Node::ArgumentData{std::string(name.lexeme)};
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
    arena_[node].data = Node::ArgumentData{std::nullopt};
    NodeIndex value = parse_expression();
    if (value != NULL_NODE) {
        arena_.add_child(node, value);
    }
    return node;
}

// Mini-notation literal parsing

NodeIndex Parser::parse_mini_literal() {
    Token kw_tok = advance();
    PatternType pat_type;

    switch (kw_tok.type) {
        case TokenType::Pat:      pat_type = PatternType::Pat; break;
        case TokenType::Seq:      pat_type = PatternType::Seq; break;
        case TokenType::Timeline: pat_type = PatternType::Timeline; break;
        case TokenType::Note:     pat_type = PatternType::Note; break;
        default:
            error("Expected pattern keyword");
            return NULL_NODE;
    }

    NodeIndex node = make_node(NodeType::MiniLiteral, kw_tok);
    arena_[node].data = Node::PatternData{pat_type};

    consume(TokenType::LParen, "Expected '(' after pattern keyword");

    // First argument: the mini-notation string
    if (!check(TokenType::String)) {
        error("Expected string for mini-notation pattern");
        return node;
    }

    NodeIndex pattern_str = parse_string();
    arena_.add_child(node, pattern_str);

    // Optional second argument: closure
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
    return node;
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
