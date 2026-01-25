#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/audio_arena.hpp>

#include <cstdint>
#include <random>
#include <vector>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Unit Tests [arena]
// ============================================================================

TEST_CASE("AudioArena allocation basics", "[arena]") {
    AudioArena arena(4096);  // 4KB for testing

    SECTION("allocate returns valid aligned pointer") {
        float* ptr = arena.allocate(128);
        REQUIRE(ptr != nullptr);
        // Check 32-byte alignment
        REQUIRE((reinterpret_cast<std::uintptr_t>(ptr) % 32) == 0);
    }

    SECTION("multiple allocations return distinct pointers") {
        float* ptr1 = arena.allocate(64);
        float* ptr2 = arena.allocate(64);
        float* ptr3 = arena.allocate(64);

        REQUIRE(ptr1 != nullptr);
        REQUIRE(ptr2 != nullptr);
        REQUIRE(ptr3 != nullptr);
        REQUIRE(ptr1 != ptr2);
        REQUIRE(ptr2 != ptr3);
        REQUIRE(ptr1 != ptr3);
    }

    SECTION("allocated buffers are writable and readable") {
        float* ptr = arena.allocate(128);
        REQUIRE(ptr != nullptr);

        // Write pattern
        for (std::size_t i = 0; i < 128; ++i) {
            ptr[i] = static_cast<float>(i) * 0.01f;
        }

        // Read back and verify
        for (std::size_t i = 0; i < 128; ++i) {
            CHECK_THAT(ptr[i], WithinAbs(static_cast<float>(i) * 0.01f, 1e-6f));
        }
    }

    SECTION("reset allows reallocation") {
        float* ptr1 = arena.allocate(64);
        REQUIRE(ptr1 != nullptr);
        std::size_t used_before = arena.used();
        REQUIRE(used_before > 0);

        arena.reset();

        REQUIRE(arena.used() == 0);
        REQUIRE(arena.available() == arena.capacity());

        float* ptr2 = arena.allocate(64);
        REQUIRE(ptr2 != nullptr);
        // After reset, new allocation should start from beginning
        REQUIRE(ptr2 == ptr1);
    }

    SECTION("owns correctly identifies arena pointers") {
        float* ptr = arena.allocate(64);
        REQUIRE(ptr != nullptr);
        CHECK(arena.owns(ptr));
        CHECK(arena.owns(ptr + 32));

        // Stack pointer should not be owned
        float stack_var = 0.0f;
        CHECK_FALSE(arena.owns(&stack_var));

        // Heap pointer should not be owned
        std::vector<float> heap_vec(64);
        CHECK_FALSE(arena.owns(heap_vec.data()));

        // nullptr should not be owned
        CHECK_FALSE(arena.owns(nullptr));
    }

    SECTION("capacity used available consistency") {
        REQUIRE(arena.capacity() == 4096);
        REQUIRE(arena.used() == 0);
        REQUIRE(arena.available() == 4096);

        float* ptr = arena.allocate(64);
        REQUIRE(ptr != nullptr);

        REQUIRE(arena.used() > 0);
        REQUIRE(arena.available() < arena.capacity());
        REQUIRE(arena.used() + arena.available() <= arena.capacity());
    }

    SECTION("is_valid returns true for valid arena") {
        CHECK(arena.is_valid());
    }
}

TEST_CASE("AudioArena move semantics", "[arena]") {
    SECTION("move constructor transfers ownership") {
        AudioArena arena1(4096);
        float* ptr = arena1.allocate(64);
        REQUIRE(ptr != nullptr);
        REQUIRE(arena1.owns(ptr));

        AudioArena arena2(std::move(arena1));

        // arena2 now owns the memory
        CHECK(arena2.is_valid());
        CHECK(arena2.owns(ptr));
        CHECK(arena2.capacity() == 4096);

        // arena1 is moved-from (should be in valid but empty state)
        CHECK_FALSE(arena1.is_valid());
        CHECK_FALSE(arena1.owns(ptr));
    }

    SECTION("move assignment transfers ownership") {
        AudioArena arena1(4096);
        float* ptr = arena1.allocate(64);
        REQUIRE(ptr != nullptr);

        AudioArena arena2(2048);
        float* ptr2 = arena2.allocate(32);
        REQUIRE(ptr2 != nullptr);

        arena2 = std::move(arena1);

        CHECK(arena2.is_valid());
        CHECK(arena2.owns(ptr));
        CHECK(arena2.capacity() == 4096);
    }
}

// ============================================================================
// Edge Cases [arena][edge]
// ============================================================================

