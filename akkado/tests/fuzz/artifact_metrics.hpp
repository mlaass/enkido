// Shared audio-artifact metrics for fuzz tests.
//
// These are deliberately simple, header-only, allocation-free helpers so
// they can be dropped into any Catch2 test without build plumbing. Each
// metric is a small struct that you feed audio blocks as they're rendered
// and query at the end (or per-block).
//
// Conventions:
//   * "click" = a sample-to-sample delta above a threshold. Default 0.5
//     catches the obvious crack/pop without flagging legitimate transient
//     attacks (an oscillator switching value mid-cycle naturally jumps).
//   * "backward beat jump" = `current_beat_position()` moved backwards by
//     more than EPS within one block. Indicates clock corruption — pattern
//     cycle resets, seek going wrong, etc.
//   * "trigger pulse" = a rising edge in the buffer the test wants to
//     watch (e.g. a `@.trig` route). Counts samples that cross 0.5
//     going up.
//
// Lives in `akkado/tests/fuzz/` because the C++ fuzz tests are its
// primary consumer; cedar tests can include it via the akkado test target
// (header-only, no extra link).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fuzz {

// ---------------------------------------------------------------------------
// Sample-level click detector.
// ---------------------------------------------------------------------------
// Tracks the worst |x[n] - x[n-1]| seen so far across all submitted blocks.
// Stores the absolute sample index where it occurred so failures can be
// correlated with swap events.
struct ClickDetector {
    float threshold = 0.5f;          // delta above this counts as a click
    float worst_delta = 0.0f;
    std::uint64_t worst_index = 0;   // absolute sample index of worst delta
    std::uint64_t click_count = 0;
    float prev_sample = 0.0f;        // carried across block boundaries
    bool primed = false;             // ignore the first sample
    std::uint64_t total_samples = 0;

    void observe(const float* block, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            if (primed) {
                const float d = std::fabs(block[i] - prev_sample);
                if (d > worst_delta) {
                    worst_delta = d;
                    worst_index = total_samples + i;
                }
                if (d > threshold) ++click_count;
            }
            prev_sample = block[i];
            primed = true;
        }
        total_samples += n;
    }
};

// ---------------------------------------------------------------------------
// NaN/Inf and clipping scanner.
// ---------------------------------------------------------------------------
struct SanityScanner {
    float clip_threshold = 4.0f;     // generated fuzz programs use modest gain
    std::uint64_t first_bad_index = UINT64_MAX;
    std::string first_bad_reason;
    std::uint64_t nan_count = 0;
    std::uint64_t inf_count = 0;
    std::uint64_t clip_count = 0;
    float peak = 0.0f;
    std::uint64_t total_samples = 0;

    void observe(const float* block, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const float x = block[i];
            if (std::isnan(x)) {
                ++nan_count;
                if (first_bad_index == UINT64_MAX) {
                    first_bad_index = total_samples + i;
                    first_bad_reason = "NaN";
                }
            } else if (std::isinf(x)) {
                ++inf_count;
                if (first_bad_index == UINT64_MAX) {
                    first_bad_index = total_samples + i;
                    first_bad_reason = "Inf";
                }
            } else {
                const float ax = std::fabs(x);
                if (ax > peak) peak = ax;
                if (ax > clip_threshold) {
                    ++clip_count;
                    if (first_bad_index == UINT64_MAX) {
                        first_bad_index = total_samples + i;
                        first_bad_reason = "clip > " + std::to_string(clip_threshold);
                    }
                }
            }
        }
        total_samples += n;
    }

    bool ok() const { return first_bad_index == UINT64_MAX; }
};

// ---------------------------------------------------------------------------
// Backward beat-position tracker.
// ---------------------------------------------------------------------------
// Beat position is sampled once per block (cheap). A backward jump bigger
// than `eps` flags a clock anomaly. Recompiles must NOT cause the global
// clock to rewind.
struct BeatMonotonicity {
    float eps = 1e-4f;
    float last_beat = -1.0f;         // -1 = not yet sampled
    float worst_backward = 0.0f;     // largest backward jump magnitude
    std::uint32_t worst_block = 0;
    std::uint32_t backward_count = 0;

    void observe(float beat, std::uint32_t block_idx) {
        if (last_beat >= 0.0f) {
            const float d = last_beat - beat;
            if (d > eps) {
                ++backward_count;
                if (d > worst_backward) {
                    worst_backward = d;
                    worst_block = block_idx;
                }
            }
        }
        last_beat = beat;
    }
};

