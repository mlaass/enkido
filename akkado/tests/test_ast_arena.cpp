#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <akkado/ast.hpp>

#include <vector>
#include <string>

using namespace akkado;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Unit Tests [ast_arena]
// ============================================================================

TEST_CASE("AstArena allocation basics", "[ast_arena]") {
    AstArena arena;

    SECTION("alloc returns incrementing indices") {
        SourceLocation loc{1, 1, 0, 0};

        NodeIndex idx1 = arena.alloc(NodeType::NumberLit, loc);
        NodeIndex idx2 = arena.alloc(NodeType::StringLit, loc);
        NodeIndex idx3 = arena.alloc(NodeType::Identifier, loc);

        // Indices should be sequential (implementation-dependent, but typical)
        CHECK(idx1 != NULL_NODE);
        CHECK(idx2 != NULL_NODE);
        CHECK(idx3 != NULL_NODE);
        CHECK(idx1 != idx2);
        CHECK(idx2 != idx3);
    }

    SECTION("operator[] accesses correct node") {
        SourceLocation loc1{1, 1, 0, 0};
        SourceLocation loc2{2, 5, 10, 20};

        NodeIndex idx1 = arena.alloc(NodeType::NumberLit, loc1);
        NodeIndex idx2 = arena.alloc(NodeType::StringLit, loc2);

        CHECK(arena[idx1].type == NodeType::NumberLit);
        CHECK(arena[idx1].location.line == 1);

        CHECK(arena[idx2].type == NodeType::StringLit);
        CHECK(arena[idx2].location.line == 2);
    }

    SECTION("operator[] const access") {
        SourceLocation loc{1, 1, 0, 0};
        NodeIndex idx = arena.alloc(NodeType::BinaryOp, loc);

        const AstArena& const_arena = arena;
        CHECK(const_arena[idx].type == NodeType::BinaryOp);
    }

    SECTION("valid returns correct values") {
        SourceLocation loc{1, 1, 0, 0};
        NodeIndex idx = arena.alloc(NodeType::NumberLit, loc);

        CHECK(arena.valid(idx));
        CHECK_FALSE(arena.valid(NULL_NODE));
        CHECK_FALSE(arena.valid(99999));  // Out of bounds
    }

    SECTION("size tracks allocations") {
        REQUIRE(arena.size() == 0);

        SourceLocation loc{1, 1, 0, 0};
        arena.alloc(NodeType::NumberLit, loc);
        CHECK(arena.size() == 1);

        arena.alloc(NodeType::StringLit, loc);
        CHECK(arena.size() == 2);

        arena.alloc(NodeType::Identifier, loc);
        CHECK(arena.size() == 3);
    }
}

TEST_CASE("AstArena child management", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("add_child and child_count") {
        NodeIndex parent = arena.alloc(NodeType::Block, loc);
        NodeIndex child1 = arena.alloc(NodeType::NumberLit, loc);
        NodeIndex child2 = arena.alloc(NodeType::StringLit, loc);
        NodeIndex child3 = arena.alloc(NodeType::Identifier, loc);

        CHECK(arena.child_count(parent) == 0);

        arena.add_child(parent, child1);
        CHECK(arena.child_count(parent) == 1);

        arena.add_child(parent, child2);
        CHECK(arena.child_count(parent) == 2);

        arena.add_child(parent, child3);
        CHECK(arena.child_count(parent) == 3);
    }

    SECTION("for_each_child iterates correctly") {
        NodeIndex parent = arena.alloc(NodeType::Call, loc);

        std::vector<NodeIndex> children;
        for (int i = 0; i < 5; ++i) {
            NodeIndex child = arena.alloc(NodeType::NumberLit, loc);
            arena[child].data = Node::NumberData{static_cast<double>(i), true};
            arena.add_child(parent, child);
            children.push_back(child);
        }

        std::vector<NodeIndex> visited;
        arena.for_each_child(parent, [&](NodeIndex idx, const Node&) {
            visited.push_back(idx);
        });

        REQUIRE(visited.size() == children.size());
        for (std::size_t i = 0; i < children.size(); ++i) {
            CHECK(visited[i] == children[i]);
        }
    }

    SECTION("first_child and next_sibling linked list") {
        NodeIndex parent = arena.alloc(NodeType::Block, loc);
        NodeIndex child1 = arena.alloc(NodeType::NumberLit, loc);
        NodeIndex child2 = arena.alloc(NodeType::StringLit, loc);

        arena.add_child(parent, child1);
        arena.add_child(parent, child2);

        // first_child should point to child1
        CHECK(arena[parent].first_child == child1);

        // child1.next_sibling should point to child2
        CHECK(arena[child1].next_sibling == child2);

        // child2.next_sibling should be NULL_NODE
        CHECK(arena[child2].next_sibling == NULL_NODE);
    }
}

