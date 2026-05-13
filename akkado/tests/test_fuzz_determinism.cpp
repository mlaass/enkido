// Fuzz harness for Akkado compile- and render-determinism.
//
// Generates seeded-random valid Akkado programs and asserts:
//   1. compile(src) == compile(src) byte-for-byte
//   2. Hot-swap idempotence: a recompile of identical source preserves
//      SequenceState alternation step (the live-coding bug we just fixed)
//   3. Output is bounded (no NaN/Inf)
//
// When an invariant fails the test prints the seed AND the program, so the
// failing case can be added to a regression corpus.

#include <catch2/catch_test_macros.hpp>
#include "akkado/akkado.hpp"
#include <cedar/vm/vm.hpp>
#include <cedar/vm/state_pool.hpp>
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>
#include <cedar/opcodes/sequence.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fuzz {

// ============================================================================
// Program generator
// ============================================================================
//
// Grammar (deliberately small — chosen for stability, not coverage):
//
//   program       := chord_chain | note_chain
//   chord_chain   := 'c"' chord_mini '"' '|> soundfont(@, "gm", 0)'
//                    out_chain
//   note_chain    := 'pat("' note_mini '")' '|> osc("sin", %.freq)'
//                    out_chain
//   out_chain     := '|> out(@ * 0.5)'
//   chord_mini    := slowcat(group(chord)+) | seq(chord+) | chord
//   note_mini     := slowcat(group(pitch)+) | seq(pitch+) | pitch
//   group(X)      := '[' X X ']'
//   slowcat(X)    := '<' X+ '>'
//   seq(X)        := X (' ' X)*
//   chord         := Am | Dm | G | C | Em | Cmaj7 | F | Am7 | Dm7 | Em7
//   pitch         := c3 | d3 | e3 | f3 | g3 | a3 | c4 | e4 | g4 | b4

class Generator {
public:
    explicit Generator(std::uint64_t seed) : rng_(seed) {}

    std::string program() {
        std::uniform_int_distribution<int> chain_kind(0, 1);
        if (chain_kind(rng_) == 0) {
            return chord_chain();
        }
        return note_chain();
    }

private:
    std::mt19937_64 rng_;

    int rand_range(int lo, int hi) {
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng_);
    }

    template <typename T, std::size_t N>
    const T& pick(const T (&arr)[N]) {
        return arr[static_cast<std::size_t>(rand_range(0, static_cast<int>(N) - 1))];
    }

    std::string chord_atom() {
        static const char* atoms[] = {
            "Am", "Dm", "G", "C", "Em", "F", "Am7", "Dm7", "Em7", "Cmaj7"
        };
        return pick(atoms);
    }

    std::string note_atom() {
        static const char* atoms[] = {
            "c3", "d3", "e3", "g3", "a3", "c4", "d4", "e4", "g4", "b4"
        };
        return pick(atoms);
    }

    template <typename AtomFn>
    std::string mini_term(AtomFn atom_fn, int depth) {
        // depth limits recursion to keep generated programs small.
        if (depth <= 0) return atom_fn();
        int k = rand_range(0, 4);
        if (k == 0) return atom_fn();
        if (k == 1) {
            // group [a b]
            return "[" + mini_term(atom_fn, depth - 1) + " " +
                   mini_term(atom_fn, depth - 1) + "]";
        }
        if (k == 2) {
            // seq a b c (2..4 atoms)
            int n = rand_range(2, 4);
            std::string s = atom_fn();
            for (int i = 1; i < n; ++i) s += " " + atom_fn();
            return s;
        }
        if (k == 3) {
            // slowcat <a b c> (2..4 children)
            int n = rand_range(2, 4);
            std::string s = "<" + mini_term(atom_fn, depth - 1);
            for (int i = 1; i < n; ++i) s += " " + mini_term(atom_fn, depth - 1);
            return s + ">";
        }
        // replicate atom*N
        return atom_fn() + "*" + std::to_string(rand_range(2, 4));
    }

    std::string chord_chain() {
        std::ostringstream os;
        os << "c\"" << mini_term([this]() { return chord_atom(); }, 2)
           << "\" |> soundfont(@, \"gm\", 0) |> out(@ * 0.5)";
        return os.str();
    }

    std::string note_chain() {
        std::ostringstream os;
        os << "pat(\"" << mini_term([this]() { return note_atom(); }, 2)
           << "\") |> osc(\"sin\", %.freq) |> out(% * 0.3, % * 0.3)";
        return os.str();
    }
};

