// L2 shared-block `fn` lowering — end-to-end tests.
//
// PRD prd-runtime-functions-control-flow L2: an eligible non-`#inline` user
// `fn` called from >=2 sites is compiled ONCE into a subprogram block and
// dispatched at runtime via BLOCK_CALL, instead of being inlined at every
// call site. `#inline` always forces inlining; an ineligible fn falls back to
// inlining (a pure optimization — falling back is always correct).
//
// These tests assert the contract that matters: shared-block lowering must be
// audibly transparent. A shared-block program and the same program forced to
// inline (`#inline`) must render sample-identical audio. They also pin the
// eligibility rules and the bytecode-size win.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/vm.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& r) {
    std::vector<cedar::Instruction> insts;
    insts.resize(r.program.bytecode.size() / sizeof(cedar::Instruction));
    if (!insts.empty()) {
        std::memcpy(insts.data(), r.program.bytecode.data(), r.program.bytecode.size());
    }
    return insts;
}

std::size_t count_op(const akkado::CompileResult& r, cedar::Opcode op) {
    std::size_t n = 0;
    for (const auto& i : get_instructions(r)) {
        if (i.opcode == op) ++n;
    }
    return n;
}

std::size_t instruction_count(const akkado::CompileResult& r) {
    return r.program.bytecode.size() / sizeof(cedar::Instruction);
}

// Compile + load + render `blocks` blocks of stereo audio. Returns interleaved
// L/R samples. Handles the [main | block bodies] subprogram-table layout.
// cedar::VM is ~8 MB — heap-allocate it, never put it on the stack.
std::vector<float> render(const akkado::CompileResult& cr, int blocks) {
    REQUIRE(cr.success);
    auto insts = get_instructions(cr);

    auto vm = std::make_unique<cedar::VM>();
    vm->set_crossfade_blocks(0);
    bool loaded;
    if (!cr.program.block_table.empty()) {
        loaded = vm->load_program_with_blocks_immediate(
            insts, cr.program.block_table, cr.program.main_instruction_count);
    } else {
        loaded = vm->load_program_immediate(insts);
    }
    REQUIRE(loaded);

    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(blocks) * cedar::BLOCK_SIZE * 2);
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int b = 0; b < blocks; ++b) {
        vm->process_block(L.data(), R.data());
        for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
            out.push_back(L[i]);
            out.push_back(R[i]);
        }
    }
    return out;
}

// Whole-render exact equality. Shared-block dispatch must not change a single
// sample versus inlining — same opcodes, same buffers, same arithmetic order.
bool samples_identical(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

}  // namespace

// ============================================================================
// Transparency: shared-block audio == inlined audio
// ============================================================================

TEST_CASE("BLOCK_CALL: 2-call-site fn renders identically to inlined",
          "[block_call][L2]") {
    const char* shared_src =
        "fn half(x) -> x * 0.5\n"
        "saw(half(220)) + saw(half(330)) |> out(@)";
    const char* inline_src =
        "#inline fn half(x) -> x * 0.5\n"
        "saw(half(220)) + saw(half(330)) |> out(@)";

    auto shared = akkado::compile(shared_src);
    auto inlined = akkado::compile(inline_src);
    REQUIRE(shared.success);
    REQUIRE(inlined.success);

    // The shared build dispatches; the #inline build expands the body twice.
    CHECK(count_op(shared, cedar::Opcode::BLOCK_CALL) == 2);
    CHECK(count_op(shared, cedar::Opcode::MUL) == 1);
    CHECK(count_op(inlined, cedar::Opcode::BLOCK_CALL) == 0);
    CHECK(count_op(inlined, cedar::Opcode::MUL) == 2);

    auto a = render(shared, 300);
    auto b = render(inlined, 300);
    CHECK(samples_identical(a, b));
}

