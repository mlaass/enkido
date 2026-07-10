// Unit tests for the HOST_OP dispatch path and arena-backed host-op state
// (prd-host-extension-api §10, Phase 3). Compiled only when
// CEDAR_HOST_EXTENSIONS is on; the OFF build must not see HOST_OP at all.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/vm.hpp>
#include <cedar/vm/host_op_registry.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/opcodes/dsp_state.hpp>
#include <cedar/opcodes/utility.hpp>

#include <array>

using namespace cedar;
using Catch::Matchers::WithinAbs;

namespace {

// Host op 0: out[i] = in[i] * 2. Stateless.
void host_double(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    const float* in = ctx.buffers->get(inst.inputs[0]);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = in[i] * 2.0f;
}

// Host op 1: a running sample counter kept in arena-backed state. Writes the
// counter value to every output sample so state persistence is observable.
//
// `magic` distinguishes a freshly zeroed arena region from a rebound one, so
// the test can count *instantiations* the way a pooled plugin fixture would.
constexpr std::uint32_t kMagic = 0x00C0FFEE;

struct CounterState {
    std::uint32_t magic;
    std::uint32_t count;
};

int g_instantiations = 0;

void host_counter(ExecutionContext& ctx, const Instruction& inst) {
    float* out = ctx.buffers->get(inst.out_buffer);
    auto& st = ctx.states->get_or_create<HostOpState>(inst.state_id);
    auto* counter = static_cast<CounterState*>(st.ptr);
    if (counter == nullptr) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = -1.0f;
        return;
    }
    if (counter->magic != kMagic) {  // first touch of a zeroed region
        counter->magic = kMagic;
        counter->count = 0;
        ++g_instantiations;
    }
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = static_cast<float>(counter->count++);
    }
}

struct RegistryGuard {
    RegistryGuard() { HostOpRegistry::instance().reset_for_testing(); g_instantiations = 0; }
    ~RegistryGuard() { HostOpRegistry::instance().reset_for_testing(); }
};

}  // namespace

TEST_CASE("HostOpRegistry rejects collisions and null impls", "[host-op]") {
    RegistryGuard guard;
    auto& reg = HostOpRegistry::instance();

    REQUIRE(reg.register_op(0, &host_double, 0));
    REQUIRE(reg.is_bound(0));

    // Same index twice → rejected, first binding survives.
    REQUIRE_FALSE(reg.register_op(0, &host_counter, sizeof(CounterState)));
    REQUIRE(reg.get(0) == &host_double);
    REQUIRE(reg.state_bytes(0) == 0);

    // Null impl → rejected.
    REQUIRE_FALSE(reg.register_op(1, nullptr, 0));
    REQUIRE_FALSE(reg.is_bound(1));
}

TEST_CASE("HOST_OP dispatches to the registered impl", "[host-op]") {
    RegistryGuard guard;
    REQUIRE(HostOpRegistry::instance().register_op(0, &host_double, 0));

    VM vm;
    vm.set_sample_rate(48000.0f);

    // buf0 = 0.25 ; buf1 = host_double(buf0) ; OUTPUT buf1
    Instruction host{};
    host.opcode = Opcode::HOST_OP;
    host.rate = 0;  // registry index
    host.out_buffer = 1;
    host.inputs[0] = 0;
    host.inputs[1] = host.inputs[2] = host.inputs[3] = host.inputs[4] = 0xFFFF;

    std::array<Instruction, 3> program = {
        make_const_instruction(Opcode::PUSH_CONST, 0, 0.25f),
        host,
        Instruction::make_unary(Opcode::OUTPUT, 0, 1),
    };
    vm.load_program_immediate(program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    vm.process_block(left.data(), right.data());

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        REQUIRE_THAT(left[i], WithinAbs(0.5f, 1e-6f));
    }
}

