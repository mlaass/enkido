#include <catch2/catch_test_macros.hpp>

#include <akkado/codegen.hpp>

using namespace akkado;

// ============================================================================
// Unit Tests [buffer_allocator]
// ============================================================================

TEST_CASE("BufferAllocator basic operations", "[buffer_allocator]") {
    BufferAllocator alloc;

    SECTION("allocate returns incrementing indices") {
        std::uint16_t idx1 = alloc.allocate();
        std::uint16_t idx2 = alloc.allocate();
        std::uint16_t idx3 = alloc.allocate();

        CHECK(idx1 == 0);
        CHECK(idx2 == 1);
        CHECK(idx3 == 2);
    }

    SECTION("count tracks allocations") {
        CHECK(alloc.count() == 0);

        alloc.allocate();
        CHECK(alloc.count() == 1);

        alloc.allocate();
        CHECK(alloc.count() == 2);

        for (int i = 0; i < 10; ++i) {
            alloc.allocate();
        }
        CHECK(alloc.count() == 12);
    }

    SECTION("has_available returns correct values") {
        CHECK(alloc.has_available());

        // Allocate some
        for (int i = 0; i < 100; ++i) {
            alloc.allocate();
        }

        CHECK(alloc.has_available());
    }

    SECTION("allocate returns distinct values") {
        std::vector<std::uint16_t> indices;
        for (int i = 0; i < 50; ++i) {
            indices.push_back(alloc.allocate());
        }

        // Check all are unique
        for (std::size_t i = 0; i < indices.size(); ++i) {
            for (std::size_t j = i + 1; j < indices.size(); ++j) {
                CHECK(indices[i] != indices[j]);
            }
        }
    }
}

// ============================================================================
// Edge Cases [buffer_allocator][edge]
// ============================================================================

TEST_CASE("BufferAllocator edge cases", "[buffer_allocator][edge]") {
    BufferAllocator alloc;

    SECTION("allocate exactly MAX_BUFFERS times") {
        for (int i = 0; i < BufferAllocator::MAX_BUFFERS; ++i) {
            std::uint16_t idx = alloc.allocate();
            CHECK(idx != BufferAllocator::BUFFER_UNUSED);
            CHECK(idx == static_cast<std::uint16_t>(i));
        }

        CHECK(alloc.count() == BufferAllocator::MAX_BUFFERS);
        CHECK_FALSE(alloc.has_available());
    }

    SECTION("next allocation after MAX_BUFFERS returns BUFFER_UNUSED") {
        // Fill up
        for (int i = 0; i < BufferAllocator::MAX_BUFFERS; ++i) {
            alloc.allocate();
        }

        // Next should fail
        std::uint16_t overflow = alloc.allocate();
        CHECK(overflow == BufferAllocator::BUFFER_UNUSED);
    }

    SECTION("multiple overflow allocations return BUFFER_UNUSED") {
        // Fill up
        for (int i = 0; i < BufferAllocator::MAX_BUFFERS; ++i) {
            alloc.allocate();
        }

        // Multiple overflow attempts
        for (int i = 0; i < 10; ++i) {
            std::uint16_t overflow = alloc.allocate();
            CHECK(overflow == BufferAllocator::BUFFER_UNUSED);
        }

        // Count should still be MAX_BUFFERS
        CHECK(alloc.count() == BufferAllocator::MAX_BUFFERS);
    }

    SECTION("fresh allocator state") {
        CHECK(alloc.count() == 0);
        CHECK(alloc.has_available());
    }

    SECTION("allocate at boundary") {
        // Allocate MAX_BUFFERS - 1
        for (int i = 0; i < MAX_BUFFERS - 1; ++i) {
            alloc.allocate();
        }

        CHECK(alloc.has_available());

        // Allocate the last one
        std::uint16_t last = alloc.allocate();
        CHECK(last == MAX_BUFFERS - 1);
        CHECK_FALSE(alloc.has_available());
    }

    SECTION("BUFFER_UNUSED constant value") {
        CHECK(BUFFER_UNUSED == 0xFFFF);
    }

    SECTION("MAX_BUFFERS constant value") {
        CHECK(MAX_BUFFERS == 256);
    }
}

// ============================================================================
// Stress Tests [buffer_allocator][stress]
// ============================================================================

TEST_CASE("BufferAllocator stress test", "[buffer_allocator][stress]") {
    SECTION("many allocator instances") {
        for (int instance = 0; instance < 1000; ++instance) {
            BufferAllocator alloc;

            // Allocate random amount
            int num_allocs = instance % 100;
            for (int i = 0; i < num_allocs; ++i) {
                std::uint16_t idx = alloc.allocate();
                CHECK(idx == static_cast<std::uint16_t>(i));
            }

            CHECK(alloc.count() == static_cast<std::uint16_t>(num_allocs));
        }
    }

    SECTION("verify allocation sequence consistency") {
        // Create multiple allocators and verify they all produce same sequence
        std::vector<BufferAllocator> allocators(10);

        for (int i = 0; i < MAX_BUFFERS; ++i) {
            std::uint16_t expected = allocators[0].allocate();

            for (std::size_t a = 1; a < allocators.size(); ++a) {
                std::uint16_t got = allocators[a].allocate();
                CHECK(got == expected);
            }
        }
    }
}

// ============================================================================
// Integration with codegen concepts
// ============================================================================

TEST_CASE("BufferAllocator in codegen context", "[buffer_allocator]") {
    BufferAllocator alloc;

    SECTION("simulate expression codegen") {
        // Simulating: (a + b) * (c - d)
        // Need buffers for: a, b, a+b, c, d, c-d, final result

        std::uint16_t buf_a = alloc.allocate();  // 0
        std::uint16_t buf_b = alloc.allocate();  // 1
        std::uint16_t buf_add = alloc.allocate();  // 2

        std::uint16_t buf_c = alloc.allocate();  // 3
        std::uint16_t buf_d = alloc.allocate();  // 4
        std::uint16_t buf_sub = alloc.allocate();  // 5

        std::uint16_t buf_result = alloc.allocate();  // 6

        CHECK(buf_a == 0);
        CHECK(buf_b == 1);
        CHECK(buf_add == 2);
        CHECK(buf_c == 3);
        CHECK(buf_d == 4);
        CHECK(buf_sub == 5);
        CHECK(buf_result == 6);
        CHECK(alloc.count() == 7);
    }

    SECTION("simulate function with local variables") {
        // Simulating a function that needs:
        // - 4 input parameters
        // - 3 local temporaries
        // - 1 return buffer

        std::vector<std::uint16_t> params;
        for (int i = 0; i < 4; ++i) {
            params.push_back(alloc.allocate());
        }

        std::vector<std::uint16_t> temps;
        for (int i = 0; i < 3; ++i) {
            temps.push_back(alloc.allocate());
        }

        std::uint16_t ret = alloc.allocate();

        CHECK(params.size() == 4);
        CHECK(temps.size() == 3);
        CHECK(ret == 7);
        CHECK(alloc.count() == 8);
    }

    SECTION("detect buffer exhaustion during codegen") {
        // Simulate a program that needs too many buffers
        bool exhausted = false;

        for (int i = 0; i < MAX_BUFFERS + 10; ++i) {
            std::uint16_t idx = alloc.allocate();
            if (idx == BUFFER_UNUSED) {
                exhausted = true;
                CHECK(i == MAX_BUFFERS);
                break;
            }
        }

        CHECK(exhausted);
    }
}