TEST_CASE("AstArena node data", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("NumberData storage") {
        NodeIndex idx = arena.alloc(NodeType::NumberLit, loc);
        arena[idx].data = Node::NumberData{42.5, false};

        CHECK(arena[idx].type == NodeType::NumberLit);
        CHECK_THAT(arena[idx].as_number(), WithinAbs(42.5, 1e-10));
    }

    SECTION("StringData storage") {
        NodeIndex idx = arena.alloc(NodeType::StringLit, loc);
        arena[idx].data = Node::StringData{"hello world"};

        CHECK(arena[idx].type == NodeType::StringLit);
        CHECK(arena[idx].as_string() == "hello world");
    }

    SECTION("IdentifierData storage") {
        NodeIndex idx = arena.alloc(NodeType::Identifier, loc);
        arena[idx].data = Node::IdentifierData{"my_var"};

        CHECK(arena[idx].type == NodeType::Identifier);
        CHECK(arena[idx].as_identifier() == "my_var");
    }

    SECTION("BinaryOpData storage") {
        NodeIndex idx = arena.alloc(NodeType::BinaryOp, loc);
        arena[idx].data = Node::BinaryOpData{BinOp::Add};

        CHECK(arena[idx].type == NodeType::BinaryOp);
        CHECK(arena[idx].as_binop() == BinOp::Add);
    }
}

// ============================================================================
// Edge Cases [ast_arena][edge]
// ============================================================================

TEST_CASE("AstArena edge cases", "[ast_arena][edge]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("allocate 10000 nodes") {
        for (int i = 0; i < 10000; ++i) {
            NodeIndex idx = arena.alloc(NodeType::NumberLit, loc);
            arena[idx].data = Node::NumberData{static_cast<double>(i), true};
        }

        CHECK(arena.size() == 10000);

        // Verify first and last
        CHECK_THAT(arena[0].as_number(), WithinAbs(0.0, 1e-10));
        CHECK_THAT(arena[9999].as_number(), WithinAbs(9999.0, 1e-10));
    }

    SECTION("deep nesting - 100 levels") {
        NodeIndex current = arena.alloc(NodeType::Block, loc);
        NodeIndex root = current;

        for (int depth = 0; depth < 100; ++depth) {
            NodeIndex child = arena.alloc(NodeType::Block, loc);
            arena.add_child(current, child);
            current = child;
        }

        // Traverse to count depth
        int measured_depth = 0;
        current = root;
        while (arena[current].first_child != NULL_NODE) {
            current = arena[current].first_child;
            ++measured_depth;
        }

        CHECK(measured_depth == 100);
    }

    SECTION("wide tree - 1000 children per node") {
        NodeIndex parent = arena.alloc(NodeType::Block, loc);

        for (int i = 0; i < 1000; ++i) {
            NodeIndex child = arena.alloc(NodeType::NumberLit, loc);
            arena[child].data = Node::NumberData{static_cast<double>(i), true};
            arena.add_child(parent, child);
        }

        CHECK(arena.child_count(parent) == 1000);

        // Verify children values
        int count = 0;
        arena.for_each_child(parent, [&](NodeIndex idx, const Node&) {
            CHECK_THAT(arena[idx].as_number(), WithinAbs(static_cast<double>(count), 1e-10));
            ++count;
        });
        CHECK(count == 1000);
    }

    SECTION("NULL_NODE handling") {
        CHECK_FALSE(arena.valid(NULL_NODE));
        // Note: child_count(NULL_NODE) would be UB since it dereferences nodes_[NULL_NODE]
        // So we don't test that case
    }

    SECTION("node with no children") {
        NodeIndex leaf = arena.alloc(NodeType::NumberLit, loc);
        CHECK(arena.child_count(leaf) == 0);
        CHECK(arena[leaf].first_child == NULL_NODE);
    }

    SECTION("empty string data") {
        NodeIndex idx = arena.alloc(NodeType::StringLit, loc);
        arena[idx].data = Node::StringData{""};
        CHECK(arena[idx].as_string() == "");
    }

    SECTION("very long string data") {
        std::string long_str(10000, 'x');
        NodeIndex idx = arena.alloc(NodeType::StringLit, loc);
        arena[idx].data = Node::StringData{long_str};
        CHECK(arena[idx].as_string().size() == 10000);
    }
}

