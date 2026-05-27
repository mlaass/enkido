#!/usr/bin/env python3
"""Render each .akk patch and verify it produces audible output.

Usage:
    uv run python scripts/verify-patches.py web/static/patches/welcome
    uv run python scripts/verify-patches.py web/static/patches/welcome web/static/patches
    uv run python scripts/verify-patches.py --seconds 10 web/static/patches/welcome

A patch passes if the rendered WAV has both peak amplitude >= PEAK_THRESHOLD
AND RMS >= RMS_THRESHOLD over the render window. Patches that compile but
emit silence (e.g. an empty event stream, a missing envelope trigger) are
flagged "QUIET". Patches whose render fails at all are flagged "RENDER".

Exit code 0 on full pass, 1 if any patch fails.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import scipy.io.wavfile as wav

REPO_ROOT = Path(__file__).resolve().parent.parent
NKIDO_CLI = REPO_ROOT / "build" / "debug" / "bin" / "nkido"

PEAK_THRESHOLD = 0.01     # below this = effectively inaudible
RMS_THRESHOLD = 0.0005    # below this = no sustained signal


def render(patch: Path, seconds: float, out: Path, timeout: float) -> tuple[bool, str]:
    proc = subprocess.run(
        [
            str(NKIDO_CLI), "render",
            "--seconds", str(seconds),
            "-o", str(out),
            str(patch),
        ],
        capture_output=True, text=True, timeout=timeout,
    )
    if proc.returncode != 0:
        return False, (proc.stderr or proc.stdout).strip().splitlines()[-1] if (proc.stderr or proc.stdout) else "nonzero exit"
    if not out.exists():
        return False, "no output file"
    return True, ""


def analyze(wav_path: Path) -> tuple[float, float]:
    _, data = wav.read(wav_path)
    if np.issubdtype(data.dtype, np.integer):
        info = np.iinfo(data.dtype)
        data = data.astype(np.float32) / max(abs(info.min), info.max)
    else:
        data = data.astype(np.float32)
    peak = float(np.max(np.abs(data))) if data.size else 0.0
    rms = float(np.sqrt(np.mean(data.astype(np.float64) ** 2))) if data.size else 0.0
    return peak, rms


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dirs", nargs="+", type=Path, help="patch dirs or individual .akk files")
    p.add_argument("--seconds", type=float, default=8.0, help="render duration per patch (default: 8.0)")
    p.add_argument("--timeout", type=float, default=60.0, help="per-patch timeout in seconds")
    p.add_argument("--peak", type=float, default=PEAK_THRESHOLD, help=f"min peak amplitude (default: {PEAK_THRESHOLD})")
    p.add_argument("--rms", type=float, default=RMS_THRESHOLD, help=f"min RMS amplitude (default: {RMS_THRESHOLD})")
    args = p.parse_args()

    if not NKIDO_CLI.exists():
        print(f"nkido not built at {NKIDO_CLI}", file=sys.stderr)
        return 1

    patches: list[Path] = []
    for d in args.dirs:
        if d.is_file():
            patches.append(d)
        elif d.is_dir():
            patches.extend(sorted(d.glob("*.akk")))
        else:
            print(f"not a file or directory: {d}", file=sys.stderr)
            return 1

    if not patches:
        print("no .akk patches found", file=sys.stderr)
        return 1

    fails: list[tuple[str, str]] = []
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        for patch in patches:
            try:
                rel = patch.relative_to(REPO_ROOT)
            except ValueError:
                rel = patch
            out = tdp / (patch.stem + ".wav")
            try:
                ok, err = render(patch, args.seconds, out, args.timeout)
            except subprocess.TimeoutExpired:
                print(f"  TIMEOUT  {rel}")
                fails.append((str(rel), "timeout"))
                continue
            if not ok:
                print(f"  RENDER   {rel}  ({err})")
                fails.append((str(rel), f"render: {err}"))
                continue
            peak, rms = analyze(out)
            if peak < args.peak or rms < args.rms:
                tag = "QUIET    "
                fails.append((str(rel), f"peak={peak:.4f} rms={rms:.4f}"))
            else:
                tag = "OK       "
            print(f"  {tag} peak={peak:.4f}  rms={rms:.4f}  {rel}")

    print()
    if fails:
        print(f"{len(fails)} of {len(patches)} patches failed:")
        for name, why in fails:
            print(f"  - {name}: {why}")
        return 1
    print(f"OK — all {len(patches)} patches produce audible output (peak >= {args.peak}, rms >= {args.rms})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