// ============================================================================
// Invariants
// ============================================================================

struct DiffResult {
    bool equal = true;
    std::string detail;
};

DiffResult diff_bytecode(const akkado::CompileResult& a,
                         const akkado::CompileResult& b) {
    DiffResult r;
    if (a.bytecode.size() != b.bytecode.size()) {
        r.equal = false;
        r.detail = "bytecode size differs: " + std::to_string(a.bytecode.size())
                 + " vs " + std::to_string(b.bytecode.size());
        return r;
    }
    if (std::memcmp(a.bytecode.data(), b.bytecode.data(), a.bytecode.size()) != 0) {
        r.equal = false;
        r.detail = "bytecode bytes differ";
    }
    return r;
}

DiffResult diff_sequence_events(const akkado::CompileResult& a,
                                const akkado::CompileResult& b) {
    DiffResult r;
    if (a.state_inits.size() != b.state_inits.size()) {
        r.equal = false;
        r.detail = "state_inits count differs";
        return r;
    }
    for (std::size_t i = 0; i < a.state_inits.size(); ++i) {
        const auto& A = a.state_inits[i];
        const auto& B = b.state_inits[i];
        if (A.state_id != B.state_id) {
            r.equal = false;
            r.detail = "state_id differs at index " + std::to_string(i);
            return r;
        }
        if (A.sequence_events.size() != B.sequence_events.size()) {
            r.equal = false;
            r.detail = "sequence count differs at state " + std::to_string(i);
            return r;
        }
        for (std::size_t s = 0; s < A.sequence_events.size(); ++s) {
            if (A.sequence_events[s].size() != B.sequence_events[s].size()) {
                r.equal = false;
                r.detail = "event count differs at state " + std::to_string(i) +
                           " seq " + std::to_string(s);
                return r;
            }
            for (std::size_t e = 0; e < A.sequence_events[s].size(); ++e) {
                const auto& EA = A.sequence_events[s][e];
                const auto& EB = B.sequence_events[s][e];
                if (EA.num_values != EB.num_values ||
                    EA.time != EB.time || EA.duration != EB.duration ||
                    EA.velocity != EB.velocity || EA.midi_note != EB.midi_note) {
                    r.equal = false;
                    r.detail = "event header differs at " + std::to_string(i) +
                               "/" + std::to_string(s) + "/" + std::to_string(e);
                    return r;
                }
                for (std::uint8_t v = 0; v < EA.num_values; ++v) {
                    if (EA.values[v] != EB.values[v]) {
                        r.equal = false;
                        r.detail = "event value " + std::to_string(int(v)) +
                                   " differs at " + std::to_string(i) + "/" +
                                   std::to_string(s) + "/" + std::to_string(e);
                        return r;
                    }
                }
            }
        }
    }
    return r;
}

// Apply a compile result's sequence state inits to the VM, matching the
// host-side logic in web/wasm/nkido_wasm.cpp:911. The seq_storage buffer
// keeps source sequences alive while init_sequence_program copies events
// into arena memory.
void apply_seq_state_inits(cedar::VM& vm, const akkado::CompileResult& cr,
                           std::vector<std::vector<cedar::Sequence>>& seq_storage) {
    for (const auto& init : cr.state_inits) {
        if (init.type != akkado::StateInitData::Type::SequenceProgram) continue;
        std::vector<cedar::Sequence> seq_copy = init.sequences;
        for (std::size_t i = 0; i < seq_copy.size() && i < init.sequence_events.size(); ++i) {
            if (!init.sequence_events[i].empty()) {
                seq_copy[i].events = const_cast<cedar::Event*>(init.sequence_events[i].data());
                seq_copy[i].num_events = static_cast<std::uint32_t>(init.sequence_events[i].size());
                seq_copy[i].capacity = static_cast<std::uint32_t>(init.sequence_events[i].size());
            }
        }
        seq_storage.push_back(std::move(seq_copy));
        auto& stored = seq_storage.back();
        vm.init_sequence_program_state(
            init.state_id, stored.data(), stored.size(),
            init.cycle_length, init.is_sample_pattern, init.total_events);
    }
}

struct RenderResult {
    bool ok = false;
    bool finite = true;
    float peak = 0.0f;
    std::string failure;
};