// ============================================================================
// Ast Wrapper Tests [ast_arena]
// ============================================================================

TEST_CASE("Ast wrapper", "[ast_arena]") {
    SECTION("default construction") {
        Ast ast;
        CHECK(ast.root == NULL_NODE);
        CHECK_FALSE(ast.valid());
    }

    SECTION("valid ast with root") {
        Ast ast;
        SourceLocation loc{1, 1, 0, 0};

        ast.root = ast.arena.alloc(NodeType::Program, loc);
        CHECK(ast.valid());
        CHECK(ast.arena[ast.root].type == NodeType::Program);
    }

    SECTION("ast with tree structure") {
        Ast ast;
        SourceLocation loc{1, 1, 0, 0};

        ast.root = ast.arena.alloc(NodeType::Program, loc);

        NodeIndex stmt1 = ast.arena.alloc(NodeType::NumberLit, loc);
        ast.arena[stmt1].data = Node::NumberData{1.0, false};
        ast.arena.add_child(ast.root, stmt1);

        NodeIndex stmt2 = ast.arena.alloc(NodeType::NumberLit, loc);
        ast.arena[stmt2].data = Node::NumberData{2.0, false};
        ast.arena.add_child(ast.root, stmt2);

        CHECK(ast.valid());
        CHECK(ast.arena.child_count(ast.root) == 2);
    }
}

// ============================================================================
// Stress Tests [ast_arena][stress]
// ============================================================================

// ============================================================================
// Node Type Name Tests [ast_arena]
// ============================================================================

TEST_CASE("node_type_name returns correct strings", "[ast_arena]") {
    CHECK(std::string(node_type_name(NodeType::NumberLit)) == "NumberLit");
    CHECK(std::string(node_type_name(NodeType::BoolLit)) == "BoolLit");
    CHECK(std::string(node_type_name(NodeType::StringLit)) == "StringLit");
    CHECK(std::string(node_type_name(NodeType::PitchLit)) == "PitchLit");
    CHECK(std::string(node_type_name(NodeType::ChordLit)) == "ChordLit");
    CHECK(std::string(node_type_name(NodeType::ArrayLit)) == "ArrayLit");
    CHECK(std::string(node_type_name(NodeType::Identifier)) == "Identifier");
    CHECK(std::string(node_type_name(NodeType::Hole)) == "Hole");
    CHECK(std::string(node_type_name(NodeType::BinaryOp)) == "BinaryOp");
    CHECK(std::string(node_type_name(NodeType::Call)) == "Call");
    CHECK(std::string(node_type_name(NodeType::MethodCall)) == "MethodCall");
    CHECK(std::string(node_type_name(NodeType::Index)) == "Index");
    CHECK(std::string(node_type_name(NodeType::Pipe)) == "Pipe");
    CHECK(std::string(node_type_name(NodeType::Closure)) == "Closure");
    CHECK(std::string(node_type_name(NodeType::Argument)) == "Argument");
    CHECK(std::string(node_type_name(NodeType::MiniLiteral)) == "MiniLiteral");
    CHECK(std::string(node_type_name(NodeType::MiniPattern)) == "MiniPattern");
    CHECK(std::string(node_type_name(NodeType::MiniAtom)) == "MiniAtom");
    CHECK(std::string(node_type_name(NodeType::MiniGroup)) == "MiniGroup");
    CHECK(std::string(node_type_name(NodeType::MiniSequence)) == "MiniSequence");
    CHECK(std::string(node_type_name(NodeType::MiniPolyrhythm)) == "MiniPolyrhythm");
    CHECK(std::string(node_type_name(NodeType::MiniPolymeter)) == "MiniPolymeter");
    CHECK(std::string(node_type_name(NodeType::MiniChoice)) == "MiniChoice");
    CHECK(std::string(node_type_name(NodeType::MiniEuclidean)) == "MiniEuclidean");
    CHECK(std::string(node_type_name(NodeType::MiniModified)) == "MiniModified");
    CHECK(std::string(node_type_name(NodeType::Assignment)) == "Assignment");
    CHECK(std::string(node_type_name(NodeType::PostStmt)) == "PostStmt");
    CHECK(std::string(node_type_name(NodeType::Block)) == "Block");
    CHECK(std::string(node_type_name(NodeType::FunctionDef)) == "FunctionDef");
    CHECK(std::string(node_type_name(NodeType::MatchExpr)) == "MatchExpr");
    CHECK(std::string(node_type_name(NodeType::MatchArm)) == "MatchArm");
    CHECK(std::string(node_type_name(NodeType::RecordLit)) == "RecordLit");
    CHECK(std::string(node_type_name(NodeType::FieldAccess)) == "FieldAccess");
    CHECK(std::string(node_type_name(NodeType::PipeBinding)) == "PipeBinding");
    CHECK(std::string(node_type_name(NodeType::Program)) == "Program");
}

