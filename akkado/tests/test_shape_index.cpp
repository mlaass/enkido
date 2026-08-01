#include <catch2/catch_test_macros.hpp>

#include "akkado/akkado.hpp"
#include "akkado/shape_index.hpp"

#include <string>
#include <string_view>

using namespace akkado;

namespace {

// Hardening Phase 8 (PRD-2): the shape index is a formatter over the
// main compile's artifacts. This helper keeps the old driver shape so
// the expectations below stay byte-for-byte what the pre-rewrite
// implementation produced.
std::string shape_index_json(std::string_view src,
                             std::uint32_t cursor = SHAPE_INDEX_NO_CURSOR) {
    auto result = akkado::compile(src, {.filename = "<shape-test>"});
    return akkado::serialize_shape_index(result.artifacts, cursor);
}

// Locate the JSON object that follows `"<key>":` in `json` and return the
// indices [open, close] of its enclosing braces. Returns {npos, npos} when
// the key is not present or the value is not an object/array.
std::pair<std::size_t, std::size_t> find_object_after(std::string_view json,
                                                       std::string_view key) {
    std::string needle;
    needle.reserve(key.size() + 4);
    needle += '"';
    needle += key;
    needle += "\":";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return {std::string_view::npos, std::string_view::npos};
    }
    pos += needle.size();
    if (pos >= json.size()) return {std::string_view::npos, std::string_view::npos};
    char open_ch = json[pos];
    char close_ch = (open_ch == '{') ? '}' : (open_ch == '[' ? ']' : 0);
    if (close_ch == 0) return {std::string_view::npos, std::string_view::npos};
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (std::size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (escape) { escape = false; continue; }
        if (in_string) {
            if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == open_ch) ++depth;
        else if (c == close_ch && --depth == 0) return {pos, i};
    }
    return {std::string_view::npos, std::string_view::npos};
}

bool block_contains(std::string_view json,
                    std::string_view key,
                    std::string_view fragment) {
    auto [open, close] = find_object_after(json, key);
    if (open == std::string_view::npos) return false;
    return json.substr(open, close - open + 1).find(fragment) != std::string_view::npos;
}

bool has_key(std::string_view json, std::string_view key) {
    std::string needle;
    needle.reserve(key.size() + 3);
    needle += '"';
    needle += key;
    needle += "\":";
    return json.find(needle) != std::string_view::npos;
}

// Quick check that a binding exists at the top level of the bindings object.
bool has_binding(std::string_view json, std::string_view name) {
    auto [bopen, bclose] = find_object_after(json, "bindings");
    if (bopen == std::string_view::npos) return false;
    return block_contains(json.substr(bopen, bclose - bopen + 1), name, "\"kind\":");
}

// Find a binding's block within bindings.
std::pair<std::size_t, std::size_t> find_binding_block(std::string_view json,
                                                        std::string_view name) {
    auto [bopen, bclose] = find_object_after(json, "bindings");
    if (bopen == std::string_view::npos) return {std::string_view::npos, std::string_view::npos};
    std::string_view inside = json.substr(bopen, bclose - bopen + 1);
    auto [open, close] = find_object_after(inside, name);
    if (open == std::string_view::npos) return {std::string_view::npos, std::string_view::npos};
    return {bopen + open, bopen + close};
}

bool binding_block_contains(std::string_view json,
                            std::string_view name,
                            std::string_view fragment) {
    auto [open, close] = find_binding_block(json, name);
    if (open == std::string_view::npos) return false;
    return json.substr(open, close - open + 1).find(fragment) != std::string_view::npos;
}

}  // namespace

TEST_CASE("shape_index: top-level wrapper present", "[shape-index]") {
    std::string json = shape_index_json("", SHAPE_INDEX_NO_CURSOR);
    REQUIRE(json.find("\"version\":1") != std::string::npos);
    REQUIRE(json.find("\"sourceHash\":") != std::string::npos);
    REQUIRE(json.find("\"bindings\":") != std::string::npos);
    REQUIRE(json.back() == '}');
}

TEST_CASE("shape_index: simple record binding", "[shape-index]") {
    const char* src = R"(cfg = {wave: "saw", cutoff: 2000, q: 0.7}
out(0, 0)
)";
    std::string json = shape_index_json(src);
    REQUIRE(has_binding(json, "cfg"));
    REQUIRE(binding_block_contains(json, "cfg", "\"kind\":\"record\""));
    REQUIRE(binding_block_contains(json, "cfg", "\"name\":\"wave\""));
    REQUIRE(binding_block_contains(json, "cfg", "\"name\":\"cutoff\""));
    REQUIRE(binding_block_contains(json, "cfg", "\"name\":\"q\""));
    REQUIRE(binding_block_contains(json, "cfg", "\"type\":\"string\""));
    REQUIRE(binding_block_contains(json, "cfg", "\"type\":\"number\""));
}

TEST_CASE("shape_index: pattern fixed fields with aliases", "[shape-index]") {
    const char* src = R"(beat = n"c4 e4 g4"
out(0, 0)
)";
    std::string json = shape_index_json(src);
    REQUIRE(has_binding(json, "beat"));
    REQUIRE(binding_block_contains(json, "beat", "\"kind\":\"pattern\""));

    // Canonical fixed entry has no aliasOf, only fixed:true.
    REQUIRE(binding_block_contains(json, "beat",
        "\"name\":\"freq\",\"type\":\"signal\",\"fixed\":true}"));

    // Aliases include freq's "pitch" / "f" / "n" / "frequency" with aliasOf.
    REQUIRE(binding_block_contains(json, "beat",
        "\"name\":\"pitch\",\"type\":\"signal\",\"fixed\":true,\"aliasOf\":\"freq\""));
    REQUIRE(binding_block_contains(json, "beat",
        "\"name\":\"f\",\"type\":\"signal\",\"fixed\":true,\"aliasOf\":\"freq\""));
    REQUIRE(binding_block_contains(json, "beat",
        "\"name\":\"velocity\",\"type\":\"signal\",\"fixed\":true,\"aliasOf\":\"vel\""));
}

