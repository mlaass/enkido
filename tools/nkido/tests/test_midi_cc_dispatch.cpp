// Unit tests for dispatch_midi_cc — the host-side helper that turns an
// incoming MIDI message into vm.set_param() calls (PRD prd-midi-input §4.8).
// dispatch_midi_cc is an inline header function, so we exercise it directly
// without spinning up RtMidi.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "midi_input.hpp"
#include "cedar/vm/vm.hpp"
#include "cedar/vm/state_pool.hpp"  // for fnv1a_hash_runtime

#include <cstdint>
#include <cstring>

namespace {

// Read the most-recent target value set on a named param via the EnvMap.
// We bypass the audio thread's interpolation by reading `target` directly —
// this is what dispatch_midi_cc writes to via vm.set_param().
float read_target(cedar::VM& vm, const char* name) {
    std::uint32_t hash =
        cedar::fnv1a_hash_runtime(name, std::strlen(name));
    return vm.env_map().get_target(hash);
}

}  // namespace

TEST_CASE("dispatch_midi_cc — plain CC", "[midi_cc][dispatch]") {
    cedar::VM vm;
    nkido::MidiCcRouteTable table = {
        {"cutoff", /*cc_num=*/74, /*channel_filter=*/0,
         /*scale=*/4950.0f, /*bias=*/50.0f, /*slew_ms=*/5.0f},
    };

    SECTION("d2 = 0 → bias") {
        nkido::dispatch_midi_cc(vm, table, 0xB0u, 74, 0, /*channel=*/1);
        CHECK(read_target(vm, "cutoff") == Catch::Approx(50.0f));
    }

    SECTION("d2 = 127 → bias + scale") {
        nkido::dispatch_midi_cc(vm, table, 0xB0u, 74, 127, /*channel=*/1);
        CHECK(read_target(vm, "cutoff") == Catch::Approx(5000.0f));
    }

    SECTION("d2 = 64 → roughly midpoint") {
        nkido::dispatch_midi_cc(vm, table, 0xB0u, 74, 64, /*channel=*/1);
        CHECK(read_target(vm, "cutoff") ==
              Catch::Approx(64.0f / 127.0f * 4950.0f + 50.0f).margin(0.01f));
    }

    SECTION("non-matching CC number is ignored") {
        // Write a known value first so we can prove no change happened.
        vm.set_param("cutoff", 1234.0f, 0.0f);
        nkido::dispatch_midi_cc(vm, table, 0xB0u, /*cc=*/75, 127, /*channel=*/1);
        CHECK(read_target(vm, "cutoff") == Catch::Approx(1234.0f));
    }
}

TEST_CASE("dispatch_midi_cc — channel filter", "[midi_cc][dispatch]") {
    cedar::VM vm;
    nkido::MidiCcRouteTable table = {
        {"x", 1, /*channel_filter=*/5, 1.0f, 0.0f, 5.0f},
    };

    vm.set_param("x", 999.0f, 0.0f);

    SECTION("channel 5 matches") {
        nkido::dispatch_midi_cc(vm, table, 0xB0u, 1, 127, /*channel=*/5);
        CHECK(read_target(vm, "x") == Catch::Approx(1.0f));
    }

    SECTION("channel 6 rejected") {
        nkido::dispatch_midi_cc(vm, table, 0xB0u, 1, 127, /*channel=*/6);
        CHECK(read_target(vm, "x") == Catch::Approx(999.0f));
    }
}

TEST_CASE("dispatch_midi_cc — pitch-bend (14-bit)", "[midi_cc][dispatch]") {
    cedar::VM vm;
    // Default PB range -1..+1: scale=2, bias=-1.
    nkido::MidiCcRouteTable table = {
        {"bend", /*cc_num=*/-1, 0, /*scale=*/2.0f, /*bias=*/-1.0f, 5.0f},
    };

    SECTION("center (d1=0, d2=64) ≈ 0") {
        // 14-bit value = (64 << 7) | 0 = 8192 → ~midpoint of 16383 → ~0.0001
        nkido::dispatch_midi_cc(vm, table, 0xE0u, 0, 64, /*channel=*/1);
        CHECK(read_target(vm, "bend") == Catch::Approx(0.0f).margin(0.001f));
    }

    SECTION("min (0, 0) ≈ -1") {
        nkido::dispatch_midi_cc(vm, table, 0xE0u, 0, 0, /*channel=*/1);
        CHECK(read_target(vm, "bend") == Catch::Approx(-1.0f).margin(0.001f));
    }

    SECTION("max (127, 127) ≈ +1") {
        nkido::dispatch_midi_cc(vm, table, 0xE0u, 127, 127, /*channel=*/1);
        CHECK(read_target(vm, "bend") == Catch::Approx(1.0f).margin(0.001f));
    }

    SECTION("CC route does not consume PB message") {
        nkido::MidiCcRouteTable cc_only = {
            {"y", 74, 0, 1.0f, 0.0f, 5.0f},
        };
        vm.set_param("y", 999.0f, 0.0f);
        nkido::dispatch_midi_cc(vm, cc_only, 0xE0u, 74, 127, /*channel=*/1);
        CHECK(read_target(vm, "y") == Catch::Approx(999.0f));
    }
}

TEST_CASE("dispatch_midi_cc — channel aftertouch", "[midi_cc][dispatch]") {
    cedar::VM vm;
    nkido::MidiCcRouteTable table = {
        {"press", /*cc_num=*/-2, 0, /*scale=*/1.0f, /*bias=*/0.0f, 5.0f},
    };

    SECTION("d1 = 0 → bias") {
        nkido::dispatch_midi_cc(vm, table, 0xD0u, 0, 0, /*channel=*/1);
        CHECK(read_target(vm, "press") == Catch::Approx(0.0f));
    }

    SECTION("d1 = 127 → bias + scale") {
        nkido::dispatch_midi_cc(vm, table, 0xD0u, 127, 0, /*channel=*/1);
        CHECK(read_target(vm, "press") == Catch::Approx(1.0f));
    }

    SECTION("d1 = 64 → midpoint") {
        nkido::dispatch_midi_cc(vm, table, 0xD0u, 64, 0, /*channel=*/1);
        CHECK(read_target(vm, "press") ==
              Catch::Approx(64.0f / 127.0f).margin(0.01f));
    }
}

TEST_CASE("dispatch_midi_cc — non-CC/PB/AT statuses are silently ignored",
          "[midi_cc][dispatch]") {
    cedar::VM vm;
    nkido::MidiCcRouteTable table = {
        {"x", 74, 0, 1.0f, 0.0f, 5.0f},
    };
    vm.set_param("x", 42.0f, 0.0f);

    // Note-on, note-off, program change, etc. — all should be ignored.
    nkido::dispatch_midi_cc(vm, table, 0x90u, 60, 100, /*channel=*/1);
    nkido::dispatch_midi_cc(vm, table, 0x80u, 60, 100, /*channel=*/1);
    nkido::dispatch_midi_cc(vm, table, 0xC0u, 5, 0, /*channel=*/1);

    CHECK(read_target(vm, "x") == Catch::Approx(42.0f));
}