// ============================================================================
// BinOp Function Name Tests [ast_arena]
// ============================================================================

TEST_CASE("binop_function_name returns correct strings", "[ast_arena]") {
    CHECK(std::string(binop_function_name(BinOp::Add)) == "add");
    CHECK(std::string(binop_function_name(BinOp::Sub)) == "sub");
    CHECK(std::string(binop_function_name(BinOp::Mul)) == "mul");
    CHECK(std::string(binop_function_name(BinOp::Div)) == "div");
    CHECK(std::string(binop_function_name(BinOp::Pow)) == "pow");
}

// ============================================================================
// Node Accessor Tests [ast_arena]
// ============================================================================

TEST_CASE("Node::as_bool accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    NodeIndex idx = arena.alloc(NodeType::BoolLit, loc);
    arena[idx].data = Node::BoolData{true};
    CHECK(arena[idx].as_bool() == true);

    arena[idx].data = Node::BoolData{false};
    CHECK(arena[idx].as_bool() == false);
}

TEST_CASE("Node::as_pitch accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    NodeIndex idx = arena.alloc(NodeType::PitchLit, loc);
    arena[idx].data = Node::PitchData{60};  // Middle C
    CHECK(arena[idx].as_pitch() == 60);

    arena[idx].data = Node::PitchData{69};  // A4
    CHECK(arena[idx].as_pitch() == 69);
}

TEST_CASE("Node::as_chord accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    NodeIndex idx = arena.alloc(NodeType::ChordLit, loc);
    arena[idx].data = Node::ChordData{60, {0, 4, 7}};  // C major
    const auto& chord = arena[idx].as_chord();
    CHECK(chord.root_midi == 60);
    REQUIRE(chord.intervals.size() == 3);
    CHECK(chord.intervals[0] == 0);
    CHECK(chord.intervals[1] == 4);
    CHECK(chord.intervals[2] == 7);
}

TEST_CASE("Node::as_arg_name accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("named argument") {
        NodeIndex idx = arena.alloc(NodeType::Argument, loc);
        arena[idx].data = Node::ArgumentData{"freq"};
        const auto& name = arena[idx].as_arg_name();
        REQUIRE(name.has_value());
        CHECK(name.value() == "freq");
    }

    SECTION("positional argument") {
        NodeIndex idx = arena.alloc(NodeType::Argument, loc);
        arena[idx].data = Node::ArgumentData{std::nullopt};
        const auto& name = arena[idx].as_arg_name();
        CHECK_FALSE(name.has_value());
    }
}

