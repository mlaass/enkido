#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/buffer_pool.hpp>
#include <cedar/dsp/constants.hpp>

#include <cmath>
#include <limits>
#include <random>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Unit Tests [buffer_pool]
// ============================================================================

TEST_CASE("BufferPool basic operations", "[buffer_pool]") {
    BufferPool pool;

    SECTION("get returns valid pointers for all indices") {
        for (std::uint16_t i = 0; i < 10; ++i) {
            float* ptr = pool.get(i);
            REQUIRE(ptr != nullptr);
        }
    }

    SECTION("get returns same pointer for same index") {
        float* ptr1 = pool.get(5);
        float* ptr2 = pool.get(5);
        CHECK(ptr1 == ptr2);
    }

    SECTION("get returns distinct pointers for distinct indices") {
        float* ptr0 = pool.get(0);
        float* ptr1 = pool.get(1);
        float* ptr2 = pool.get(2);

        CHECK(ptr0 != ptr1);
        CHECK(ptr1 != ptr2);
        CHECK(ptr0 != ptr2);
    }

    SECTION("clear zeros specific buffer") {
        float* ptr = pool.get(3);

        // Fill with non-zero
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            ptr[i] = 1.0f;
        }

        pool.clear(3);

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(ptr[i], WithinAbs(0.0f, 1e-6f));
        }
    }

    SECTION("clear_all zeros all buffers") {
        // Fill several buffers
        for (std::uint16_t idx = 0; idx < 5; ++idx) {
            float* ptr = pool.get(idx);
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                ptr[i] = static_cast<float>(idx + 1);
            }
        }

        pool.clear_all();

        for (std::uint16_t idx = 0; idx < 5; ++idx) {
            float* ptr = pool.get(idx);
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
                CHECK_THAT(ptr[i], WithinAbs(0.0f, 1e-6f));
            }
        }
    }

    SECTION("fill sets all samples to constant") {
        pool.fill(7, 3.14159f);

        float* ptr = pool.get(7);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(ptr[i], WithinAbs(3.14159f, 1e-6f));
        }
    }

    SECTION("copy duplicates buffer contents") {
        float* src = pool.get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            src[i] = static_cast<float>(i) * 0.01f;
        }

        pool.copy(1, 0);  // dst=1, src=0

        float* dst = pool.get(1);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(dst[i], WithinAbs(static_cast<float>(i) * 0.01f, 1e-6f));
        }
    }

    SECTION("buffers are 32-byte aligned") {
        for (std::uint16_t i = 0; i < 10; ++i) {
            float* ptr = pool.get(i);
            auto addr = reinterpret_cast<std::uintptr_t>(ptr);
            CHECK((addr % 32) == 0);
        }
    }
}

// ============================================================================
// Edge Cases [buffer_pool][edge]
// ============================================================================

