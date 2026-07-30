#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include "akkado/file_resolver.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstring>
#include <unordered_set>

// Helper to check if a diagnostic with a specific code exists
static bool has_diagnostic_code(const std::vector<akkado::Diagnostic>& diagnostics, const std::string& code) {
    for (const auto& d : diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

// Helper to decode float from PUSH_CONST instruction
static float decode_const_float(const cedar::Instruction& inst) {
    float value;
    std::memcpy(&value, &inst.state_id, sizeof(float));
    return value;
}

TEST_CASE("Import integration: compile with resolver", "[import]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("utils", "fn double(x) -> x * 2");

    SECTION("function from imported module is available") {
        auto result = akkado::compile("import \"utils\"\ndouble(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
        CHECK(result.program.bytecode.size() > 0);
    }

    SECTION("transitive imports work") {
        resolver.register_module("base", "fn identity(x) -> x");
        resolver.register_module("mid", "import \"base\"\nfn wrap(x) -> identity(x)");

        auto result = akkado::compile("import \"mid\"\nwrap(440)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("circular import produces E500") {
        resolver.register_module("a", "import \"b\"\nfn fa() -> 1");
        resolver.register_module("b", "import \"a\"\nfn fb() -> 2");

        auto result = akkado::compile("import \"a\"\nfa()", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E500"));
    }

    SECTION("missing module produces E502") {
        auto result = akkado::compile("import \"nonexistent\"\n42", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E502"));
    }
}

TEST_CASE("Import integration: no resolver", "[import]") {
    SECTION("import without resolver produces E505") {
        auto result = akkado::compile("import \"utils\"\n42", {.filename = "main.ak"});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E505"));
    }

    SECTION("no imports without resolver works normally") {
        auto result = akkado::compile("42", {.filename = "main.ak"});
        REQUIRE(result.success);
        CHECK(result.program.bytecode.size() == sizeof(cedar::Instruction));
    }
}

TEST_CASE("Import integration: backward compatibility", "[import]") {
    SECTION("existing code compiles identically without resolver") {
        auto result = akkado::compile("saw(440)");
        REQUIRE(result.success);
        CHECK(result.program.bytecode.size() == 2 * sizeof(cedar::Instruction));
    }

    SECTION("existing code compiles identically with empty resolver") {
        akkado::VirtualResolver resolver;
        auto result = akkado::compile("saw(440)", {.resolver = &resolver});
        REQUIRE(result.success);
        CHECK(result.program.bytecode.size() == 2 * sizeof(cedar::Instruction));
    }
}

TEST_CASE("Import integration: error location mapping", "[import]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("utils", "fn double(x) -> x * 2");

    SECTION("error in user code has correct line number") {
        auto result = akkado::compile("import \"utils\"\nundefined_fn(440)", {.filename = "main.ak", .resolver = &resolver});
        // Should fail because undefined_fn doesn't exist
        // The error should point to line 2, not some offset line
        CHECK_FALSE(result.success);
        bool found_user_error = false;
        for (const auto& d : result.diagnostics) {
            if (d.filename == "main.ak" && d.location.line == 2) {
                found_user_error = true;
            }
        }
        CHECK(found_user_error);
    }
}

TEST_CASE("Import integration: parser E501 for late import", "[import]") {
    // When no resolver is used and import appears after code,
    // the parser should report E501
    SECTION("import after code produces E501") {
        auto result = akkado::compile("42\nimport \"utils\"");
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E501"));
    }
}

TEST_CASE("Import integration: imported definitions accessible", "[import]") {
    akkado::VirtualResolver resolver;

    SECTION("imported constant is accessible") {
        resolver.register_module("constants", "pi = 3.14159");
        auto result = akkado::compile("import \"constants\"\npi", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("multiple imports all available") {
        resolver.register_module("mod_a", "fn fa() -> 100");
        resolver.register_module("mod_b", "fn fb() -> 200");
        auto result = akkado::compile("import \"mod_a\"\nimport \"mod_b\"\nfa() + fb()", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }
}

TEST_CASE("Namespace import: function call via alias", "[import][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("mymod", "fn double(x) -> x * 2\nfn triple(x) -> x * 3");

    SECTION("m.double(220) compiles") {
        auto result = akkado::compile("import \"mymod\" as m\nm.double(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
        CHECK(result.program.bytecode.size() > 0);
    }

    SECTION("m.triple(220) compiles") {
        auto result = akkado::compile("import \"mymod\" as m\nm.triple(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }
}

TEST_CASE("Namespace import: hides unqualified access", "[import][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("mymod", "fn double(x) -> x * 2");

    SECTION("unqualified double() fails after namespace import") {
        auto result = akkado::compile("import \"mymod\" as m\ndouble(220)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        // Should get E004 (unknown function) or E005 (undefined identifier)
        bool found_error = has_diagnostic_code(result.diagnostics, "E004")
                        || has_diagnostic_code(result.diagnostics, "E005");
        CHECK(found_error);
    }
}

TEST_CASE("Namespace import: E504 for nonexistent member", "[import][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("mymod", "fn double(x) -> x * 2");

    SECTION("m.nonexistent() produces E504") {
        auto result = akkado::compile("import \"mymod\" as m\nm.nonexistent(220)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E504"));
    }
}

TEST_CASE("Namespace import: variable/constant access via alias", "[import][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("constants", "my_pi = 3.14159\nmy_tau = 6.28318");

    SECTION("c.my_pi compiles as field access") {
        auto result = akkado::compile("import \"constants\" as c\nc.my_pi", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("unqualified my_pi fails") {
        auto result = akkado::compile("import \"constants\" as c\nmy_pi", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("Namespace import: mixed direct and namespaced", "[import][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("mod_a", "fn fa() -> 100");
    resolver.register_module("mod_b", "fn fb() -> 200");

    SECTION("direct import visible, namespaced hidden") {
        auto result = akkado::compile("import \"mod_a\"\nimport \"mod_b\" as b\nfa() + b.fb()", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("namespaced fb() not accessible unqualified") {
        auto result = akkado::compile("import \"mod_a\"\nimport \"mod_b\" as b\nfa() + fb()", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("Namespace import: same bytecode as direct import", "[import][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("utils", "fn double(x) -> x * 2");

    SECTION("f.double(220) produces same bytecode as direct double(220)") {
        auto direct = akkado::compile("import \"utils\"\ndouble(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(direct.success);

        auto namespaced = akkado::compile("import \"utils\" as u\nu.double(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(namespaced.success);

        // Bytecodes should have the same size (same operations)
        CHECK(direct.program.bytecode.size() == namespaced.program.bytecode.size());
    }
}

// ============================================================================
// Group 1: Direct import — all definition types
// ============================================================================

TEST_CASE("Direct import: all definition types", "[import][direct]") {
    akkado::VirtualResolver resolver;

    SECTION("variable") {
        resolver.register_module("mod", "my_val = 440");
        auto result = akkado::compile("import \"mod\"\nmy_val", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("pattern") {
        resolver.register_module("mod", "my_pat = n\"c4 e4 g4\"");
        auto result = akkado::compile("import \"mod\"\nmy_pat.freq", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("array") {
        resolver.register_module("mod", "my_arr = [1, 2, 3]");
        auto result = akkado::compile("import \"mod\"\nmy_arr", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("record") {
        resolver.register_module("mod", "my_rec = {freq: 440, vel: 0.8}");
        auto result = akkado::compile("import \"mod\"\nmy_rec.freq", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("lambda") {
        resolver.register_module("mod", "my_fn = (x) -> x * 2");
        auto result = akkado::compile("import \"mod\"\nmy_fn(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("const") {
        resolver.register_module("mod", "const MY_C = 3.14159");
        auto result = akkado::compile("import \"mod\"\nMY_C", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("function") {
        resolver.register_module("mod", "fn double(x) -> x * 2");
        auto result = akkado::compile("import \"mod\"\ndouble(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }
}

// ============================================================================
// Group 2: Namespace import — all definition types
// ============================================================================

TEST_CASE("Namespace import: all definition types", "[import][namespace]") {
    akkado::VirtualResolver resolver;

    SECTION("variable") {
        resolver.register_module("mod", "my_val = 440");
        auto result = akkado::compile("import \"mod\" as m\nm.my_val", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("pattern field") {
        resolver.register_module("mod", "my_pat = n\"c4 e4 g4\"");
        auto result = akkado::compile("import \"mod\" as m\nm.my_pat.freq", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("array") {
        resolver.register_module("mod", "my_arr = [1, 2, 3]");
        auto result = akkado::compile("import \"mod\" as m\nm.my_arr", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("record field") {
        resolver.register_module("mod", "my_rec = {freq: 440, vel: 0.8}");
        auto result = akkado::compile("import \"mod\" as m\nm.my_rec.freq", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("lambda call") {
        resolver.register_module("mod", "my_fn = (x) -> x * 2");
        auto result = akkado::compile("import \"mod\" as m\nm.my_fn(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("const") {
        resolver.register_module("mod", "const MY_C = 3.14159");
        auto result = akkado::compile("import \"mod\" as m\nm.MY_C", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("function call") {
        resolver.register_module("mod", "fn double(x) -> x * 2");
        auto result = akkado::compile("import \"mod\" as m\nm.double(220)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }
}

// ============================================================================
// Group 3: Namespace hiding — all definition types
// ============================================================================

TEST_CASE("Namespace hiding: all definition types", "[import][namespace]") {
    akkado::VirtualResolver resolver;

    SECTION("variable hidden") {
        resolver.register_module("mod", "my_val = 440");
        auto result = akkado::compile("import \"mod\" as m\nmy_val", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E005"));
    }

    SECTION("pattern hidden") {
        resolver.register_module("mod", "my_pat = n\"c4 e4 g4\"");
        auto result = akkado::compile("import \"mod\" as m\nmy_pat", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E005"));
    }

    SECTION("array hidden") {
        resolver.register_module("mod", "my_arr = [1, 2, 3]");
        auto result = akkado::compile("import \"mod\" as m\nmy_arr", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E005"));
    }

    SECTION("record hidden") {
        resolver.register_module("mod", "my_rec = {freq: 440, vel: 0.8}");
        auto result = akkado::compile("import \"mod\" as m\nmy_rec", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E005"));
    }

    SECTION("lambda hidden") {
        resolver.register_module("mod", "my_fn = (x) -> x * 2");
        auto result = akkado::compile("import \"mod\" as m\nmy_fn(220)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E004"));
    }

    SECTION("const hidden") {
        resolver.register_module("mod", "const MY_C = 3.14159");
        auto result = akkado::compile("import \"mod\" as m\nMY_C", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E005"));
    }

    SECTION("function hidden") {
        resolver.register_module("mod", "fn double(x) -> x * 2");
        auto result = akkado::compile("import \"mod\" as m\ndouble(220)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E004"));
    }
}

// ============================================================================
// Group 4: Namespace functions in expressions and pipes
// ============================================================================

TEST_CASE("Namespace import: functions in expressions and pipes", "[import][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("math", "fn double(x) -> x * 2\nfn triple(x) -> x * 3");

    SECTION("binary expr") {
        auto result = akkado::compile("import \"math\" as m\nm.double(100) + m.double(200)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("as builtin arg") {
        auto result = akkado::compile("import \"math\" as m\nsaw(m.double(220))", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("pipe from namespace call") {
        auto result = akkado::compile("import \"math\" as m\nm.double(220) |> saw(%)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("nested call") {
        auto result = akkado::compile("import \"math\" as m\nm.double(m.triple(10))", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("multiply with namespace result") {
        auto result = akkado::compile("import \"math\" as m\nsaw(440) * m.double(0.5)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }
}

// ============================================================================
// Group 5: Namespace variables in expressions
// ============================================================================

TEST_CASE("Namespace import: variables in expressions", "[import][namespace]") {
    akkado::VirtualResolver resolver;

    SECTION("binary expr") {
        resolver.register_module("mod", "my_freq = 440");
        auto result = akkado::compile("import \"mod\" as m\nm.my_freq + 220", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("as function arg") {
        resolver.register_module("mod", "my_freq = 440");
        auto result = akkado::compile("import \"mod\" as m\nsaw(m.my_freq)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("pipe multiply") {
        resolver.register_module("mod", "my_gain = 0.5");
        auto result = akkado::compile("import \"mod\" as m\nsaw(440) |> % * m.my_gain", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("const in expr") {
        resolver.register_module("mod", "const MY_C = 3.14159");
        auto result = akkado::compile("import \"mod\" as m\nm.MY_C * 2", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("multiple vars") {
        resolver.register_module("mod", "my_freq = 440\nmy_gain = 0.8");
        auto result = akkado::compile("import \"mod\" as m\nsaw(m.my_freq) * m.my_gain", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }
}

// ============================================================================
// Group 6: E504 — all access forms
// ============================================================================

TEST_CASE("Namespace import: E504 all access forms", "[import][namespace]") {
    akkado::VirtualResolver resolver;

    SECTION("nonexistent call") {
        resolver.register_module("mod", "fn double(x) -> x * 2");
        auto result = akkado::compile("import \"mod\" as m\nm.nonexist(220)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E504"));
    }

    SECTION("nonexistent field") {
        resolver.register_module("mod", "my_val = 440");
        auto result = akkado::compile("import \"mod\" as m\nm.nonexist", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E504"));
    }

    SECTION("typo in name") {
        resolver.register_module("mod", "fn double(x) -> x * 2");
        auto result = akkado::compile("import \"mod\" as m\nm.doublee(220)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E504"));
    }

    SECTION("empty module") {
        resolver.register_module("mod", "42");
        auto result = akkado::compile("import \"mod\" as m\nm.anything", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E504"));
    }
}

// ============================================================================
// Group 7: E503 — transitive read failure
// ============================================================================

// Custom resolver where resolve() succeeds but read() can fail
class FailReadResolver : public akkado::FileResolver {
    std::unordered_map<std::string, std::string> modules_;
    std::unordered_set<std::string> fail_reads_;
public:
    void add(std::string path, std::string source) { modules_[path] = source; }
    void fail_read(std::string path) { modules_[path] = ""; fail_reads_.insert(path); }

    std::optional<std::string> resolve(
        std::string_view p, std::string_view) const override {
        std::string k(p);
        return modules_.count(k) ? std::optional{k} : std::nullopt;
    }

    std::optional<std::string> read(
        std::string_view p) const override {
        std::string k(p);
        if (fail_reads_.count(k)) return std::nullopt;
        auto it = modules_.find(k);
        return it != modules_.end() ? std::optional{it->second} : std::nullopt;
    }
};

TEST_CASE("Import: E503 read failure", "[import]") {
    SECTION("direct read failure") {
        FailReadResolver resolver;
        resolver.fail_read("broken");
        auto result = akkado::compile("import \"broken\"\n42", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E503"));
    }

    SECTION("transitive read failure") {
        FailReadResolver resolver;
        resolver.add("mid", "import \"deep\"\nfn wrap(x) -> x");
        resolver.fail_read("deep");
        auto result = akkado::compile("import \"mid\"\n42", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E503"));
    }
}

// ============================================================================
// Group 8: Edge cases
// ============================================================================

TEST_CASE("Import edge cases", "[import][namespace]") {
    akkado::VirtualResolver resolver;

    SECTION("dual import: namespace access works") {
        resolver.register_module("mod", "fn double(x) -> x * 2\nmy_val = 100");
        // When same module is imported both direct and as namespace,
        // the namespace import hides direct defs; namespace access still works
        auto result = akkado::compile("import \"mod\"\nimport \"mod\" as m\nm.double(220) + m.my_val", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("two aliases from different modules") {
        resolver.register_module("mod_a", "fn fa(x) -> x * 2");
        resolver.register_module("mod_b", "fn fb(x) -> x * 3");
        auto result = akkado::compile("import \"mod_a\" as a\nimport \"mod_b\" as b\na.fa(b.fb(10))", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("different fns in two namespaced modules") {
        resolver.register_module("mod_a", "fn scale(x) -> x * 2");
        resolver.register_module("mod_b", "fn shift(x) -> x + 3");
        auto result = akkado::compile("import \"mod_a\" as a\nimport \"mod_b\" as b\na.scale(b.shift(10))", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("transitive defs leak through namespace import") {
        resolver.register_module("base", "fn base_fn(x) -> x");
        resolver.register_module("mid", "import \"base\"\nfn mid_fn(x) -> base_fn(x)");
        // base_fn is globally injected (transitive direct import from mid),
        // so it should be available even though mid is namespace-imported
        auto result = akkado::compile("import \"mid\" as m\nbase_fn(440)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("mid's own defs hidden via namespace") {
        resolver.register_module("base", "fn base_fn(x) -> x");
        resolver.register_module("mid", "import \"base\"\nfn mid_fn(x) -> base_fn(x)");
        auto result = akkado::compile("import \"mid\" as m\nmid_fn(440)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        CHECK(has_diagnostic_code(result.diagnostics, "E004"));
    }

    SECTION("mid's own defs via alias") {
        resolver.register_module("base", "fn base_fn(x) -> x");
        resolver.register_module("mid", "import \"base\"\nfn mid_fn(x) -> base_fn(x)");
        auto result = akkado::compile("import \"mid\" as m\nm.mid_fn(440)", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(result.success);
    }

    SECTION("later direct import wins on name collision") {
        resolver.register_module("mod_a", "fn helper(x) -> x * 2");
        resolver.register_module("mod_b", "fn helper(x) -> x * 3");
        auto result = akkado::compile("import \"mod_a\"\nimport \"mod_b\"\nhelper(10)", {.filename = "main.ak", .resolver = &resolver});
        // Should succeed — mod_b's definition wins since it's later in topo order
        REQUIRE(result.success);
    }
}

// ============================================================================
// Group 9: Error location mapping
// ============================================================================

TEST_CASE("Import: error location mapping", "[import][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("mod", "fn double(x) -> x * 2");

    SECTION("E504 on correct line") {
        auto result = akkado::compile("import \"mod\" as m\nm.nonexist(220)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E504" && d.location.line == 2 && d.filename == "main.ak") {
                found = true;
            }
        }
        CHECK(found);
    }

    SECTION("E004 on correct line") {
        auto result = akkado::compile("import \"mod\" as m\ndouble(220)", {.filename = "main.ak", .resolver = &resolver});
        CHECK_FALSE(result.success);
        bool found = false;
        for (const auto& d : result.diagnostics) {
            if (d.code == "E004" && d.location.line == 2 && d.filename == "main.ak") {
                found = true;
            }
        }
        CHECK(found);
    }
}

// ============================================================================
// Group 10: Bytecode equivalence
// ============================================================================

TEST_CASE("Import: bytecode equivalence for variables and consts", "[import][namespace]") {
    akkado::VirtualResolver resolver;

    SECTION("variable") {
        resolver.register_module("mod", "my_val = 440");

        auto direct = akkado::compile("import \"mod\"\nmy_val", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(direct.success);

        auto namespaced = akkado::compile("import \"mod\" as m\nm.my_val", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(namespaced.success);

        CHECK(direct.program.bytecode.size() == namespaced.program.bytecode.size());
    }

    SECTION("const") {
        resolver.register_module("mod", "const MY_C = 3.14159");

        auto direct = akkado::compile("import \"mod\"\nMY_C", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(direct.success);

        auto namespaced = akkado::compile("import \"mod\" as m\nm.MY_C", {.filename = "main.ak", .resolver = &resolver});
        REQUIRE(namespaced.success);

        CHECK(direct.program.bytecode.size() == namespaced.program.bytecode.size());
    }
}
