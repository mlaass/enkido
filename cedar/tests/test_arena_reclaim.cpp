// Unit tests for the AudioArena size-classed free list and the per-state
// release() hooks (prd-audio-arena-reclamation §8.1). These exercise the
// reclamation primitive directly; the live GC-driven reclamation and the
// bounded-arena integration are covered by test_drift_fuzz.cpp.

#include <catch2/catch_test_macros.hpp>

#include <cedar/vm/audio_arena.hpp>
#include <cedar/opcodes/dsp_state.hpp>
#include <cedar/opcodes/drywet.hpp>

#include <array>
#include <vector>

using namespace cedar;

TEST_CASE("AudioArena free list: release then same-class allocate reuses block", "[arena][reclaim]") {
    AudioArena arena(1u << 20);  // 1 MB

    float* p1 = arena.allocate(100);  // class covers [65,128]
    REQUIRE(p1 != nullptr);
    arena.release(p1, 100);

    // A later request in the SAME size class (120 rounds to the same 128-float
    // class as 100) pops the freed block back — same pointer.
    float* p2 = arena.allocate(120);
    REQUIRE(p2 == p1);
}

TEST_CASE("AudioArena free list: reused block is re-zeroed", "[arena][reclaim]") {
    AudioArena arena(1u << 20);

    float* p1 = arena.allocate(100);
    REQUIRE(p1 != nullptr);
    for (int i = 0; i < 128; ++i) p1[i] = 1234.5f;  // dirty the whole class block
    arena.release(p1, 100);

    float* p2 = arena.allocate(100);
    REQUIRE(p2 == p1);
    for (int i = 0; i < 128; ++i) {
        REQUIRE(p2[i] == 0.0f);  // recycled buffer must be clean (no stale audio)
    }
}

TEST_CASE("AudioArena free list: cross-class requests do not reuse", "[arena][reclaim]") {
    AudioArena arena(1u << 20);

    float* small = arena.allocate(100);   // small class
    arena.release(small, 100);

    float* large = arena.allocate(2000);  // different (larger) class → bumps fresh
    REQUIRE(large != small);

    float* small2 = arena.allocate(100);  // back in the small class → reuses
    REQUIRE(small2 == small);
}

TEST_CASE("AudioArena free list: grow path reuses the old block later", "[arena][reclaim]") {
    AudioArena arena(1u << 20);

    float* small = arena.allocate(50);    // class A
    arena.release(small, 50);             // grow-path returns the old block
    float* large = arena.allocate(5000);  // class B (no reuse of A)
    REQUIRE(large != small);

    float* small2 = arena.allocate(60);   // class A again → old block back
    REQUIRE(small2 == small);
}

TEST_CASE("AudioArena free list: overflow drops blocks without crash or bump growth", "[arena][reclaim]") {
    AudioArena arena(1u << 20);

    // Allocate more class-3 (8-float) blocks than a class can hold.
    std::vector<float*> blocks;
    for (int i = 0; i < 300; ++i) {
        float* p = arena.allocate(8);
        REQUIRE(p != nullptr);
        blocks.push_back(p);
    }
    const std::size_t used_after_alloc = arena.bytes_used();

    // Releasing 300 with a 256-cap drops 44; release never advances the bump.
    for (float* p : blocks) arena.release(p, 8);
    REQUIRE(arena.bytes_used() == used_after_alloc);

    // The pooled blocks are still reusable; no crash.
    for (int i = 0; i < 256; ++i) {
        REQUIRE(arena.allocate(8) != nullptr);
    }
}

TEST_CASE("AudioArena free list: cleared on reset", "[arena][reclaim]") {
    AudioArena arena(1u << 20);
    float* p1 = arena.allocate(100);
    arena.release(p1, 100);
    arena.reset();
    REQUIRE(arena.bytes_used() == 0);
    // After reset the free list is empty: allocation bumps from offset 0.
    float* p2 = arena.allocate(100);
    REQUIRE(p2 != nullptr);
    REQUIRE(arena.bytes_used() > 0);
}

TEST_CASE("DSP state release() returns buffers to the arena for reuse", "[arena][reclaim]") {
    AudioArena arena(1u << 20);

    SECTION("DelayState") {
        DelayState d;
        d.ensure_buffer(1000, &arena);
        REQUIRE(d.buffers_ready());
        const std::size_t used = arena.bytes_used();

        d.release(arena);
        REQUIRE_FALSE(d.buffers_ready());

        DelayState d2;
        d2.ensure_buffer(1000, &arena);
        REQUIRE(d2.buffers_ready());
        REQUIRE(arena.bytes_used() == used);  // both buffers reclaimed, no new bump
    }

    SECTION("FreeverbState (multi-buffer)") {
        FreeverbState fv;
        fv.ensure_buffers(&arena);
        REQUIRE(fv.buffers_ready());
        const std::size_t used = arena.bytes_used();

        fv.release(arena);
        REQUIRE_FALSE(fv.buffers_ready());

        FreeverbState fv2;
        fv2.ensure_buffers(&arena);
        REQUIRE(fv2.buffers_ready());
        REQUIRE(arena.bytes_used() == used);  // all 24 buffers reclaimed
    }
}

TEST_CASE("drywet::passthrough_stereo copies dry signal", "[arena][reclaim][drywet]") {
    std::array<float, 4> in_l{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> in_r{5.0f, 6.0f, 7.0f, 8.0f};
    std::array<float, 4> out_l{}, out_r{};

    SECTION("mono input broadcasts to both lanes") {
        drywet::passthrough_stereo(in_l.data(), nullptr, false, out_l.data(), out_r.data(), 4);
        for (std::size_t i = 0; i < 4; ++i) {
            REQUIRE(out_l[i] == in_l[i]);
            REQUIRE(out_r[i] == in_l[i]);
        }
    }

    SECTION("stereo input passes each lane through") {
        drywet::passthrough_stereo(in_l.data(), in_r.data(), true, out_l.data(), out_r.data(), 4);
        for (std::size_t i = 0; i < 4; ++i) {
            REQUIRE(out_l[i] == in_l[i]);
            REQUIRE(out_r[i] == in_r[i]);
        }
    }
}
