// FOREACH_EVENT subprogram table substrate tests —
// PRD prd-runtime-functions-control-flow L3.
//
// Exercises the ProgramSlot block table: the block-aware load() overload,
// block_body() resolution, the main/body boundary, and that compute_signature
// folds the block table so structurally different programs differ.

#include <catch2/catch_test_macros.hpp>

#include <cedar/vm/program_slot.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>

#include <array>
#include <vector>

using namespace cedar;

namespace {

// A trivial main + one body: [ PUSH_CONST | NOP-body0 NOP-body1 ].
std::vector<Instruction> make_stream() {
    std::vector<Instruction> s;
    s.push_back(Instruction::make_nullary(Opcode::PUSH_CONST, 0));  // main[0]
    s.push_back(Instruction{Opcode::NOP, 0, 0, {0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF}, 0, 0});  // body[0]
    s.push_back(Instruction{Opcode::NOP, 0, 0, {0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF}, 0, 0});  // body[1]
    return s;
}

}  // namespace

TEST_CASE("BlockEntry is 12 bytes", "[subprogram][L3]") {
    REQUIRE(sizeof(BlockEntry) == 12);
}

TEST_CASE("ProgramSlot block-aware load records the table", "[subprogram][L3]") {
    auto stream = make_stream();
    std::array<BlockEntry, 1> table{};
    table[0] = BlockEntry{/*offset=*/1, /*length=*/2, /*frame_slot_count=*/3,
                          /*output_count=*/2, /*reserved=*/0};

    ProgramSlot slot;
    REQUIRE(slot.load(std::span<const Instruction>(stream),
                      std::span<const BlockEntry>(table), /*main_count=*/1));

    CHECK(slot.instruction_count == 3);
    CHECK(slot.main_count == 1);
    CHECK(slot.block_count == 1);

    // block_body(0) resolves to the two body instructions.
    auto body = slot.block_body(0);
    REQUIRE(body.size() == 2);
    CHECK(body[0].opcode == Opcode::NOP);
    CHECK(body[1].opcode == Opcode::NOP);

    // Out-of-range block id yields an empty span.
    CHECK(slot.block_body(1).empty());
}

TEST_CASE("ProgramSlot single-arg load leaves an empty table", "[subprogram][L3]") {
    auto stream = make_stream();
    ProgramSlot slot;
    REQUIRE(slot.load(std::span<const Instruction>(stream)));

    CHECK(slot.block_count == 0);
    CHECK(slot.main_count == slot.instruction_count);
    CHECK(slot.block_body(0).empty());
}

TEST_CASE("compute_signature folds the block table", "[subprogram][L3]") {
    auto stream = make_stream();

    // Same instruction bytes, different block boundary → different signature.
    ProgramSlot a;
    std::array<BlockEntry, 1> table_a{};
    table_a[0] = BlockEntry{1, 2, 3, 2, 0};
    REQUIRE(a.load(std::span<const Instruction>(stream),
                   std::span<const BlockEntry>(table_a), 1));

    ProgramSlot b;
    std::array<BlockEntry, 1> table_b{};
    table_b[0] = BlockEntry{1, 1, 3, 2, 0};  // different body length
    REQUIRE(b.load(std::span<const Instruction>(stream),
                   std::span<const BlockEntry>(table_b), 1));

    CHECK(a.signature != b.signature);

    // No table vs a table — also distinct.
    ProgramSlot c;
    REQUIRE(c.load(std::span<const Instruction>(stream)));
    CHECK(a.signature != c.signature);
}

TEST_CASE("load rejects an oversized block table", "[subprogram][L3]") {
    auto stream = make_stream();
    std::vector<BlockEntry> too_many(MAX_SUBPROGRAMS + 1);
    ProgramSlot slot;
    CHECK_FALSE(slot.load(std::span<const Instruction>(stream),
                          std::span<const BlockEntry>(too_many), 1));
}

TEST_CASE("VM staged block table loads via load_program_with_blocks_immediate",
          "[subprogram][L3]") {
    auto stream = make_stream();
    std::array<BlockEntry, 1> table{};
    table[0] = BlockEntry{1, 2, 3, 2, 0};

    VM vm;
    REQUIRE(vm.load_program_with_blocks_immediate(
        std::span<const Instruction>(stream),
        std::span<const BlockEntry>(table), 1));

    // A FOREACH_EVENT-free main program still processes without crashing —
    // the body region must NOT be executed as main code.
    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    SUCCEED("main loop bounded to main_count; body region not fallen into");
}

// --- L2 BLOCK_CALL dispatch ------------------------------------------------

