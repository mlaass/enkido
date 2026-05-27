#!/usr/bin/env python3
"""
test_op_loop — bounded static iteration `loop()` (PRD prd-runtime-functions-control-flow L1)

`loop(N) { body }` lowers to the LOOP_STATIC opcode: the VM re-runs the body's
instructions N times in sequence, threading the accumulator (`@` is the running
value — iteration k+1's `@` is iteration k's output). N is a compile-time
constant.

Tests (rendered through `nkido render`, the real compile + VM path):

  1. Long-render stability — a 300 s render of a pattern-driven oscillator
     through a `loop(4)` gain chain. Output must stay finite and bounded for
     the whole render, with no NaN/inf and no silent stretches (the pattern
     keeps re-triggering).

  2. Iteration count honored — `loop(4) { @ * 0.85 }` must attenuate the seed
     by 0.85^4, and `loop(0) { ... }` must leave it untouched. The measured
     peak ratio pins down that the body ran exactly N times.

If a test fails: report the failing block/time. Do NOT shorten the render to
make it pass — investigate LOOP_STATIC dispatch in cedar/src/vm/vm.cpp or the
codegen in akkado/src/codegen_control_flow.cpp.
"""

import subprocess
import sys
from pathlib import Path

import numpy as np
import scipy.io.wavfile

from cedar_testing import output_dir

REPO_ROOT = Path(__file__).resolve().parents[1]
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido" / "nkido"
OUT = output_dir("op_loop")


def render(name: str, source: str, seconds: float) -> Path | None:
    wav = Path(OUT) / f"{name}.wav"
    if wav.exists():
        wav.unlink()
    cmd = [
        str(NKIDO_CLI), "render", "--source", source,
        "--seconds", str(seconds), "--output", str(wav),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if proc.returncode != 0 or not wav.exists():
        print(f"  ✗ render failed for {name}")
        print(f"    stdout: {proc.stdout}")
        print(f"    stderr: {proc.stderr}")
        return None
    return wav


def load_mono(wav_path: Path) -> tuple[int, np.ndarray]:
    sr, raw = scipy.io.wavfile.read(wav_path)
    data = raw.mean(axis=1) if raw.ndim == 2 else raw
    data = data.astype(np.float64)
    if np.issubdtype(raw.dtype, np.integer):
        data = data / 32768.0
    return sr, data


# A pattern-driven saw through a 4-stage static gain loop. The pattern keeps
# the output moving so a 300 s render genuinely exercises time variation.
LONG_SOURCE = 'n"c3 e3 g3 c4 g3 e3" as p |> saw(p.freq) |> loop(4) { @ * 0.85 } |> out(@)'
LONG_SECONDS = 300.0


def test_long_render():
    """300 s render of a loop() chain must stay finite, bounded, non-silent."""
    print(f"\n[1] Long render — {LONG_SECONDS:.0f} s of a loop() gain chain")
    wav = render("loop_long", LONG_SOURCE, LONG_SECONDS)
    if wav is None:
        print("  ✗ FAIL: render failed")
        return False

    sr, audio = load_mono(wav)
    dur = len(audio) / sr
    print(f"  rendered {dur:.1f} s ({len(audio)} samples)")

    ok = True
    if not np.isfinite(audio).all():
        print("  ✗ FAIL: NaN/inf in output")
        ok = False
    if dur < LONG_SECONDS - 1.0:
        print(f"  ✗ FAIL: short render ({dur:.1f}s < {LONG_SECONDS:.0f}s)")
        ok = False

    peak = float(np.max(np.abs(audio)))
    print(f"  peak amplitude: {peak:.4f}")
    if peak > 1.5:
        print("  ✗ FAIL: output blew up — shared loop state may be unstable")
        ok = False

    # No silent stretch longer than 2 s — the pattern keeps re-triggering.
    win = int(2.0 * sr)
    worst = 1.0
    for a in range(0, len(audio) - win, win):
        rms = float(np.sqrt(np.mean(audio[a:a + win] ** 2)))
        worst = min(worst, rms)
    print(f"  quietest 2 s window RMS: {worst:.4f}")
    if worst < 1e-3:
        print("  ✗ FAIL: a 2 s window is silent — loop output dropped out")
        ok = False

    short = render("loop_short", LONG_SOURCE, 12.0)
    if short is not None:
        print(f"  Saved {short} — listen for a steady arpeggiated saw")

    if ok:
        print("  ✓ PASS: 300 s render stays finite, bounded, and non-silent")
    return ok


def test_iteration_count():
    """loop(4){@*0.85} attenuates by 0.85^4; loop(0) leaves the seed alone."""
    print("\n[2] Iteration count honored")
    base = 'n"c3 e3 g3 c4" as p |> saw(p.freq)'
    w4 = render("loop_n4", f'{base} |> loop(4) {{ @ * 0.85 }} |> out(@)', 8.0)
    w0 = render("loop_n0", f'{base} |> loop(0) {{ @ * 0.85 }} |> out(@)', 8.0)
    if w4 is None or w0 is None:
        print("  ✗ FAIL: a render failed")
        return False

    _, a4 = load_mono(w4)
    _, a0 = load_mono(w0)
    peak4, peak0 = float(np.max(np.abs(a4))), float(np.max(np.abs(a0)))
    if peak0 < 1e-3:
        print("  ✗ FAIL: loop(0) reference is silent")
        return False

    ratio = peak4 / peak0
    expected = 0.85 ** 4
    print(f"  loop(0) peak = {peak0:.4f}   loop(4) peak = {peak4:.4f}")
    print(f"  ratio = {ratio:.4f}   expected 0.85^4 = {expected:.4f}")

    if abs(ratio - expected) > 0.03:
        print("  ✗ FAIL: ratio off — loop body did not run exactly 4 times")
        return False
    print("  ✓ PASS: loop body ran exactly N times")
    return True


def main():
    if not NKIDO_CLI.exists():
        print(f"error: nkido not built at {NKIDO_CLI}")
        print(f"  build it: cmake --build {REPO_ROOT}/build --target nkido")
        sys.exit(1)

    results = [test_long_render(), test_iteration_count()]
    print()
    if all(results):
        print("ALL TESTS PASSED")
        sys.exit(0)
    print(f"{results.count(False)}/{len(results)} TESTS FAILED")
    sys.exit(1)


if __name__ == "__main__":
    main()
