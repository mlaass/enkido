#include "akkado/voicing.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>

namespace akkado::voicing {

namespace {

// Process-lifetime registry. mutex guards register/lookup against compile-time
// concurrency (uncommon, but cheap).
std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, VoicingDict>& voicing_registry() {
    static std::unordered_map<std::string, VoicingDict> reg = []() {
        std::unordered_map<std::string, VoicingDict> r;
        VoicingDict close{};   close.builtin_kind = 0;  r["close"] = close;
        VoicingDict open{};    open.builtin_kind = 1;   r["open"] = open;
        VoicingDict drop2{};   drop2.builtin_kind = 2;  r["drop2"] = drop2;
        VoicingDict drop3{};   drop3.builtin_kind = 3;  r["drop3"] = drop3;
        return r;
    }();
    return reg;
}

// Apply a built-in voicing transform to a sorted ascending chord (notes as
// MIDI). Returns the transformed (still sorted ascending) note vector.
std::vector<int> apply_builtin(const std::vector<int>& notes_asc, int kind) {
    if (notes_asc.size() < 2 || kind == 0) return notes_asc;
    std::vector<int> r = notes_asc;
    if (kind == 1) {
        // Open voicing: drop the lowest note an octave.
        r[0] -= 12;
    } else if (kind == 2 && r.size() >= 2) {
        // Drop-2: drop the 2nd-highest by an octave.
        r[r.size() - 2] -= 12;
    } else if (kind == 3 && r.size() >= 3) {
        // Drop-3: drop the 3rd-highest by an octave.
        r[r.size() - 3] -= 12;
    }
    std::sort(r.begin(), r.end());
    return r;
}

// Octave-shift a chord (uniformly add 12*k to every note).
std::vector<int> shift_octaves(const std::vector<int>& notes, int k) {
    std::vector<int> r;
    r.reserve(notes.size());
    for (int n : notes) r.push_back(n + 12 * k);
    return r;
}

int total_distance(const std::vector<int>& a, const std::vector<int>& b) {
    if (a.size() != b.size()) return std::numeric_limits<int>::max();
    int d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        d += std::abs(a[i] - b[i]);
    }
    return d;
}

int distance_from_anchor_sum(const std::vector<int>& notes, int anchor) {
    int d = 0;
    for (int n : notes) d += std::abs(n - anchor);
    return d;
}

// Generate candidate voicings for a single chord: enumerate all rotations
// (inversions in the abstract sense — each note can become the bottom),
// apply built-in transform, and tile across octaves [-2, +2].
std::vector<std::vector<int>> generate_candidates(const ChordSpec& chord,
                                                  const VoicingDict* dict) {
    // Resolve which interval set to use. User-registered dicts (addVoicings)
    // can override per-quality intervals — e.g. {M:[0,4,7,11,14]} for a
    // 5-note major9 default. Falls back to the chord parser's intrinsic
    // intervals when the dict has no entry for the chord's quality.
    const std::vector<int>* intervals = &chord.intervals;
    if (dict != nullptr && !dict->qualities.empty() && !chord.quality.empty()) {
        auto it = dict->qualities.find(chord.quality);
        if (it != dict->qualities.end()) {
            intervals = &it->second;
        }
    }

    std::vector<int> notes;
    notes.reserve(intervals->size());
    for (int iv : *intervals) {
        notes.push_back(chord.root_midi + iv);
    }
    if (notes.empty()) {
        notes.push_back(chord.root_midi);
    }
    std::sort(notes.begin(), notes.end());

    // Inversions: rotate the bottom note up an octave to get N rotations of
    // the chord. The voicing transform is NOT applied yet — applying it
    // before the rotation would let the rotation undo its effect (open's
    // bass-drop is re-raised by the next inversion, collapsing close and
    // open candidate sets).
    std::vector<std::vector<int>> inversions;
    inversions.push_back(notes);
    std::vector<int> inv = notes;
    for (std::size_t i = 1; i < notes.size(); ++i) {
        inv[0] += 12;
        std::sort(inv.begin(), inv.end());
        inversions.push_back(inv);
    }

    // Apply the voicing transform to each inversion. close=identity, open
    // drops the bass an octave, drop2 drops 2nd-from-top, drop3 drops
    // 3rd-from-top — applied per-inversion so each rotation contributes a
    // distinctly-shaped candidate.
    int kind = dict != nullptr ? dict->builtin_kind : 0;
    if (kind > 0) {
        for (auto& v : inversions) {
            v = apply_builtin(v, kind);
        }
    }

    // Octave shifts ∈ [-2, +2].
    std::vector<std::vector<int>> result;
    result.reserve(inversions.size() * 5);
    for (const auto& inv_v : inversions) {
        for (int k = -2; k <= 2; ++k) {
            result.push_back(shift_octaves(inv_v, k));
        }
    }
    return result;
}

bool passes_mode_filter(const std::vector<int>& notes, int anchor, Mode mode) {
    if (notes.empty()) return true;
    int lo = *std::min_element(notes.begin(), notes.end());
    int hi = *std::max_element(notes.begin(), notes.end());
    switch (mode) {
        case Mode::Below: return hi <= anchor;
        case Mode::Above: return lo >= anchor;
        case Mode::Duck:
            return std::find(notes.begin(), notes.end(), anchor) == notes.end();
        case Mode::Root:
            // Root mode handled separately — accept all candidates here.
            return true;
    }
    return true;
}

} // namespace

