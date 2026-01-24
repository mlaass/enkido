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
