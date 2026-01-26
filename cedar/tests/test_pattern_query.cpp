// Tests for lazy queryable patterns (Tidal-style PAT_QUERY/PAT_STEP)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cedar/opcodes/dsp_state.hpp>
#include <cedar/opcodes/sequencing.hpp>
#include <cedar/vm/context.hpp>
#include <cedar/vm/buffer_pool.hpp>
#include <cedar/vm/state_pool.hpp>
#include <cmath>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// Helper to create a test execution context
static ExecutionContext make_test_context(BufferPool& buffers, StatePool& states) {
    ExecutionContext ctx;
    ctx.buffers = &buffers;
    ctx.states = &states;
    ctx.sample_rate = 48000.0f;
    ctx.inv_sample_rate = 1.0f / 48000.0f;
    ctx.bpm = 120.0f;
    ctx.global_sample_counter = 0;
    ctx.block_counter = 0;
    return ctx;
}

TEST_CASE("PatternNode basic structure", "[pattern_query]") {
    PatternNode node;

    SECTION("Default construction") {
        CHECK(node.op == PatternOp::SILENCE);
        CHECK(node.num_children == 0);
        CHECK(node.first_child_idx == 0);
        CHECK(node.time_offset == 0.0f);
    }

    SECTION("ATOM node") {
        node.op = PatternOp::ATOM;
        node.data.float_val = 440.0f;
        CHECK(node.data.float_val == 440.0f);
    }

    SECTION("EUCLID node") {
        node.op = PatternOp::EUCLID;
        node.data.euclid.hits = 3;
        node.data.euclid.steps = 8;
        node.data.euclid.rotation = 0;
        CHECK(node.data.euclid.hits == 3);
        CHECK(node.data.euclid.steps == 8);
    }
}

TEST_CASE("PatternQueryState size check", "[pattern_query]") {
    // Verify PatternQueryState size is reasonable
    INFO("sizeof(PatternNode) = " << sizeof(PatternNode));
    INFO("sizeof(QueryEvent) = " << sizeof(QueryEvent));
    INFO("sizeof(PatternQueryState) = " << sizeof(PatternQueryState));
    INFO("sizeof(DSPState) = " << sizeof(DSPState));

    CHECK(sizeof(PatternNode) == 12);
    CHECK(sizeof(QueryEvent) == 16);
    // PatternQueryState should be under 700 bytes to fit in state pool
    CHECK(sizeof(PatternQueryState) < 700);
}

TEST_CASE("PatternQueryState initialization", "[pattern_query]") {
    // Create a simple pattern: single ATOM
    PatternNode nodes[1];
    nodes[0].op = PatternOp::ATOM;
    nodes[0].data.float_val = 440.0f;
    nodes[0].num_children = 0;

    // Test without state pool first - direct initialization
    PatternQueryState direct_state{};
    direct_state.num_nodes = 1;
    direct_state.nodes[0] = nodes[0];
    direct_state.cycle_length = 4.0f;
    direct_state.is_sample_pattern = false;
    direct_state.pattern_seed = 0x12345678;

    CHECK(direct_state.num_nodes == 1);
    CHECK(direct_state.cycle_length == 4.0f);
    CHECK(direct_state.nodes[0].op == PatternOp::ATOM);
    CHECK(direct_state.nodes[0].data.float_val == 440.0f);
}

TEST_CASE("PatternQueryState in StatePool", "[pattern_query]") {
    StatePool states;

    // Create a simple pattern: single ATOM
    PatternNode nodes[1];
    nodes[0].op = PatternOp::ATOM;
    nodes[0].data.float_val = 440.0f;
    nodes[0].num_children = 0;

    states.init_pattern_program(0x12345678, nodes, 1, 4.0f, false);

    auto& state = states.get_or_create<PatternQueryState>(0x12345678);
    CHECK(state.num_nodes == 1);
    CHECK(state.cycle_length == 4.0f);
    CHECK(state.is_sample_pattern == false);
    CHECK(state.pattern_seed != 0);  // Should be initialized from state_id
    CHECK(state.nodes[0].op == PatternOp::ATOM);
    CHECK(state.nodes[0].data.float_val == 440.0f);
}

