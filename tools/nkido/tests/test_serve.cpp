// Integration tests for nkido serve mode. We spawn the binary as a
// subprocess, pipe a sequence of JSON commands into stdin, and check stdout
// for the expected events.
//
// The motivating bug: the VS Code "Nkido extension" launches `nkido serve`
// with no `--bank` flag, and previously the JSON wire protocol had no way to
// register a bank. Sample patches typed in the editor would print
// `warning: sample 'X' not found in any loaded bank` and play silence.
// The `load_bank` command added in serve_mode.cpp closes that hole, and this
// test prevents it from regressing.
//
// We run with `SDL_AUDIODRIVER=dummy` so audio init succeeds on headless CI;
// the dummy driver opens a no-op output device.

#include <catch2/catch_test_macros.hpp>

#include "test_paths.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>  // for close()

namespace {

// Pipe `commands` into `argv` over stdin and capture stdout+stderr. Returns
// the captured combined output. exit_status is set to the child's exit code.
// `env_prefix`, if given, is prepended to the shell command (e.g.
// "FOO=bar BAZ=qux") and overrides nothing else. SDL_AUDIODRIVER=dummy is
// always set so headless audio init succeeds.
std::string run_with_stdin(const std::string& argv,
                           const std::string& commands,
                           int* exit_status,
                           const std::string& env_prefix = "") {
    // Write commands to a tempfile, redirect stdin from it, redirect both
    // stdout and stderr to a tempfile we read back. This is portable enough
    // for a Linux dev box without pulling in a process-spawning lib.
    char cmd_path[] = "/tmp/nkido_serve_test_in_XXXXXX";
    int cmd_fd = mkstemp(cmd_path);
    REQUIRE(cmd_fd >= 0);
    {
        std::ofstream cmd_file(cmd_path);
        cmd_file << commands;
    }
    close(cmd_fd);

    char out_path[] = "/tmp/nkido_serve_test_out_XXXXXX";
    int out_fd = mkstemp(out_path);
    REQUIRE(out_fd >= 0);
    close(out_fd);

    std::string shell_cmd =
        "SDL_AUDIODRIVER=dummy ";
    if (!env_prefix.empty()) {
        shell_cmd += env_prefix + " ";
    }
    shell_cmd += argv +
        " < " + cmd_path +
        " > " + out_path + " 2>&1";

    const int rc = std::system(shell_cmd.c_str());
    if (exit_status) *exit_status = rc;

    std::ifstream out_file(out_path);
    std::stringstream buf;
    buf << out_file.rdbuf();

    std::remove(cmd_path);
    std::remove(out_path);

    return buf.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("serve mode: load_bank registers a manifest and emits bank_registered",
          "[serve][sample][load_bank]") {
    // Without load_bank, sample patches fail to resolve. With it, the bank is
    // registered immediately and the manifest is fetched once on registration
    // (so the user gets synchronous success/failure feedback, and subsequent
    // load commands don't trigger redundant fetches in the log).
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"({"cmd":"load_bank","uri":"file://)" +
                std::string(nkido::test::LOCAL_BANK_MANIFEST) + R"("})" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(args, commands, &exit_code);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, R"("event":"ready")"));
    CHECK(contains(output, R"("event":"bank_registered")"));
    CHECK(contains(output, R"("sample_count":1)"));  // local_bank has 1 sample (bd)
    // No load was issued, so we should not see the missing-sample warning.
    CHECK_FALSE(contains(output, "not found in any loaded bank"));
}

TEST_CASE("serve mode: load after load_bank resolves sample names without warnings",
          "[serve][sample][load_bank]") {
    // The user's actual scenario (paraphrased): launch serve, register a bank,
    // then send a sample-pattern source. Before the fix, no bank was registered
    // and the load printed "sample 'bd' not found in any loaded bank". After
    // the fix, load_bank populates s.opts.bank_uris so the subsequent load's
    // call into prepare_program_assets() picks the bank up and resolves
    // "bd" to a valid sample ID.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"({"cmd":"load_bank","uri":"file://)" +
                std::string(nkido::test::LOCAL_BANK_MANIFEST) + R"("})" "\n";
    commands += R"json({"cmd":"load","source":"s\"bd\".out()"})json" "\n";
    commands += R"({"cmd":"stop"})" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(args, commands, &exit_code);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, R"("event":"ready")"));
    CHECK(contains(output, R"("event":"bank_registered")"));
    CHECK(contains(output, R"("event":"compiled","ok":true)"));
    CHECK_FALSE(contains(output, "sample 'bd' not found in any loaded bank"));
}

