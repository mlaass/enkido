#!/usr/bin/env python3
"""
test_op_when — forward control flow `when()` (PRD prd-runtime-functions-control-flow L1)

`when(cond, true_branch, false_branch)` is block-rate conditional bypass: the
compiler lowers it to SKIP_IF_ZERO / SKIP_IF_NONZERO opcodes so the VM executes
ONLY the taken branch's instructions. Unlike `select()`, the untaken branch is
skipped entirely — its opcodes never run.

Tests (rendered through `nkido-cli render`, the real compile + VM path):

  1. Long-render correctness — a 300 s render with a 0.1 Hz square LFO driving
     the condition. The output must alternate between an audible region (true
     branch) and a silent region (false branch), once per LFO half-period, with
     no NaN/inf anywhere.

  2. Bypass CPU benefit — render the same expensive reverb chain behind
     `when(0, expensive, cheap)` (branch skipped) vs `when(1, expensive, cheap)`
     (branch runs). The skipped render must be substantially faster, proving the
     opcodes are actually bypassed rather than muted.

If a test fails: report the failing window/time. Do NOT shorten the render to
make it pass — investigate the SKIP_IF dispatch in cedar/src/vm/vm.cpp or the
codegen in akkado/src/codegen_control_flow.cpp.
"""

import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import scipy.io.wavfile

from cedar_testing import output_dir

REPO_ROOT = Path(__file__).resolve().parents[1]
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido-cli" / "nkido-cli"
OUT = output_dir("op_when")
SR = 48000