TEST_CASE("Deterministic randomness", "[pattern_query]") {
    SECTION("Same seed and time produce same result") {
        std::uint64_t seed = 0x123456789ABCDEFull;
        float time1 = 1.5f;

        float r1 = deterministic_random(seed, time1);
        float r2 = deterministic_random(seed, time1);

        CHECK(r1 == r2);
    }

    SECTION("Different seeds produce different results") {
        std::uint64_t seed1 = 0x123456789ABCDEFull;
        std::uint64_t seed2 = 0xFEDCBA9876543210ull;
        float time = 1.5f;

        float r1 = deterministic_random(seed1, time);
        float r2 = deterministic_random(seed2, time);

        CHECK(r1 != r2);
    }

    SECTION("Different times produce different results") {
        std::uint64_t seed = 0x123456789ABCDEFull;
        float time1 = 1.5f;
        float time2 = 2.5f;

        float r1 = deterministic_random(seed, time1);
        float r2 = deterministic_random(seed, time2);

        CHECK(r1 != r2);
    }

    SECTION("Results are in [0, 1) range") {
        std::uint64_t seed = 0x123456789ABCDEFull;
        for (float t = 0.0f; t < 10.0f; t += 0.1f) {
            float r = deterministic_random(seed, t);
            CHECK(r >= 0.0f);
            CHECK(r < 1.0f);
        }
    }
}

TEST_CASE("Pattern evaluation - simple ATOM", "[pattern_query]") {
    StatePool states;

    // Create a pattern with single ATOM
    PatternNode nodes[1];
    nodes[0].op = PatternOp::ATOM;
    nodes[0].data.float_val = 440.0f;
    nodes[0].num_children = 0;

    states.init_pattern_program(0x1111, nodes, 1, 4.0f, false);
    auto& state = states.get_or_create<PatternQueryState>(0x1111);

    // Query pattern for beat 0-4 (one cycle)
    PatternQueryContext ctx{
        .arc_start = 0.0f,
        .arc_end = 4.0f,
        .time_scale = 4.0f,
        .time_offset = 0.0f,
        .rng_seed = state.pattern_seed,
        .state = &state
    };

    state.num_events = 0;
    evaluate_pattern_node(&state, 0, ctx);

    // Should have one event at time 0
    CHECK(state.num_events == 1);
    CHECK_THAT(state.events[0].value, WithinAbs(440.0f, 0.001f));
}

TEST_CASE("Pattern evaluation - CAT (sequential)", "[pattern_query]") {
    StatePool states;

    // Create a pattern: CAT [ATOM(220), ATOM(440)]
    PatternNode nodes[3];

    // Node 0: CAT with 2 children starting at index 1
    nodes[0].op = PatternOp::CAT;
    nodes[0].num_children = 2;
    nodes[0].first_child_idx = 1;

    // Node 1: First ATOM (220 Hz)
    nodes[1].op = PatternOp::ATOM;
    nodes[1].data.float_val = 220.0f;
    nodes[1].num_children = 0;

    // Node 2: Second ATOM (440 Hz)
    nodes[2].op = PatternOp::ATOM;
    nodes[2].data.float_val = 440.0f;
    nodes[2].num_children = 0;

    states.init_pattern_program(0x2222, nodes, 3, 4.0f, false);
    auto& state = states.get_or_create<PatternQueryState>(0x2222);

    // Query full cycle
    PatternQueryContext ctx{
        .arc_start = 0.0f,
        .arc_end = 4.0f,
        .time_scale = 4.0f,
        .time_offset = 0.0f,
        .rng_seed = state.pattern_seed,
        .state = &state
    };

    state.num_events = 0;
    evaluate_pattern_node(&state, 0, ctx);
    sort_query_events(state);

    // Should have two events
    REQUIRE(state.num_events == 2);

    // First event at time 0 with 220 Hz
    CHECK_THAT(state.events[0].time, WithinAbs(0.0f, 0.01f));
    CHECK_THAT(state.events[0].value, WithinAbs(220.0f, 0.001f));

    // Second event at time 2 (half cycle) with 440 Hz
    CHECK_THAT(state.events[1].time, WithinAbs(2.0f, 0.01f));
    CHECK_THAT(state.events[1].value, WithinAbs(440.0f, 0.001f));
}