TEST_CASE("Node::as_closure_param accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("param without default") {
        NodeIndex idx = arena.alloc(NodeType::Identifier, loc);
        arena[idx].data = Node::ClosureParamData{"x", std::nullopt};
        const auto& param = arena[idx].as_closure_param();
        CHECK(param.name == "x");
        CHECK_FALSE(param.default_value.has_value());
    }

    SECTION("param with default") {
        NodeIndex idx = arena.alloc(NodeType::Identifier, loc);
        arena[idx].data = Node::ClosureParamData{"freq", 440.0};
        const auto& param = arena[idx].as_closure_param();
        CHECK(param.name == "freq");
        REQUIRE(param.default_value.has_value());
        CHECK_THAT(*param.default_value, WithinAbs(440.0, 1e-10));
    }
}

TEST_CASE("Node::as_mini_atom accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("pitch atom") {
        NodeIndex idx = arena.alloc(NodeType::MiniAtom, loc);
        Node::MiniAtomData data;
        data.kind = Node::MiniAtomKind::Pitch;
        data.midi_note = 60;
        arena[idx].data = data;

        const auto& atom = arena[idx].as_mini_atom();
        CHECK(atom.kind == Node::MiniAtomKind::Pitch);
        CHECK(atom.midi_note == 60);
    }

    SECTION("sample atom") {
        NodeIndex idx = arena.alloc(NodeType::MiniAtom, loc);
        Node::MiniAtomData data;
        data.kind = Node::MiniAtomKind::Sample;
        data.sample_name = "kick";
        data.sample_variant = 2;
        arena[idx].data = data;

        const auto& atom = arena[idx].as_mini_atom();
        CHECK(atom.kind == Node::MiniAtomKind::Sample);
        CHECK(atom.sample_name == "kick");
        CHECK(atom.sample_variant == 2);
    }

    SECTION("rest atom") {
        NodeIndex idx = arena.alloc(NodeType::MiniAtom, loc);
        Node::MiniAtomData data;
        data.kind = Node::MiniAtomKind::Rest;
        arena[idx].data = data;

        const auto& atom = arena[idx].as_mini_atom();
        CHECK(atom.kind == Node::MiniAtomKind::Rest);
    }

    SECTION("chord atom") {
        NodeIndex idx = arena.alloc(NodeType::MiniAtom, loc);
        Node::MiniAtomData data;
        data.kind = Node::MiniAtomKind::Chord;
        data.chord_root = "A";
        data.chord_quality = "m7";
        data.chord_root_midi = 69;
        data.chord_intervals = {0, 3, 7, 10};
        arena[idx].data = data;

        const auto& atom = arena[idx].as_mini_atom();
        CHECK(atom.kind == Node::MiniAtomKind::Chord);
        CHECK(atom.chord_root == "A");
        CHECK(atom.chord_quality == "m7");
        CHECK(atom.chord_root_midi == 69);
        REQUIRE(atom.chord_intervals.size() == 4);
    }
}

TEST_CASE("Node::as_mini_euclidean accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    NodeIndex idx = arena.alloc(NodeType::MiniEuclidean, loc);
    arena[idx].data = Node::MiniEuclideanData{3, 8, 2};  // (3,8,2)

    const auto& eucl = arena[idx].as_mini_euclidean();
    CHECK(eucl.hits == 3);
    CHECK(eucl.steps == 8);
    CHECK(eucl.rotation == 2);
}

TEST_CASE("Node::as_mini_modifier accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("speed modifier") {
        NodeIndex idx = arena.alloc(NodeType::MiniModified, loc);
        arena[idx].data = Node::MiniModifierData{Node::MiniModifierType::Speed, 2.0f};

        const auto& mod = arena[idx].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Speed);
        CHECK(mod.value == 2.0f);
    }

    SECTION("slow modifier") {
        NodeIndex idx = arena.alloc(NodeType::MiniModified, loc);
        arena[idx].data = Node::MiniModifierData{Node::MiniModifierType::Slow, 4.0f};

        const auto& mod = arena[idx].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Slow);
        CHECK(mod.value == 4.0f);
    }

    SECTION("repeat modifier") {
        NodeIndex idx = arena.alloc(NodeType::MiniModified, loc);
        arena[idx].data = Node::MiniModifierData{Node::MiniModifierType::Repeat, 3.0f};

        const auto& mod = arena[idx].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Repeat);
        CHECK(mod.value == 3.0f);
    }

    SECTION("chance modifier") {
        NodeIndex idx = arena.alloc(NodeType::MiniModified, loc);
        arena[idx].data = Node::MiniModifierData{Node::MiniModifierType::Chance, 0.5f};

        const auto& mod = arena[idx].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Chance);
        CHECK(mod.value == 0.5f);
    }

    SECTION("weight modifier") {
        NodeIndex idx = arena.alloc(NodeType::MiniModified, loc);
        arena[idx].data = Node::MiniModifierData{Node::MiniModifierType::Weight, 2.0f};

        const auto& mod = arena[idx].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Weight);
        CHECK(mod.value == 2.0f);
    }

    SECTION("duration modifier") {
        NodeIndex idx = arena.alloc(NodeType::MiniModified, loc);
        arena[idx].data = Node::MiniModifierData{Node::MiniModifierType::Duration, 1.5f};

        const auto& mod = arena[idx].as_mini_modifier();
        CHECK(mod.modifier_type == Node::MiniModifierType::Duration);
        CHECK(mod.value == 1.5f);
    }
}

