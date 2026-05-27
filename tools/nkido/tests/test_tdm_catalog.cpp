// Unit tests for the Tidal Drum Machines catalog parser
// (asset_loader.cpp parse_tdm_catalog). The catalog is the single source of
// truth shared by the CLI and the web app — a parser regression silently
// breaks `.bank("<Machine>")` resolution, so it gets a dedicated test.

#include <catch2/catch_test_macros.hpp>

#include "asset_loader.hpp"
#include "test_paths.hpp"

#include <fstream>
#include <sstream>
#include <string>

using nkido::parse_tdm_catalog;

TEST_CASE("parse_tdm_catalog parses banks, sounds and variants", "[tdm][catalog]") {
    const std::string json = R"({
      "_source": "github:geikha/tidal-drum-machines",
      "_commit": "abc123",
      "_generated": "2026-05-22",
      "_base": "https://raw.githubusercontent.com/geikha/tidal-drum-machines/abc123/machines/",
      "banks": {
        "RolandTR808": {
          "_path": "RolandTR808/",
          "bd": ["rolandtr808-bd/BD0000.WAV", "rolandtr808-bd/BD0010.WAV"],
          "sd": ["rolandtr808-sd/SD.WAV"]
        },
        "LinnDrum": {
          "_path": "LinnDrum/",
          "hh": ["linndrum-hh/Closed Hat.wav"]
        }
      }
    })";

    auto cat = parse_tdm_catalog(json);
    CHECK(cat.commit == "abc123");
    CHECK(cat.base ==
          "https://raw.githubusercontent.com/geikha/tidal-drum-machines/"
          "abc123/machines/");
    REQUIRE(cat.banks.size() == 2);

    REQUIRE(cat.has_bank("RolandTR808"));
    const auto& tr808 = cat.banks.at("RolandTR808");
    CHECK(tr808.path == "RolandTR808/");
    REQUIRE(tr808.sounds.count("bd") == 1);
    REQUIRE(tr808.sounds.at("bd").size() == 2);
    CHECK(tr808.sounds.at("bd")[0] == "rolandtr808-bd/BD0000.WAV");
    CHECK(tr808.sounds.at("bd")[1] == "rolandtr808-bd/BD0010.WAV");
    REQUIRE(tr808.sounds.count("sd") == 1);
    CHECK(tr808.sounds.at("sd").size() == 1);

    REQUIRE(cat.has_bank("LinnDrum"));
    // Filenames with spaces survive parsing verbatim — percent-encoding
    // happens later, only when a GitHub raw URL is built.
    CHECK(cat.banks.at("LinnDrum").sounds.at("hh")[0] ==
          "linndrum-hh/Closed Hat.wav");

    CHECK_FALSE(cat.has_bank("Nonexistent"));
}

TEST_CASE("parse_tdm_catalog rejects malformed input", "[tdm][catalog]") {
    CHECK_THROWS(parse_tdm_catalog("{ not json"));
    CHECK_THROWS(parse_tdm_catalog("[]"));
    // A well-formed object with no `_base` is unusable — URLs cannot be built.
    CHECK_THROWS(parse_tdm_catalog("{}"));
}

TEST_CASE("parse_tdm_catalog tolerates an empty banks object", "[tdm][catalog]") {
    auto cat = parse_tdm_catalog(R"({"_base":"https://x/","banks":{}})");
    CHECK(cat.base == "https://x/");
    CHECK(cat.banks.empty());
}

TEST_CASE("the committed catalog.json parses and is well-formed",
          "[tdm][catalog]") {
    // Guards the output of scripts/generate-tdm-catalog.sh: the in-tree
    // catalog must parse, expose a real commit SHA, and carry the well-known
    // machines that the docs and examples reference.
    std::ifstream f(nkido::test::TDM_CATALOG, std::ios::binary);
    REQUIRE(f.is_open());
    std::stringstream ss;
    ss << f.rdbuf();

    auto cat = parse_tdm_catalog(ss.str());
    CHECK(cat.banks.size() >= 60);
    CHECK(cat.commit.size() == 40);  // full git SHA-1
    CHECK(cat.base.rfind("https://raw.githubusercontent.com/", 0) == 0);

    REQUIRE(cat.has_bank("RolandTR808"));
    REQUIRE(cat.has_bank("RolandTR909"));
    CHECK(cat.banks.at("RolandTR808").sounds.count("bd") == 1);
}