TEST_CASE("Pattern evaluation - SILENCE", "[pattern_query]") {
    StatePool states;

    PatternNode nodes[1];
    nodes[0].op = PatternOp::SILENCE;
    nodes[0].num_children = 0;

    states.init_pattern_program(0x3333, nodes, 1, 4.0f, false);
    auto& state = states.get_or_create<PatternQueryState>(0x3333);

    PatternQueryContext ctx{
        .arc_start = 0.0f,
        .arc_end = 4.0f,
        .time_scale = 4.0f,
        .time_offset = 0.0f,
        .rng_seed = state.pattern_seed,
        .state = &state
    };

    state.num_events = 0;
    evaluate_pattern_node(&state, 0, ctx);

    // Silence produces no events
    CHECK(state.num_events == 0);
}

TEST_CASE("Pattern evaluation - FAST modifier", "[pattern_query]") {
    StatePool states;

    // FAST(2) with single ATOM
    PatternNode nodes[2];

    nodes[0].op = PatternOp::FAST;
    nodes[0].data.float_val = 2.0f;  // Speed up 2x
    nodes[0].num_children = 1;
    nodes[0].first_child_idx = 1;

    nodes[1].op = PatternOp::ATOM;
    nodes[1].data.float_val = 440.0f;
    nodes[1].num_children = 0;

    states.init_pattern_program(0x4444, nodes, 2, 4.0f, false);
    auto& state = states.get_or_create<PatternQueryState>(0x4444);

    PatternQueryContext ctx{
        .arc_start = 0.0f,
        .arc_end = 4.0f,
        .time_scale = 4.0f,
        .time_offset = 0.0f,
        .rng_seed = state.pattern_seed,
        .state = &state
    };

    state.num_events = 0;
    evaluate_pattern_node(&state, 0, ctx);

    // FAST(2) should compress time scale
    REQUIRE(state.num_events >= 1);
    // The event should have shorter duration due to FAST
    CHECK(state.events[0].duration < 4.0f);
}

TEST_CASE("Pattern evaluation - EUCLID", "[pattern_query]") {
    StatePool states;

    // EUCLID(3, 8) with ATOM child
    PatternNode nodes[2];

    nodes[0].op = PatternOp::EUCLID;
    nodes[0].data.euclid.hits = 3;
    nodes[0].data.euclid.steps = 8;
    nodes[0].data.euclid.rotation = 0;
    nodes[0].num_children = 1;
    nodes[0].first_child_idx = 1;

    nodes[1].op = PatternOp::ATOM;
    nodes[1].data.float_val = 440.0f;
    nodes[1].num_children = 0;

    states.init_pattern_program(0x5555, nodes, 2, 4.0f, false);
    auto& state = states.get_or_create<PatternQueryState>(0x5555);

    PatternQueryContext ctx{
        .arc_start = 0.0f,
        .arc_end = 4.0f,
        .time_scale = 4.0f,
        .time_offset = 0.0f,
        .rng_seed = state.pattern_seed,
        .state = &state
    };

    state.num_events = 0;
    evaluate_pattern_node(&state, 0, ctx);

    // E(3,8) should produce 3 hits spread across 8 steps
    CHECK(state.num_events == 3);
}

TEST_CASE("op_pat_query basic operation", "[pattern_query]") {
    BufferPool buffers;
    StatePool states;
    auto ctx = make_test_context(buffers, states);

    // Create simple pattern
    PatternNode nodes[1];
    nodes[0].op = PatternOp::ATOM;
    nodes[0].data.float_val = 440.0f;
    nodes[0].num_children = 0;

    std::uint32_t state_id = 0x6666;
    states.init_pattern_program(state_id, nodes, 1, 4.0f, false);

    // Create instruction
    Instruction inst{};
    inst.opcode = Opcode::PAT_QUERY;
    inst.state_id = state_id;

    // Run query
    op_pat_query(ctx, inst);

    // Check state was queried
    auto& state = states.get_or_create<PatternQueryState>(state_id);
    CHECK(state.num_events >= 1);
}

