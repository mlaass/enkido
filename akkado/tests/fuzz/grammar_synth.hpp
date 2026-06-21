#pragma once
//
// Grammar synthesizer for the mutating recompile drift fuzz
// (docs/prd-memory-integrity-tests.md Leg 4, §7.3).
//
// Generates small RANDOM VALID Akkado programs unrelated to the seed corpus,
// so the fuzz isn't limited to mutations of existing patches. Each template is
// drawn from forms known to compile (mirrors the shapes exercised in
// test_fuzz_recompile_audio.cpp) with randomised numeric parameters and note
// sequences: oscillators, a filter, a delay/reverb FX, and pattern-driven
// synths — enough variety to stress codegen + hot-swap + state allocation.

#include <cstdint>
#include <random>
#include <sstream>
#include <string>

namespace fuzz {

class GrammarSynth {
public:
    explicit GrammarSynth(std::uint64_t seed) : rng_(seed) {}

    std::string program() {
        switch (rand_range(0, 5)) {
            case 0:  return osc_gain();
            case 1:  return osc_filter();
            case 2:  return pattern_synth();
            case 3:  return osc_delay();
            case 4:  return osc_reverb();
            default: return value_pattern();
        }
    }

private:
    std::mt19937_64 rng_;

    int rand_range(int lo, int hi) {
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng_);
    }
    float rand_freq() {
        static const float f[] = {110.0f, 220.0f, 330.0f, 440.0f, 660.0f, 880.0f};
        return f[static_cast<std::size_t>(rand_range(0, 5))];
    }
    float rand_gain() {
        static const float g[] = {0.2f, 0.3f, 0.4f, 0.5f};
        return g[static_cast<std::size_t>(rand_range(0, 3))];
    }
    std::string rand_osc() {
        static const char* o[] = {"sine", "saw", "tri", "square"};
        return o[static_cast<std::size_t>(rand_range(0, 3))];
    }
    std::string note_seq() {
        static const char* n[] = {"c3", "d3", "e3", "g3", "a3",
                                  "c4", "d4", "e4", "g4", "b4"};
        int len = rand_range(2, 5);
        std::string s = n[static_cast<std::size_t>(rand_range(0, 9))];
        for (int i = 1; i < len; ++i)
            s += std::string(" ") + n[static_cast<std::size_t>(rand_range(0, 9))];
        return s;
    }

    std::string osc_gain() {
        std::ostringstream os;
        os << rand_osc() << "(" << rand_freq() << ") * " << rand_gain()
           << " |> out(@, @)";
        return os.str();
    }
    std::string osc_filter() {
        static const char* filt[] = {"lp", "hp"};
        std::ostringstream os;
        os << rand_osc() << "(" << rand_freq() << ") |> "
           << filt[static_cast<std::size_t>(rand_range(0, 1))]
           << "(@, " << rand_range(400, 4000) << ") |> out(@ * " << rand_gain()
           << ", @ * " << rand_gain() << ")";
        return os.str();
    }
    std::string pattern_synth() {
        std::ostringstream os;
        os << "n\"" << note_seq() << "\" |> " << rand_osc()
           << "(%.freq) |> out(% * " << rand_gain() << ", % * " << rand_gain()
           << ")";
        return os.str();
    }
    std::string osc_delay() {
        std::ostringstream os;
        os << rand_osc() << "(" << rand_freq() << ") * " << rand_gain()
           << " |> delay(@, 0." << rand_range(1, 4) << ", 0."
           << rand_range(2, 7) << ") |> out(@, @)";
        return os.str();
    }
    std::string osc_reverb() {
        std::ostringstream os;
        os << rand_osc() << "(" << rand_freq() << ") * " << rand_gain()
           << " |> reverb(@) |> out(@, @)";
        return os.str();
    }
    std::string value_pattern() {
        std::ostringstream os;
        os << "v\"<0." << rand_range(1, 9) << " 0." << rand_range(1, 9)
           << " 0." << rand_range(1, 9) << ">\" * " << rand_osc() << "("
           << rand_freq() << ") |> out(@, @)";
        return os.str();
    }
};

}  // namespace fuzz
