// Phase 0 of prd-parser-codegen-correctness.md: per-fixture bytecode
// disassembly snapshot harness. Compiles every .ak under
// akkado/tests/fixtures/ and diffs the disassembly against the checked-in
// .disasm file under akkado/tests/snapshots/. Regenerate baselines with
//
//     NKIDO_UPDATE_SNAPSHOTS=1 ./build/akkado/tests/akkado_tests "[snapshot]"
//
// Intentional bytecode changes (e.g. Phase 2's right-assoc ^ fix) are
// reviewed as snapshot diffs in the same PR that ships the behavior change.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include "bytecode_dump.hpp"
#include <cedar/vm/instruction.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::vector<cedar::Instruction> decode_instructions(const std::vector<std::uint8_t>& bytecode) {
    std::vector<cedar::Instruction> insts;
    const std::size_t count = bytecode.size() / sizeof(cedar::Instruction);
    insts.resize(count);
    if (count > 0) {
        std::memcpy(insts.data(), bytecode.data(), count * sizeof(cedar::Instruction));
    }
    return insts;
}

std::string render_snapshot(const std::filesystem::path& fixture_path,
                            const akkado::CompileResult& result) {
    const auto insts = decode_instructions(result.bytecode);

    std::ostringstream out;
    out << "# fixture: " << fixture_path.filename().string() << "\n";
    out << "# main: " << result.main_instruction_count
        << ", blocks: " << result.block_table.size()
        << ", total: " << insts.size()
        << "\n";

    if (!result.block_table.empty()) {
        out << "# block table:\n";
        for (std::size_t i = 0; i < result.block_table.size(); ++i) {
            const auto& b = result.block_table[i];
            out << "#   block " << i
                << ": body=[" << b.offset << ".." << (b.offset + b.length) << ")"
                << ", frame_slots=" << b.frame_slot_count
                << ", outputs=" << static_cast<int>(b.output_count)
                << "\n";
        }
    }

    out << "\n";
    out << nkido::format_program(std::span<const cedar::Instruction>(insts.data(), insts.size()));
    return out.str();
}

bool update_mode_enabled() {
    const char* v = std::getenv("NKIDO_UPDATE_SNAPSHOTS");
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

}  // namespace

TEST_CASE("bytecode disassembly snapshots", "[snapshot]") {
    const std::filesystem::path fixtures_dir{AKKADO_TEST_FIXTURES_DIR};
    const std::filesystem::path snapshots_dir{AKKADO_TEST_SNAPSHOTS_DIR};

    REQUIRE(std::filesystem::is_directory(fixtures_dir));

    std::set<std::filesystem::path> fixtures;
    for (const auto& entry : std::filesystem::directory_iterator(fixtures_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ak") {
            fixtures.insert(entry.path());
        }
    }
    REQUIRE_FALSE(fixtures.empty());

    const bool updating = update_mode_enabled();

    for (const auto& fixture : fixtures) {
        CAPTURE(fixture.string());

        const std::string source = read_file(fixture);
        auto result = akkado::compile(source,
                                      fixture.filename().string(),
                                      /*sample_registry=*/nullptr,
                                      /*resolver=*/nullptr,
                                      /*lint_strict=*/false,
                                      /*bypass_master=*/false);
        if (!result.success) {
            for (const auto& d : result.diagnostics) {
                UNSCOPED_INFO("diag " << d.code << " @ " << d.location.line
                              << ":" << d.location.column << " — " << d.message);
            }
        }
        REQUIRE(result.success);

        const std::string snapshot = render_snapshot(fixture, result);
        const std::filesystem::path snapshot_path =
            snapshots_dir / (fixture.stem().string() + ".disasm");

        if (updating) {
            write_file(snapshot_path, snapshot);
            continue;
        }

        if (!std::filesystem::exists(snapshot_path)) {
            UNSCOPED_INFO("missing snapshot " << snapshot_path.string()
                          << "\n  regenerate with: NKIDO_UPDATE_SNAPSHOTS=1 "
                             "./build/akkado/tests/akkado_tests \"[snapshot]\"");
            CHECK(false);
            continue;
        }

        const std::string expected = read_file(snapshot_path);
        if (expected != snapshot) {
            UNSCOPED_INFO("snapshot mismatch for " << fixture.filename().string()
                          << "\n  expected: " << snapshot_path.string()
                          << "\n  regenerate with: NKIDO_UPDATE_SNAPSHOTS=1 "
                             "./build/akkado/tests/akkado_tests \"[snapshot]\"");
        }
        CHECK(expected == snapshot);
    }
}
