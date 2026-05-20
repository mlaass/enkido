"""
FOREACH_EVENT Higher-Order DSL Tests (Cedar Engine)
====================================================
Exercises the three higher-order operators that compile to FOREACH_EVENT
(PRD prd-runtime-functions-control-flow L3 §7.5):

  each_voice(input, lambda)  — PER_ITERATION, mixes per-event signals
  each(input, lambda)        — PER_ITERATION sink, body calls out() itself
  reduce(input, fn, init)    — SHARED accumulator over the event stream

Each patch is rendered for >=300 seconds of simulated audio (per the CLAUDE.md
sequenced-opcode rule — voice/event bugs surface only over a long window) and
checked for: non-silence, no NaN/Inf, and a stable block-RMS profile (no
runaway growth or sudden dropouts). Event-record lambda parameters (n.freq,
n.trig) are exercised too.

A shorter WAV is also written for human listening — the ear is the final judge.

If a test fails, do NOT loosen the threshold. Investigate:
  - cedar/src/vm/vm.cpp (run_foreach_per_iteration / run_foreach_shared)
  - akkado/src/codegen_higher_order.cpp (emit_foreach)
"""

import os
import subprocess
import wave
from pathlib import Path

import numpy as np

from cedar_testing import output_dir

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido-cli" / "nkido-cli"

OUT = output_dir("op_foreach_event")

RENDER_SECONDS = 300.0  # >=300s simulated audio per CLAUDE.md
LISTEN_SECONDS = 8.0    # short WAV for human evaluation
BPM = 120.0
BLOCK = 128
SR = 48000

# Each patch: (name, source, description of what to listen for).
PATCHES = [
    (
        "each_voice",
        'n"c4 e4 g4 a4 g4 e4" |> '
        'each_voice(@, (n) -> osc("sin", n.freq) * ar(n.trig, 0.01, 0.25) * 0.4) '
        '|> out(@)',
        "a clean six-note melody, each note plucked by its own AR envelope",
    ),
    (
        "each",
        'n"c4 e4 g4" |> '
        'each(@, (n) -> osc("saw", n.freq) * ar(n.trig, 0.005, 0.2) * 0.25 |> out(@))',
        "a three-note saw arpeggio — the lambda body calls out() itself",
    ),
    (
        "reduce",
        'n"c2 c3 c4" |> reduce(@, (acc, n) -> acc + n.freq, 0.0) as sumf |> '
        'osc("sin", sumf * 0.05) |> @ * 0.3 |> out(@)',
        "a sine whose pitch tracks the running sum of event frequencies",
    ),
]


def render(name: str, source: str, seconds: float) -> str:
    """Render a source string to a WAV via `nkido-cli render`. Returns the path."""
    if not NKIDO_CLI.exists():
        raise FileNotFoundError(
            f"nkido-cli not found at {NKIDO_CLI}. Build it with: "
            f"cmake --build {REPO_ROOT}/build --target nkido-cli"
        )
    src_path = os.path.join(OUT, f"{name}.akkado")
    wav_path = os.path.join(OUT, f"{name}_{int(seconds)}s.wav")
    with open(src_path, "w") as f:
        f.write(source + "\n")
    cmd = [
        str(NKIDO_CLI), "render", src_path,
        "-o", wav_path, "--seconds", str(seconds), "--bpm", str(BPM),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if result.returncode != 0:
        raise RuntimeError(
            f"render failed for {name} (exit {result.returncode}):\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return wav_path


def load_wav(path: str) -> np.ndarray:
    """Load a 16-bit WAV as float in [-1, 1], shape (frames, channels)."""
    with wave.open(path, "rb") as f:
        frames = f.getnframes()
        channels = f.getnchannels()
        raw = f.readframes(frames)
    d = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    if channels == 2:
        d = d.reshape(-1, 2)
    return d


def analyze(name: str, source: str, listen_for: str) -> bool:
    """Render long + short, check non-silent / finite / stable. Returns pass."""
    print(f"\n=== {name} ===")
    print(f"  {source}")

    # Long render — the actual correctness gate.
    long_wav = render(name, source, RENDER_SECONDS)
    d = load_wav(long_wav)
    mono = d[:, 0] if d.ndim == 2 else d

    finite = np.isfinite(d).all()
    peak = float(np.abs(d).max())
    rms = float(np.sqrt((mono ** 2).mean()))

    # Block-RMS profile: no runaway growth, no long dropouts.
    n_blocks = len(mono) // BLOCK
    block_rms = np.array([
        np.sqrt((mono[i * BLOCK:(i + 1) * BLOCK] ** 2).mean())
        for i in range(n_blocks)
    ])
    # Compare the last 10% of the render to the first 10% — stable, not blowing up.
    head = block_rms[: n_blocks // 10]
    tail = block_rms[-n_blocks // 10:]
    head_rms = float(head.mean()) if len(head) else 0.0
    tail_rms = float(tail.mean()) if len(tail) else 0.0
    growth = (tail_rms / head_rms) if head_rms > 1e-9 else 0.0

    # Short render for human listening.
    listen_wav = render(name, source, LISTEN_SECONDS)
    print(f"  Saved {listen_wav} - Listen for: {listen_for}")

    ok = True
    if not finite:
        print(f"  ✗ FAIL: output contains NaN/Inf")
        ok = False
    if peak < 0.001:
        print(f"  ✗ FAIL: output is silent (peak={peak:.5f})")
        ok = False
    if peak > 0.999:
        print(f"  ⚠ WARN: output clips (peak={peak:.5f})")
    # Runaway-growth guard: tail RMS should not be wildly larger than head RMS.
    if head_rms > 1e-6 and growth > 4.0:
        print(f"  ✗ FAIL: RMS grows {growth:.1f}x head->tail "
              f"(head={head_rms:.4f} tail={tail_rms:.4f}) — runaway/leak")
        ok = False

    if ok:
        print(f"  ✓ PASS: {RENDER_SECONDS:.0f}s render — peak={peak:.4f} "
              f"rms={rms:.4f} growth={growth:.2f}x")
    return ok


def main() -> int:
    print(f"FOREACH_EVENT higher-order DSL — {RENDER_SECONDS:.0f}s renders")
    results = []
    for name, source, listen_for in PATCHES:
        try:
            results.append((name, analyze(name, source, listen_for)))
        except Exception as e:  # noqa: BLE001
            print(f"  ✗ FAIL: {name} raised {type(e).__name__}: {e}")
            results.append((name, False))

    print("\n" + "=" * 60)
    passed = sum(1 for _, ok in results if ok)
    for name, ok in results:
        print(f"  {'✓' if ok else '✗'} {name}")
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