TEST_CASE("AudioArena edge cases", "[arena][edge]") {
    SECTION("arena exhaustion returns nullptr") {
        AudioArena arena(512);  // Small arena

        // Allocate most of it
        float* ptr1 = arena.allocate(100);
        REQUIRE(ptr1 != nullptr);

        // Try to allocate more than remaining
        float* ptr2 = arena.allocate(200);
        CHECK(ptr2 == nullptr);
    }

    SECTION("partial allocation exceeding capacity fails") {
        AudioArena arena(256);

        // Request more than total capacity
        float* ptr = arena.allocate(1000);
        CHECK(ptr == nullptr);
        CHECK(arena.used() == 0);  // No change in used
    }

    SECTION("zero-size allocation behavior") {
        AudioArena arena(4096);

        float* ptr = arena.allocate(0);
        // Implementation may return nullptr or valid ptr for zero-size
        // Either is acceptable, but should not crash
        if (ptr != nullptr) {
            CHECK(arena.owns(ptr));
        }
    }

    SECTION("allocation of exactly remaining capacity") {
        AudioArena arena(512);

        // Calculate how many floats we can allocate accounting for alignment
        std::size_t remaining = arena.available() / sizeof(float);
        float* ptr = arena.allocate(remaining);

        // Should succeed
        CHECK(ptr != nullptr);

        // Next allocation should fail
        float* ptr2 = arena.allocate(1);
        CHECK(ptr2 == nullptr);
    }

    SECTION("very small arena (64 bytes)") {
        AudioArena arena(64);

        REQUIRE(arena.is_valid());
        REQUIRE(arena.capacity() == 64);

        // Can allocate small amount
        float* ptr = arena.allocate(8);
        CHECK(ptr != nullptr);

        // Limited capacity
        float* ptr2 = arena.allocate(16);
        CHECK(ptr2 == nullptr);
    }

    SECTION("allocation causing alignment waste at boundary") {
        AudioArena arena(256);

        // Allocate 17 floats - not aligned to 32 bytes
        float* ptr1 = arena.allocate(17);
        REQUIRE(ptr1 != nullptr);
        CHECK((reinterpret_cast<std::uintptr_t>(ptr1) % 32) == 0);

        // Next allocation should still be aligned
        float* ptr2 = arena.allocate(17);
        if (ptr2 != nullptr) {
            CHECK((reinterpret_cast<std::uintptr_t>(ptr2) % 32) == 0);
        }
    }

    SECTION("large allocation near capacity") {
        AudioArena arena(AudioArena::DEFAULT_SIZE);  // 32MB

        // Allocate almost all of it
        std::size_t large_count = (arena.capacity() / sizeof(float)) - 1000;
        float* ptr = arena.allocate(large_count);
        CHECK(ptr != nullptr);

        // Small allocation should still work
        float* ptr2 = arena.allocate(100);
        CHECK((ptr2 != nullptr || arena.available() < 100 * sizeof(float)));
    }
}

// ============================================================================
// ArenaBuffer Tests
// ============================================================================

TEST_CASE("ArenaBuffer operations", "[arena]") {
    AudioArena arena(4096);

    SECTION("ArenaBuffer basic operations") {
        float* raw = arena.allocate(128);
        REQUIRE(raw != nullptr);

        ArenaBuffer buf{raw, 128};

        CHECK(buf.is_valid());
        CHECK(buf.size == 128);
        CHECK(buf.data == raw);

        // Indexing
        buf[0] = 1.0f;
        buf[127] = 2.0f;
        CHECK_THAT(buf[0], WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(buf[127], WithinAbs(2.0f, 1e-6f));
    }

    SECTION("ArenaBuffer clear") {
        float* raw = arena.allocate(64);
        REQUIRE(raw != nullptr);

        ArenaBuffer buf{raw, 64};

        // Fill with non-zero
        for (std::size_t i = 0; i < 64; ++i) {
            buf[i] = static_cast<float>(i);
        }

        buf.clear();

        for (std::size_t i = 0; i < 64; ++i) {
            CHECK_THAT(buf[i], WithinAbs(0.0f, 1e-6f));
        }
    }

    SECTION("ArenaBuffer invalid state") {
        ArenaBuffer buf{nullptr, 0};
        CHECK_FALSE(buf.is_valid());
    }
}

// ============================================================================
// Stress Tests [arena][stress]
// ============================================================================

TEST_CASE("AudioArena stress test", "[arena][stress]") {
    std::mt19937 rng(42);  // Deterministic seed

    SECTION("10000 allocate/reset cycles with random sizes") {
        AudioArena arena(64 * 1024);  // 64KB

        for (int cycle = 0; cycle < 10000; ++cycle) {
            arena.reset();

            // Random number of allocations per cycle
            int num_allocs = (rng() % 10) + 1;
            std::vector<float*> ptrs;

            for (int i = 0; i < num_allocs; ++i) {
                std::size_t size = (rng() % 256) + 1;
                float* ptr = arena.allocate(size);
                if (ptr == nullptr) break;
                ptrs.push_back(ptr);

                // Write unique pattern
                float pattern = static_cast<float>(cycle * 100 + i);
                for (std::size_t j = 0; j < size; ++j) {
                    ptr[j] = pattern;
                }
            }

            // Verify memory integrity - each buffer should have its pattern
            for (std::size_t i = 0; i < ptrs.size(); ++i) {
                float expected = static_cast<float>(cycle * 100 + static_cast<int>(i));
                CHECK_THAT(ptrs[i][0], WithinAbs(expected, 1e-6f));
            }
        }
    }

    SECTION("fragmentation pattern - many small allocations") {
        AudioArena arena(16 * 1024);  // 16KB

        for (int cycle = 0; cycle < 100; ++cycle) {
            arena.reset();
            int alloc_count = 0;

            // Allocate as many small buffers as possible
            while (true) {
                float* ptr = arena.allocate(8);
                if (ptr == nullptr) break;
                ptr[0] = static_cast<float>(alloc_count);
                ++alloc_count;
            }

            // Should have allocated many buffers
            CHECK(alloc_count > 100);
        }
    }
}