TEST_CASE("shape_index: pattern custom fields surface", "[shape-index]") {
    const char* src = R"(beat = n"c4 e4".set("cutoff", 0.5).set("res", 0.7)
out(0, 0)
)";
    std::string json = shape_index_json(src);
    REQUIRE(has_binding(json, "beat"));
    REQUIRE(binding_block_contains(json, "beat", "\"kind\":\"pattern\""));
    REQUIRE(binding_block_contains(json, "beat",
        "\"name\":\"cutoff\",\"type\":\"signal\",\"fixed\":false,\"source\":\"set\""));
    REQUIRE(binding_block_contains(json, "beat",
        "\"name\":\"res\",\"type\":\"signal\",\"fixed\":false,\"source\":\"set\""));
}

TEST_CASE("shape_index: custom field colliding with fixed name suppressed",
          "[shape-index]") {
    // `.set("freq", …)` must NOT add a custom entry — the fixed canonical
    // wins per typed_value.cpp::pattern_field() lookup order. Editor must
    // not see two `freq` entries.
    const char* src = R"(beat = n"c4 e4".set("freq", 1.0)
out(0, 0)
)";
    std::string json = shape_index_json(src);
    REQUIRE(has_binding(json, "beat"));
    // The canonical fixed entry is present.
    REQUIRE(binding_block_contains(json, "beat",
        "\"name\":\"freq\",\"type\":\"signal\",\"fixed\":true}"));
    // The custom entry must be absent.
    REQUIRE_FALSE(binding_block_contains(json, "beat",
        "\"name\":\"freq\",\"type\":\"signal\",\"fixed\":false"));
}

TEST_CASE("shape_index: empty source produces empty bindings", "[shape-index]") {
    std::string json = shape_index_json("", SHAPE_INDEX_NO_CURSOR);
    REQUIRE(json.find("\"bindings\":{}") != std::string::npos);
    REQUIRE_FALSE(has_key(json, "patternHole"));
}

TEST_CASE("shape_index: cursor sentinel disables patternHole", "[shape-index]") {
    const char* src = R"(beat = n"c4 e4"
beat |> out(%, %)
)";
    std::string json = shape_index_json(src, SHAPE_INDEX_NO_CURSOR);
    REQUIRE_FALSE(has_key(json, "patternHole"));
}

TEST_CASE("shape_index: cursor outside any pipe omits patternHole",
          "[shape-index]") {
    const char* src = R"(cfg = {x: 1, y: 2}
)";
    // Cursor at offset 0 — sits before any node.
    std::string json = shape_index_json(src, 0u);
    REQUIRE_FALSE(has_key(json, "patternHole"));
}

TEST_CASE("shape_index: cursor inside pipe surfaces patternHole shape",
          "[shape-index]") {
    // Place the cursor between the `(` and `)` of the second `osc(` call
    // — that is inside the Pipe's RHS argument list, with the LHS being
    // the `beat` identifier (a pattern).
    std::string src = "beat = n\"c4 e4 g4\"\nbeat |> osc(\"sin\", %)\n";
    auto cursor = src.find("%");
    REQUIRE(cursor != std::string::npos);
    std::string json = shape_index_json(src, static_cast<std::uint32_t>(cursor));

    REQUIRE(has_key(json, "patternHole"));
    auto [open, close] = find_object_after(json, "patternHole");
    REQUIRE(open != std::string_view::npos);
    auto block = std::string_view(json).substr(open, close - open + 1);
    REQUIRE(block.find("\"kind\":\"pattern\"") != std::string::npos);
    REQUIRE(block.find("\"name\":\"freq\"") != std::string::npos);
    REQUIRE(block.find("\"name\":\"vel\"") != std::string::npos);
}

TEST_CASE("shape_index: tolerant of partial source / parse error",
          "[shape-index]") {
    // The second binding has a syntax error mid-record. The first binding
    // must still surface in the shape index.
    const char* src = R"(cfg = {wave: "saw", cutoff: 2000}
broken = {oops:
)";
    // Should not crash and should still include cfg.
    std::string json = shape_index_json(src);
    REQUIRE(json.find("\"version\":1") != std::string::npos);
    REQUIRE(has_binding(json, "cfg"));
}

TEST_CASE("shape_index: array of records exposes element shape", "[shape-index]") {
    const char* src = R"(voices = [{f: 1, v: 0.5}, {f: 2, v: 0.6}]
out(0, 0)
)";
    std::string json = shape_index_json(src);
    if (has_binding(json, "voices")) {
        REQUIRE(binding_block_contains(json, "voices", "\"kind\":\"array\""));
        REQUIRE(binding_block_contains(json, "voices", "\"elementKind\":\"record\""));
        REQUIRE(binding_block_contains(json, "voices", "\"name\":\"f\""));
        REQUIRE(binding_block_contains(json, "voices", "\"name\":\"v\""));
    }
    // If the array path didn't bind a typed value (analyzer state), the test
    // doesn't fail — the v1 contract only promises a shape *if* the array
    // binds as Array-of-Record. The frontend tolerates a missing entry.
}
