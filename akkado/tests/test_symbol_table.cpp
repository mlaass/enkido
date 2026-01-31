#include <catch2/catch_test_macros.hpp>

#include <akkado/symbol_table.hpp>

#include <string>
#include <vector>

using namespace akkado;

// ============================================================================
// Unit Tests [symbol_table]
// ============================================================================

TEST_CASE("SymbolTable scope management", "[symbol_table]") {
    SymbolTable table;

    // Note: SymbolTable starts with 1 scope (global scope containing builtins)
    SECTION("initial scope depth is 1 (global scope)") {
        CHECK(table.scope_depth() == 1);
    }

    SECTION("push_scope increases depth") {
        std::size_t initial = table.scope_depth();

        table.push_scope();
        CHECK(table.scope_depth() == initial + 1);

        table.push_scope();
        CHECK(table.scope_depth() == initial + 2);

        table.push_scope();
        CHECK(table.scope_depth() == initial + 3);
    }

    SECTION("pop_scope decreases depth") {
        std::size_t initial = table.scope_depth();

        table.push_scope();
        table.push_scope();
        table.push_scope();
        REQUIRE(table.scope_depth() == initial + 3);

        table.pop_scope();
        CHECK(table.scope_depth() == initial + 2);

        table.pop_scope();
        CHECK(table.scope_depth() == initial + 1);

        table.pop_scope();
        CHECK(table.scope_depth() == initial);
    }

    SECTION("push and pop sequence") {
        std::size_t initial = table.scope_depth();

        for (int i = 0; i < 10; ++i) {
            table.push_scope();
            CHECK(table.scope_depth() == initial + static_cast<std::size_t>(i + 1));
        }

        for (int i = 9; i >= 0; --i) {
            table.pop_scope();
            CHECK(table.scope_depth() == initial + static_cast<std::size_t>(i));
        }
    }
}

TEST_CASE("SymbolTable define operations", "[symbol_table]") {
    SymbolTable table;

    SECTION("define adds symbol to current scope") {
        bool ok = table.define_variable("x", 0);
        CHECK(ok);

        auto sym = table.lookup("x");
        REQUIRE(sym.has_value());
        CHECK(sym->name == "x");
        CHECK(sym->kind == SymbolKind::Variable);
        CHECK(sym->buffer_index == 0);
    }

    SECTION("define_variable creates variable symbol") {
        table.define_variable("myVar", 42);

        auto sym = table.lookup("myVar");
        REQUIRE(sym.has_value());
        CHECK(sym->kind == SymbolKind::Variable);
        CHECK(sym->buffer_index == 42);
    }

    SECTION("define_parameter creates parameter symbol") {
        table.push_scope();  // Function scope
        table.define_parameter("param1", 0);
        table.define_parameter("param2", 1);

        auto sym1 = table.lookup("param1");
        auto sym2 = table.lookup("param2");

        REQUIRE(sym1.has_value());
        REQUIRE(sym2.has_value());
        CHECK(sym1->kind == SymbolKind::Parameter);
        CHECK(sym2->kind == SymbolKind::Parameter);
    }

    SECTION("duplicate definition in same scope fails") {
        table.define_variable("dup", 0);
        bool ok = table.define_variable("dup", 1);
        CHECK_FALSE(ok);
    }

    SECTION("same name in different scopes succeeds") {
        table.define_variable("x", 0);
        table.push_scope();
        bool ok = table.define_variable("x", 1);
        CHECK(ok);
    }
}