TEST_CASE("BLOCK_CALL: 10-call-site fn -> one shared block, identical audio",
          "[block_call][L2]") {
    std::string body = "fn quad(x) -> x * 0.25\n";
    std::string shared = body;
    std::string inlined = "#inline " + body;
    std::string expr;
    for (int i = 0; i < 10; ++i) {
        if (i) expr += " + ";
        expr += "saw(quad(" + std::to_string(110 + i * 20) + "))";
    }
    expr += " |> out(@)";

    auto cr_shared = akkado::compile(shared + expr);
    auto cr_inline = akkado::compile(inlined + expr);
    REQUIRE(cr_shared.success);
    REQUIRE(cr_inline.success);

    // Exactly one subprogram block, dispatched from all ten call sites.
    CHECK(cr_shared.program.block_table.size() == 1);
    CHECK(count_op(cr_shared, cedar::Opcode::BLOCK_CALL) == 10);
    CHECK(count_op(cr_shared, cedar::Opcode::MUL) == 1);

    // The #inline build expands the body ten times and carries no block table.
    CHECK(cr_inline.program.block_table.empty());
    CHECK(count_op(cr_inline, cedar::Opcode::MUL) == 10);

    auto a = render(cr_shared, 300);
    auto b = render(cr_inline, 300);
    CHECK(samples_identical(a, b));
}

TEST_CASE("BLOCK_CALL: shared block shrinks the program",
          "[block_call][L2]") {
    std::string body = "fn quad(x) -> x * 0.25\n";
    std::string expr;
    for (int i = 0; i < 10; ++i) {
        if (i) expr += " + ";
        expr += "saw(quad(" + std::to_string(110 + i * 20) + "))";
    }
    expr += " |> out(@)";

    auto cr_shared = akkado::compile(body + expr);
    auto cr_inline = akkado::compile("#inline " + body + expr);
    REQUIRE(cr_shared.success);
    REQUIRE(cr_inline.success);

    // Body stored once: the shared program's main stream is materially
    // smaller, and so is the whole [main | body] stream.
    CHECK(cr_shared.program.main_instruction_count < instruction_count(cr_inline));
    CHECK(instruction_count(cr_shared) < instruction_count(cr_inline));
}

// ============================================================================
// Per-call-site state isolation
// ============================================================================

TEST_CASE("BLOCK_CALL: stateful fn body isolates state per call site",
          "[block_call][L2]") {
    // `flt` wraps a stateful filter. Two call sites with different cutoffs
    // must keep independent filter state. If the shared body leaked state
    // between call sites, the shared render would diverge from the inlined
    // reference (which has two physically distinct `lp` instructions).
    const char* shared_src =
        "fn flt(s, c) -> lp(s, c)\n"
        "flt(saw(220), 600) + flt(saw(330), 1800) |> out(@)";
    const char* inline_src =
        "#inline fn flt(s, c) -> lp(s, c)\n"
        "flt(saw(220), 600) + flt(saw(330), 1800) |> out(@)";

    auto shared = akkado::compile(shared_src);
    auto inlined = akkado::compile(inline_src);
    REQUIRE(shared.success);
    REQUIRE(inlined.success);
    CHECK(count_op(shared, cedar::Opcode::BLOCK_CALL) == 2);

    auto a = render(shared, 300);
    auto b = render(inlined, 300);
    CHECK(samples_identical(a, b));
}

// ============================================================================
// Hot-swap: per-call-site state survives a recompile
// ============================================================================

TEST_CASE("BLOCK_CALL: shared-block state survives hot-swap",
          "[block_call][L2][hot_swap]") {
    // Render a stateful shared-block program straight through as the
    // reference. Then render the first half, hot-swap to a separately
    // compiled identical program, and render the second half. The post-swap
    // audio must match the reference — per-call-site state IDs are path-
    // stable, so the filter state is rebound rather than reset.
    const char* src =
        "fn flt(s, c) -> lp(s, c)\n"
        "flt(saw(220), 600) + flt(saw(330), 1800) |> out(@)";

    auto cr = akkado::compile(src);
    REQUIRE(cr.success);
    REQUIRE(count_op(cr, cedar::Opcode::BLOCK_CALL) == 2);

    constexpr int kHalf = 200;
    auto reference = render(cr, kHalf * 2);

    // Swapped run: same VM, recompile + hot-swap at the midpoint.
    auto insts = get_instructions(cr);
    auto vm = std::make_unique<cedar::VM>();
    vm->set_crossfade_blocks(0);
    REQUIRE(vm->load_program_with_blocks_immediate(
        insts, cr.program.block_table, cr.program.main_instruction_count));

    std::vector<float> swapped;
    swapped.reserve(reference.size());
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int b = 0; b < kHalf; ++b) {
        vm->process_block(L.data(), R.data());
        for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
            swapped.push_back(L[i]);
            swapped.push_back(R[i]);
        }
    }

    auto cr2 = akkado::compile(src);
    REQUIRE(cr2.success);
    auto insts2 = get_instructions(cr2);
    REQUIRE(vm->load_program_with_blocks(
                insts2, cr2.program.block_table, cr2.program.main_instruction_count) ==
            cedar::VM::LoadResult::Success);

    for (int b = 0; b < kHalf; ++b) {
        vm->process_block(L.data(), R.data());
        for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
            swapped.push_back(L[i]);
            swapped.push_back(R[i]);
        }
    }

    REQUIRE(swapped.size() == reference.size());
    // Second half: identical-source hot-swap preserves filter state.
    bool second_half_ok = true;
    for (std::size_t i = kHalf * cedar::BLOCK_SIZE * 2; i < reference.size(); ++i) {
        if (swapped[i] != reference[i]) { second_half_ok = false; break; }
    }
    CHECK(second_half_ok);
}

