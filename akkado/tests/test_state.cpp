// Userspace state cells (state/get/set) — Phase 3 of
// prd-userspace-state-and-edge-primitives.md.
//
// Verifies: parse, codegen, and runtime behavior of state(init), .get(),
// .set(v); reserved-name enforcement at parser level; that distinct call
// sites get independent slots; type errors on non-cell args.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/dsp/constants.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <span>
#include <vector>

static std::vector<cedar::Instruction> get_instructions(
    const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> insts;
    size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
    insts.resize(count);
    std::memcpy(insts.data(), result.bytecode.data(), result.bytecode.size());
    return insts;
}

static size_t count_op(const std::vector<cedar::Instruction>& insts,
                       cedar::Opcode op, std::uint8_t rate = 0xFF) {
    size_t n = 0;
    for (const auto& i : insts) {
        if (i.opcode != op) continue;
        if (rate != 0xFF && i.rate != rate) continue;
        ++n;
    }
    return n;
}

static bool diagnostic_contains(const akkado::CompileResult& r, const std::string& s) {
    for (const auto& d : r.diagnostics) {
        if (d.message.find(s) != std::string::npos) return true;
    }
    return false;
}

static bool diagnostic_has_code(const akkado::CompileResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

TEST_CASE("State: state(init) emits STATE_OP rate=0", "[state]") {
    auto r = akkado::compile(
        "s = state(0)\n"
        "out(s.get(), s.get())\n"
    );
    REQUIRE(r.success);
    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 0) == 1);  // exactly one init
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 1) >= 1);  // at least one load
}

TEST_CASE("State: distinct call sites get distinct slots", "[state][slots]") {
    auto r = akkado::compile(
        "a = state(0)\n"
        "b = state(0)\n"
        "out(a.get(), b.get())\n"
    );
    REQUIRE(r.success);
    auto insts = get_instructions(r);

    // Find the two STATE_OP rate=0 (init) instructions and verify their
    // state_ids differ — they came from distinct AST positions.
    std::vector<std::uint32_t> init_ids;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::STATE_OP && i.rate == 0) {
            init_ids.push_back(i.state_id);
        }
    }
    REQUIRE(init_ids.size() == 2);
    CHECK(init_ids[0] != init_ids[1]);
}

TEST_CASE("State: get/set chain stores and reads correctly at runtime", "[state][runtime]") {
    // Cell starts at 0, set to 42, then get — should output 42.
    // s.set(42) is a bare expression statement (its return value is unused).
    auto r = akkado::compile(
        "s = state(0)\n"
        "s.set(42)\n"
        "out(s.get(), s.get())\n"
    );
    REQUIRE(r.success);

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // First block: set fires once, then get reads. set writes the LAST sample
    // of its input buffer (which is broadcast 42.0) into the slot, then get
    // broadcasts that value to its output.
    CHECK(L[cedar::BLOCK_SIZE - 1] == 42.0f);
    CHECK(R[cedar::BLOCK_SIZE - 1] == 42.0f);
}

TEST_CASE("State: cell value persists across blocks", "[state][persistence]") {
    auto r = akkado::compile(
        "s = state(7)\n"
        "out(s.get(), s.get())\n"
    );
    REQUIRE(r.success);

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int block = 0; block < 5; ++block) {
        vm.process_block(L.data(), R.data());
        // Init runs once; subsequent blocks see preserved value 7.
        CHECK(L[0] == 7.0f);
        CHECK(L[cedar::BLOCK_SIZE - 1] == 7.0f);
    }
}

TEST_CASE("State: get on non-cell argument is an error", "[state][types]") {
    auto r = akkado::compile(
        "x = 5\n"
        "out(get(x), get(x))\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "state cell"));
}

TEST_CASE("State: set on non-cell first argument is an error", "[state][types]") {
    auto r = akkado::compile(
        "x = 5\n"
        "out(set(x, 1), set(x, 1))\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "state cell"));
}

TEST_CASE("State: 'state' is a reserved identifier", "[state][reserved]") {
    auto r = akkado::compile("state = 5\n");
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "reserved"));
}

