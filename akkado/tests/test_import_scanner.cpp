#include <catch2/catch_test_macros.hpp>
#include "akkado/import_scanner.hpp"
#include "akkado/file_resolver.hpp"

// Helper to check if a diagnostic with a specific code exists
static bool has_diagnostic_code(const std::vector<akkado::Diagnostic>& diagnostics, const std::string& code) {
    for (const auto& d : diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

TEST_CASE("scan_imports basic", "[import_scanner]") {
    SECTION("single import") {
        auto directives = akkado::scan_imports(R"(import "utils")");
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "utils");
        CHECK(directives[0].alias.empty());
        CHECK(directives[0].line_number == 1);
    }

    SECTION("import with as alias") {
        auto directives = akkado::scan_imports(R"(import "utils" as u)");
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "utils");
        CHECK(directives[0].alias == "u");
    }

    SECTION("multiple imports") {
        auto directives = akkado::scan_imports(
            "import \"utils\"\n"
            "import \"filters\"\n"
        );
        REQUIRE(directives.size() == 2);
        CHECK(directives[0].path == "utils");
        CHECK(directives[0].line_number == 1);
        CHECK(directives[1].path == "filters");
        CHECK(directives[1].line_number == 2);
    }

    SECTION("comments before imports are skipped") {
        auto directives = akkado::scan_imports(
            "// This is a comment\n"
            "import \"utils\"\n"
        );
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "utils");
        CHECK(directives[0].line_number == 2);
    }

    SECTION("blank lines before imports are skipped") {
        auto directives = akkado::scan_imports(
            "\n"
            "\n"
            "import \"utils\"\n"
        );
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "utils");
        CHECK(directives[0].line_number == 3);
    }

    SECTION("non-import line stops scanning") {
        auto directives = akkado::scan_imports(
            "import \"utils\"\n"
            "x = 42\n"
            "import \"late\"\n"
        );
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "utils");
    }

    SECTION("no imports returns empty") {
        auto directives = akkado::scan_imports("x = 42\nsaw(440)");
        CHECK(directives.empty());
    }

    SECTION("import with relative path") {
        auto directives = akkado::scan_imports(R"(import "./utils")");
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "./utils");
    }
}

TEST_CASE("resolve_imports basic", "[import_scanner]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("utils", "fn double(x) -> x * 2");

    SECTION("single import resolves") {
        auto result = akkado::resolve_imports(
            "import \"utils\"\ndouble(220)",
            "main.ak", resolver);
        REQUIRE(result.success);
        REQUIRE(result.modules.size() == 1);
        CHECK(result.modules[0].canonical_path == "utils");
        CHECK(result.modules[0].source == "fn double(x) -> x * 2");
    }

    SECTION("root source has import lines blanked") {
        auto result = akkado::resolve_imports(
            "import \"utils\"\ndouble(220)",
            "main.ak", resolver);
        REQUIRE(result.success);
        // Import line should be blanked (spaces + preserved newline)
        CHECK(result.root_source[0] == ' ');  // 'i' -> ' '
        CHECK(result.root_source.find('\n') != std::string::npos);
        // Code after import should be unchanged
        auto code_start = result.root_source.find("double");
        CHECK(code_start != std::string::npos);
    }

    SECTION("no imports returns unchanged source") {
        auto result = akkado::resolve_imports(
            "saw(440)", "main.ak", resolver);
        REQUIRE(result.success);
        CHECK(result.modules.empty());
        CHECK(result.root_source == "saw(440)");
    }
}

TEST_CASE("resolve_imports transitive", "[import_scanner]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("base", "fn identity(x) -> x");
    resolver.register_module("utils", "import \"base\"\nfn double(x) -> x * 2");

    auto result = akkado::resolve_imports(
        "import \"utils\"\ndouble(220)",
        "main.ak", resolver);
    REQUIRE(result.success);
    // base comes before utils in topo order
    REQUIRE(result.modules.size() == 2);
    CHECK(result.modules[0].canonical_path == "base");
    CHECK(result.modules[1].canonical_path == "utils");
}