TEST_CASE("Node::as_mini_polymeter accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("polymeter with explicit step count") {
        NodeIndex idx = arena.alloc(NodeType::MiniPolymeter, loc);
        arena[idx].data = Node::MiniPolymeterData{5};  // %5

        const auto& poly = arena[idx].as_mini_polymeter();
        CHECK(poly.step_count == 5);
    }

    SECTION("polymeter without step count uses 0") {
        NodeIndex idx = arena.alloc(NodeType::MiniPolymeter, loc);
        arena[idx].data = Node::MiniPolymeterData{0};

        const auto& poly = arena[idx].as_mini_polymeter();
        CHECK(poly.step_count == 0);
    }
}

TEST_CASE("Node::as_function_def accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    NodeIndex idx = arena.alloc(NodeType::FunctionDef, loc);
    arena[idx].data = Node::FunctionDefData{"myFunc", 3};

    const auto& fn_def = arena[idx].as_function_def();
    CHECK(fn_def.name == "myFunc");
    CHECK(fn_def.param_count == 3);
}

TEST_CASE("Node::as_match_arm accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("wildcard arm without guard") {
        NodeIndex idx = arena.alloc(NodeType::MatchArm, loc);
        arena[idx].data = Node::MatchArmData{true, false, NULL_NODE};

        const auto& arm = arena[idx].as_match_arm();
        CHECK(arm.is_wildcard == true);
        CHECK(arm.has_guard == false);
        CHECK(arm.guard_node == NULL_NODE);
    }

    SECTION("pattern arm with guard") {
        NodeIndex guard = arena.alloc(NodeType::BoolLit, loc);
        NodeIndex idx = arena.alloc(NodeType::MatchArm, loc);
        arena[idx].data = Node::MatchArmData{false, true, guard};

        const auto& arm = arena[idx].as_match_arm();
        CHECK(arm.is_wildcard == false);
        CHECK(arm.has_guard == true);
        CHECK(arm.guard_node == guard);
    }
}

TEST_CASE("Node::as_match_expr accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("match with scrutinee") {
        NodeIndex idx = arena.alloc(NodeType::MatchExpr, loc);
        arena[idx].data = Node::MatchExprData{true};

        const auto& match = arena[idx].as_match_expr();
        CHECK(match.has_scrutinee == true);
    }

    SECTION("guard-only match") {
        NodeIndex idx = arena.alloc(NodeType::MatchExpr, loc);
        arena[idx].data = Node::MatchExprData{false};

        const auto& match = arena[idx].as_match_expr();
        CHECK(match.has_scrutinee == false);
    }
}

TEST_CASE("Node::as_record_field accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("explicit field") {
        NodeIndex idx = arena.alloc(NodeType::Argument, loc);
        arena[idx].data = Node::RecordFieldData{"freq", false};

        const auto& field = arena[idx].as_record_field();
        CHECK(field.name == "freq");
        CHECK(field.is_shorthand == false);
    }

    SECTION("shorthand field") {
        NodeIndex idx = arena.alloc(NodeType::Argument, loc);
        arena[idx].data = Node::RecordFieldData{"x", true};

        const auto& field = arena[idx].as_record_field();
        CHECK(field.name == "x");
        CHECK(field.is_shorthand == true);
    }
}