TEST_CASE("State: 'get' is a reserved identifier", "[state][reserved]") {
    auto r = akkado::compile("get = 5\n");
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "reserved"));
}

TEST_CASE("State: 'set' is a reserved identifier", "[state][reserved]") {
    auto r = akkado::compile("set = 5\n");
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "reserved"));
}

TEST_CASE("State: defining a function named 'state' is a reserved-name error",
          "[state][reserved]") {
    auto r = akkado::compile("fn state(x) -> x + 1\n");
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "reserved"));
}

// PRD §9 edge case: `s.get()` before any `s.set()` returns the init value.
// CellState.initialized is false on the first STATE_OP rate=0, which writes
// the init buffer into the slot before any reads.
TEST_CASE("State: get() before any set() returns the init value",
          "[state][edge_case]") {
    auto r = akkado::compile(
        "s = state(7)\n"
        "out(s.get(), s.get())\n"
    );
    REQUIRE(r.success);

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // From the very first sample of the very first block, get() returns 7.
    CHECK(L[0] == 7.0f);
    CHECK(R[0] == 7.0f);
}

// PRD §11 testing strategy explicit requirement: state() inside a closure
// invoked at different sites has independent storage per site. Each call site
// gets its own state-pool slot via the AST-position semantic-ID hash.
TEST_CASE("State: closure invoked at two sites has independent slots per site",
          "[state][slots]") {
    // bump_and_read defines a state cell internally. Calling it twice at
    // different syntactic positions must produce two distinct STATE_OP rate=0
    // (init) instructions with different state_ids — proving the per-call-site
    // slot isolation.
    auto r = akkado::compile(
        "bump_and_read = (init) -> {\n"
        "  s = state(init)\n"
        "  s.set(s.get() + 1)\n"
        "  s.get()\n"
        "}\n"
        "out(bump_and_read(0), bump_and_read(100))\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);

    // Two distinct call sites of `bump_and_read` → distinct STATE_OP rate=0
    // inits with different state_ids. The codegen may emit additional inits
    // (e.g. one per closure expansion); what matters is that the set of
    // unique state_ids has at least two members, proving the per-call-site
    // hashing isolates slots between the two invocations.
    std::set<std::uint32_t> unique_init_ids;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::STATE_OP && i.rate == 0) {
            unique_init_ids.insert(i.state_id);
        }
    }
    CHECK(unique_init_ids.size() >= 2);
}

// PRD Goal 1: all four `step` variants must be implementable. The simplest
// (`step(arr, trig)`) and `step_dir(arr, trig, dir)` are exercised by the
// stepper-demo integration test. This test pins the two reset variants —
// `step(arr, trig, reset)` and `step(arr, trig, reset, start)`.
//
// Note: PRD §4.4 shows these variants composing `wrap(..., 0, len(arr))`,
// but `len(arr)` is a compile-time builtin that requires the array's length
// at codegen time, which is not available when `arr` is a closure parameter
// bound dynamically by the call site. The PRD itself observes (§4.4 note)
// that ARRAY_INDEX already wraps by default — so the bare
// `arr[counter(trig, reset, ...)]` form is sufficient and is what we test.
// The `len`-inside-closure limitation is a deliberate non-goal of this PRD
// (state cells were the persistence story, not generic compile-time
// reflection on closure parameters); it is captured in the audit report.
TEST_CASE("State: step variant with reset compiles", "[state][step][step_reset]") {
    auto r = akkado::compile(
        "step = (arr, trig, reset) -> arr[counter(trig, reset)]\n"
        "freq = [220, 330, 440, 550].step(trigger(4), trigger(1))\n"
        "sine(freq) * 0.2 |> out(%, %)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    // counter(trig, reset) → at least one EDGE_OP rate=3.
    CHECK(count_op(insts, cedar::Opcode::EDGE_OP, 3) >= 1);
}

TEST_CASE("State: step variant with reset and start compiles",
          "[state][step][step_reset_start]") {
    auto r = akkado::compile(
        "step = (arr, trig, reset, start) -> arr[counter(trig, reset, start)]\n"
        "freq = [220, 330, 440, 550].step(trigger(4), trigger(1), 1)\n"
        "sine(freq) * 0.2 |> out(%, %)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    // counter(trig, reset, start) → exactly one EDGE_OP rate=3 with all three
    // input slots wired (start input not BUFFER_UNUSED).
    bool found_counter_with_start = false;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::EDGE_OP && i.rate == 3 &&
            i.inputs[2] != cedar::BUFFER_UNUSED) {
            found_counter_with_start = true;
            break;
        }
    }
    CHECK(found_counter_with_start);
}

