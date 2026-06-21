#pragma once
//
// Source mutators for the recompile drift fuzz
// (docs/prd-memory-integrity-tests.md Leg 4, §7).
//
// Two families:
//   * Token-level (§7.1): tokenize the source, apply delete/duplicate/swap/
//     replace/insert, rejoin. Cheap; cheerfully produces INVALID programs,
//     which is wanted — invalid input exercises the compiler's error-recovery
//     path (the load-bearing ~30% of iterations per §9.1).
//   * Structural / "AST-level" (§7.2): operate on the pipe-chain structure of
//     the source — reorder stages, delete a stage, replace a builtin with one
//     of similar arity. Implemented as source-structural edits on the `|>`
//     chain rather than a parse-tree pretty-print roundtrip (Akkado exposes no
//     public unparser); this covers the PIPE_REORDER / BUILTIN_REPLACE /
//     stage-delete intent and mostly yields valid programs.
//
// Both are deterministic given the seed, so any failure reproduces from the
// (seed, iteration) pair the drift fuzz prints.

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fuzz {

class Mutator {
public:
    explicit Mutator(std::uint64_t seed) : rng_(seed) {}

    // Apply `n` token-level mutations.
    std::string mutate_token(const std::string& src, int n) {
        std::vector<std::string> toks = tokenize(src);
        for (int i = 0; i < n && !toks.empty(); ++i) apply_token_op(toks);
        return join(toks);
    }

    // Apply one structural mutation to the pipe chain.
    std::string mutate_structural(const std::string& src) {
        std::vector<std::string> stages = split_pipe(src);
        if (stages.size() < 2) return mutate_token(src, 1);  // nothing to restructure
        const int r = rand_range(0, 99);
        if (r < 35) return stage_swap(stages);      // SUBTREE_SWAP (§7.2) 35%
        if (r < 55) return stage_delete(stages);    // EXPRESSION_DELETE     20%
        if (r < 75) return pipe_reorder(stages);    // PIPE_REORDER          20%
        return builtin_replace(src);                // BUILTIN_REPLACE       25%
    }

private:
    std::mt19937_64 rng_;

    int rand_range(int lo, int hi) {
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng_);
    }
    template <typename T>
    std::size_t rand_index(const std::vector<T>& v) {
        return static_cast<std::size_t>(rand_range(0, static_cast<int>(v.size()) - 1));
    }

    // --- token-level ---------------------------------------------------------

    static bool is_ident_char(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '#';
    }

    static std::vector<std::string> tokenize(const std::string& s) {
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
            if (c == '"') {  // string literal: keep quotes + body as one token
                std::size_t j = i + 1;
                while (j < s.size() && s[j] != '"') ++j;
                if (j < s.size()) ++j;  // include closing quote
                out.push_back(s.substr(i, j - i));
                i = j;
                continue;
            }
            if (is_ident_char(c)) {
                std::size_t j = i;
                while (j < s.size() && is_ident_char(s[j])) ++j;
                out.push_back(s.substr(i, j - i));
                i = j;
                continue;
            }
            out.emplace_back(1, c);  // single punctuation/operator char
            ++i;
        }
        return out;
    }

    static std::string join(const std::vector<std::string>& toks) {
        std::string out;
        for (std::size_t i = 0; i < toks.size(); ++i) {
            if (i) out += ' ';
            out += toks[i];
        }
        return out;
    }

    const std::string& vocab() {
        static const std::vector<std::string> v = {
            "sine", "saw", "out", "lp", "hp", "@", "%", "*", "+", "-",
            "(", ")", ",", "0.5", "220", "440", ".freq", "n", "[", "]",
            "<", ">", "|>", "\"c4 e4\"", "reverb", "delay"};
        return v[rand_index(v)];
    }

    void apply_token_op(std::vector<std::string>& toks) {
        // Frequencies per §7.1: delete 25, dup 15, swap 20, replace 25, insert 15.
        int r = rand_range(0, 99);
        if (r < 25) {  // DELETE
            toks.erase(toks.begin() + static_cast<long>(rand_index(toks)));
        } else if (r < 40) {  // DUPLICATE
            std::size_t k = rand_index(toks);
            toks.insert(toks.begin() + static_cast<long>(k), toks[k]);
        } else if (r < 60) {  // SWAP adjacent
            if (toks.size() >= 2) {
                std::size_t k = static_cast<std::size_t>(
                    rand_range(0, static_cast<int>(toks.size()) - 2));
                std::swap(toks[k], toks[k + 1]);
            }
        } else if (r < 85) {  // REPLACE
            toks[rand_index(toks)] = vocab();
        } else {  // INSERT
            std::size_t k = rand_index(toks);
            toks.insert(toks.begin() + static_cast<long>(k), vocab());
        }
    }

    // --- structural ----------------------------------------------------------

    static std::vector<std::string> split_pipe(const std::string& s) {
        std::vector<std::string> stages;
        std::size_t start = 0;
        for (std::size_t i = 0; i + 1 < s.size(); ++i) {
            if (s[i] == '|' && s[i + 1] == '>') {
                stages.push_back(trim(s.substr(start, i - start)));
                start = i + 2;
                ++i;
            }
        }
        stages.push_back(trim(s.substr(start)));
        return stages;
    }

    static std::string trim(std::string s) {
        std::size_t a = s.find_first_not_of(" \t\n\r");
        std::size_t b = s.find_last_not_of(" \t\n\r");
        if (a == std::string::npos) return {};
        return s.substr(a, b - a + 1);
    }

    static std::string rejoin_pipe(const std::vector<std::string>& stages) {
        std::string out;
        for (std::size_t i = 0; i < stages.size(); ++i) {
            if (i) out += " |> ";
            out += stages[i];
        }
        return out;
    }

    std::string stage_swap(std::vector<std::string> stages) {
        std::size_t a = rand_index(stages), b = rand_index(stages);
        std::swap(stages[a], stages[b]);
        return rejoin_pipe(stages);
    }
    std::string stage_delete(std::vector<std::string> stages) {
        stages.erase(stages.begin() + static_cast<long>(rand_index(stages)));
        return rejoin_pipe(stages);
    }
    std::string pipe_reorder(std::vector<std::string> stages) {
        std::shuffle(stages.begin(), stages.end(), rng_);
        return rejoin_pipe(stages);
    }
    std::string builtin_replace(const std::string& src) {
        static const char* from[] = {"sine", "saw", "tri", "square", "lp", "hp", "reverb"};
        static const char* to[]   = {"saw", "sine", "square", "tri", "hp", "lp", "delay"};
        std::string s = src;
        for (int i = 0; i < 7; ++i) {
            auto pos = s.find(from[i]);
            if (pos != std::string::npos) {
                s.replace(pos, std::char_traits<char>::length(from[i]), to[i]);
                break;
            }
        }
        return s;
    }
};

}  // namespace fuzz