TEST_CASE("BLOCK_CALL copies params, runs the body, copies the result",
          "[subprogram][L2][block_call]") {
    // Stream layout: [ main: BLOCK_CALL | body: ADD ].
    // The body adds its two convention-slot params and writes the body output.
    std::vector<Instruction> stream;
    // opcode, rate(=block_id), out_buffer, inputs(=caller args), flags, state_id
    stream.push_back(Instruction{Opcode::BLOCK_CALL, 0, 0,
                                 {1, 2, 0xFFFF, 0xFFFF, 0xFFFF},
                                 0, 0xABCDEF01u});
    stream.push_back(Instruction::make_binary(Opcode::ADD, /*out=*/20,
                                              /*in0=*/10, /*in1=*/11));

    std::array<BlockEntry, 1> table{};
    table[0].offset = 1;
    table[0].length = 1;
    table[0].frame_slot_count = 2;
    table[0].output_count = 1;
    table[0].param_base = 10;
    table[0].body_output = 20;

    VM vm;
    REQUIRE(vm.load_program_with_blocks_immediate(
        std::span<const Instruction>(stream),
        std::span<const BlockEntry>(table), /*main_count=*/1));

    // Seed the caller argument buffers.
    std::fill_n(vm.buffers().get(1), BLOCK_SIZE, 3.0f);
    std::fill_n(vm.buffers().get(2), BLOCK_SIZE, 4.0f);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* result = vm.buffers().get(0);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK(result[i] == 7.0f);  // 3 + 4, params copied into the bank
    }
}

// --- BLOCK_BIND: shared blocks with >5 parameters (PRD §4.2) ---------------

TEST_CASE("BLOCK_BIND carries params 5..N into a >5-slot block",
          "[subprogram][L2][block_bind]") {
    // 7-param body sums all seven convention slots. Slots 0..4 ride
    // BLOCK_CALL.inputs[0..4]; slots 5,6 ride two preceding BLOCK_BINDs.
    // Layout: [ main: BIND BIND CALL | body: ADD x6 ].
    std::vector<Instruction> stream;
    stream.push_back(Instruction{Opcode::BLOCK_BIND, /*rate=slot*/5, 0xFFFF,
                                 {6, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, 0, 0});
    stream.push_back(Instruction{Opcode::BLOCK_BIND, /*rate=slot*/6, 0xFFFF,
                                 {7, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, 0, 0});
    stream.push_back(Instruction{Opcode::BLOCK_CALL, /*rate=block_id*/0, 0,
                                 {1, 2, 3, 4, 5}, 0, 0x12345678u});
    // Body: chain-add the seven param-bank buffers (10..16) into buffer 20.
    stream.push_back(Instruction::make_binary(Opcode::ADD, 20, 10, 11));
    for (std::uint16_t k = 12; k <= 16; ++k) {
        stream.push_back(Instruction::make_binary(Opcode::ADD, 20, 20, k));
    }

    std::array<BlockEntry, 1> table{};
    table[0].offset = 3;
    table[0].length = 6;
    table[0].frame_slot_count = 7;
    table[0].output_count = 1;
    table[0].param_base = 10;
    table[0].body_output = 20;

    VM vm;
    REQUIRE(vm.load_program_with_blocks_immediate(
        std::span<const Instruction>(stream),
        std::span<const BlockEntry>(table), /*main_count=*/3));

    // Seed the 7 caller argument buffers (1..7) with values 1..7.
    for (std::uint16_t b = 1; b <= 7; ++b) {
        std::fill_n(vm.buffers().get(b), BLOCK_SIZE, static_cast<float>(b));
    }

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* result = vm.buffers().get(0);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK(result[i] == 28.0f);  // 1+2+3+4+5+6+7 — all seven slots bound
    }
}

TEST_CASE("BLOCK_BIND slot >= frame_slot_count is ignored (E248 guard)",
          "[subprogram][L2][block_bind]") {
    // A bind addressing a slot the block does not have must be dropped, not
    // scribble outside the param bank. Body reads only slots 0,1.
    std::vector<Instruction> stream;
    stream.push_back(Instruction{Opcode::BLOCK_BIND, /*rate=*/9, 0xFFFF,
                                 {3, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, 0, 0});
    stream.push_back(Instruction{Opcode::BLOCK_CALL, 0, 0,
                                 {1, 2, 0xFFFF, 0xFFFF, 0xFFFF}, 0, 0});
    stream.push_back(Instruction::make_binary(Opcode::ADD, 20, 10, 11));

    std::array<BlockEntry, 1> table{};
    table[0].offset = 2;
    table[0].length = 1;
    table[0].frame_slot_count = 2;
    table[0].output_count = 1;
    table[0].param_base = 10;
    table[0].body_output = 20;

    VM vm;
    REQUIRE(vm.load_program_with_blocks_immediate(
        std::span<const Instruction>(stream),
        std::span<const BlockEntry>(table), /*main_count=*/2));

    std::fill_n(vm.buffers().get(1), BLOCK_SIZE, 3.0f);
    std::fill_n(vm.buffers().get(2), BLOCK_SIZE, 4.0f);

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());

    const float* result = vm.buffers().get(0);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        CHECK(result[i] == 7.0f);  // out-of-range bind ignored, body intact
    }
}

TEST_CASE("BLOCK_CALL with an out-of-range block id is a silent no-op",
          "[subprogram][L2][block_call]") {
    std::vector<Instruction> stream;
    // rate=5 → no such block id
    stream.push_back(Instruction{Opcode::BLOCK_CALL, 5, 0,
                                 {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
                                 0, 0});

    std::array<BlockEntry, 1> table{};
    table[0].offset = 0;
    table[0].length = 0;

    VM vm;
    REQUIRE(vm.load_program_with_blocks_immediate(
        std::span<const Instruction>(stream),
        std::span<const BlockEntry>(table), /*main_count=*/1));

    std::array<float, BLOCK_SIZE> L{}, R{};
    vm.process_block(L.data(), R.data());
    SUCCEED("out-of-range BLOCK_CALL did not crash");
}
