// Regression guard for prd-extended-params-migration.
//
// The migration moved per-opcode parameters out of `inst.rate` bit-packing
// (and the seqpat_transport inputs[3]/[4] halving) into the canonical
// ExtendedParams<N> mechanism. This test locks that in: each migrated opcode
// body must contain no `inst.rate` read. If a future change re-introduces
// rate-field packing into one of these opcodes, this test fails.
//
// It deliberately does NOT police the whole opcode space — `inst.rate` is
// still legitimate for enum dispatch and time-unit selection elsewhere
// (see docs/extended-params-mechanism.md §5).

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Extract the body of `inline void op_<name>(...) { ... }` by brace matching.
// Returns the text between the opening and closing brace (exclusive).
std::string extract_function_body(const std::string& src, const std::string& fn_name) {
    const std::string marker = "op_" + fn_name + "(";
    std::size_t pos = src.find(marker);
    REQUIRE(pos != std::string::npos);
    std::size_t brace = src.find('{', pos);
    REQUIRE(brace != std::string::npos);
    int depth = 0;
    std::size_t i = brace;
    for (; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) break;
        }
    }
    REQUIRE(depth == 0);
    return src.substr(brace + 1, i - brace - 1);
}

// Strip // line comments so a comment mentioning "inst.rate" (e.g. a migration
// note) is not mistaken for a real rate-field read.
std::string strip_line_comments(const std::string& body) {
    std::string out;
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        std::size_t slash = line.find("//");
        if (slash != std::string::npos) line = line.substr(0, slash);
        out += line;
        out += '\n';
    }
    return out;
}

bool reads_inst_rate(const std::string& opcodes_dir,
                     const std::string& file, const std::string& fn_name) {
    const std::string src = read_file(opcodes_dir + "/" + file);
    const std::string body = strip_line_comments(extract_function_body(src, fn_name));
    return body.find("inst.rate") != std::string::npos;
}

}  // namespace

TEST_CASE("audit: rate-migrated opcodes read no inst.rate", "[audit][extended-params]") {
    const std::string dir = CEDAR_OPCODES_DIR;

    // (file, opcode-function) pairs whose params moved from inst.rate
    // bit-packing into ExtendedParams<N> by prd-extended-params-migration.
    const std::vector<std::pair<std::string, std::string>> migrated = {
        {"dynamics.hpp", "dynamics_comp"},
        {"dynamics.hpp", "dynamics_limiter"},
        {"dynamics.hpp", "dynamics_gate"},
        {"reverbs.hpp",  "reverb_dattorro"},
        {"modulation.hpp", "effect_flanger"},
        {"modulation.hpp", "effect_comb"},
    };

    for (const auto& [file, fn] : migrated) {
        INFO("op_" << fn << " in " << file
             << " must not read inst.rate after the ExtendedParams migration");
        CHECK_FALSE(reads_inst_rate(dir, file, fn));
    }
}

TEST_CASE("audit: seqpat_transport no longer halves cycle_length into inputs[]",
          "[audit][extended-params]") {
    // cycle_length used to be bit_cast into the inputs[3]/[4] slot pair.
    // After the migration it lives in an ExtendedParams<1> slot.
    const std::string dir = CEDAR_OPCODES_DIR;
    const std::string src = read_file(dir + "/sequencing.hpp");
    const std::string body =
        strip_line_comments(extract_function_body(src, "seqpat_transport"));
    INFO("op_seqpat_transport must not bit_cast inputs[] into cycle_length");
    CHECK(body.find("bit_cast") == std::string::npos);
    CHECK(body.find("inst.inputs[3]") == std::string::npos);
    CHECK(body.find("inst.inputs[4]") == std::string::npos);
}
