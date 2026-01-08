#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "akkado/diagnostics.hpp"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("Diagnostic formatting", "[diagnostics]") {
    akkado::Diagnostic diag{
        .severity = akkado::Severity::Error,
        .code = "E001",
        .message = "unexpected token",
        .filename = "test.ak",
        .location = {.line = 5, .column = 10, .offset = 50, .length = 3}
    };

    SECTION("terminal format includes location") {
        std::string source = "line1\nline2\nline3\nline4\nlet x = 42;\nline6";
        auto output = akkado::format_diagnostic(diag, source);

        CHECK_THAT(output, ContainsSubstring("test.ak:5:10"));
        CHECK_THAT(output, ContainsSubstring("error"));
        CHECK_THAT(output, ContainsSubstring("E001"));
        CHECK_THAT(output, ContainsSubstring("unexpected token"));
    }

    SECTION("JSON format") {
        auto json = akkado::format_diagnostic_json(diag);

        CHECK_THAT(json, ContainsSubstring(R"("severity":"error")"));
        CHECK_THAT(json, ContainsSubstring(R"("code":"E001")"));
        CHECK_THAT(json, ContainsSubstring(R"("line":4)")); // 0-indexed
        CHECK_THAT(json, ContainsSubstring(R"("character":9)")); // 0-indexed
    }
}

TEST_CASE("has_errors helper", "[diagnostics]") {
    SECTION("empty list has no errors") {
        std::vector<akkado::Diagnostic> diags;
        CHECK_FALSE(akkado::has_errors(diags));
    }

    SECTION("warnings are not errors") {
        std::vector<akkado::Diagnostic> diags{
            {.severity = akkado::Severity::Warning},
            {.severity = akkado::Severity::Info}
        };
        CHECK_FALSE(akkado::has_errors(diags));
    }

    SECTION("detects errors") {
        std::vector<akkado::Diagnostic> diags{
            {.severity = akkado::Severity::Warning},
            {.severity = akkado::Severity::Error}
        };
        CHECK(akkado::has_errors(diags));
    }
}
