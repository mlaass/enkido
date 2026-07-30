// End-to-end hot-swap test for record-spread options (`..{dry: 0, wet: 0}`).
//
// Bug repro (user-reported): starting a freeverb with `..{dry: 0, wet: 0}`
// is silent (correct), but starting with `..{dry: 1, wet: 0.5}` and live-
// editing to `..{dry: 0, wet: 0}` leaves the signal audible — the new
// ExtendedParams constants never reach audio.
//
// The cedar-side test (cedar/tests/test_hot_swap_extended_params.cpp)
// confirms that the StatePool + VM init path works in isolation. So if
// THIS test fails, the bug is on the akkado side — most likely in how
// the compiler assigns state_ids when record-spread options change.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "akkado/akkado.hpp"
#include <cedar/vm/vm.hpp>
#include <cedar/vm/state_pool.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/dsp_state.hpp>
#include <cedar/opcodes/sequence.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

// Mirrors the WASM `_cedar_apply_state_inits` body
// (web/wasm/nkido_wasm.cpp:1152) and CLI `apply_state_inits`
// (tools/nkido/program_loader.cpp:288). seq_storage keeps the per-load
// sequence vectors alive across the VM's lifetime; without it the
// sequence event pointers would dangle.
void apply_all_state_inits(cedar::VM& vm, const akkado::CompileResult& cr,
                           std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    for (const auto& init : cr.program.state_inits) {
        using T = akkado::StateInitData::Type;
        switch (init.type) {
            case T::SequenceProgram: {
                std::vector<cedar::Sequence> seq_copy = init.sequences;
                for (std::size_t i = 0;
                     i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
                    if (!init.sequence_events[i].empty()) {
                        seq_copy[i].events = const_cast<cedar::Event*>(
                            init.sequence_events[i].data());
                        seq_copy[i].num_events =
                            static_cast<std::uint32_t>(init.sequence_events[i].size());
                        seq_copy[i].capacity =
                            static_cast<std::uint32_t>(init.sequence_events[i].size());
                    }
                }
                seq_storage.push_back(std::move(seq_copy));
                auto& stored = seq_storage.back();
                vm.init_sequence_program_state(
                    init.state_id, stored.data(), stored.size(),
                    init.cycle_length, init.is_sample_pattern, init.total_events);
                break;
            }
            case T::ExtendedParams: {
                vm.init_extended_params(init.state_id,
                                        init.ext_constants.data(),
                                        init.ext_buffer_indices.data(),
                                        init.ext_count);
                break;
            }
            default:
                // Other types not exercised by these tests.
                break;
        }
    }
}

// live_load + retry: queues the new program, applies state inits. Mirrors
// the worklet's load + apply_state_inits sequence.
void live_load(cedar::VM& vm, const akkado::CompileResult& cr,
               std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    REQUIRE(cr.success);
    const std::size_t n_inst = cr.program.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> insts(n_inst);
    std::memcpy(insts.data(), cr.program.bytecode.data(), cr.program.bytecode.size());

    // Retry like the worklet does when slots are busy mid-crossfade.
    auto result = cedar::VM::LoadResult::SlotBusy;
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int retry = 0; retry < 64; ++retry) {
        result = vm.load_program(insts);
        if (result == cedar::VM::LoadResult::Success) break;
        vm.process_block(L.data(), R.data());
    }
    REQUIRE(result == cedar::VM::LoadResult::Success);
    apply_all_state_inits(vm, cr, seq_storage);
}

// Settle past the crossfade + DSP-tail decay window, then measure peak
// over the last few blocks. The settle window MUST exceed the crossfade
// duration (3 blocks default) plus the reverb-tank decay, otherwise the
// peak captures audio from the OLD program still mixing in.
float settle_then_peak(cedar::VM& vm, int settle_blocks, int measure_blocks) {
    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (int b = 0; b < settle_blocks; ++b) vm.process_block(L.data(), R.data());
    float peak = 0.0f;
    for (int b = 0; b < measure_blocks; ++b) {
        vm.process_block(L.data(), R.data());
        for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
            peak = std::max(peak, std::max(std::abs(L[i]), std::abs(R[i])));
        }
    }
    return peak;
}

void compile_and_print(const char* label, const std::string& src,
                       akkado::CompileResult& out) {
    out = akkado::compile(src);
    if (!out.success) {
        UNSCOPED_INFO(label << " compile diagnostics:");
        for (const auto& d : out.diagnostics) {
            UNSCOPED_INFO("  " << d.code << ": " << d.message);
        }
    }
    REQUIRE(out.success);
}

}  // namespace

// ============================================================================
// Canonical repro: the user's exact scenario.
// ============================================================================

TEST_CASE("hot-swap: freeverb wet:0.5 -> wet:0 via record-options silences audio",
          "[hot_swap][options][freeverb]") {
    // Use saw oscillator as a non-trivial audio source so silence is clearly
    // distinguishable from passthrough.
    constexpr const char* kSrcAudible =
        R"(saw(220) |> freeverb(@, ..{dry: 1, wet: 0.5}) |> out(@))";
    constexpr const char* kSrcSilent =
        R"(saw(220) |> freeverb(@, ..{dry: 0, wet: 0}) |> out(@))";

    akkado::CompileResult crA, crB;
    compile_and_print("audible", kSrcAudible, crA);
    compile_and_print("silent",  kSrcSilent,  crB);

    cedar::VM vm;
    vm.set_crossfade_blocks(3);
    std::vector<std::vector<cedar::Sequence>> seq_storage;

    // Load A: should produce audible output.
    live_load(vm, crA, seq_storage);
    // Settle past the initial swap (no crossfade — old slot was empty), let
    // the reverb tank fill, then sample a steady block.
    const float peak_a = settle_then_peak(vm, 128, 1);
    INFO("audible peak: " << peak_a);
    REQUIRE(peak_a > 0.05f);

    // Hot-swap to B: same opcode, different record-spread options.
    live_load(vm, crB, seq_storage);
    // Settle past the crossfade (default 3 blocks) + reverb tank decay,
    // then sample. With dry=0/wet=0 the freeverb produces silence.
    const float peak_b = settle_then_peak(vm, 64, 4);
    INFO("silent peak: " << peak_b);
    CHECK(peak_b < 1e-3f);
}