// ============================================================================
// Eligibility — ineligible fns fall back to inlining (no block emitted)
// ============================================================================

TEST_CASE("BLOCK_CALL: single-call fn is not shared",
          "[block_call][L2][eligibility]") {
    // Sharing a body called only once saves nothing.
    auto r = akkado::compile("fn once(x) -> x * 2\nsaw(once(220)) |> out(@)");
    REQUIRE(r.success);
    CHECK(r.program.block_table.empty());
    CHECK(count_op(r, cedar::Opcode::BLOCK_CALL) == 0);
}

TEST_CASE("BLOCK_CALL: const fn is not shared",
          "[block_call][L2][eligibility]") {
    // `const fn` const-folds at compile time — no runtime body to share.
    auto r = akkado::compile(
        "const fn cf(x) -> x * 2\nsaw(cf(110)) + saw(cf(220)) |> out(@)");
    REQUIRE(r.success);
    CHECK(r.program.block_table.empty());
    CHECK(count_op(r, cedar::Opcode::BLOCK_CALL) == 0);
}

TEST_CASE("BLOCK_CALL: #inline fn is never shared",
          "[block_call][L2][eligibility]") {
    auto r = akkado::compile(
        "#inline fn h(x) -> x * 0.5\nsaw(h(220)) + saw(h(330)) |> out(@)");
    REQUIRE(r.success);
    CHECK(r.program.block_table.empty());
    CHECK(count_op(r, cedar::Opcode::BLOCK_CALL) == 0);
}

TEST_CASE("BLOCK_CALL: fn with >32 params falls back to inlining",
          "[block_call][L2][eligibility]") {
    // MAX_SHARED_FN_PARAMS = 32; a fn beyond that inlines (BLOCK_BIND's slot
    // index lives in a uint8 `rate`, and the contiguous param bank gets
    // impractically large — graceful fallback, never an error).
    std::string fn = "fn big(";
    std::string sum;
    for (int i = 0; i < 33; ++i) {
        if (i) { fn += ", "; sum += " + "; }
        fn += "p" + std::to_string(i);
        sum += "p" + std::to_string(i);
    }
    fn += ") -> " + sum + "\n";
    auto args = [](int base) {
        std::string a;
        for (int i = 0; i < 33; ++i) {
            if (i) a += ",";
            a += std::to_string(base + i);
        }
        return a;
    };
    auto r = akkado::compile(
        fn + "saw(big(" + args(1) + ")) + saw(big(" + args(2) + ")) |> out(@)");
    REQUIRE(r.success);
    CHECK(r.program.block_table.empty());
    CHECK(count_op(r, cedar::Opcode::BLOCK_CALL) == 0);
}

// ============================================================================
// BLOCK_BIND — shared blocks with >5 parameters (PRD §4.2)
// ============================================================================

