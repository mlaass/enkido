// Hot-swap regression for ExtendedParams<N>.
//
// Bug repro (user-reported): a freeverb with `..{wet: 0.5}` live-edited to
// `..{wet: 0}` did not produce silence — the new constant never reached audio
// after the hot-swap. Root cause turned out to be that
// `StatePool::init_extended_params_runtime()` calls the templated
// `init_extended_params<N>()` via a generic lambda using
// `[&]<std::size_t N>()`. GCC misparses this when the lambda is used as a
// runtime dispatch target; the call ended up being a no-op for the
// re-init case while the initial init worked through a different code path.
//
// Coverage: 18 builtins use ExtendedParams<N>, including non-mix params
// (comp.attack/release, limiter.lookahead, gate.attack/hold/release,
// comb.damping). This file covers them at the VM level: every flavor of N
// (1, 2, 3, 5) round-trips constant updates across a load+re-init cycle.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cedar/vm/vm.hpp"
#include "cedar/vm/instruction.hpp"
#include "cedar/vm/state_pool.hpp"
#include "cedar/dsp/constants.hpp"
#include "cedar/opcodes/dsp_state.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

Instruction push_const(std::uint16_t out, float value) {
    Instruction inst{};
    inst.opcode = Opcode::PUSH_CONST;
    inst.out_buffer = out;
    for (auto& in : inst.inputs) in = BUFFER_UNUSED;
    inst.state_id = std::bit_cast<std::uint32_t>(value);
    return inst;
}

// Drive the VM long enough for any pending swap + crossfade to complete.
void run_blocks(VM& vm, int n) {
    std::array<float, BLOCK_SIZE> L{}, R{};
    for (int i = 0; i < n; ++i) vm.process_block(L.data(), R.data());
}

// Load a program, retrying across blocks while the slot is busy (the
// crossfade window owns both slots). Matches the worklet's retry pattern.
void load_with_retry(VM& vm, std::span<const Instruction> bc) {
    auto result = VM::LoadResult::SlotBusy;
    std::array<float, BLOCK_SIZE> L{}, R{};
    for (int retry = 0; retry < 64; ++retry) {
        result = vm.load_program(bc);
        if (result == VM::LoadResult::Success) return;
        vm.process_block(L.data(), R.data());
    }
    FAIL("load_program never returned Success");
}

float peak_block(VM& vm) {
    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    float peak = 0.0f;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        peak = std::max(peak, std::max(std::abs(L[i]), std::abs(R[i])));
    }
    return peak;
}

// Apply a constants-only ExtendedParams init for a given opcode state_id.
// Mirrors the production path: codegen stores `ext_params_state_id(state_id)`
// in StateInitData, and program_loader passes that XOR'd id to
// `vm.init_extended_params` — so the slot read by the opcode (which
// applies the XOR itself when reading via `ext_params_state_id(inst.state_id)`)
// is the one we're initialising.
template<std::size_t N>
void init_ext(VM& vm, std::uint32_t opcode_state_id, std::array<float, N> consts) {
    std::array<std::uint16_t, N> bufs;
    bufs.fill(0xFFFFu);
    vm.init_extended_params(ext_params_state_id(opcode_state_id),
                            consts.data(), bufs.data(),
                            static_cast<std::uint8_t>(N));
}

}  // namespace

// =============================================================================
// Layer 1: StatePool unit tests — does init_extended_params_runtime actually
// update an existing slot?
// =============================================================================