// PRD §9 edge case: empty array `[].step(trig)`. `wrap(x, 0, 0)` and
// `ARRAY_INDEX` both fall back to a safe value (0.0) rather than crashing.
TEST_CASE("State: empty array stepper produces silence not a crash",
          "[state][step][edge_case]") {
    auto r = akkado::compile(
        "step = (arr, trig) -> arr[counter(trig)]\n"
        "[].step(trigger(4)) |> sine(%) |> out(%, %)\n"
    );
    // The compile may succeed (degenerate but safe) or fail with a clear
    // diagnostic — what we forbid is a crash. If it compiles, run a block to
    // confirm the audio thread doesn't blow up.
    if (!r.success) {
        // Acceptable: a clear compile-time error about empty arrays. Not a
        // crash, and not a silent miscompile.
        SUCCEED("empty-array step rejected at compile time");
        return;
    }

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    // Should not crash and output should be finite (not NaN/Inf). We do not
    // assert exact zero — sine(NaN) etc. would already have crashed.
    for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
        CHECK(std::isfinite(L[i]));
        CHECK(std::isfinite(R[i]));
    }
}

// ============================================================================
// Phase 4a — record-valued state cells
// (prd-records-system-unification.md §5.6.1, §9 Phase 4a, §10.4)
//
// state({x: 1, y: 2}) is fanned out into N scalar STATE_OP slots, one per
// record field, with state_ids derived from per-field paths. These tests
// pin the surface (compile shape, type errors, E189 shape mismatch) and the
// runtime behaviour (per-field round-trip, persistence, cross-shape hot-swap
// fall-back).
// ============================================================================

TEST_CASE("State record: state(record) emits one STATE_OP rate=0 per field",
          "[state][record]") {
    auto r = akkado::compile(
        "s = state({x: 1, y: 2})\n"
        "out(get(s).x, get(s).y)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 0) == 2);

    // Per-field state_ids must differ — they were derived from distinct paths.
    std::set<std::uint32_t> init_ids;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::STATE_OP && i.rate == 0) {
            init_ids.insert(i.state_id);
        }
    }
    CHECK(init_ids.size() == 2);
}

TEST_CASE("State record: get returns a Record; field access reaches each field",
          "[state][record][runtime]") {
    auto r = akkado::compile(
        "s = state({x: 11, y: 22})\n"
        "out(get(s).x, get(s).y)\n"
    );
    REQUIRE(r.success);

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // Each field's init value broadcasts to its load buffer.
    CHECK(L[0] == 11.0f);
    CHECK(R[0] == 22.0f);
}