// ============================================================================
// Same shape, different builtin families — covers state_id stability across
// option changes for several ExtendedParams users.
// ============================================================================

namespace {

struct OptionsCase {
    const char* tag;
    const char* src_audible;
    const char* src_silent;
};

void run_options_silence_case(const OptionsCase& tc) {
    INFO("case: " << tc.tag);
    akkado::CompileResult crA, crB;
    compile_and_print("audible", tc.src_audible, crA);
    compile_and_print("silent",  tc.src_silent,  crB);

    cedar::VM vm;
    vm.set_crossfade_blocks(3);
    std::vector<std::vector<cedar::Sequence>> seq_storage;

    live_load(vm, crA, seq_storage);
    const float peak_a = settle_then_peak(vm, 128, 1);
    INFO("audible peak: " << peak_a);
    REQUIRE(peak_a > 0.01f);

    live_load(vm, crB, seq_storage);
    const float peak_b = settle_then_peak(vm, 64, 4);
    INFO("silent peak: " << peak_b);
    CHECK(peak_b < 1e-3f);
}

}  // namespace

TEST_CASE("hot-swap: record-options silence works across builtin families",
          "[hot_swap][options][family_sweep]") {
    const OptionsCase cases[] = {
        {"dattorro",
         R"(saw(220) |> dattorro(@, ..{dry: 1, wet: 0.5}) |> out(@))",
         R"(saw(220) |> dattorro(@, ..{dry: 0, wet: 0}) |> out(@))"},
        {"chorus",
         R"(saw(220) |> chorus(@, ..{dry: 1, wet: 0.5}) |> out(@))",
         R"(saw(220) |> chorus(@, ..{dry: 0, wet: 0}) |> out(@))"},
        {"phaser",
         R"(saw(220) |> phaser(@, ..{dry: 1, wet: 0.5}) |> out(@))",
         R"(saw(220) |> phaser(@, ..{dry: 0, wet: 0}) |> out(@))"},
        {"flanger",
         R"(saw(220) |> flanger(@, ..{dry: 1, wet: 0.5}) |> out(@))",
         R"(saw(220) |> flanger(@, ..{dry: 0, wet: 0}) |> out(@))"},
        {"moog",
         R"(saw(220) |> moog(@, 800) |> @ * 1.0 |> moog(@, 800, ..{dry: 1, wet: 0.5}) |> out(@))",
         R"(saw(220) |> moog(@, 800) |> @ * 1.0 |> moog(@, 800, ..{dry: 0, wet: 0}) |> out(@))"},
    };

    for (const auto& tc : cases) {
        run_options_silence_case(tc);
    }
}

// ============================================================================
// State-id stability check: does the freeverb opcode end up with the SAME
// state_id when only record-spread options change? If state_ids drift the
// new state_inits write to a NEW slot and the opcode (with the OLD bytecode
// state_id) keeps reading the OLD slot — that's one of the suspected root
// causes from the plan.
// ============================================================================

namespace {

std::uint32_t find_first_state_id_for_opcode(const akkado::CompileResult& cr,
                                             cedar::Opcode op) {
    const std::size_t n = cr.program.bytecode.size() / sizeof(cedar::Instruction);
    for (std::size_t i = 0; i < n; ++i) {
        cedar::Instruction inst{};
        std::memcpy(&inst, cr.program.bytecode.data() + i * sizeof(cedar::Instruction),
                    sizeof(inst));
        if (inst.opcode == op) return inst.state_id;
    }
    return 0;
}

}  // namespace

TEST_CASE("hot-swap: freeverb state_id is stable across record-options changes",
          "[hot_swap][options][state_id]") {
    constexpr const char* kSrcA =
        R"(saw(220) |> freeverb(@, ..{dry: 1, wet: 0.5}) |> out(@))";
    constexpr const char* kSrcB =
        R"(saw(220) |> freeverb(@, ..{dry: 0, wet: 0}) |> out(@))";

    akkado::CompileResult crA, crB;
    compile_and_print("A", kSrcA, crA);
    compile_and_print("B", kSrcB, crB);

    const auto sid_a = find_first_state_id_for_opcode(crA,
        cedar::Opcode::REVERB_FREEVERB);
    const auto sid_b = find_first_state_id_for_opcode(crB,
        cedar::Opcode::REVERB_FREEVERB);

    REQUIRE(sid_a != 0);
    REQUIRE(sid_b != 0);
    INFO("freeverb state_id A: 0x" << std::hex << sid_a);
    INFO("freeverb state_id B: 0x" << std::hex << sid_b);

    // Hot-swap state preservation hinges on this: opcodes at the "same
    // semantic position" must keep the same state_id even when their
    // record-options change. Otherwise both their DSP state and their
    // ExtendedParams slot drift, defeating preservation + re-init.
    CHECK(sid_a == sid_b);
}