RenderResult render_n_blocks(const akkado::CompileResult& cr,
                             std::size_t blocks) {
    RenderResult r;
    cedar::VM vm;
    vm.set_crossfade_blocks(0);

    const std::size_t n_inst = cr.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> insts(n_inst);
    std::memcpy(insts.data(), cr.bytecode.data(), cr.bytecode.size());
    if (vm.load_program(insts) != cedar::VM::LoadResult::Success) {
        r.failure = "load_program returned non-Success";
        return r;
    }
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_seq_state_inits(vm, cr, seq_storage);

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    for (std::size_t b = 0; b < blocks; ++b) {
        vm.process_block(L.data(), R.data());
        for (std::size_t i = 0; i < cedar::BLOCK_SIZE; ++i) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) {
                r.finite = false;
                r.failure = "non-finite sample at block " + std::to_string(b) +
                            " sample " + std::to_string(i);
                return r;
            }
            float amax = std::max(std::abs(L[i]), std::abs(R[i]));
            if (amax > r.peak) r.peak = amax;
        }
    }
    r.ok = true;
    return r;
}

// Hot-swap idempotence: render N blocks, hot-swap with the same source, then
// the SequenceState's step counter must equal what it was before the swap.
// This is the user-reported live-coding bug, captured as an invariant.
struct HotSwapResult {
    bool ok = false;
    std::uint32_t step_before = 0;
    std::uint32_t step_after = 0;
    std::string failure;
};

