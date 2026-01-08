#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"

TEST_CASE("Akkado compilation", "[akkado]") {
    SECTION("empty source produces error") {
        auto result = akkado::compile("");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.diagnostics.size() >= 1);
        CHECK(result.diagnostics[0].severity == akkado::Severity::Error);
        CHECK(result.diagnostics[0].code == "E001");
    }

    SECTION("placeholder implementation") {
        auto result = akkado::compile("// test");

        // Currently not implemented, so should fail
        REQUIRE_FALSE(result.success);
    }
}

TEST_CASE("Akkado version", "[akkado]") {
    CHECK(akkado::Version::major == 0);
    CHECK(akkado::Version::minor == 1);
    CHECK(akkado::Version::patch == 0);
    CHECK(akkado::Version::string() == "0.1.0");
}