std::optional<int> parse_anchor(std::string_view s) {
    // e.g. "c4", "C#5", "Bb3", "Eb-1"
    if (s.empty()) return std::nullopt;
    std::size_t i = 0;
    char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    int pc = 0;
    switch (letter) {
        case 'c': pc = 0; break;
        case 'd': pc = 2; break;
        case 'e': pc = 4; break;
        case 'f': pc = 5; break;
        case 'g': pc = 7; break;
        case 'a': pc = 9; break;
        case 'b': pc = 11; break;
        default: return std::nullopt;
    }
    ++i;
    if (i < s.size() && (s[i] == '#' || s[i] == 'b' || s[i] == 'B')) {
        char acc = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        if (acc == '#') pc += 1;
        else if (acc == 'b') pc -= 1;
        ++i;
    }
    if (i >= s.size()) return std::nullopt;
    int sign = 1;
    if (s[i] == '-') { sign = -1; ++i; }
    if (i >= s.size()) return std::nullopt;
    int oct = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        oct = oct * 10 + (s[i] - '0');
        ++i;
    }
    if (i < s.size()) return std::nullopt;  // trailing chars
    oct *= sign;
    // MIDI: C4 = 60. Pitch-class offset by octave.
    int midi = (oct + 1) * 12 + pc;
    return midi;
}

std::optional<Mode> parse_mode(std::string_view s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "below") return Mode::Below;
    if (lower == "above") return Mode::Above;
    if (lower == "duck")  return Mode::Duck;
    if (lower == "root")  return Mode::Root;
    return std::nullopt;
}

std::vector<std::vector<int>> voice_chords(
    const std::vector<ChordSpec>& chords,
    int anchor_midi,
    Mode mode,
    const VoicingDict* dict) {

    std::vector<std::vector<int>> result;
    result.reserve(chords.size());

    std::vector<int> prev;
    for (const auto& chord : chords) {
        if (mode == Mode::Root) {
            // Root mode: bass note in lowest octave near anchor; rest of
            // the chord stacked closest to anchor.
            std::vector<int> notes;
            for (int iv : chord.intervals) {
                notes.push_back(chord.root_midi + iv);
            }
            if (notes.empty()) notes.push_back(chord.root_midi);
            std::sort(notes.begin(), notes.end());

            // Find octave shift bringing the LOWEST note as close as
            // possible to anchor - 12 (one octave below anchor).
            int target_bass = anchor_midi - 12;
            int bass_shift = static_cast<int>(std::round(
                static_cast<float>(target_bass - notes[0]) / 12.0f));
            int bass = notes[0] + 12 * bass_shift;

            // Voice the upper notes near the anchor.
            std::vector<int> upper(notes.begin() + 1, notes.end());
            // Shift upper voices uniformly so their mean is near anchor.
            if (!upper.empty()) {
                int upper_min = *std::min_element(upper.begin(), upper.end());
                int upper_max = *std::max_element(upper.begin(), upper.end());
                int upper_mid = (upper_min + upper_max) / 2;
                int up_shift = static_cast<int>(std::round(
                    static_cast<float>(anchor_midi - upper_mid) / 12.0f));
                upper = shift_octaves(upper, up_shift);
            }
            std::vector<int> voiced;
            voiced.push_back(bass);
            for (int u : upper) voiced.push_back(u);
            std::sort(voiced.begin(), voiced.end());
            result.push_back(voiced);
            prev = voiced;
            continue;
        }

        auto candidates = generate_candidates(chord, dict);
        if (candidates.empty()) {
            result.emplace_back();
            prev.clear();
            continue;
        }

        // Filter by mode; if none pass, fall back to closest-by-anchor.
        std::vector<std::vector<int>> filtered;
        for (const auto& c : candidates) {
            if (passes_mode_filter(c, anchor_midi, mode)) {
                filtered.push_back(c);
            }
        }
        if (filtered.empty()) filtered = candidates;

        // Score: if we have a previous voicing, minimize movement; else
        // minimize sum-of-distances from anchor.
        const std::vector<int>* best = &filtered.front();
        int best_score = std::numeric_limits<int>::max();
        for (const auto& c : filtered) {
            int score;
            if (!prev.empty() && c.size() == prev.size()) {
                score = total_distance(prev, c);
            } else {
                score = distance_from_anchor_sum(c, anchor_midi);
            }
            if (score < best_score) {
                best_score = score;
                best = &c;
            }
        }

        result.push_back(*best);
        prev = *best;
    }
    return result;
}

const VoicingDict* lookup_voicing(std::string_view name) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto& reg = voicing_registry();
    auto it = reg.find(std::string(name));
    if (it != reg.end()) return &it->second;
    return nullptr;
}

void register_voicing(std::string_view name, VoicingDict dict) {
    std::lock_guard<std::mutex> lock(registry_mutex());
    voicing_registry()[std::string(name)] = std::move(dict);
}

} // namespace akkado::voicing