HotSwapResult check_hot_swap_idempotence(const akkado::CompileResult& cr) {
    HotSwapResult r;
    cedar::VM vm;
    vm.set_crossfade_blocks(0);

    const std::size_t n_inst = cr.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> insts(n_inst);
    std::memcpy(insts.data(), cr.bytecode.data(), cr.bytecode.size());

    if (vm.load_program(insts) != cedar::VM::LoadResult::Success) {
        r.failure = "first load_program failed";
        return r;
    }
    std::vector<std::vector<cedar::Sequence>> seq_storage;
    apply_seq_state_inits(vm, cr, seq_storage);

    std::array<float, cedar::BLOCK_SIZE> L{}, R{};
    // ~4000 blocks ≈ 10s @ 48kHz / 128. Enough for several `<...>` cycles.
    for (int b = 0; b < 4000; ++b) {
        vm.process_block(L.data(), R.data());
    }

    // Find a SequenceState in the pool and capture its first alternation
    // step counter. Patterns without ALTERNATE mode return step_before=0
    // and the test passes trivially — that's correct (nothing to preserve).
    std::uint32_t seq_state_id = 0;
    for (const auto& init : cr.state_inits) {
        if (init.type == akkado::StateInitData::Type::SequenceProgram) {
            seq_state_id = init.state_id;
            break;
        }
    }
    if (seq_state_id == 0) {
        r.ok = true;  // not a pattern program; nothing to check
        return r;
    }
    auto* st = vm.states().get_if<cedar::SequenceState>(seq_state_id);
    if (!st || st->num_sequences == 0) {
        r.ok = true;
        return r;
    }
    std::int32_t alt_idx = -1;
    for (std::uint32_t i = 0; i < st->num_sequences; ++i) {
        if (st->sequences[i].mode == cedar::SequenceMode::ALTERNATE) {
            alt_idx = static_cast<std::int32_t>(i);
            break;
        }
    }
    if (alt_idx < 0) { r.ok = true; return r; }  // no alternation, no risk
    r.step_before = st->sequences[alt_idx].step;

    // Recompile and hot-swap. The new compile MUST preserve the alternation
    // step counter.
    std::vector<cedar::Instruction> insts2(n_inst);
    std::memcpy(insts2.data(), cr.bytecode.data(), cr.bytecode.size());
    if (vm.load_program(insts2) != cedar::VM::LoadResult::Success) {
        r.failure = "second load_program failed";
        return r;
    }
    apply_seq_state_inits(vm, cr, seq_storage);

    auto* st2 = vm.states().get_if<cedar::SequenceState>(seq_state_id);
    if (!st2 || st2->num_sequences == 0) {
        r.failure = "state pool lost SequenceState across swap";
        return r;
    }
    r.step_after = st2->sequences[alt_idx].step;
    if (r.step_after != r.step_before) {
        r.failure = "alternation step shifted on hot-swap: " +
                    std::to_string(r.step_before) + " → " +
                    std::to_string(r.step_after);
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace fuzz

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("fuzz: compile-determinism over random programs",
          "[fuzz][determinism]") {
    constexpr std::size_t N = 100;
    // Bound the seed range so the test is reproducible from this file alone.
    for (std::size_t s = 0; s < N; ++s) {
        const std::uint64_t seed = 0xDEADBEEFull + s * 1009ull;
        fuzz::Generator gen(seed);
        const std::string src = gen.program();

        auto a = akkado::compile(src);
        if (!a.success) continue;  // generator may produce edge cases
        auto b = akkado::compile(src);
        REQUIRE(b.success);

        auto bd = fuzz::diff_bytecode(a, b);
        auto sd = fuzz::diff_sequence_events(a, b);
        if (!bd.equal || !sd.equal) {
            UNSCOPED_INFO("seed=" << seed << " src=" << src);
            UNSCOPED_INFO("bytecode diff: " << bd.detail);
            UNSCOPED_INFO("state init diff: " << sd.detail);
        }
        CHECK(bd.equal);
        CHECK(sd.equal);
    }
}

TEST_CASE("fuzz: render-bounded over random programs",
          "[fuzz][determinism][render]") {
    constexpr std::size_t N = 30;  // render is slower than compile
    for (std::size_t s = 0; s < N; ++s) {
        const std::uint64_t seed = 0xCAFEBABEull + s * 1013ull;
        fuzz::Generator gen(seed);
        const std::string src = gen.program();

        auto cr = akkado::compile(src);
        if (!cr.success) continue;

        // ~100 blocks ≈ 0.27s @ 48kHz/128. Long enough to surface immediate
        // blow-ups, short enough to keep the test under a few seconds total.
        auto r = fuzz::render_n_blocks(cr, 100);
        if (!r.ok || !r.finite) {
            UNSCOPED_INFO("seed=" << seed << " src=" << src);
            UNSCOPED_INFO("render failure: " << r.failure);
        }
        REQUIRE(r.ok);
        CHECK(r.finite);
        // Generated programs use modest gain (×0.3..×0.5) so peak should
        // stay comfortably below 4.0 even with chord polyphony summing.
        CHECK(r.peak < 4.0f);
    }
}

TEST_CASE("fuzz: hot-swap idempotence over random programs",
          "[fuzz][determinism][hot_swap]") {
    constexpr std::size_t N = 30;
    for (std::size_t s = 0; s < N; ++s) {
        const std::uint64_t seed = 0xFEEDFACEull + s * 1019ull;
        fuzz::Generator gen(seed);
        const std::string src = gen.program();

        auto cr = akkado::compile(src);
        if (!cr.success) continue;

        auto r = fuzz::check_hot_swap_idempotence(cr);
        if (!r.ok) {
            UNSCOPED_INFO("seed=" << seed << " src=" << src);
            UNSCOPED_INFO("hot-swap failure: " << r.failure);
            UNSCOPED_INFO("step before/after: " << r.step_before
                          << " / " << r.step_after);
        }
        CHECK(r.ok);
    }
}

// ============================================================================
// Regression corpus — seeds that previously triggered failures. Add new
// failing seeds here when the fuzz test surfaces a bug, then keep them
// running forever as a regression guard.
// ============================================================================

TEST_CASE("fuzz corpus: known historical failures", "[fuzz][corpus]") {
    struct Case {
        const char* name;
        const char* src;
    };
    static const Case cases[] = {
        // User-reported case: live-recompile resets `<...>` alternation
        // step counter, audibly changing voicings. Fixed in state_pool.hpp
        // by snapshotting playback state across init_sequence_program.
        {"user-chord-pattern",
         R"(c"<[Am Am7] [Dm7 Dm] G CM >" |> soundfont(@, "gm", 0) |> out(@*.85))"},
    };

    for (const auto& c : cases) {
        UNSCOPED_INFO("corpus case: " << c.name);
        auto cr = akkado::compile(c.src);
        REQUIRE(cr.success);

        auto cr2 = akkado::compile(c.src);
        REQUIRE(cr2.success);
        auto bd = fuzz::diff_bytecode(cr, cr2);
        auto sd = fuzz::diff_sequence_events(cr, cr2);
        CHECK(bd.equal);
        CHECK(sd.equal);

        auto rr = fuzz::render_n_blocks(cr, 200);
        CHECK(rr.ok);
        CHECK(rr.finite);

        auto hs = fuzz::check_hot_swap_idempotence(cr);
        if (!hs.ok) {
            UNSCOPED_INFO("hot-swap failure: " << hs.failure);
        }
        CHECK(hs.ok);
    }
}