TEST_CASE("serve mode: --no-default-bank suppresses the built-in default kit",
          "[serve][sample][load_bank][default_kit]") {
    // Negative-control test: with the built-in default kit suppressed AND no
    // user-supplied bank, sample patches must still emit the not-found
    // warning. Empty NKIDO_DEFAULT_KIT belt-and-suspenders the flag.
    //
    // If this test stops failing, either the default-kit auto-load path
    // ignored --no-default-bank, or the diagnostic path is broken — both
    // worth catching.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --no-default-bank --rate 48000 --buffer 512";
    std::string commands;
    commands += R"json({"cmd":"load","source":"s\"bd\".out()"})json" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(args, commands, &exit_code,
                                              "NKIDO_DEFAULT_KIT=");

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, "sample 'bd' not found in any loaded bank"));
}

TEST_CASE("serve mode: bare sample name resolves via built-in default kit",
          "[serve][sample][default_kit]") {
    // Parity with the web UI: launching `nkido serve` with no `--bank`
    // and no `samples()` call must still let `s"bd"` resolve, because the
    // built-in default kit is auto-registered. We point NKIDO_DEFAULT_KIT
    // at a minimal one-entry fixture so the test doesn't depend on any
    // particular set of WAVs.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"json({"cmd":"load","source":"s\"bd\".out()"})json" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string env_prefix = std::string("NKIDO_DEFAULT_KIT=") +
                                   nkido::test::DEFAULT_KIT_FIXTURE;
    const std::string output = run_with_stdin(args, commands, &exit_code,
                                              env_prefix);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, R"("event":"compiled","ok":true)"));
    CHECK_FALSE(contains(output, "sample 'bd' not found in any loaded bank"));
    // Confirm the default kit was actually loaded (info line).
    CHECK(contains(output, "Loaded default kit"));
}

TEST_CASE("serve mode: user load_bank shadows the default kit",
          "[serve][sample][default_kit][load_bank]") {
    // User-supplied banks must be searched before the default kit so that
    // `bd` registered via load_bank wins. We can't easily assert which
    // file got loaded without an introspection hook, so we settle for:
    // (a) bank_registered fires before compiled, and (b) no not-found
    // warning. This pins precedence even if the user's `bd` and the
    // default kit's `bd` happen to point at the same file.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"({"cmd":"load_bank","uri":"file://)" +
                std::string(nkido::test::USER_OVERRIDE_BANK_MANIFEST) +
                R"("})" "\n";
    commands += R"json({"cmd":"load","source":"s\"bd\".out()"})json" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string env_prefix = std::string("NKIDO_DEFAULT_KIT=") +
                                   nkido::test::DEFAULT_KIT_FIXTURE;
    const std::string output = run_with_stdin(args, commands, &exit_code,
                                              env_prefix);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, R"("event":"bank_registered")"));
    CHECK(contains(output, R"("event":"compiled","ok":true)"));
    CHECK_FALSE(contains(output, "sample 'bd' not found in any loaded bank"));
    // bank_registered must appear before compiled in the stream so the user
    // bank is searched first by register_required_samples.
    const auto bank_pos = output.find(R"("event":"bank_registered")");
    const auto compile_pos = output.find(R"("event":"compiled","ok":true)");
    CHECK(bank_pos != std::string::npos);
    CHECK(compile_pos != std::string::npos);
    CHECK(bank_pos < compile_pos);
}

TEST_CASE("serve mode: missing default kit URI degrades to a warning, not a crash",
          "[serve][sample][default_kit]") {
    // If the discoverable default kit URI fails to fetch (bad path, network
    // hiccup, etc.) we should emit one info line and still let the load
    // command complete with the standard not-found warning — never crash.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"json({"cmd":"load","source":"s\"bd\".out()"})json" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(
        args, commands, &exit_code,
        "NKIDO_DEFAULT_KIT=file:///definitely/does/not/exist.json");

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, "sample 'bd' not found in any loaded bank"));
    // The "could not be loaded" info line should appear since the program
    // does reference a sample.
    CHECK(contains(output, "default kit"));
    CHECK(contains(output, "could not be loaded"));
}

TEST_CASE("serve mode: load_bank with a bad URI emits an error event",
          "[serve][sample][load_bank]") {
    // Invalid URIs should fail at registration time (immediate fetch) rather
    // than silently succeeding and only erroring on the next load command.
    std::string args = std::string(nkido::test::CLI_BINARY) +
                       " serve --rate 48000 --buffer 512";
    std::string commands;
    commands += R"({"cmd":"load_bank","uri":"file:///definitely/does/not/exist.json"})" "\n";
    commands += R"({"cmd":"quit"})" "\n";

    int exit_code = -1;
    const std::string output = run_with_stdin(args, commands, &exit_code);

    INFO("stdout/stderr was:\n" << output);
    CHECK(contains(output, R"("event":"error")"));
    CHECK(contains(output, "failed to load"));
    // bank_registered must NOT fire for a failed bank.
    CHECK_FALSE(contains(output, R"("event":"bank_registered")"));
}