// ---------------------------------------------------------------------------
// Rising-edge counter (trigger pulses).
// ---------------------------------------------------------------------------
// Counts samples where the buffer crosses `threshold` going upward and
// records each pulse's absolute sample index. The positions enable
// inter-pulse interval analysis — a trigger that fires on time will have
// constant spacing; a hot-swap that drops or shifts a pulse shows up as
// a missing interval or a jitter spike at the swap boundary.
struct EdgeCounter {
    float threshold = 0.5f;
    std::uint64_t rising_edges = 0;
    std::vector<std::uint64_t> positions;   // absolute sample index of each edge
    float prev_sample = 0.0f;
    bool primed = false;
    std::uint64_t total_samples = 0;

    void observe(const float* block, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            if (primed) {
                if (prev_sample < threshold && block[i] >= threshold) {
                    ++rising_edges;
                    positions.push_back(total_samples + i);
                }
            }
            prev_sample = block[i];
            primed = true;
        }
        total_samples += n;
    }
};

// ---------------------------------------------------------------------------
// Inter-pulse interval analysis.
// ---------------------------------------------------------------------------
// Given a series of pulse positions and an expected interval (in samples),
// flags pulses whose spacing deviates by more than `tolerance_samples`.
// Designed to catch the failure modes the swap-path can introduce:
//   - dropped pulse  → one interval ≈ 2× expected
//   - duplicated pulse → one interval ≈ 0 (or far below expected)
//   - shifted pulse → jitter spike
//
// `worst_drift_samples` is the largest deviation from expected_interval
// across all observed intervals; `worst_drift_index` is the *pulse number*
// (1-based — interval i sits between pulse i-1 and pulse i).
struct IntervalAnalysis {
    bool ok = true;
    std::uint32_t bad_count = 0;
    float worst_drift_samples = 0.0f;
    std::uint32_t worst_drift_index = 0;
    std::uint64_t worst_drift_position = 0;   // absolute sample of the late pulse

    static IntervalAnalysis analyze(const std::vector<std::uint64_t>& positions,
                                    double expected_interval,
                                    double tolerance_samples) {
        IntervalAnalysis a;
        if (positions.size() < 2) return a;
        for (std::size_t i = 1; i < positions.size(); ++i) {
            const double interval =
                static_cast<double>(positions[i] - positions[i - 1]);
            const double drift = interval - expected_interval;
            const double adrift = std::fabs(drift);
            if (adrift > tolerance_samples) {
                ++a.bad_count;
                a.ok = false;
                if (adrift > a.worst_drift_samples) {
                    a.worst_drift_samples = static_cast<float>(adrift);
                    a.worst_drift_index = static_cast<std::uint32_t>(i);
                    a.worst_drift_position = positions[i];
                }
            }
        }
        return a;
    }
};

// ---------------------------------------------------------------------------
// Sliding-window energy tracker (tail integrity).
// ---------------------------------------------------------------------------
// Records per-block RMS so a test can assert "the level didn't drop to
// zero" (tail killed by swap) or "didn't spike 100x" (feedback burst).
struct BlockEnergyTrace {
    std::vector<float> rms;          // length == blocks observed

    void observe(const float* block, std::size_t n) {
        double acc = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            acc += static_cast<double>(block[i]) * block[i];
        }
        rms.push_back(static_cast<float>(std::sqrt(acc / static_cast<double>(n))));
    }

    // Returns true if any adjacent block-pair within [first, last) shows
    // an energy ratio greater than `ratio_threshold` (e.g. 100x burst) or
    // a drop from > floor to ~0. `floor` filters silent-leading-silent
    // false positives.
    struct Anomaly {
        bool found = false;
        std::uint32_t block_idx = 0;
        float ratio = 1.0f;
        std::string kind;            // "burst" or "silence"
    };
    Anomaly check_window(std::uint32_t first, std::uint32_t last,
                         float ratio_threshold, float floor) const {
        Anomaly a;
        last = std::min<std::uint32_t>(last, static_cast<std::uint32_t>(rms.size()));
        for (std::uint32_t i = first + 1; i < last; ++i) {
            const float prev = rms[i - 1];
            const float cur = rms[i];
            if (prev > floor && cur > floor) {
                const float r = cur / prev;
                const float rr = (r > 1.0f) ? r : 1.0f / r;
                if (rr > ratio_threshold) {
                    a.found = true;
                    a.block_idx = i;
                    a.ratio = r;
                    a.kind = (r > 1.0f) ? "burst" : "drop";
                    return a;
                }
            } else if (prev > floor && cur <= floor * 0.01f) {
                a.found = true;
                a.block_idx = i;
                a.ratio = 0.0f;
                a.kind = "silence";
                return a;
            }
        }
        return a;
    }
};

}  // namespace fuzz
