"""
Audio-artifact metrics for Python fuzz tests.

Mirrors akkado/tests/fuzz/artifact_metrics.hpp so opcode-level Python fuzz
drivers can use the same vocabulary as the C++ tests.

All functions operate on full numpy arrays (not streaming). For long
renders, accumulate audio per block and feed the full array at the end —
the math is cheap (vectorized) and the lookups are O(n).
"""

from __future__ import annotations

from dataclasses import dataclass
import numpy as np


@dataclass
class ClickReport:
    worst_delta: float
    worst_index: int
    click_count: int  # samples where |Δ| > threshold
    threshold: float


def detect_clicks(signal: np.ndarray, threshold: float = 0.5) -> ClickReport:
    """Sample-to-sample delta scanner.

    A "click" is any |x[n] - x[n-1]| above `threshold`. The default 0.5 is
    a deliberately loose threshold — catches the obvious pop/crack without
    flagging legitimate transient attacks. Trigger-bearing programs need
    a higher threshold (e.g. 1.5) because their output naturally jumps 0→1
    in one sample.
    """
    if signal.size < 2:
        return ClickReport(0.0, 0, 0, threshold)
    diff = np.abs(np.diff(signal))
    worst_idx = int(np.argmax(diff))
    return ClickReport(
        worst_delta=float(diff[worst_idx]),
        worst_index=worst_idx + 1,  # diff[i] is between signal[i] and signal[i+1]
        click_count=int(np.sum(diff > threshold)),
        threshold=threshold,
    )


@dataclass
class SanityReport:
    nan_count: int
    inf_count: int
    clip_count: int
    peak: float
    first_bad_index: int  # -1 if clean
    first_bad_reason: str


def scan_sanity(signal: np.ndarray, clip_threshold: float = 4.0) -> SanityReport:
    """NaN/Inf/clip scanner with first-offender reporting."""
    nan_mask = np.isnan(signal)
    inf_mask = np.isinf(signal)
    abs_signal = np.where(nan_mask | inf_mask, 0.0, np.abs(signal))
    clip_mask = abs_signal > clip_threshold

    nan_count = int(np.sum(nan_mask))
    inf_count = int(np.sum(inf_mask))
    clip_count = int(np.sum(clip_mask))
    peak = float(np.max(abs_signal)) if abs_signal.size else 0.0

    first_bad = -1
    reason = ""
    bad_mask = nan_mask | inf_mask | clip_mask
    if bad_mask.any():
        first_bad = int(np.argmax(bad_mask))
        if nan_mask[first_bad]:
            reason = "NaN"
        elif inf_mask[first_bad]:
            reason = "Inf"
        else:
            reason = f"clip > {clip_threshold}"
    return SanityReport(nan_count, inf_count, clip_count, peak, first_bad, reason)


def count_rising_edges(signal: np.ndarray, threshold: float = 0.5) -> int:
    """Count samples that cross `threshold` going upward.

    A trigger pulse (1 sample at ~1.0 surrounded by zeros) contributes one
    rising edge per pulse — useful for asserting "the recompile didn't
    drop or duplicate any triggers."
    """
    if signal.size < 2:
        return 0
    above = signal >= threshold
    # rising = above[i] is True, above[i-1] is False
    return int(np.sum(above[1:] & ~above[:-1]))


@dataclass
class EnergyAnomaly:
    found: bool
    block_idx: int
    ratio: float
    kind: str  # "burst" / "drop" / "silence" / ""


def block_rms(signal: np.ndarray, block_size: int) -> np.ndarray:
    """Per-block RMS for a render. `signal` length must be a multiple of `block_size`."""
    n_blocks = signal.size // block_size
    trimmed = signal[: n_blocks * block_size].reshape(n_blocks, block_size)
    return np.sqrt(np.mean(trimmed * trimmed, axis=1).astype(np.float64)).astype(np.float32)


def find_energy_anomaly(
    rms: np.ndarray,
    first_block: int = 0,
    ratio_threshold: float = 50.0,
    floor: float = 1e-5,
) -> EnergyAnomaly:
    """Scan per-block RMS for sudden bursts (> ratio_threshold gain) or
    silence (drop from > floor to ~0). Mirrors the C++ side's check_window.
    """
    last = rms.size
    for i in range(max(first_block + 1, 1), last):
        prev = float(rms[i - 1])
        cur = float(rms[i])
        if prev > floor and cur > floor:
            r = cur / prev
            rr = r if r > 1.0 else 1.0 / r
            if rr > ratio_threshold:
                return EnergyAnomaly(True, i, r, "burst" if r > 1.0 else "drop")
        elif prev > floor and cur <= floor * 0.01:
            return EnergyAnomaly(True, i, 0.0, "silence")
    return EnergyAnomaly(False, 0, 1.0, "")