TEST_CASE("BLOCK_BIND: 7-param fn is shared, renders identically to inlined",
          "[block_call][L2][block_bind]") {
    // Params 0..4 ride BLOCK_CALL.inputs[0..4]; params 5..6 ride a run of two
    // BLOCK_BIND instructions before each BLOCK_CALL. Audio must be sample-
    // identical to the #inline expansion.
    const char* shared_src =
        "fn mix7(a, b, c, d, e, g, h) -> a + b + c + d + e + g + h\n"
        "saw(mix7(30,40,50,60,70,80,90)) "
        "+ saw(mix7(90,80,70,60,50,40,30)) |> out(@)";
    const char* inline_src =
        "#inline fn mix7(a, b, c, d, e, g, h) -> a + b + c + d + e + g + h\n"
        "saw(mix7(30,40,50,60,70,80,90)) "
        "+ saw(mix7(90,80,70,60,50,40,30)) |> out(@)";

    auto shared = akkado::compile(shared_src);
    auto inlined = akkado::compile(inline_src);
    REQUIRE(shared.success);
    REQUIRE(inlined.success);

    // One shared body; two call sites; two BLOCK_BIND per site (slots 5,6).
    CHECK(shared.program.block_table.size() == 1);
    CHECK(count_op(shared, cedar::Opcode::BLOCK_CALL) == 2);
    CHECK(count_op(shared, cedar::Opcode::BLOCK_BIND) == 4);
    CHECK(count_op(inlined, cedar::Opcode::BLOCK_CALL) == 0);
    CHECK(count_op(inlined, cedar::Opcode::BLOCK_BIND) == 0);

    auto a = render(shared, 300);
    auto b = render(inlined, 300);
    CHECK(samples_identical(a, b));
}

TEST_CASE("BLOCK_BIND: each BLOCK_BIND run precedes its BLOCK_CALL",
          "[block_call][L2][block_bind]") {
    // The VM dispatch loop relies on BLOCK_BIND runs being contiguous and
    // terminated by exactly one BLOCK_CALL. Pin that codegen invariant.
    auto r = akkado::compile(
        "fn mix6(a, b, c, d, e, g) -> a + b + c + d + e + g\n"
        "saw(mix6(1,2,3,4,5,6)) + saw(mix6(6,5,4,3,2,1)) |> out(@)");
    REQUIRE(r.success);

    auto insts = get_instructions(r);
    int bind_run = 0;
    int calls_with_run = 0;
    for (std::size_t i = 0; i < r.program.main_instruction_count; ++i) {
        if (insts[i].opcode == cedar::Opcode::BLOCK_BIND) {
            ++bind_run;
            // BLOCK_BIND only ever binds slots >= 5.
            CHECK(insts[i].rate >= 5);
        } else if (insts[i].opcode == cedar::Opcode::BLOCK_CALL) {
            if (bind_run > 0) {
                CHECK(bind_run == 1);  // 6-param fn -> exactly slot 5
                ++calls_with_run;
            }
            bind_run = 0;
        } else {
            // No BLOCK_BIND may dangle without an immediately following CALL.
            CHECK(bind_run == 0);
        }
    }
    CHECK(calls_with_run == 2);
}

TEST_CASE("BLOCK_BIND: arrow-body fn followed by a parenthesized call stmt",
          "[block_call][L2][block_bind]") {
    // Regression: a `fn ... -> body` whose body ends in a bare identifier,
    // followed by a statement that opens with `(`, must not merge — the
    // newline-insensitive grammar would otherwise absorb the call statement
    // into the fn body and report a false E240 recursion. Exercised here at
    // >5 params (the BLOCK_BIND path) since multi-arg fn bodies routinely
    // end in a bare parameter identifier.
    auto r = akkado::compile(
        "fn mix7(a, b, c, d, e, g, h) -> a + b + c + d + e + g + h\n"
        "(saw(mix7(30,40,50,60,70,80,90)) "
        "+ saw(mix7(90,80,70,60,50,40,30))) * 0.5 |> out(@)");
    REQUIRE(r.success);
    for (const auto& d : r.diagnostics) {
        CHECK(d.code != "E240");  // no false recursion
    }
    CHECK(r.program.block_table.size() == 1);
    CHECK(count_op(r, cedar::Opcode::BLOCK_CALL) == 2);
    CHECK(count_op(r, cedar::Opcode::BLOCK_BIND) == 4);
}