def render(name: str, source: str, seconds: float) -> Path | None:
    """Render `source` to a WAV via nkido-cli. Returns the path or None."""
    wav = Path(OUT) / f"{name}.wav"
    if wav.exists():
        wav.unlink()
    cmd = [
        str(NKIDO_CLI), "render",
        "--source", source,
        "--seconds", str(seconds),
        "--output", str(wav),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if proc.returncode != 0 or not wav.exists():
        print(f"  ✗ render failed for {name}")
        print(f"    stdout: {proc.stdout}")
        print(f"    stderr: {proc.stderr}")
        return None
    return wav


def timed_render(name: str, source: str, seconds: float, runs: int = 3) -> float:
    """Render `runs` times, return the minimum wall-clock time (seconds)."""
    best = float("inf")
    for _ in range(runs):
        wav = Path(OUT) / f"{name}.wav"
        if wav.exists():
            wav.unlink()
        cmd = [
            str(NKIDO_CLI), "render", "--source", source,
            "--seconds", str(seconds), "--output", str(wav),
        ]
        t0 = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        dt = time.perf_counter() - t0
        if proc.returncode != 0:
            print(f"  ✗ timed render failed for {name}: {proc.stderr}")
            return float("inf")
        best = min(best, dt)
    return best


def load_mono(wav_path: Path) -> tuple[int, np.ndarray]:
    sr, data = scipy.io.wavfile.read(wav_path)
    if data.ndim == 2:
        data = data.mean(axis=1)
    data = data.astype(np.float64)
    if np.issubdtype(scipy.io.wavfile.read(wav_path)[1].dtype, np.integer):
        data = data / 32768.0
    return sr, data


# ---------------------------------------------------------------------------
# Test 1 — long-render correctness
# ---------------------------------------------------------------------------

# 0.1 Hz square LFO -> half-period 5 s. ctrl is 1 for 5 s, then 0 for 5 s, ...
# The true branch is a filtered saw; the false branch is stereo silence.
LONG_SOURCE = """\
sig = saw(110)
ctrl = (sqr(0.1) + 1) * 0.5
out(when(ctrl, sig |> lp(@, 1200) |> @ * 0.3, stereo(sig * 0)))
"""
LONG_SECONDS = 300.0
MICRO = 0.25  # classification window, seconds

# A micro-window is unambiguously the true branch if its RMS is near the
# filtered-saw level, and unambiguously the false branch if it is dead silent.
# Windows straddling an LFO edge fall between the two and are ignored — we do
# NOT assume the LFO lands on a fixed grid (low-frequency oscillator phase
# drifts slowly over a 5-minute render; that drift is not a when() bug).
LOUD_RMS = 0.05
SILENT_RMS = 1e-5


def test_long_render():
    """Render 300 s of a toggling when() and verify the bypass alternates."""
    print(f"\n[1] Long render — {LONG_SECONDS:.0f} s toggling when()")
    wav = render("when_long", LONG_SOURCE, LONG_SECONDS)
    if wav is None:
        print("  ✗ FAIL: render failed")
        return False

    sr, audio = load_mono(wav)
    dur = len(audio) / sr
    print(f"  rendered {dur:.1f} s ({len(audio)} samples)")

    if not np.isfinite(audio).all():
        print("  ✗ FAIL: NaN/inf in output")
        return False
    if dur < LONG_SECONDS - 1.0:
        print(f"  ✗ FAIL: short render ({dur:.1f}s < {LONG_SECONDS:.0f}s)")
        return False

    # Classify every 0.25 s window as loud / silent / ambiguous.
    step = int(MICRO * sr)
    states = []  # 'L', 'S', or '?'
    for a in range(0, len(audio) - step, step):
        rms = float(np.sqrt(np.mean(audio[a:a + step] ** 2)))
        states.append("L" if rms > LOUD_RMS else
                       "S" if rms < SILENT_RMS else "?")

    n_loud = states.count("L")
    n_silent = states.count("S")
    # Count contiguous runs of each unambiguous state (ignoring '?').
    runs = []
    for s in states:
        if s == "?":
            continue
        if not runs or runs[-1] != s:
            runs.append(s)
    loud_runs = runs.count("L")
    silent_runs = runs.count("S")
    print(f"  loud windows: {n_loud}   silent windows: {n_silent}")
    print(f"  alternating runs: {loud_runs} loud / {silent_runs} silent")

    ok = True
    if n_loud < 100:
        print("  ✗ FAIL: true branch barely ran")
        ok = False
    if n_silent < 100:
        print("  ✗ FAIL: false branch barely ran")
        ok = False
    if loud_runs < 15 or silent_runs < 15:
        print("  ✗ FAIL: branches did not alternate across the render")
        ok = False
    # The false branch is stereo silence: when selected, output must be
    # *exactly* zero — proving only the false branch ran (no bleed-through
    # from the bypassed true branch). Check a late window so this holds for
    # the whole 300 s, not just the start.
    found_zero_late = False
    for a in range(int(250 * sr), len(audio) - step, step):
        if float(np.max(np.abs(audio[a:a + step]))) == 0.0:
            found_zero_late = True
            break
    if not found_zero_late:
        print("  ✗ FAIL: no bit-exact-silent window after 250 s — "
              "false branch may be leaking the bypassed true branch")
        ok = False

    # Save a short listenable excerpt.
    short = render("when_short", LONG_SOURCE, 12.0)
    if short is not None:
        print(f"  Saved {short} — listen for the saw cutting in/out")

    if ok:
        print(f"  ✓ PASS: branches alternate cleanly over 300 s, "
              f"false branch is bit-exact silent, no NaN/inf")
    return ok


# ---------------------------------------------------------------------------
# Test 2 — bypass CPU benefit
# ---------------------------------------------------------------------------

CPU_SECONDS = 240.0
EXPENSIVE = "sig |> dattorro(@) |> dattorro(@) |> dattorro(@)"
CHEAP = "stereo(sig * 0.2)"
CPU_SKIP = f"sig = saw(110)\nout(when(0, {EXPENSIVE}, {CHEAP}))\n"
CPU_RUN = f"sig = saw(110)\nout(when(1, {EXPENSIVE}, {CHEAP}))\n"


def test_cpu_bypass():
    """when(0, expensive, cheap) must be much cheaper than when(1, ...)."""
    print(f"\n[2] Bypass CPU benefit — {CPU_SECONDS:.0f} s renders, 3 runs each")
    t_run = timed_render("when_cpu_run", CPU_RUN, CPU_SECONDS)
    t_skip = timed_render("when_cpu_skip", CPU_SKIP, CPU_SECONDS)
    if not np.isfinite([t_run, t_skip]).all():
        print("  ✗ FAIL: a timed render failed")
        return False

    reduction = 100.0 * (1.0 - t_skip / t_run)
    print(f"  when(1, expensive, cheap): {t_run:.3f} s  (branch runs)")
    print(f"  when(0, expensive, cheap): {t_skip:.3f} s  (branch skipped)")
    print(f"  CPU reduction when skipped: {reduction:.1f}%")

    # The reverb chain is the dominant cost; skipping it must save a large
    # fraction. Threshold is conservative vs the PRD's 80% target because
    # wall-clock includes fixed compile + I/O overhead.
    if reduction < 50.0:
        print("  ✗ FAIL: skipped branch not substantially cheaper — "
              "SKIP_IF may not be bypassing opcodes")
        return False
    print("  ✓ PASS: skipped branch bypasses the expensive opcodes")
    return True


def main():
    if not NKIDO_CLI.exists():
        print(f"error: nkido-cli not built at {NKIDO_CLI}")
        print(f"  build it: cmake --build {REPO_ROOT}/build --target nkido-cli")
        sys.exit(1)

    results = [test_long_render(), test_cpu_bypass()]
    print()
    if all(results):
        print("ALL TESTS PASSED")
        sys.exit(0)
    print(f"{results.count(False)}/{len(results)} TESTS FAILED")
    sys.exit(1)


if __name__ == "__main__":
    main()