TEST_CASE("StatePool: init_extended_params_runtime overwrites existing constants",
          "[hot_swap][ext_params][state_pool]") {
    StatePool pool;
    constexpr std::uint32_t sid = 0x12345678u;

    // First "compile": dry=1.0, wet=0.5.
    {
        std::array<float, 2> c = {1.0f, 0.5f};
        std::array<std::uint16_t, 2> b = {0xFFFFu, 0xFFFFu};
        pool.init_extended_params_runtime(sid, c.data(), b.data(), 2);
    }
    {
        const auto* ep = pool.get_if<ExtendedParams<2>>(sid);
        REQUIRE(ep != nullptr);
        CHECK_THAT(ep->params[0].constant, WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(ep->params[1].constant, WithinAbs(0.5f, 1e-6f));
    }

    // Second "compile" (simulating hot-swap recompile): dry=0.0, wet=0.0.
    {
        std::array<float, 2> c = {0.0f, 0.0f};
        std::array<std::uint16_t, 2> b = {0xFFFFu, 0xFFFFu};
        pool.init_extended_params_runtime(sid, c.data(), b.data(), 2);
    }
    {
        const auto* ep = pool.get_if<ExtendedParams<2>>(sid);
        REQUIRE(ep != nullptr);
        CHECK_THAT(ep->params[0].constant, WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(ep->params[1].constant, WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("StatePool: init_extended_params_runtime updates each N variant",
          "[hot_swap][ext_params][state_pool]") {
    StatePool pool;

    // Each N variant (1, 2, 3, 5, 8) lives at its own state_id so the slot
    // type is unambiguous. Two inits per N: distinct constants prove updates.
    struct NCase { std::uint8_t count; std::uint32_t sid; };
    const NCase cases[] = {
        {1, 0x1100u}, {2, 0x2200u}, {3, 0x3300u}, {5, 0x5500u}, {8, 0x8800u},
    };

    for (const auto& tc : cases) {
        std::array<float, 8> first{}, second{};
        std::array<std::uint16_t, 8> bufs;
        bufs.fill(0xFFFFu);
        for (std::size_t i = 0; i < tc.count; ++i) {
            first[i]  = 0.25f * static_cast<float>(i + 1);
            second[i] = 0.75f * static_cast<float>(i + 1);
        }
        pool.init_extended_params_runtime(tc.sid, first.data(), bufs.data(), tc.count);
        pool.init_extended_params_runtime(tc.sid, second.data(), bufs.data(), tc.count);

        // Read back. Each variant uses its own template instantiation.
        auto check = [&]<std::size_t N>() {
            const auto* ep = pool.get_if<ExtendedParams<N>>(tc.sid);
            REQUIRE(ep != nullptr);
            for (std::size_t i = 0; i < N; ++i) {
                CHECK_THAT(ep->params[i].constant,
                           WithinAbs(0.75f * static_cast<float>(i + 1), 1e-6f));
            }
        };
        if      (tc.count == 1) check.template operator()<1>();
        else if (tc.count == 2) check.template operator()<2>();
        else if (tc.count == 3) check.template operator()<3>();
        else if (tc.count == 5) check.template operator()<5>();
        else                    check.template operator()<8>();
    }
}

// =============================================================================
// Layer 2: VM-level repro — the user's exact freeverb scenario.
// =============================================================================

TEST_CASE("hot-swap: freeverb wet:0.5 -> wet:0 produces silence",
          "[hot_swap][ext_params][freeverb]") {
    // Program: PUSH_CONST(1.0) -> REVERB_FREEVERB -> OUTPUT(stereo).
    // The freeverb is stereo-native and writes (out_buffer, out_buffer+1).
    constexpr std::uint16_t BUF_IN    = 0;
    constexpr std::uint16_t BUF_ROOM  = 1;
    constexpr std::uint16_t BUF_DAMP  = 2;
    constexpr std::uint16_t BUF_RS    = 3;
    constexpr std::uint16_t BUF_RO    = 4;
    constexpr std::uint16_t BUF_OUT_L = 5;
    constexpr std::uint16_t BUF_OUT_R = 6;
    constexpr std::uint32_t REVERB_ID = 0xDEADC0DEu;

    auto make_program = []() {
        Instruction reverb{};
        reverb.opcode = Opcode::REVERB_FREEVERB;
        reverb.out_buffer = BUF_OUT_L;
        reverb.inputs[0] = BUF_IN;
        reverb.inputs[1] = BUF_ROOM;
        reverb.inputs[2] = BUF_DAMP;
        reverb.inputs[3] = BUF_RS;
        reverb.inputs[4] = BUF_RO;
        reverb.state_id = REVERB_ID;

        return std::vector<Instruction>{
            push_const(BUF_IN,   0.5f),
            push_const(BUF_ROOM, 0.5f),
            push_const(BUF_DAMP, 0.5f),
            push_const(BUF_RS,   0.28f),
            push_const(BUF_RO,   0.7f),
            reverb,
            Instruction::make_binary(Opcode::OUTPUT, 0, BUF_OUT_L, BUF_OUT_R),
        };
    };

    VM vm;
    vm.set_crossfade_blocks(3);

    // Load A: dry=1.0, wet=0.5 — must be audible.
    auto bcA = make_program();
    REQUIRE(vm.load_program(bcA) == VM::LoadResult::Success);
    init_ext<2>(vm, REVERB_ID, {1.0f, 0.5f});
    run_blocks(vm, 128);  // fill reverb tank + clear initial-swap crossfade
    const float peak_a = peak_block(vm);
    REQUIRE(peak_a > 0.1f);

    // Load B: same program, but state init now silences both dry and wet.
    auto bcB = make_program();
    load_with_retry(vm, bcB);
    init_ext<2>(vm, REVERB_ID, {0.0f, 0.0f});

    // Run blocks past the crossfade + let the reverb tank decay.
    run_blocks(vm, 256);
    const float peak_b = peak_block(vm);
    CHECK(peak_b < 1e-4f);
}

// =============================================================================
// Layer 3: parametric VM-level sweep across every dry/wet builtin.
// =============================================================================
//
// For each builtin with dry/wet in ExtendedParams<N>, build a minimal
// program, run with audible defaults, then re-init with dry=0/wet=0 and
// verify the output goes silent.

namespace {

struct DryWetCase {
    const char* name;
    Opcode      opcode;
    bool        stereo_native;  // true if writes to (out, out+1)
    bool        stereo_input;   // pingpong feeds inputs[1] = right channel
    std::uint8_t ext_count;
    std::uint8_t dry_slot;
    std::uint8_t wet_slot;
    // input_count + optional_count, in declaration order (the buffer index
    // each input slot maps to). nullptr -> no slot wired.
    std::array<float, 5> input_defaults; // values pushed onto BUF_INPUT_BASE+i
    std::array<float, 8> ext_audible;    // defaults that produce audible output
    std::uint32_t state_id;
};

void run_drywet_silence_test(const DryWetCase& tc) {
    INFO("builtin: " << tc.name);

    // Buffer layout:
    //   0..N-1: input slot consts (per tc.input_defaults)
    //   N..   : output (1 buffer if mono, 2 if stereo-native)
    constexpr std::uint16_t BUF_BASE = 0;
    const std::uint16_t BUF_OUT_L = static_cast<std::uint16_t>(BUF_BASE + 5);
    const std::uint16_t BUF_OUT_R = static_cast<std::uint16_t>(BUF_OUT_L + 1);

    auto make_program = [&]() {
        std::vector<Instruction> prog;
        for (std::uint16_t i = 0; i < 5; ++i) {
            prog.push_back(push_const(static_cast<std::uint16_t>(BUF_BASE + i),
                                      tc.input_defaults[i]));
        }
        Instruction op{};
        op.opcode = tc.opcode;
        op.out_buffer = BUF_OUT_L;
        for (auto& in : op.inputs) in = BUFFER_UNUSED;
        for (std::uint16_t i = 0; i < 5; ++i) {
            op.inputs[i] = static_cast<std::uint16_t>(BUF_BASE + i);
        }
        if (tc.stereo_input) {
            // pingpong: inputs[0]=L, inputs[1]=R — the consts at buffer 0/1
            // already act as L/R. The opcode requires the STEREO_INPUT flag.
            op.flags = static_cast<std::uint8_t>(op.flags |
                                                 InstructionFlag::STEREO_INPUT);
        }
        op.state_id = tc.state_id;
        prog.push_back(op);

        // OUTPUT (stereo if the opcode is stereo-native, mono otherwise).
        const std::uint16_t right = tc.stereo_native ? BUF_OUT_R : BUF_OUT_L;
        prog.push_back(Instruction::make_binary(Opcode::OUTPUT, 0, BUF_OUT_L, right));
        return prog;
    };

    VM vm;
    vm.set_crossfade_blocks(3);

    // Load A with audible defaults.
    auto bcA = make_program();
    REQUIRE(vm.load_program(bcA) == VM::LoadResult::Success);
    {
        std::array<std::uint16_t, 8> bufs;
        bufs.fill(0xFFFFu);
        vm.init_extended_params(ext_params_state_id(tc.state_id),
                                tc.ext_audible.data(),
                                bufs.data(), tc.ext_count);
    }
    run_blocks(vm, 128);
    const float peak_audible = peak_block(vm);
    INFO("audible peak: " << peak_audible);
    REQUIRE(peak_audible > 1e-3f);

    // Load B (same program) and re-init ext with silent dry/wet.
    auto bcB = make_program();
    load_with_retry(vm, bcB);
    {
        std::array<float, 8> silent = tc.ext_audible;
        silent[tc.dry_slot] = 0.0f;
        silent[tc.wet_slot] = 0.0f;
        std::array<std::uint16_t, 8> bufs;
        bufs.fill(0xFFFFu);
        vm.init_extended_params(ext_params_state_id(tc.state_id),
                                silent.data(), bufs.data(),
                                tc.ext_count);
    }
    run_blocks(vm, 256);
    const float peak_silent = peak_block(vm);
    INFO("silent peak: " << peak_silent);
    CHECK(peak_silent < 1e-3f);
}

}  // namespace

TEST_CASE("hot-swap: dry/wet silenced across all ExtendedParams effects",
          "[hot_swap][ext_params][drywet_sweep]") {
    // ext layouts and audible-defaults are mirrored from BuiltinInfo
    // (akkado/include/akkado/builtins.hpp). Input slot defaults are chosen
    // to give a non-trivial, audible processed signal.
    const DryWetCase cases[] = {
        // ext[0]=dry, ext[1]=wet (10 builtins)
        {"freeverb",  Opcode::REVERB_FREEVERB,   true, false, 2, 0, 1,
         /*in=*/{0.5f, 0.5f, 0.5f, 0.28f, 0.7f},
         /*ext=*/{1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001000u},
        {"pingpong",  Opcode::DELAY_PINGPONG,    true, true,  2, 0, 1,
         /*in=L,R,time,fb,width=*/{0.5f, 0.5f, 0.005f, 0.3f, 1.0f},
         /*ext=*/{1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001001u},
        {"moog",      Opcode::FILTER_MOOG,       true, false, 2, 0, 1,
         /*in=in,freq,res,max_res,in_scale=*/{0.5f, 800.0f, 0.5f, 4.0f, 1.0f},
         /*ext: dry=1 (passthrough audible)=*/{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001002u},
        {"diode",     Opcode::FILTER_DIODE,      true, false, 2, 0, 1,
         /*in=in,freq,res,?,?=*/{0.5f, 800.0f, 0.5f, 0.0f, 0.0f},
         /*ext: dry=1=*/{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001003u},
        {"formant",   Opcode::FILTER_FORMANT,    true, false, 2, 0, 1,
         /*in=in,vowel,morph,?,?=*/{0.5f, 0.0f, 0.0f, 0.0f, 0.0f},
         /*ext: dry=1=*/{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001004u},
        {"sallenkey", Opcode::FILTER_SALLENKEY,  true, false, 2, 0, 1,
         /*in=in,freq,res,?,?=*/{0.5f, 800.0f, 0.5f, 0.0f, 0.0f},
         /*ext: dry=1=*/{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001005u},
        {"fold",      Opcode::DISTORT_FOLD,      true, false, 2, 0, 1,
         /*in=in,thresh,?,?,?=*/{0.5f, 0.3f, 0.0f, 0.0f, 0.0f},
         /*ext: dry=1=*/{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001006u},
        {"tape",      Opcode::DISTORT_TAPE,      true, false, 2, 0, 1,
         /*in=in,drive,warmth,soft_th,warm_scale=*/{0.5f, 3.0f, 0.3f, 0.5f, 0.7f},
         /*ext: dry=1=*/{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001007u},
        {"xfmr",      Opcode::DISTORT_XFMR,      true, false, 2, 0, 1,
         /*in=in,drive,bass,bass_freq,?=*/{0.5f, 3.0f, 5.0f, 60.0f, 0.0f},
         /*ext: dry=1=*/{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001008u},
        {"excite",    Opcode::DISTORT_EXCITE,    true, false, 2, 0, 1,
         /*in=in,amount,freq,h_odd,h_even=*/{0.5f, 0.5f, 3000.0f, 0.4f, 0.6f},
         /*ext: dry=1=*/{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x10001009u},

        // ext[1]=dry, ext[2]=wet (chorus)
        {"chorus",    Opcode::EFFECT_CHORUS,     true, false, 3, 1, 2,
         /*in=in,rate,depth,base,range=*/{0.5f, 0.5f, 0.5f, 20.0f, 10.0f},
         /*ext=lfo_phase,dry,wet=*/{0.25f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x1000100Au},

        // ext[2]=dry, ext[3]=wet (flanger)
        {"flanger",   Opcode::EFFECT_FLANGER,    true, false, 4, 2, 3,
         /*in=in,rate,depth,min_d,max_d=*/{0.5f, 1.0f, 0.7f, 0.1f, 10.0f},
         /*ext=fb,lfo_phase,dry,wet=*/{-0.5f, 0.25f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f},
         0x1000100Bu},

        // ext[3]=dry, ext[4]=wet (3 builtins)
        {"dattorro",  Opcode::REVERB_DATTORRO,   true, false, 5, 3, 4,
         /*in=in,decay,predelay,in_diff,dec_diff=*/{0.5f, 0.7f, 20.0f, 0.75f, 0.625f},
         /*ext=damp,mod_d,lfo_r,dry,wet=*/{0.0f, 0.0f, 0.5f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f},
         0x1000100Cu},
        {"phaser",    Opcode::EFFECT_PHASER,     true, false, 5, 3, 4,
         /*in=in,rate,depth,min_f,max_f=*/{0.5f, 0.5f, 0.8f, 200.0f, 4000.0f},
         /*ext=fb,stages,lfo_phase,dry,wet=*/{0.5f, 4.0f, 0.25f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f},
         0x1000100Du},
        {"gate",      Opcode::DYNAMICS_GATE,     true, false, 5, 3, 4,
         /*in=in,thresh,range,hyst,close_t=*/{0.5f, -40.0f, -40.0f, 6.0f, 5.0f},
         /*ext=attack,hold,release,dry,wet=*/{0.1f, 0.0f, 10.0f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f},
         0x1000100Eu},
    };

    for (const auto& tc : cases) {
        run_drywet_silence_test(tc);
    }
}

// =============================================================================
// Non-dry/wet ext-param coverage via slot readback.
//
// For params like comp.attack, limiter.lookahead, gate.hold, comb.damping the
// audio-behavior delta on a stable DC input is essentially zero (envelopes
// reach the same steady state regardless of attack). To still catch a
// regression in the re-init path for these params, after the second
// `init_extended_params` call we read the slot back through the VM's state
// pool and verify the new constants are present. This proves the value
// reached the place the opcode reads from.
// =============================================================================

namespace {

template<std::size_t N>
void check_slot_constants(VM& vm, std::uint32_t opcode_state_id,
                          std::array<float, N> expected) {
    const auto* ep = vm.states().get_if<ExtendedParams<N>>(
        ext_params_state_id(opcode_state_id));
    REQUIRE(ep != nullptr);
    for (std::size_t i = 0; i < N; ++i) {
        INFO("ext slot " << i);
        CHECK(ep->params[i].is_constant());
        CHECK_THAT(ep->params[i].constant, WithinAbs(expected[i], 1e-6f));
    }
}

}  // namespace

TEST_CASE("hot-swap: non-mix ext params (comp/limiter/comb/gate) propagate on re-init",
          "[hot_swap][ext_params][nonmix]") {
    // For each non-dry/wet ext-param builtin: load a trivial program that
    // references its opcode (with a fixed state_id), apply initial constants,
    // run a few blocks, re-apply different constants, read the slot back.
    constexpr std::uint16_t BUF_BASE  = 0;
    constexpr std::uint16_t BUF_OUT_L = 5;
    constexpr std::uint16_t BUF_OUT_R = 6;

    struct NonMixCase {
        const char* name;
        Opcode opcode;
        std::uint8_t ext_count;
        std::array<float, 5> input_defaults;
        std::array<float, 5> init_a;
        std::array<float, 5> init_b;
        std::uint32_t state_id;
    };

    const NonMixCase cases[] = {
        {"comb",    Opcode::EFFECT_COMB,     1,
         /*in=*/{0.5f, 0.01f, 0.5f, 0.0f, 1.0f},
         /*A=*/{0.0f, 0.0f, 0.0f, 0.0f, 0.0f},     // damping=0
         /*B=*/{0.85f, 0.0f, 0.0f, 0.0f, 0.0f},    // damping=0.85
         0x20002000u},
        {"comp",    Opcode::DYNAMICS_COMP,   2,
         /*in=*/{0.9f, -12.0f, 4.0f, 0.0f, 1.0f},
         /*A=*/{0.1f, 10.0f, 0.0f, 0.0f, 0.0f},    // attack=0.1, release=10
         /*B=*/{50.0f, 250.0f, 0.0f, 0.0f, 0.0f},  // attack=50, release=250
         0x20002001u},
        {"limiter", Opcode::DYNAMICS_LIMITER, 1,
         /*in=*/{0.95f, -3.0f, 50.0f, 0.0f, 1.0f},
         /*A=*/{0.0f, 0.0f, 0.0f, 0.0f, 0.0f},     // lookahead=0
         /*B=*/{5.0f, 0.0f, 0.0f, 0.0f, 0.0f},     // lookahead=5
         0x20002002u},
        {"gate",    Opcode::DYNAMICS_GATE,    5,
         /*in=*/{0.5f, -40.0f, -40.0f, 6.0f, 5.0f},
         /*A=*/{0.1f, 0.0f, 10.0f, 1.0f, 0.5f},
         /*B=*/{99.0f, 50.0f, 333.0f, 0.0f, 0.25f},
         0x20002003u},
    };

    for (const auto& tc : cases) {
        INFO("builtin: " << tc.name);

        auto make_program = [&]() {
            std::vector<Instruction> prog;
            for (std::uint16_t i = 0; i < 5; ++i) {
                prog.push_back(push_const(static_cast<std::uint16_t>(BUF_BASE + i),
                                          tc.input_defaults[i]));
            }
            Instruction op{};
            op.opcode = tc.opcode;
            op.out_buffer = BUF_OUT_L;
            for (auto& in : op.inputs) in = BUFFER_UNUSED;
            for (std::uint16_t i = 0; i < 5; ++i) {
                op.inputs[i] = static_cast<std::uint16_t>(BUF_BASE + i);
            }
            op.state_id = tc.state_id;
            prog.push_back(op);
            prog.push_back(Instruction::make_binary(Opcode::OUTPUT, 0, BUF_OUT_L, BUF_OUT_R));
            return prog;
        };

        VM vm;
        vm.set_crossfade_blocks(3);

        // Load A.
        auto bcA = make_program();
        REQUIRE(vm.load_program(bcA) == VM::LoadResult::Success);
        std::array<std::uint16_t, 8> bufs;
        bufs.fill(0xFFFFu);
        vm.init_extended_params(ext_params_state_id(tc.state_id),
                                tc.init_a.data(), bufs.data(), tc.ext_count);
        run_blocks(vm, 16);

        // Load B with new constants. Verify the slot now holds the new values.
        auto bcB = make_program();
        load_with_retry(vm, bcB);
        vm.init_extended_params(ext_params_state_id(tc.state_id),
                                tc.init_b.data(), bufs.data(), tc.ext_count);
        run_blocks(vm, 8);

        auto check = [&]<std::size_t N>() {
            std::array<float, N> expected{};
            for (std::size_t i = 0; i < N; ++i) expected[i] = tc.init_b[i];
            check_slot_constants<N>(vm, tc.state_id, expected);
        };
        if      (tc.ext_count == 1) check.template operator()<1>();
        else if (tc.ext_count == 2) check.template operator()<2>();
        else                        check.template operator()<5>();
    }
}