TEST_CASE("BLOCK_BIND: 7-param stateful fn isolates state per call site",
          "[block_call][L2][block_bind]") {
    // A >5-param body carrying a stateful opcode (lp) must keep independent
    // state per call site, exactly as the <=5-param path does.
    const char* shared_src =
        "fn flt7(s, c, g1, g2, g3, g4, g5) -> "
        "lp(s, c) * g1 * g2 * g3 * g4 * g5\n"
        "flt7(saw(220), 600, 1, 1, 1, 1, 1) "
        "+ flt7(saw(330), 1800, 1, 1, 1, 1, 1) |> out(@)";
    const char* inline_src =
        "#inline fn flt7(s, c, g1, g2, g3, g4, g5) -> "
        "lp(s, c) * g1 * g2 * g3 * g4 * g5\n"
        "flt7(saw(220), 600, 1, 1, 1, 1, 1) "
        "+ flt7(saw(330), 1800, 1, 1, 1, 1, 1) |> out(@)";

    auto shared = akkado::compile(shared_src);
    auto inlined = akkado::compile(inline_src);
    REQUIRE(shared.success);
    REQUIRE(inlined.success);
    CHECK(count_op(shared, cedar::Opcode::BLOCK_CALL) == 2);
    CHECK(count_op(shared, cedar::Opcode::BLOCK_BIND) == 4);

    auto a = render(shared, 300);
    auto b = render(inlined, 300);
    CHECK(samples_identical(a, b));
}

TEST_CASE("BLOCK_CALL: fn with a default param is not shared",
          "[block_call][L2][eligibility]") {
    // A per-call default cannot be expressed by the fixed convention bank.
    auto r = akkado::compile(
        "fn d(x, y = 2) -> x * y\nsaw(d(110)) + saw(d(220)) |> out(@)");
    REQUIRE(r.success);
    CHECK(r.program.block_table.empty());
    CHECK(count_op(r, cedar::Opcode::BLOCK_CALL) == 0);
}

TEST_CASE("BLOCK_CALL: fn whose body needs a const arg is not shared",
          "[block_call][L2][eligibility]") {
    // `osc` needs a compile-time shape string. With the param compiled as a
    // runtime buffer the speculative body compile fails and rolls back, so
    // the fn inlines (where each call site supplies its literal shape).
    auto r = akkado::compile(
        "fn mk(shape) -> osc(shape, 220)\n"
        "mk(\"sin\") + mk(\"saw\") |> out(@)");
    REQUIRE(r.success);
    CHECK(r.program.block_table.empty());
    CHECK(count_op(r, cedar::Opcode::BLOCK_CALL) == 0);
}

// ============================================================================
// Recursion is still rejected (unchanged by L2 shared-block lowering)
// ============================================================================

TEST_CASE("BLOCK_CALL: recursive fn is still rejected",
          "[block_call][L2][recursion]") {
    auto direct = akkado::compile("fn f(x) -> f(x)\nf(440) |> out(@)");
    CHECK_FALSE(direct.success);
    bool e240 = false;
    for (const auto& d : direct.diagnostics) if (d.code == "E240") e240 = true;
    CHECK(e240);
}

// ============================================================================
// Many distinct shared blocks coexist
// ============================================================================

TEST_CASE("BLOCK_CALL: many distinct shared fns compile and stay within cap",
          "[block_call][L2]") {
    // Many distinct eligible fns, each called twice -> one shared block each.
    // The subprogram table must never exceed MAX_SUBPROGRAMS. (Reaching the
    // 64-block cap itself is unreachable here: the 256-entry compile-time
    // buffer pool is exhausted long before 64 distinct shared fns, so the
    // cap's overflow-to-inline path is defensive-only.)
    constexpr int kFns = 20;
    std::string src;
    for (int i = 0; i < kFns; ++i) {
        src += "fn f" + std::to_string(i) + "(x) -> x + " +
               std::to_string(i) + "\n";
    }
    std::string expr;
    for (int i = 0; i < kFns; ++i) {
        if (i) expr += " + ";
        std::string c = "f" + std::to_string(i);
        expr += "(saw(f" + std::to_string(i) + "(110)) + saw(" + c + "(220)))";
    }
    src += "(" + expr + ") * 0.01 |> out(@)";

    auto r = akkado::compile(src);
    REQUIRE(r.success);
    CHECK(r.program.block_table.size() == static_cast<std::size_t>(kFns));
    CHECK(r.program.block_table.size() <= cedar::MAX_SUBPROGRAMS);
    CHECK(count_op(r, cedar::Opcode::BLOCK_CALL) == 2 * kFns);

    // Audio renders without NaN/Inf.
    auto audio = render(r, 100);
    bool finite = true;
    for (float s : audio) {
        if (!std::isfinite(s)) { finite = false; break; }
    }
    CHECK(finite);
}
