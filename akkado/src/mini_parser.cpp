#include "akkado/mini_parser.hpp"
#include "akkado/mini_lexer.hpp"

namespace akkado {

MiniParser::MiniParser(std::vector<MiniToken> tokens, AstArena& arena,
                       SourceLocation base_location)
    : tokens_(std::move(tokens))
    , arena_(arena)
    , base_location_(base_location)
{}

NodeIndex MiniParser::parse() {
    if (tokens_.empty() || (tokens_.size() == 1 && tokens_[0].is_eof())) {
        // Empty pattern - return empty MiniPattern node
        return make_node(NodeType::MiniPattern, base_location_);
    }

    return parse_pattern();
}

// Token navigation

const MiniToken& MiniParser::current() const {
    if (current_idx_ >= tokens_.size()) {
        return tokens_.back(); // Return Eof
    }
    return tokens_[current_idx_];
}

const MiniToken& MiniParser::previous() const {
    if (current_idx_ == 0) {
        return tokens_[0];
    }
    return tokens_[current_idx_ - 1];
}

bool MiniParser::is_at_end() const {
    return current().type == MiniTokenType::Eof;
}

bool MiniParser::check(MiniTokenType type) const {
    return current().type == type;
}

bool MiniParser::match(MiniTokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

const MiniToken& MiniParser::advance() {
    if (!is_at_end()) {
        current_idx_++;
    }
    return previous();
}

const MiniToken& MiniParser::consume(MiniTokenType type, std::string_view message) {
    if (check(type)) {
        return advance();
    }
    error(message);
    return current();
}

// Error handling

void MiniParser::error(std::string_view message) {
    error_at(current(), message);
}

void MiniParser::error_at(const MiniToken& token, std::string_view message) {
    diagnostics_.push_back(Diagnostic{
        .severity = Severity::Error,
        .code = "MP01",
        .message = std::string(message),
        .filename = "<pattern>",
        .location = token.location
    });
}

// Node creation

NodeIndex MiniParser::make_node(NodeType type, const MiniToken& token) {
    return arena_.alloc(type, token.location);
}

NodeIndex MiniParser::make_node(NodeType type, SourceLocation loc) {
    return arena_.alloc(type, loc);
}

// Pattern parsing

NodeIndex MiniParser::parse_pattern() {
    // Pattern is a sequence of choice expressions
    NodeIndex root = make_node(NodeType::MiniPattern, current().location);

    while (!is_at_end()) {
        NodeIndex element = parse_choice();
        if (element == NULL_NODE) {
            break;
        }
        arena_.add_child(root, element);
    }

    return root;
}

NodeIndex MiniParser::parse_choice() {
    // choice = element { "|" element }
    NodeIndex left = parse_element();
    if (left == NULL_NODE) {
        return NULL_NODE;
    }

    if (!check(MiniTokenType::Pipe)) {
        return left;
    }

    // We have a choice - create MiniChoice node
    NodeIndex choice = make_node(NodeType::MiniChoice, previous().location);
    arena_.add_child(choice, left);

    while (match(MiniTokenType::Pipe)) {
        NodeIndex right = parse_element();
        if (right == NULL_NODE) {
            error("Expected element after '|'");
            break;
        }
        arena_.add_child(choice, right);
    }

    return choice;
}

NodeIndex MiniParser::parse_element() {
    // element = atom [ modifiers ] | group | sequence
    NodeIndex atom = parse_atom();
    if (atom == NULL_NODE) {
        return NULL_NODE;
    }

    // Check for euclidean rhythm: atom(k, n, r)
    if (check(MiniTokenType::LParen)) {
        atom = parse_euclidean(atom);
    }

    // Check for modifiers: *n, /n, :n, @n, !n, ?n
    if (check(MiniTokenType::Star) || check(MiniTokenType::Slash) ||
        check(MiniTokenType::Colon) || check(MiniTokenType::At) ||
        check(MiniTokenType::Bang) || check(MiniTokenType::Question)) {
        atom = parse_modifiers(atom);
    }

    return atom;
}

bool MiniParser::is_atom_start() const {
    MiniTokenType type = current().type;
    return type == MiniTokenType::PitchToken ||
           type == MiniTokenType::SampleToken ||
           type == MiniTokenType::Rest ||
           type == MiniTokenType::LBracket ||
           type == MiniTokenType::LAngle ||
           type == MiniTokenType::LBrace;
}

NodeIndex MiniParser::parse_atom() {
    // atom = pitch | sample | rest | group | sequence

    if (match(MiniTokenType::PitchToken)) {
        return parse_pitch_atom(previous());
    }

    if (match(MiniTokenType::SampleToken)) {
        return parse_sample_atom(previous());
    }

    if (match(MiniTokenType::Rest)) {
        return parse_rest();
    }

    if (check(MiniTokenType::LBracket)) {
        return parse_group();
    }

    if (check(MiniTokenType::LAngle)) {
        return parse_sequence();
    }

    if (check(MiniTokenType::LBrace)) {
        return parse_polymeter();
    }

    // Not an atom - this is OK, pattern might be empty or we're at end
    return NULL_NODE;
}

NodeIndex MiniParser::parse_pitch_atom(const MiniToken& token) {
    NodeIndex node = make_node(NodeType::MiniAtom, token);
    const MiniPitchData& pitch = token.as_pitch();

    arena_[node].data = Node::MiniAtomData{
        .kind = Node::MiniAtomKind::Pitch,
        .midi_note = pitch.midi_note,
        .sample_name = "",
        .sample_variant = 0
    };

    return node;
}

NodeIndex MiniParser::parse_sample_atom(const MiniToken& token) {
    NodeIndex node = make_node(NodeType::MiniAtom, token);
    const MiniSampleData& sample = token.as_sample();

    arena_[node].data = Node::MiniAtomData{
        .kind = Node::MiniAtomKind::Sample,
        .midi_note = 0,
        .sample_name = sample.name,
        .sample_variant = sample.variant
    };

    return node;
}

NodeIndex MiniParser::parse_rest() {
    NodeIndex node = make_node(NodeType::MiniAtom, previous());

    arena_[node].data = Node::MiniAtomData{
        .kind = Node::MiniAtomKind::Rest,
        .midi_note = 0,
        .sample_name = "",
        .sample_variant = 0
    };

    return node;
}

NodeIndex MiniParser::parse_group() {
    // group = "[" pattern "]" or polyrhythm = "[" atom { "," atom } "]"
    MiniToken open = advance(); // consume '['

    // Check if this is a polyrhythm by looking for comma
    std::vector<NodeIndex> elements;
    bool is_polyrhythm = false;

    // Parse first element
    if (!is_at_end() && !check(MiniTokenType::RBracket)) {
        NodeIndex first = parse_choice();
        if (first != NULL_NODE) {
            elements.push_back(first);
        }

        // Check for comma (polyrhythm) or continue as group
        if (check(MiniTokenType::Comma)) {
            is_polyrhythm = true;
            while (match(MiniTokenType::Comma)) {
                NodeIndex elem = parse_choice();
                if (elem == NULL_NODE) {
                    error("Expected element after ','");
                    break;
                }
                elements.push_back(elem);
            }
        } else {
            // Regular group - continue parsing elements
            while (!is_at_end() && !check(MiniTokenType::RBracket)) {
                NodeIndex elem = parse_choice();
                if (elem == NULL_NODE) {
                    break;
                }
                elements.push_back(elem);
            }
        }
    }

    consume(MiniTokenType::RBracket, "Expected ']' after group");

    // Create appropriate node type
    NodeType type = is_polyrhythm ? NodeType::MiniPolyrhythm : NodeType::MiniGroup;
    NodeIndex node = make_node(type, open);

    for (NodeIndex elem : elements) {
        arena_.add_child(node, elem);
    }

    return node;
}

NodeIndex MiniParser::parse_sequence() {
    // sequence = "<" pattern ">"
    MiniToken open = advance(); // consume '<'

    NodeIndex node = make_node(NodeType::MiniSequence, open);

    while (!is_at_end() && !check(MiniTokenType::RAngle)) {
        NodeIndex elem = parse_choice();
        if (elem == NULL_NODE) {
            break;
        }
        arena_.add_child(node, elem);
    }

    consume(MiniTokenType::RAngle, "Expected '>' after sequence");

    return node;
}

NodeIndex MiniParser::parse_polymeter() {
    // polymeter = "{" pattern "}" [ "%" number ]
    MiniToken open = advance(); // consume '{'

    NodeIndex node = make_node(NodeType::MiniPolymeter, open);

    // Parse children until '}'
    while (!is_at_end() && !check(MiniTokenType::RBrace)) {
        NodeIndex elem = parse_choice();
        if (elem == NULL_NODE) {
            break;
        }
        arena_.add_child(node, elem);
    }

    consume(MiniTokenType::RBrace, "Expected '}' after polymeter");

    // Optional %n modifier for step count
    std::uint8_t step_count = 0;  // 0 means use child count
    if (match(MiniTokenType::Percent)) {
        if (!check(MiniTokenType::Number)) {
            error("Expected step count after '%'");
        } else {
            step_count = static_cast<std::uint8_t>(current().as_number());
            advance();
        }
    }

    arena_[node].data = Node::MiniPolymeterData{
        .step_count = step_count
    };

    return node;
}

NodeIndex MiniParser::parse_euclidean(NodeIndex atom) {
    // euclidean = atom "(" number "," number [ "," number ] ")"
    MiniToken open = advance(); // consume '('

    // Parse hits
    if (!match(MiniTokenType::Number)) {
        error("Expected number for euclidean hits");
        return atom;
    }
    double hits = previous().as_number();

    consume(MiniTokenType::Comma, "Expected ',' after euclidean hits");

    // Parse steps
    if (!match(MiniTokenType::Number)) {
        error("Expected number for euclidean steps");
        return atom;
    }
    double steps = previous().as_number();

    // Optional rotation
    double rotation = 0.0;
    if (match(MiniTokenType::Comma)) {
        if (!match(MiniTokenType::Number)) {
            error("Expected number for euclidean rotation");
        } else {
            rotation = previous().as_number();
        }
    }

    consume(MiniTokenType::RParen, "Expected ')' after euclidean parameters");

    // Create MiniEuclidean node with atom as child
    NodeIndex node = make_node(NodeType::MiniEuclidean, open);
    arena_[node].data = Node::MiniEuclideanData{
        .hits = static_cast<std::uint8_t>(hits),
        .steps = static_cast<std::uint8_t>(steps),
        .rotation = static_cast<std::uint8_t>(rotation)
    };

    arena_.add_child(node, atom);

    return node;
}

NodeIndex MiniParser::parse_modifiers(NodeIndex atom) {
    // modifiers = { "*" number | "/" number | ":" number | "@" number | "!" number | "?" number }

    while (true) {
        Node::MiniModifierType mod_type;
        bool has_modifier = false;

        if (match(MiniTokenType::Star)) {
            mod_type = Node::MiniModifierType::Speed;
            has_modifier = true;
        } else if (match(MiniTokenType::Slash)) {
            mod_type = Node::MiniModifierType::Slow;
            has_modifier = true;
        } else if (match(MiniTokenType::Colon)) {
            mod_type = Node::MiniModifierType::Duration;
            has_modifier = true;
        } else if (match(MiniTokenType::At)) {
            mod_type = Node::MiniModifierType::Weight;
            has_modifier = true;
        } else if (match(MiniTokenType::Bang)) {
            mod_type = Node::MiniModifierType::Repeat;
            has_modifier = true;
        } else if (match(MiniTokenType::Question)) {
            mod_type = Node::MiniModifierType::Chance;
            has_modifier = true;
        }

        if (!has_modifier) {
            break;
        }

        // Parse the modifier value
        float value = 1.0f;
        if (check(MiniTokenType::Number)) {
            advance();
            value = static_cast<float>(previous().as_number());
        } else {
            // Set defaults for modifiers without explicit numbers
            switch (mod_type) {
                case Node::MiniModifierType::Repeat:
                    value = 2.0f;  // ! defaults to 2 repeats
                    break;
                case Node::MiniModifierType::Chance:
                    value = 0.5f;  // ? defaults to 50% chance
                    break;
                default:
                    error("Expected number after modifier");
                    break;
            }
        }

        // Wrap atom in MiniModified node
        MiniToken mod_token = previous();
        NodeIndex modified = make_node(NodeType::MiniModified, mod_token);
        arena_[modified].data = Node::MiniModifierData{
            .modifier_type = mod_type,
            .value = value
        };
        arena_.add_child(modified, atom);

        atom = modified;
    }

    return atom;
}

// Convenience function
std::pair<NodeIndex, std::vector<Diagnostic>>
parse_mini(std::string_view pattern, AstArena& arena, SourceLocation base_location,
           bool sample_only) {
    auto [tokens, lex_diags] = lex_mini(pattern, base_location, sample_only);

    MiniParser parser(std::move(tokens), arena, base_location);
    NodeIndex root = parser.parse();

    // Combine lexer and parser diagnostics
    std::vector<Diagnostic> all_diags = std::move(lex_diags);
    const auto& parse_diags = parser.diagnostics();
    all_diags.insert(all_diags.end(), parse_diags.begin(), parse_diags.end());

    return {root, std::move(all_diags)};
}

} // namespace akkado