TEST_CASE("op_pat_step basic operation", "[pattern_query]") {
    BufferPool buffers;
    StatePool states;
    auto ctx = make_test_context(buffers, states);

    // Create simple pattern
    PatternNode nodes[1];
    nodes[0].op = PatternOp::ATOM;
    nodes[0].data.float_val = 440.0f;
    nodes[0].num_children = 0;

    std::uint32_t state_id = 0x7777;
    states.init_pattern_program(state_id, nodes, 1, 4.0f, false);

    // Use fixed buffer indices
    std::uint16_t value_buf = 0;
    std::uint16_t velocity_buf = 1;
    std::uint16_t trigger_buf = 2;

    // Clear buffers first
    buffers.clear(value_buf);
    buffers.clear(velocity_buf);
    buffers.clear(trigger_buf);

    // First query the pattern
    Instruction query_inst{};
    query_inst.opcode = Opcode::PAT_QUERY;
    query_inst.state_id = state_id;
    op_pat_query(ctx, query_inst);

    // Then step through results
    Instruction step_inst{};
    step_inst.opcode = Opcode::PAT_STEP;
    step_inst.out_buffer = value_buf;
    step_inst.inputs[0] = velocity_buf;
    step_inst.inputs[1] = trigger_buf;
    step_inst.state_id = state_id;
    op_pat_step(ctx, step_inst);

    // Check outputs
    float* value_data = buffers.get(value_buf);
    float* velocity_data = buffers.get(velocity_buf);
    float* trigger_data = buffers.get(trigger_buf);

    // Should have value near 440 Hz somewhere in the buffer
    bool has_value = false;
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        if (value_data[i] > 400.0f && value_data[i] < 500.0f) {
            has_value = true;
            break;
        }
    }
    CHECK(has_value);

    // Check velocity and trigger are written (not NaN)
    CHECK(velocity_data[0] == velocity_data[0]);  // Not NaN
    CHECK(trigger_data[0] == trigger_data[0]);    // Not NaN
}

TEST_CASE("DEGRADE with deterministic randomness", "[pattern_query]") {
    StatePool states;

    // DEGRADE(0.5) with ATOM
    PatternNode nodes[2];

    nodes[0].op = PatternOp::DEGRADE;
    nodes[0].data.float_val = 0.5f;  // 50% chance
    nodes[0].num_children = 1;
    nodes[0].first_child_idx = 1;

    nodes[1].op = PatternOp::ATOM;
    nodes[1].data.float_val = 440.0f;
    nodes[1].num_children = 0;

    states.init_pattern_program(0x8888, nodes, 2, 4.0f, false);
    auto& state = states.get_or_create<PatternQueryState>(0x8888);

    // Run same query multiple times - should get same result
    auto run_query = [&]() {
        state.num_events = 0;
        PatternQueryContext ctx{
            .arc_start = 0.0f,
            .arc_end = 4.0f,
            .time_scale = 4.0f,
            .time_offset = 0.0f,
            .rng_seed = state.pattern_seed,
            .state = &state
        };
        evaluate_pattern_node(&state, 0, ctx);
        return state.num_events;
    };

    std::uint32_t result1 = run_query();
    std::uint32_t result2 = run_query();
    std::uint32_t result3 = run_query();

    // Deterministic: same query should always produce same result
    CHECK(result1 == result2);
    CHECK(result2 == result3);
}

TEST_CASE("CHOOSE with deterministic randomness", "[pattern_query]") {
    StatePool states;

    // CHOOSE between ATOM(220) and ATOM(440)
    PatternNode nodes[3];

    nodes[0].op = PatternOp::CHOOSE;
    nodes[0].num_children = 2;
    nodes[0].first_child_idx = 1;

    nodes[1].op = PatternOp::ATOM;
    nodes[1].data.float_val = 220.0f;
    nodes[1].num_children = 0;

    nodes[2].op = PatternOp::ATOM;
    nodes[2].data.float_val = 440.0f;
    nodes[2].num_children = 0;

    states.init_pattern_program(0x9999, nodes, 3, 4.0f, false);
    auto& state = states.get_or_create<PatternQueryState>(0x9999);

    // Run same query multiple times
    auto run_query = [&]() {
        state.num_events = 0;
        PatternQueryContext ctx{
            .arc_start = 0.0f,
            .arc_end = 4.0f,
            .time_scale = 4.0f,
            .time_offset = 0.0f,
            .rng_seed = state.pattern_seed,
            .state = &state
        };
        evaluate_pattern_node(&state, 0, ctx);
        return state.events[0].value;
    };

    float result1 = run_query();
    float result2 = run_query();
    float result3 = run_query();

    // Deterministic: same choice every time
    CHECK_THAT(result1, WithinAbs(result2, 0.001f));
    CHECK_THAT(result2, WithinAbs(result3, 0.001f));

    // Result should be either 220 or 440
    CHECK((std::abs(result1 - 220.0f) < 0.001f || std::abs(result1 - 440.0f) < 0.001f));
}