TEST_CASE("BufferPool edge cases", "[buffer_pool][edge]") {
    BufferPool pool;

    SECTION("boundary indices (0 and MAX_BUFFERS-1)") {
        float* ptr0 = pool.get(0);
        float* ptrMax = pool.get(MAX_BUFFERS - 1);

        REQUIRE(ptr0 != nullptr);
        REQUIRE(ptrMax != nullptr);
        CHECK(ptr0 != ptrMax);

        // Should be writable
        ptr0[0] = 123.0f;
        ptrMax[0] = 456.0f;

        CHECK_THAT(ptr0[0], WithinAbs(123.0f, 1e-6f));
        CHECK_THAT(ptrMax[0], WithinAbs(456.0f, 1e-6f));
    }

    SECTION("special float values - NaN") {
        float* ptr = pool.get(0);

        ptr[0] = std::numeric_limits<float>::quiet_NaN();
        CHECK(std::isnan(ptr[0]));

        ptr[1] = std::numeric_limits<float>::signaling_NaN();
        CHECK(std::isnan(ptr[1]));
    }

    SECTION("special float values - infinity") {
        float* ptr = pool.get(1);

        ptr[0] = std::numeric_limits<float>::infinity();
        ptr[1] = -std::numeric_limits<float>::infinity();

        CHECK(std::isinf(ptr[0]));
        CHECK(ptr[0] > 0);
        CHECK(std::isinf(ptr[1]));
        CHECK(ptr[1] < 0);
    }

    SECTION("special float values - denormals") {
        float* ptr = pool.get(2);

        ptr[0] = std::numeric_limits<float>::denorm_min();
        CHECK(ptr[0] == std::numeric_limits<float>::denorm_min());

        ptr[1] = std::numeric_limits<float>::min() / 2.0f;
        CHECK(std::fpclassify(ptr[1]) == FP_SUBNORMAL);
    }

    SECTION("modifications to one buffer don't affect others") {
        // Clear all first
        pool.clear_all();

        // Fill buffer 5
        pool.fill(5, 42.0f);

        // Check adjacent buffers are still zero
        float* ptr4 = pool.get(4);
        float* ptr6 = pool.get(6);

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(ptr4[i], WithinAbs(0.0f, 1e-6f));
            CHECK_THAT(ptr6[i], WithinAbs(0.0f, 1e-6f));
        }
    }

    SECTION("fill with zero") {
        float* ptr = pool.get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            ptr[i] = 1.0f;
        }

        pool.fill(0, 0.0f);

        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(ptr[i], WithinAbs(0.0f, 1e-6f));
        }
    }

    SECTION("fill with negative value") {
        pool.fill(0, -999.0f);

        float* ptr = pool.get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(ptr[i], WithinAbs(-999.0f, 1e-6f));
        }
    }

    SECTION("copy to same index (self-copy)") {
        float* ptr = pool.get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            ptr[i] = static_cast<float>(i);
        }

        pool.copy(0, 0);  // Self-copy

        // Should still have same values
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(ptr[i], WithinAbs(static_cast<float>(i), 1e-6f));
        }
    }

    SECTION("fill with very small value") {
        pool.fill(0, 1e-38f);

        float* ptr = pool.get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(ptr[i], WithinAbs(1e-38f, 1e-40f));
        }
    }

    SECTION("fill with very large value") {
        pool.fill(0, 1e38f);

        float* ptr = pool.get(0);
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            CHECK_THAT(ptr[i], WithinAbs(1e38f, 1e36f));
        }
    }
}

// ============================================================================
// Stress Tests [buffer_pool][stress]
// ============================================================================

TEST_CASE("BufferPool stress test", "[buffer_pool][stress]") {
    BufferPool pool;
    std::mt19937 rng(42);

    SECTION("100000 random get/fill/copy operations") {
        for (int i = 0; i < 100000; ++i) {
            int op = rng() % 3;
            std::uint16_t idx1 = rng() % MAX_BUFFERS;
            std::uint16_t idx2 = rng() % MAX_BUFFERS;
            float value = static_cast<float>(rng() % 1000) * 0.001f;

            switch (op) {
                case 0: {
                    float* ptr = pool.get(idx1);
                    REQUIRE(ptr != nullptr);
                    break;
                }
                case 1: {
                    pool.fill(idx1, value);
                    break;
                }
                case 2: {
                    pool.copy(idx1, idx2);
                    break;
                }
            }
        }

        // Verify pool is still functional
        pool.fill(0, 123.456f);
        float* ptr = pool.get(0);
        CHECK_THAT(ptr[0], WithinAbs(123.456f, 1e-3f));
    }

    SECTION("sequential buffer access pattern") {
        // Simulate typical DSP processing - sequential access
        for (int block = 0; block < 1000; ++block) {
            for (std::uint16_t idx = 0; idx < 32; ++idx) {
                float* ptr = pool.get(idx);
                for (std::size_t s = 0; s < BLOCK_SIZE; ++s) {
                    ptr[s] = static_cast<float>(block * 32 + idx + s) * 0.001f;
                }
            }
        }
    }

    SECTION("copy chain") {
        // Fill first buffer
        pool.fill(0, 1.0f);

        // Copy through chain of buffers
        for (std::uint16_t i = 1; i < 100; ++i) {
            pool.copy(i, i - 1);
        }

        // All buffers should have same value
        for (std::uint16_t i = 0; i < 100; ++i) {
            float* ptr = pool.get(i);
            CHECK_THAT(ptr[0], WithinAbs(1.0f, 1e-6f));
        }
    }
}