TEST_CASE("SymbolTable lookup operations", "[symbol_table]") {
    SymbolTable table;

    SECTION("lookup returns empty for undefined symbol") {
        auto sym = table.lookup("undefined");
        CHECK_FALSE(sym.has_value());
    }

    SECTION("lookup searches all scopes (innermost first)") {
        table.define_variable("outer", 0);
        table.push_scope();
        table.define_variable("inner", 1);

        auto outer_sym = table.lookup("outer");
        auto inner_sym = table.lookup("inner");

        REQUIRE(outer_sym.has_value());
        REQUIRE(inner_sym.has_value());
        CHECK(outer_sym->buffer_index == 0);
        CHECK(inner_sym->buffer_index == 1);
    }

    SECTION("lookup by hash") {
        table.define_variable("test_var", 5);

        std::uint32_t hash = fnv1a_hash("test_var");
        auto sym = table.lookup(hash);

        REQUIRE(sym.has_value());
        CHECK(sym->name == "test_var");
        CHECK(sym->buffer_index == 5);
    }

    SECTION("shadowing: inner scope shadows outer") {
        table.define_variable("x", 100);

        table.push_scope();
        table.define_variable("x", 200);

        auto sym = table.lookup("x");
        REQUIRE(sym.has_value());
        CHECK(sym->buffer_index == 200);  // Inner value

        table.pop_scope();

        sym = table.lookup("x");
        REQUIRE(sym.has_value());
        CHECK(sym->buffer_index == 100);  // Outer value restored
    }

    SECTION("is_defined_in_current_scope") {
        table.define_variable("outer", 0);
        CHECK(table.is_defined_in_current_scope("outer"));

        table.push_scope();
        CHECK_FALSE(table.is_defined_in_current_scope("outer"));

        table.define_variable("inner", 1);
        CHECK(table.is_defined_in_current_scope("inner"));
    }
}

TEST_CASE("SymbolTable user functions", "[symbol_table]") {
    SymbolTable table;

    SECTION("define_function creates function symbol") {
        UserFunctionInfo func_info;
        func_info.name = "myFunc";
        func_info.params = {{"a", std::nullopt}, {"b", std::nullopt}};  // 2 params
        func_info.body_node = 42;
        func_info.def_node = 40;

        bool ok = table.define_function(func_info);
        CHECK(ok);

        auto sym = table.lookup("myFunc");
        REQUIRE(sym.has_value());
        CHECK(sym->kind == SymbolKind::UserFunction);
        CHECK(sym->user_function.params.size() == 2);
        CHECK(sym->user_function.body_node == 42);
    }

    SECTION("function shadowing in nested scope") {
        UserFunctionInfo outer_func;
        outer_func.name = "func";
        outer_func.params = {{"x", std::nullopt}};  // 1 param
        outer_func.body_node = 1;
        outer_func.def_node = 0;
        table.define_function(outer_func);

        table.push_scope();

        UserFunctionInfo inner_func;
        inner_func.name = "func";
        inner_func.params = {{"a", std::nullopt}, {"b", std::nullopt}, {"c", std::nullopt}};  // 3 params
        inner_func.body_node = 11;
        inner_func.def_node = 10;
        table.define_function(inner_func);

        auto sym = table.lookup("func");
        REQUIRE(sym.has_value());
        CHECK(sym->user_function.params.size() == 3);  // Inner

        table.pop_scope();

        sym = table.lookup("func");
        REQUIRE(sym.has_value());
        CHECK(sym->user_function.params.size() == 1);  // Outer
    }
}

// ============================================================================
// Edge Cases [symbol_table][edge]
// ============================================================================