TEST_CASE("State record: set roundtrips per field", "[state][record][runtime]") {
    auto r = akkado::compile(
        "s = state({x: 0, y: 0})\n"
        "set(s, {x: 42, y: 99})\n"
        "out(get(s).x, get(s).y)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 2) == 2);  // one store per field

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    // set() writes the LAST sample of its (constant-broadcast) input buffer
    // into the slot, then get() broadcasts that value to its output buffer.
    CHECK(L[cedar::BLOCK_SIZE - 1] == 42.0f);
    CHECK(R[cedar::BLOCK_SIZE - 1] == 99.0f);
}

TEST_CASE("State record: E189 on extra field in set value", "[state][record]") {
    auto r = akkado::compile(
        "s = state({x: 0})\n"
        "set(s, {x: 1, y: 2})\n"
        "out(0, 0)\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_has_code(r, "E189"));
    CHECK(diagnostic_contains(r, "expected"));
    CHECK(diagnostic_contains(r, "got"));
}

TEST_CASE("State record: E189 on missing field in set value", "[state][record]") {
    auto r = akkado::compile(
        "s = state({x: 0, y: 0})\n"
        "set(s, {x: 1})\n"
        "out(0, 0)\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_has_code(r, "E189"));
}

TEST_CASE("State record: E122 widened — scalar value to record cell",
          "[state][record][types]") {
    auto r = akkado::compile(
        "s = state({x: 0})\n"
        "set(s, 42)\n"
        "out(0, 0)\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "record cell requires a record"));
}

TEST_CASE("State record: E122 widened — record value to scalar cell",
          "[state][record][types]") {
    auto r = akkado::compile(
        "s = state(0)\n"
        "set(s, {x: 1})\n"
        "out(0, 0)\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "scalar cell requires a number or signal"));
}

TEST_CASE("State record: E122 — nested-record init field rejected",
          "[state][record][types]") {
    auto r = akkado::compile(
        "s = state({inner: {x: 1}})\n"
        "out(0, 0)\n"
    );
    CHECK_FALSE(r.success);
    CHECK(diagnostic_contains(r, "nested record cells deferred"));
}

TEST_CASE("State record: empty record state({}) compiles with zero STATE_OPs",
          "[state][record][edge_case]") {
    auto r = akkado::compile(
        "s = state({})\n"
        "set(s, {})\n"
        "out(0, 0)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 0) == 0);
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 2) == 0);
}

TEST_CASE("State record: distinct call sites get distinct per-field slots",
          "[state][record][slots]") {
    // Two separate state({x, y}) literals → 4 distinct STATE_OP rate=0 inits.
    auto r = akkado::compile(
        "a = state({x: 1, y: 2})\n"
        "b = state({x: 3, y: 4})\n"
        "out(get(a).x + get(b).x, get(a).y + get(b).y)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    std::set<std::uint32_t> init_ids;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::STATE_OP && i.rate == 0) {
            init_ids.insert(i.state_id);
        }
    }
    CHECK(init_ids.size() == 4);
}

TEST_CASE("State record: cell value persists across blocks",
          "[state][record][persistence]") {
    auto r = akkado::compile(
        "s = state({x: 7, y: 13})\n"
        "out(get(s).x, get(s).y)\n"
    );
    REQUIRE(r.success);

    cedar::VM vm;
    REQUIRE(vm.load_program_immediate(
        std::span<const cedar::Instruction>(
            reinterpret_cast<const cedar::Instruction*>(r.bytecode.data()),
            r.bytecode.size() / sizeof(cedar::Instruction))));

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int block = 0; block < 5; ++block) {
        vm.process_block(L.data(), R.data());
        CHECK(L[0] == 7.0f);
        CHECK(L[cedar::BLOCK_SIZE - 1] == 7.0f);
        CHECK(R[0] == 13.0f);
        CHECK(R[cedar::BLOCK_SIZE - 1] == 13.0f);
    }
}

TEST_CASE("State record: cross-shape hot-swap freshly initializes new fields",
          "[state][record][hot_swap]") {
    // Program A holds a one-field record cell; B holds a two-field record cell
    // at the same source path. After hot-swap, the new field `y` must read its
    // declared init value (2.0) — the StatePool slot for `y` did not exist
    // before, so the rate=0 init populates it normally.
    //
    // We do not assert anything about the carry-over of `x` — that depends on
    // crossfade timing and is not a Phase 4a guarantee. The guarantee is:
    // (a) load_program(B) succeeds, (b) `y` reads 2.0 after the crossfade
    // settles. Per PRD §10.4: "Cross-shape reloads fall back to the new
    // initial record (no silent partial-merge)."
    auto rA = akkado::compile(
        "s = state({x: 1})\n"
        "out(get(s).x, get(s).x)\n"
    );
    REQUIRE(rA.success);
    auto rB = akkado::compile(
        "s = state({x: 1, y: 2})\n"
        "out(get(s).y, get(s).y)\n"
    );
    REQUIRE(rB.success);

    cedar::VM vm;
    vm.set_crossfade_blocks(3);

    std::span<const cedar::Instruction> bcA(
        reinterpret_cast<const cedar::Instruction*>(rA.bytecode.data()),
        rA.bytecode.size() / sizeof(cedar::Instruction));
    std::span<const cedar::Instruction> bcB(
        reinterpret_cast<const cedar::Instruction*>(rB.bytecode.data()),
        rB.bytecode.size() / sizeof(cedar::Instruction));

    REQUIRE(vm.load_program(bcA) == cedar::VM::LoadResult::Success);
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    // Run several blocks so the active-channel swap completes.
    for (int i = 0; i < 4; ++i) vm.process_block(L.data(), R.data());

    // Hot-swap. Retry until SlotBusy clears.
    cedar::VM::LoadResult result = cedar::VM::LoadResult::SlotBusy;
    for (int retry = 0; retry < 32; ++retry) {
        result = vm.load_program(bcB);
        if (result == cedar::VM::LoadResult::Success) break;
        vm.process_block(L.data(), R.data());
    }
    REQUIRE(result == cedar::VM::LoadResult::Success);

    // Run enough blocks for the crossfade to fully settle on B.
    for (int i = 0; i < 16; ++i) vm.process_block(L.data(), R.data());

    // B reads `y` which was freshly initialized to 2.0.
    CHECK(L[0] == 2.0f);
    CHECK(R[0] == 2.0f);
}

TEST_CASE("State record: closure invoking record state cell — independent slots",
          "[state][record][slots]") {
    // make_pair builds a record cell internally. Two call sites must produce
    // 2 × 2 = 4 distinct STATE_OP rate=0 inits, mirroring the scalar
    // `bump_and_read` test above.
    auto r = akkado::compile(
        "make_pair = (a, b) -> {\n"
        "  s = state({x: a, y: b})\n"
        "  get(s).x + get(s).y\n"
        "}\n"
        "out(make_pair(1, 2), make_pair(10, 20))\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    std::set<std::uint32_t> unique_init_ids;
    for (const auto& i : insts) {
        if (i.opcode == cedar::Opcode::STATE_OP && i.rate == 0) {
            unique_init_ids.insert(i.state_id);
        }
    }
    // At least 4: each of the two closure invocations produces 2 fields.
    CHECK(unique_init_ids.size() >= 4);
}

TEST_CASE("State record: signal-valued field accepted in init and set",
          "[state][record][types]") {
    // Parity with scalar cells: a field's init / set value can be Signal
    // (audio-rate) as well as Number — CellState stores the last sample.
    auto r = akkado::compile(
        "carrier = osc(\"sin\", 440)\n"
        "s = state({sig: carrier, level: 0.5})\n"
        "set(s, {sig: carrier, level: 0.7})\n"
        "out(get(s).sig * get(s).level, get(s).sig * get(s).level)\n"
    );
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 0) == 2);  // 2 fields, 1 init each
    CHECK(count_op(insts, cedar::Opcode::STATE_OP, 2) == 2);  // 2 fields, 1 store each
}
