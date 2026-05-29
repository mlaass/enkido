#include <catch2/catch_test_macros.hpp>

#include <akkado/codegen.hpp>
#include <cedar/dsp/constants.hpp>

#include <set>
#include <vector>

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

        (void)alloc.allocate();
        CHECK(alloc.count() == 1);

        (void)alloc.allocate();
        CHECK(alloc.count() == 2);

        for (int i = 0; i < 10; ++i) {
            (void)alloc.allocate();
        }
        CHECK(alloc.count() == 12);
    }

    SECTION("has_available returns correct values") {
        CHECK(alloc.has_available());

        // Allocate some
        for (int i = 0; i < 100; ++i) {
            (void)alloc.allocate();
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

    SECTION("allocate fills the address space, skipping BUFFER_ZERO") {
        // The allocator hands out every index in [0, MAX_ALLOCATABLE)
        // EXCEPT cedar::BUFFER_ZERO (= 255), which is reserved.
        std::vector<std::uint16_t> seen;
        seen.reserve(BufferAllocator::MAX_ALLOCATABLE);
        for (int i = 0; i + 1 < BufferAllocator::MAX_ALLOCATABLE; ++i) {
            std::uint16_t idx = alloc.allocate();
            REQUIRE(idx != BufferAllocator::BUFFER_UNUSED);
            REQUIRE(idx != cedar::BUFFER_ZERO);
            seen.push_back(idx);
        }
        std::set<std::uint16_t> unique(seen.begin(), seen.end());
        CHECK(unique.size() == seen.size());
        CHECK_FALSE(alloc.has_available());
    }

    SECTION("next allocation after exhaustion returns BUFFER_UNUSED") {
        for (int i = 0; i + 1 < BufferAllocator::MAX_ALLOCATABLE; ++i) {
            (void)alloc.allocate();
        }

        std::uint16_t overflow = alloc.allocate();
        CHECK(overflow == BufferAllocator::BUFFER_UNUSED);
    }

    SECTION("multiple overflow allocations return BUFFER_UNUSED") {
        for (int i = 0; i + 1 < BufferAllocator::MAX_ALLOCATABLE; ++i) {
            (void)alloc.allocate();
        }

        for (int i = 0; i < 10; ++i) {
            std::uint16_t overflow = alloc.allocate();
            CHECK(overflow == BufferAllocator::BUFFER_UNUSED);
        }

        CHECK(alloc.count() == BufferAllocator::MAX_ALLOCATABLE);
    }

    SECTION("fresh allocator state") {
        CHECK(alloc.count() == 0);
        CHECK(alloc.has_available());
    }

    SECTION("BUFFER_ZERO slot is never handed out") {
        // Burn the first 256 allocations: 0..254 then skip to 256.
        std::uint16_t last_before_skip = BufferAllocator::BUFFER_UNUSED;
        std::uint16_t first_after_skip = BufferAllocator::BUFFER_UNUSED;
        for (std::uint16_t i = 0; i < 256; ++i) {
            std::uint16_t idx = alloc.allocate();
            REQUIRE(idx != BufferAllocator::BUFFER_UNUSED);
            REQUIRE(idx != cedar::BUFFER_ZERO);
            if (i == cedar::BUFFER_ZERO) first_after_skip = idx;
            if (i == cedar::BUFFER_ZERO - 1) last_before_skip = idx;
        }
        CHECK(last_before_skip == cedar::BUFFER_ZERO - 1);
        CHECK(first_after_skip == cedar::BUFFER_ZERO + 1);
    }

    SECTION("BUFFER_UNUSED constant value") {
        CHECK(BufferAllocator::BUFFER_UNUSED == 0xFFFF);
    }

    SECTION("MAX_BUFFERS matches cedar capacity") {
        CHECK(BufferAllocator::MAX_BUFFERS == cedar::MAX_BUFFERS);
    }

    SECTION("MAX_ALLOCATABLE equals MAX_BUFFERS") {
        // The allocator can address every slot; BUFFER_ZERO is excluded
        // by allocate()'s skip-logic, not by capping the cursor early.
        CHECK(BufferAllocator::MAX_ALLOCATABLE == BufferAllocator::MAX_BUFFERS);
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

        for (int i = 0; i < BufferAllocator::MAX_ALLOCATABLE; ++i) {
            std::uint16_t expected = allocators[0].allocate();

            for (std::size_t a = 1; a < allocators.size(); ++a) {
                std::uint16_t got = allocators[a].allocate();
                CHECK(got == expected);
            }
        }
    }
}

// ============================================================================
// Free-list reuse (release + reallocate)
// ============================================================================

TEST_CASE("BufferAllocator release + reuse", "[buffer_allocator][release]") {
    SECTION("released index is returned by next allocate") {
        BufferAllocator alloc;
        std::uint16_t a = alloc.allocate();
        std::uint16_t b = alloc.allocate();
        CHECK(alloc.count() == 2);

        alloc.release(a);
        std::uint16_t reused = alloc.allocate();
        CHECK(reused == a);
        CHECK(alloc.count() == 2);
        CHECK(b == 1);
    }

    SECTION("multiple releases drain LIFO") {
        BufferAllocator alloc;
        std::uint16_t a = alloc.allocate();
        std::uint16_t b = alloc.allocate();
        std::uint16_t c = alloc.allocate();

        alloc.release(a);
        alloc.release(b);
        alloc.release(c);

        CHECK(alloc.allocate() == c);
        CHECK(alloc.allocate() == b);
        CHECK(alloc.allocate() == a);
        CHECK(alloc.allocate() == 3);
        CHECK(alloc.count() == 4);
    }

    SECTION("release(BUFFER_UNUSED) is a no-op") {
        BufferAllocator alloc;
        std::uint16_t a = alloc.allocate();
        alloc.release(BufferAllocator::BUFFER_UNUSED);
        std::uint16_t b = alloc.allocate();
        CHECK(b == 1);
        CHECK(a == 0);
    }

    SECTION("release rejects indices >= MAX_ALLOCATABLE") {
        BufferAllocator alloc;
        std::uint16_t a = alloc.allocate();
        alloc.release(BufferAllocator::MAX_ALLOCATABLE);
        alloc.release(BufferAllocator::MAX_ALLOCATABLE + 100);
        CHECK(alloc.allocate() == 1);
        CHECK(a == 0);
    }

    SECTION("release rejects indices >= next_ (never handed out)") {
        BufferAllocator alloc;
        (void)alloc.allocate();
        alloc.release(17);
        CHECK(alloc.allocate() == 1);
    }
}

TEST_CASE("BufferAllocator reset_to drains stale free-list entries",
          "[buffer_allocator][reset_to]") {
    SECTION("entries past the mark are dropped from the free list") {
        BufferAllocator alloc;
        (void)alloc.allocate();
        std::uint16_t b = alloc.allocate();
        std::uint16_t c = alloc.allocate();
        (void)alloc.allocate();

        alloc.release(c);
        alloc.release(3);

        alloc.reset_to(2);
        CHECK(alloc.count() == 2);

        CHECK(alloc.allocate() == 2);
        CHECK(alloc.allocate() == 3);
        (void)b;
    }

    SECTION("entries before the mark survive reset_to") {
        BufferAllocator alloc;
        std::uint16_t a = alloc.allocate();
        (void)alloc.allocate();
        (void)alloc.allocate();

        alloc.release(a);
        alloc.reset_to(2);

        CHECK(alloc.allocate() == 0);
        CHECK(alloc.allocate() == 2);
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
        bool exhausted = false;

        for (int i = 0; i < BufferAllocator::MAX_ALLOCATABLE + 10; ++i) {
            std::uint16_t idx = alloc.allocate();
            if (idx == BufferAllocator::BUFFER_UNUSED) {
                exhausted = true;
                // Allocator exhausts after handing out every slot except
                // BUFFER_ZERO: MAX_ALLOCATABLE - 1 successful calls.
                CHECK(i == BufferAllocator::MAX_ALLOCATABLE - 1);
                break;
            }
        }

        CHECK(exhausted);
    }
}