TEST_CASE("SymbolTable edge cases", "[symbol_table][edge]") {
    SymbolTable table;

    SECTION("100 nested scopes") {
        std::size_t initial = table.scope_depth();

        for (int i = 0; i < 100; ++i) {
            table.push_scope();
            table.define_variable("level_" + std::to_string(i), static_cast<std::uint16_t>(i));
        }

        CHECK(table.scope_depth() == initial + 100);

        // All symbols should be accessible
        for (int i = 0; i < 100; ++i) {
            auto sym = table.lookup("level_" + std::to_string(i));
            REQUIRE(sym.has_value());
            CHECK(sym->buffer_index == static_cast<std::uint16_t>(i));
        }

        // Pop all scopes
        for (int i = 0; i < 100; ++i) {
            table.pop_scope();
        }

        CHECK(table.scope_depth() == initial);

        // Nested scope symbols should be gone
        for (int i = 0; i < 100; ++i) {
            auto sym = table.lookup("level_" + std::to_string(i));
            CHECK_FALSE(sym.has_value());
        }
    }

    SECTION("many symbols in single scope") {
        for (int i = 0; i < 500; ++i) {
            std::string name = "var_" + std::to_string(i);
            table.define_variable(name, static_cast<std::uint16_t>(i));
        }

        // All should be retrievable
        for (int i = 0; i < 500; ++i) {
            std::string name = "var_" + std::to_string(i);
            auto sym = table.lookup(name);
            REQUIRE(sym.has_value());
            CHECK(sym->buffer_index == static_cast<std::uint16_t>(i));
        }
    }

    SECTION("symbols with colliding hashes") {
        // While FNV-1a is collision-resistant, we test the table handles
        // potential collisions gracefully by using many similar names
        std::vector<std::string> names;
        for (int i = 0; i < 100; ++i) {
            names.push_back("a" + std::to_string(i));
            names.push_back("b" + std::to_string(i));
            names.push_back("c" + std::to_string(i));
        }

        for (std::size_t i = 0; i < names.size(); ++i) {
            table.define_variable(names[i], static_cast<std::uint16_t>(i));
        }

        for (std::size_t i = 0; i < names.size(); ++i) {
            auto sym = table.lookup(names[i]);
            REQUIRE(sym.has_value());
            CHECK(sym->buffer_index == static_cast<std::uint16_t>(i));
        }
    }

    SECTION("empty symbol name") {
        // May or may not be allowed by implementation
        bool ok = table.define_variable("", 0);
        // Just verify no crash
        (void)ok;
    }

    SECTION("very long symbol name") {
        std::string long_name(1000, 'x');
        bool ok = table.define_variable(long_name, 0);
        CHECK(ok);

        auto sym = table.lookup(long_name);
        REQUIRE(sym.has_value());
        CHECK(sym->buffer_index == 0);
    }

    SECTION("pop_scope on empty scope") {
        // At global scope, popping might be a no-op or error
        // Just verify no crash
        table.pop_scope();
    }

    SECTION("lookup after all scopes popped") {
        table.define_variable("global", 0);
        table.push_scope();
        table.define_variable("local", 1);
        table.pop_scope();

        auto global_sym = table.lookup("global");
        auto local_sym = table.lookup("local");

        REQUIRE(global_sym.has_value());
        CHECK_FALSE(local_sym.has_value());
    }
}

// ============================================================================
// FNV-1a Hash Tests [symbol_table]
// ============================================================================

TEST_CASE("SymbolTable FNV-1a hash", "[symbol_table]") {
    SECTION("hash consistency") {
        std::uint32_t h1 = fnv1a_hash("test");
        std::uint32_t h2 = fnv1a_hash("test");
        CHECK(h1 == h2);
    }

    SECTION("different strings produce different hashes") {
        std::uint32_t h1 = fnv1a_hash("foo");
        std::uint32_t h2 = fnv1a_hash("bar");
        std::uint32_t h3 = fnv1a_hash("baz");

        CHECK(h1 != h2);
        CHECK(h2 != h3);
        CHECK(h1 != h3);
    }

    SECTION("hash of empty string") {
        std::uint32_t h = fnv1a_hash("");
        // Should be non-zero (FNV offset basis)
        CHECK(h != 0);
    }

    SECTION("similar strings have different hashes") {
        std::uint32_t h1 = fnv1a_hash("abc");
        std::uint32_t h2 = fnv1a_hash("abd");
        std::uint32_t h3 = fnv1a_hash("aac");

        CHECK(h1 != h2);
        CHECK(h2 != h3);
        CHECK(h1 != h3);
    }
}

// ============================================================================
// RecordTypeInfo Tests [symbol_table]
// ============================================================================

TEST_CASE("RecordTypeInfo find_field", "[symbol_table]") {
    RecordTypeInfo record_type;
    record_type.source_node = 0;
    record_type.fields = {
        {"x", 0, SymbolKind::Variable, nullptr},
        {"y", 1, SymbolKind::Variable, nullptr},
        {"nested", 2, SymbolKind::Record, nullptr}
    };

    SECTION("find existing field") {
        const auto* field = record_type.find_field("x");
        REQUIRE(field != nullptr);
        CHECK(field->name == "x");
        CHECK(field->buffer_index == 0);
    }

    SECTION("find second field") {
        const auto* field = record_type.find_field("y");
        REQUIRE(field != nullptr);
        CHECK(field->name == "y");
        CHECK(field->buffer_index == 1);
    }

    SECTION("find nested record field") {
        const auto* field = record_type.find_field("nested");
        REQUIRE(field != nullptr);
        CHECK(field->name == "nested");
        CHECK(field->field_kind == SymbolKind::Record);
    }

    SECTION("find non-existent field returns nullptr") {
        const auto* field = record_type.find_field("z");
        CHECK(field == nullptr);
    }

    SECTION("find field in empty record") {
        RecordTypeInfo empty_record;
        empty_record.source_node = 0;

        const auto* field = empty_record.find_field("anything");
        CHECK(field == nullptr);
    }
}

