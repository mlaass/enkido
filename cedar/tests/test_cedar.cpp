#include <catch2/catch_test_macros.hpp>
#include "cedar/cedar.hpp"

TEST_CASE("Cedar initialization", "[cedar]") {
    SECTION("default config") {
        REQUIRE(cedar::init());

        auto& cfg = cedar::config();
        CHECK(cfg.sample_rate == 48000);
        CHECK(cfg.block_size == 128);
        CHECK(cfg.channels == 2);

        cedar::shutdown();
    }

    SECTION("custom config") {
        cedar::Config cfg{
            .sample_rate = 44100,
            .block_size = 256,
            .channels = 2
        };

        REQUIRE(cedar::init(cfg));

        auto& active = cedar::config();
        CHECK(active.sample_rate == 44100);
        CHECK(active.block_size == 256);

        cedar::shutdown();
    }

    SECTION("double init fails") {
        REQUIRE(cedar::init());
        REQUIRE_FALSE(cedar::init());
        cedar::shutdown();
    }
}

TEST_CASE("Cedar version", "[cedar]") {
    CHECK(cedar::Version::major == 0);
    CHECK(cedar::Version::minor == 1);
    CHECK(cedar::Version::patch == 0);
    CHECK(cedar::Version::string() == "0.1.0");
}
