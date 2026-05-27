#!/usr/bin/env python3
"""
Regression: top-level `expr as name` followed by `name |> f(@)` used to
silently rewrite the second statement into a never-called closure,
producing silent audio. The visualizations.akk patch was the public
symptom (initially misdiagnosed as visualizer opcodes "eating" the
signal); the actual fix lives in akkado/src/analyzer.cpp.

This test renders three patches through `nkido render` and asserts
each produces real audio (peak > 0.05). Run with:

    cd experiments && uv run python test_visualizations_silence.py

Or directly:

    python3 experiments/test_visualizations_silence.py
"""

import os
import subprocess
import sys
import tempfile
import wave

import numpy as np


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NKIDO_CLI = os.path.join(REPO_ROOT, "build", "tools", "nkido", "nkido")
VIZ_PATCH = os.path.join(REPO_ROOT, "web", "static", "patches", "visualizations.akk")

MINIMAL_REPRO = """\
saw(220) * 0.3 as x
x |> out(@)
"""

MID_COMPLEXITY = """\
notes = n"c4 e4 g4"
notes |> saw(@.freq) * ar(trigger(1), 0.002, 0.18) |> lp(@, 2400) as v
v |> @ + freeverb(@, 0.5, 0.5) * 0.4 |> out(@)
"""


def render(source_or_path: str, *, is_path: bool, seconds: float = 2.0) -> np.ndarray:
    if is_path:
        patch_path = source_or_path
        wav_path = tempfile.mktemp(suffix=".wav")
    else:
        patch_path = tempfile.mktemp(suffix=".akk")
        with open(patch_path, "w") as f:
            f.write(source_or_path)
        wav_path = tempfile.mktemp(suffix=".wav")

    try:
        result = subprocess.run(
            [NKIDO_CLI, "render", patch_path, "-o", wav_path,
             "--seconds", str(seconds)],
            capture_output=True, text=True, timeout=30,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"nkido render failed (rc={result.returncode}):\n"
                f"stderr: {result.stderr}\nstdout: {result.stdout}"
            )

        with wave.open(wav_path, "rb") as w:
            frames = w.readframes(w.getnframes())
            arr = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
        return arr
    finally:
        for p in (patch_path, wav_path):
            if not is_path and os.path.exists(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
        if not is_path or wav_path != source_or_path:
            if os.path.exists(wav_path):
                try:
                    os.remove(wav_path)
                except OSError:
                    pass


def assert_audible(name: str, samples: np.ndarray, threshold: float = 0.05) -> bool:
    peak = float(np.max(np.abs(samples)))
    rms = float(np.sqrt(np.mean(samples ** 2)))
    if peak >= threshold:
        print(f"  PASS [{name}]: peak={peak:.4f}  rms={rms:.4f}")
        return True
    print(f"  FAIL [{name}]: peak={peak:.4f}  rms={rms:.4f} "
          f"(expected peak >= {threshold}) — "
          f"top-level `as` binding silently dropped to closure?")
    return False


def main() -> int:
    if not os.path.exists(NKIDO_CLI):
        print(f"ERROR: nkido not built at {NKIDO_CLI}")
        print("Build first: cmake --build build --target nkido")
        return 2

    if not os.path.exists(VIZ_PATCH):
        print(f"ERROR: visualizations.akk missing at {VIZ_PATCH}")
        return 2

    results = []
    print("Rendering minimal `as` repro …")
    results.append(assert_audible("minimal_as", render(MINIMAL_REPRO, is_path=False)))

    print("Rendering mid-complexity `as v` chain …")
    results.append(assert_audible("mid_complexity_as", render(MID_COMPLEXITY, is_path=False)))

    print("Rendering visualizations.akk …")
    results.append(assert_audible("visualizations.akk",
                                  render(VIZ_PATCH, is_path=True, seconds=4.0)))

    ok = all(results)
    print("\n" + ("ALL PASS" if ok else f"FAILED {results.count(False)}/{len(results)}"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