TEST_CASE("HOST_OP with an unbound index is a no-op, not a crash", "[host-op]") {
    RegistryGuard guard;  // nothing registered

    VM vm;
    vm.set_sample_rate(48000.0f);

    Instruction host{};
    host.opcode = Opcode::HOST_OP;
    host.rate = 7;  // never bound
    host.out_buffer = 1;
    host.inputs[0] = 0;
    host.inputs[1] = host.inputs[2] = host.inputs[3] = host.inputs[4] = 0xFFFF;

    std::array<Instruction, 3> program = {
        make_const_instruction(Opcode::PUSH_CONST, 0, 0.25f),
        host,
        Instruction::make_unary(Opcode::OUTPUT, 0, 1),
    };
    vm.load_program_immediate(program);

    std::array<float, BLOCK_SIZE> left{}, right{};
    REQUIRE_NOTHROW(vm.process_block(left.data(), right.data()));
    // buf1 was never written by the missing op — it stays silent.
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        REQUIRE_THAT(left[i], WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("Stateful HOST_OP: arena state persists across blocks", "[host-op]") {
    RegistryGuard guard;
    REQUIRE(HostOpRegistry::instance().register_op(1, &host_counter, sizeof(CounterState)));

    VM vm;
    vm.set_sample_rate(48000.0f);

    Instruction host{};
    host.opcode = Opcode::HOST_OP;
    host.rate = 1;
    host.out_buffer = 0;
    host.inputs[0] = host.inputs[1] = host.inputs[2] = host.inputs[3] = host.inputs[4] = 0xFFFF;
    host.state_id = 0xABCD1234;

    std::array<Instruction, 2> program = {
        host,
        Instruction::make_unary(Opcode::OUTPUT, 0, 0),
    };
    vm.load_program_immediate(program);

    std::array<float, BLOCK_SIZE> left{}, right{};

    // First block: counter starts at 0 (loader zero-initializes the region).
    vm.process_block(left.data(), right.data());
    REQUIRE_THAT(left[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(left[BLOCK_SIZE - 1], WithinAbs(static_cast<float>(BLOCK_SIZE - 1), 1e-6f));

    // Second block continues where the first left off — state survived.
    vm.process_block(left.data(), right.data());
    REQUIRE_THAT(left[0], WithinAbs(static_cast<float>(BLOCK_SIZE), 1e-6f));
}

namespace {

std::array<Instruction, 2> counter_program(std::uint32_t state_id) {
    Instruction host{};
    host.opcode = Opcode::HOST_OP;
    host.rate = 1;
    host.out_buffer = 0;
    host.inputs[0] = host.inputs[1] = host.inputs[2] = host.inputs[3] = host.inputs[4] = 0xFFFF;
    host.state_id = state_id;
    return {host, Instruction::make_unary(Opcode::OUTPUT, 0, 0)};
}

}  // namespace

TEST_CASE("Stateful HOST_OP: state rebinds by semantic ID across hot-swaps", "[host-op]") {
    RegistryGuard guard;
    REQUIRE(HostOpRegistry::instance().register_op(1, &host_counter, sizeof(CounterState)));

    VM vm;
    vm.set_sample_rate(48000.0f);
    vm.set_crossfade_blocks(3);

    constexpr std::uint32_t kSemanticId = 0xABCD1234;
    std::array<float, BLOCK_SIZE> left{}, right{};

    auto program_a = counter_program(kSemanticId);
    REQUIRE(vm.load_program(program_a) == VM::LoadResult::Success);
    for (int i = 0; i < 4; ++i) vm.process_block(left.data(), right.data());
    REQUIRE(g_instantiations == 1);

    // 100 recompiles at the same semantic ID must never re-instantiate. This is
    // the pooling invariant the studio's plugin pool depends on.
    for (int swap = 0; swap < 100; ++swap) {
        auto next = counter_program(kSemanticId);
        REQUIRE(vm.load_program(next) == VM::LoadResult::Success);
        for (int i = 0; i < 6; ++i) vm.process_block(left.data(), right.data());
    }
    CHECK(g_instantiations == 1);

    // A different semantic ID is a structural change: fresh instance.
    auto program_c = counter_program(0x99887766);
    REQUIRE(vm.load_program(program_c) == VM::LoadResult::Success);
    for (int i = 0; i < 6; ++i) vm.process_block(left.data(), right.data());
    CHECK(g_instantiations == 2);
}