TEST_CASE("resolve_imports deduplication", "[import_scanner]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("shared", "fn helper() -> 42");
    resolver.register_module("a", "import \"shared\"\nfn fa() -> helper()");
    resolver.register_module("b", "import \"shared\"\nfn fb() -> helper()");

    auto result = akkado::resolve_imports(
        "import \"a\"\nimport \"b\"\nfa() + fb()",
        "main.ak", resolver);
    REQUIRE(result.success);
    // shared should appear only once
    int shared_count = 0;
    for (const auto& m : result.modules) {
        if (m.canonical_path == "shared") ++shared_count;
    }
    CHECK(shared_count == 1);
}

TEST_CASE("resolve_imports circular dependency", "[import_scanner]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("a", "import \"b\"\nfn fa() -> 1");
    resolver.register_module("b", "import \"a\"\nfn fb() -> 2");

    auto result = akkado::resolve_imports(
        "import \"a\"\nfa()",
        "main.ak", resolver);
    CHECK_FALSE(result.success);
    CHECK(has_diagnostic_code(result.diagnostics, "E500"));
}

TEST_CASE("resolve_imports module not found", "[import_scanner]") {
    akkado::VirtualResolver resolver;

    auto result = akkado::resolve_imports(
        "import \"nonexistent\"\n42",
        "main.ak", resolver);
    CHECK_FALSE(result.success);
    CHECK(has_diagnostic_code(result.diagnostics, "E502"));
}

TEST_CASE("resolve_imports populates namespaced_imports", "[import_scanner][namespace]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("filters", "fn lp(sg, freq) -> sg");
    resolver.register_module("utils", "fn helper() -> 42");

    SECTION("as alias populates namespaced_imports") {
        auto result = akkado::resolve_imports(
            "import \"filters\" as f\nsaw(440)",
            "main.ak", resolver);
        REQUIRE(result.success);
        REQUIRE(result.namespaced_imports.size() == 1);
        CHECK(result.namespaced_imports[0].canonical_path == "filters");
        CHECK(result.namespaced_imports[0].alias == "f");
    }

    SECTION("direct import has empty namespaced_imports") {
        auto result = akkado::resolve_imports(
            "import \"utils\"\nhelper()",
            "main.ak", resolver);
        REQUIRE(result.success);
        CHECK(result.namespaced_imports.empty());
    }

    SECTION("mixed direct and namespaced") {
        auto result = akkado::resolve_imports(
            "import \"utils\"\nimport \"filters\" as f\nhelper()",
            "main.ak", resolver);
        REQUIRE(result.success);
        REQUIRE(result.namespaced_imports.size() == 1);
        CHECK(result.namespaced_imports[0].canonical_path == "filters");
        CHECK(result.namespaced_imports[0].alias == "f");
        // Both modules should be in the modules list
        CHECK(result.modules.size() == 2);
    }
}

TEST_CASE("resolve_imports blanking preserves byte offsets", "[import_scanner]") {
    akkado::VirtualResolver resolver;
    resolver.register_module("utils", "fn helper() -> 42");

    std::string source = "import \"utils\"\nsaw(440)";
    auto result = akkado::resolve_imports(source, "main.ak", resolver);
    REQUIRE(result.success);

    // Blanked source should have same length as original
    CHECK(result.root_source.size() == source.size());
    // The newline should be preserved
    CHECK(result.root_source[source.find('\n')] == '\n');
}

// ============================================================================
// Group 11: Scanner namespace edge cases
// ============================================================================

TEST_CASE("scan_imports namespace edge cases", "[import_scanner][namespace]") {
    SECTION("alias with underscore") {
        auto directives = akkado::scan_imports(R"(import "mod" as my_mod)");
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "mod");
        CHECK(directives[0].alias == "my_mod");
    }

    SECTION("alias with digits") {
        auto directives = akkado::scan_imports(R"(import "mod" as m2)");
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "mod");
        CHECK(directives[0].alias == "m2");
    }

    SECTION("two namespaced imports") {
        auto directives = akkado::scan_imports(
            "import \"a\" as x\n"
            "import \"b\" as y\n"
        );
        REQUIRE(directives.size() == 2);
        CHECK(directives[0].path == "a");
        CHECK(directives[0].alias == "x");
        CHECK(directives[1].path == "b");
        CHECK(directives[1].alias == "y");
    }

    SECTION("namespace with relative path") {
        auto directives = akkado::scan_imports(R"(import "./utils" as u)");
        REQUIRE(directives.size() == 1);
        CHECK(directives[0].path == "./utils");
        CHECK(directives[0].alias == "u");
    }
}