TEST_CASE("Node::as_field_access accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    NodeIndex idx = arena.alloc(NodeType::FieldAccess, loc);
    arena[idx].data = Node::FieldAccessData{"velocity"};

    const auto& access = arena[idx].as_field_access();
    CHECK(access.field_name == "velocity");
}

TEST_CASE("Node::as_pipe_binding accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    NodeIndex idx = arena.alloc(NodeType::PipeBinding, loc);
    arena[idx].data = Node::PipeBindingData{"sig"};

    const auto& binding = arena[idx].as_pipe_binding();
    CHECK(binding.binding_name == "sig");
}

TEST_CASE("Node::as_hole accessor", "[ast_arena]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("bare hole") {
        NodeIndex idx = arena.alloc(NodeType::Hole, loc);
        arena[idx].data = Node::HoleData{std::nullopt};

        const auto& hole = arena[idx].as_hole();
        CHECK_FALSE(hole.field_name.has_value());
    }

    SECTION("hole with field access") {
        NodeIndex idx = arena.alloc(NodeType::Hole, loc);
        arena[idx].data = Node::HoleData{"freq"};

        const auto& hole = arena[idx].as_hole();
        REQUIRE(hole.field_name.has_value());
        CHECK(hole.field_name.value() == "freq");
    }
}

// ============================================================================
// Stress Tests [ast_arena][stress]
// ============================================================================

TEST_CASE("AstArena stress test", "[ast_arena][stress]") {
    AstArena arena;
    SourceLocation loc{1, 1, 0, 0};

    SECTION("simulate large program parsing") {
        // Create a program with 100 functions, each with 10 statements

        NodeIndex program = arena.alloc(NodeType::Program, loc);

        for (int fn = 0; fn < 100; ++fn) {
            NodeIndex func = arena.alloc(NodeType::FunctionDef, loc);
            arena.add_child(program, func);

            NodeIndex body = arena.alloc(NodeType::Block, loc);
            arena.add_child(func, body);

            for (int stmt = 0; stmt < 10; ++stmt) {
                NodeIndex binop = arena.alloc(NodeType::BinaryOp, loc);
                arena[binop].data = Node::BinaryOpData{BinOp::Add};

                NodeIndex lhs = arena.alloc(NodeType::Identifier, loc);
                arena[lhs].data = Node::IdentifierData{"var_" + std::to_string(fn) + "_" + std::to_string(stmt)};

                NodeIndex rhs = arena.alloc(NodeType::NumberLit, loc);
                arena[rhs].data = Node::NumberData{static_cast<double>(fn * 10 + stmt), true};

                arena.add_child(binop, lhs);
                arena.add_child(binop, rhs);
                arena.add_child(body, binop);
            }
        }

        CHECK(arena.child_count(program) == 100);

        // Traverse and count total nodes
        std::size_t total_nodes = arena.size();
        CHECK(total_nodes > 3000);  // 100 * (1 func + 1 body + 10 * (1 binop + 2 children))
    }

    SECTION("balanced binary tree") {
        // Create a balanced binary tree of depth 10 (1023 nodes)
        std::vector<NodeIndex> level;
        level.push_back(arena.alloc(NodeType::BinaryOp, loc));

        for (int depth = 0; depth < 10; ++depth) {
            std::vector<NodeIndex> next_level;
            for (NodeIndex parent : level) {
                NodeIndex left = arena.alloc(NodeType::BinaryOp, loc);
                NodeIndex right = arena.alloc(NodeType::BinaryOp, loc);
                arena.add_child(parent, left);
                arena.add_child(parent, right);
                next_level.push_back(left);
                next_level.push_back(right);
            }
            level = std::move(next_level);
        }

        // Should have 2^11 - 1 = 2047 nodes
        CHECK(arena.size() == 2047);
    }

    SECTION("mixed deep and wide structure") {
        NodeIndex root = arena.alloc(NodeType::Block, loc);

        // 50 chains of depth 20
        for (int chain = 0; chain < 50; ++chain) {
            NodeIndex current = root;
            for (int depth = 0; depth < 20; ++depth) {
                NodeIndex child = arena.alloc(NodeType::Block, loc);
                arena.add_child(current, child);
                current = child;
            }
        }

        CHECK(arena.size() == 1 + 50 * 20);  // root + 50 chains of 20
    }
}