TEST_CASE("RecordTypeInfo field_names", "[symbol_table]") {
    SECTION("record with multiple fields") {
        RecordTypeInfo record_type;
        record_type.source_node = 0;
        record_type.fields = {
            {"alpha", 0, SymbolKind::Variable, nullptr},
            {"beta", 1, SymbolKind::Variable, nullptr},
            {"gamma", 2, SymbolKind::Variable, nullptr}
        };

        auto names = record_type.field_names();
        REQUIRE(names.size() == 3);
        CHECK(names[0] == "alpha");
        CHECK(names[1] == "beta");
        CHECK(names[2] == "gamma");
    }

    SECTION("empty record has no field names") {
        RecordTypeInfo empty_record;
        empty_record.source_node = 0;

        auto names = empty_record.field_names();
        CHECK(names.empty());
    }

    SECTION("single field record") {
        RecordTypeInfo record_type;
        record_type.source_node = 0;
        record_type.fields = {
            {"only_field", 0, SymbolKind::Variable, nullptr}
        };

        auto names = record_type.field_names();
        REQUIRE(names.size() == 1);
        CHECK(names[0] == "only_field");
    }
}

// ============================================================================
// Stress Tests [symbol_table][stress]
// ============================================================================

TEST_CASE("SymbolTable stress test", "[symbol_table][stress]") {
    SECTION("simulate compiler with many scopes and symbols") {
        SymbolTable table;
        std::size_t initial = table.scope_depth();

        // Simulate 100 functions
        for (int fn = 0; fn < 100; ++fn) {
            table.push_scope();  // Function scope

            // 5 parameters
            for (int p = 0; p < 5; ++p) {
                std::string param_name = "param_" + std::to_string(fn) + "_" + std::to_string(p);
                table.define_parameter(param_name, static_cast<std::uint16_t>(p));
            }

            // 10 local variables
            for (int v = 0; v < 10; ++v) {
                std::string var_name = "local_" + std::to_string(fn) + "_" + std::to_string(v);
                table.define_variable(var_name, static_cast<std::uint16_t>(v + 5));
            }

            // Nested block
            table.push_scope();
            for (int b = 0; b < 3; ++b) {
                std::string block_var = "block_" + std::to_string(fn) + "_" + std::to_string(b);
                table.define_variable(block_var, static_cast<std::uint16_t>(b + 15));
            }
            table.pop_scope();

            table.pop_scope();
        }

        CHECK(table.scope_depth() == initial);
    }

    SECTION("repeated push/pop cycles") {
        SymbolTable table;

        for (int cycle = 0; cycle < 1000; ++cycle) {
            table.push_scope();
            table.define_variable("temp", 0);
            auto sym = table.lookup("temp");
            REQUIRE(sym.has_value());
            table.pop_scope();

            sym = table.lookup("temp");
            CHECK_FALSE(sym.has_value());
        }
    }

    SECTION("deep nesting with many lookups") {
        SymbolTable table;

        // Create deep nesting with symbols at each level
        for (int depth = 0; depth < 50; ++depth) {
            table.push_scope();
            table.define_variable("depth_" + std::to_string(depth), static_cast<std::uint16_t>(depth));
        }

        // Many lookups from deepest level
        for (int lookup = 0; lookup < 10000; ++lookup) {
            int target = lookup % 50;
            auto sym = table.lookup("depth_" + std::to_string(target));
            REQUIRE(sym.has_value());
            CHECK(sym->buffer_index == static_cast<std::uint16_t>(target));
        }

        // Pop all
        for (int depth = 0; depth < 50; ++depth) {
            table.pop_scope();
        }
    }
}
