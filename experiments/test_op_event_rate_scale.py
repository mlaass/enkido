"""
EVENT_RATE_SCALE Runtime Rate-Scaling Tests (Cedar Engine)
==========================================================
PRD prd-runtime-event-transforms Phase 3. fast()/slow() lower to a runtime
EVENT_RATE_SCALE opcode that mutates the upstream SequenceState.cycle_length
each block (cycle_length = original / rate). Constants and signal-rate
factors share the same opcode.

Each patch renders ≥300 s of simulated audio (per CLAUDE.md sequenced-opcode
rule — rate-scaling bugs surface only over many cycles). A shorter WAV is
also written for human listening.

If a test fails, do NOT loosen thresholds. Investigate:
  - cedar/include/cedar/opcodes/event_transforms.hpp (op_event_rate_scale)
  - cedar/include/cedar/opcodes/sequence.hpp (RateScaleState)
  - akkado/src/codegen_patterns.cpp (emit_rate_scale_call)
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

OUT = output_dir("op_event_rate_scale")

RENDER_SECONDS = 300.0
LISTEN_SECONDS = 8.0
BPM = 120.0
BLOCK = 128
SR = 48000


def render(name: str, source: str, seconds: float) -> str:
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
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        raise RuntimeError(
            f"render failed for {name} (exit {result.returncode}):\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return wav_path


def load_wav(path: str) -> np.ndarray:
    with wave.open(path, "rb") as f:
        frames = f.getnframes()
        channels = f.getnchannels()
        raw = f.readframes(frames)
    d = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    if channels == 2:
        d = d.reshape(-1, 2)
    return d


def count_onsets(mono: np.ndarray, threshold: float = 0.05,
                 min_gap_samples: int = 200) -> int:
    """Count amplitude-envelope onsets. Crude but stable enough for rate
    checks. min_gap_samples set just below the shortest expected inter-event
    interval at 2x fast (≈3000 samples at 48k/120BPM/4 notes/cycle), so it
    doesn't double-count attack transients but also doesn't merge adjacent
    events."""
    env = np.abs(mono)
    win = 16
    env = np.convolve(env, np.ones(win) / win, mode="same")
    above = env > threshold
    onsets = 0
    last = -10 * min_gap_samples
    for i in range(1, len(above)):
        if above[i] and not above[i - 1] and (i - last) > min_gap_samples:
            onsets += 1
            last = i
    return onsets


def check_finite_nonsilent(name: str, d: np.ndarray) -> bool:
    ok = True
    if not np.isfinite(d).all():
        print(f"  ✗ FAIL: {name} output contains NaN/Inf")
        ok = False
    peak = float(np.abs(d).max())
    if peak < 0.001:
        print(f"  ✗ FAIL: {name} output is silent (peak={peak:.5f})")
        ok = False
    if peak > 0.999:
        print(f"  ⚠ WARN: {name} output clips (peak={peak:.5f})")
    return ok


def test_constant_fast_doubles_event_rate() -> bool:
    """fast(p, 2) should fire approximately 2x as many events per second as
    the unmodified pattern over a long render."""
    print("\n=== fast(p, 2): event rate doubles ===")
    # 4-note pattern. Default cycle = 1 beat (= 0.5 s at 120 BPM).
    base_src = ('n"c4 e4 g4 a4" |> '
                'osc("sin", @.freq) * ar(@.trig, 0.001, 0.01) |> out(@)')
    fast_src = ('n"c4 e4 g4 a4".fast(2) |> '
                'osc("sin", @.freq) * ar(@.trig, 0.001, 0.01) |> out(@)')

    base_wav = render("base_4notes", base_src, RENDER_SECONDS)
    fast_wav = render("fast_2x", fast_src, RENDER_SECONDS)

    db = load_wav(base_wav)
    df = load_wav(fast_wav)
    mb = db[:, 0] if db.ndim == 2 else db
    mf = df[:, 0] if df.ndim == 2 else df

    if not (check_finite_nonsilent("base", db) and
            check_finite_nonsilent("fast", df)):
        return False

    base_onsets = count_onsets(mb)
    fast_onsets = count_onsets(mf)

    print(f"  base onsets: {base_onsets}, fast onsets: {fast_onsets}")
    listen = render("fast_2x_listen", fast_src, LISTEN_SECONDS)
    print(f"  Saved {listen} - Listen for: 4-note sequence at 2x speed")

    if base_onsets < 100:
        print(f"  ✗ FAIL: base onsets {base_onsets} suspiciously low")
        return False

    ratio = fast_onsets / float(base_onsets)
    # 1.85-2.15 covers onset-detection wobble around the 2x target.
    if not (1.85 <= ratio <= 2.15):
        print(f"  ✗ FAIL: fast/base onset ratio {ratio:.3f}, expected ~2.0")
        return False
    print(f"  ✓ PASS: fast/base onset ratio {ratio:.3f}")
    return True


def test_constant_slow_halves_event_rate() -> bool:
    """slow(p, 2) should fire approximately 0.5x events per second."""
    print("\n=== slow(p, 2): event rate halves ===")
    base_src = ('n"c4 e4 g4 a4" |> '
                'osc("sin", @.freq) * ar(@.trig, 0.001, 0.01) |> out(@)')
    slow_src = ('n"c4 e4 g4 a4".slow(2) |> '
                'osc("sin", @.freq) * ar(@.trig, 0.001, 0.01) |> out(@)')

    base_wav = render("base_4notes_slow", base_src, RENDER_SECONDS)
    slow_wav = render("slow_2x", slow_src, RENDER_SECONDS)

    db = load_wav(base_wav)
    ds = load_wav(slow_wav)
    mb = db[:, 0] if db.ndim == 2 else db
    ms = ds[:, 0] if ds.ndim == 2 else ds

    if not (check_finite_nonsilent("base", db) and
            check_finite_nonsilent("slow", ds)):
        return False

    base_onsets = count_onsets(mb)
    slow_onsets = count_onsets(ms)

    print(f"  base onsets: {base_onsets}, slow onsets: {slow_onsets}")
    listen = render("slow_2x_listen", slow_src, LISTEN_SECONDS)
    print(f"  Saved {listen} - Listen for: 4-note sequence at 1/2 speed")

    ratio = slow_onsets / float(base_onsets)
    if not (0.42 <= ratio <= 0.58):
        print(f"  ✗ FAIL: slow/base onset ratio {ratio:.3f}, expected ~0.5")
        return False
    print(f"  ✓ PASS: slow/base onset ratio {ratio:.3f}")
    return True


def test_signal_rate_factor_stable() -> bool:
    """fast(p, signal) must compile + render stably for ≥300s, the new Phase
    3 capability. We don't pin the onset count (signal modulation makes it
    non-deterministic) — just confirm output is finite, non-silent, and the
    RMS envelope doesn't run away."""
    print("\n=== fast(p, signal): renders stably over 300s ===")
    src = ('n"c4 e4 g4 a4".fast(osc("sin", 0.1) * 1.5 + 2) |> '
           'osc("sin", @.freq) * ar(@.trig, 0.001, 0.01) |> out(@)')
    wav = render("fast_signal", src, RENDER_SECONDS)
    d = load_wav(wav)
    mono = d[:, 0] if d.ndim == 2 else d
    if not check_finite_nonsilent("fast_signal", d):
        return False

    n_blocks = len(mono) // BLOCK
    block_rms = np.array([
        np.sqrt((mono[i * BLOCK:(i + 1) * BLOCK] ** 2).mean())
        for i in range(n_blocks)
    ])
    head = block_rms[: max(1, n_blocks // 10)].mean()
    tail = block_rms[-max(1, n_blocks // 10):].mean()
    growth = (tail / head) if head > 1e-6 else 0.0

    listen = render("fast_signal_listen", src, LISTEN_SECONDS)
    print(f"  Saved {listen} - Listen for: 4-note sequence with LFO-modulated speed")

    if head > 1e-6 and growth > 4.0:
        print(f"  ✗ FAIL: RMS grows {growth:.1f}x head->tail")
        return False
    print(f"  ✓ PASS: 300s render stable (growth={growth:.2f}x)")
    return True


def test_composition_fast_slow() -> bool:
    """fast(slow(p, 2), 3) — net 1.5x speed."""
    print("\n=== fast(slow(p, 2), 3): net 1.5x speed ===")
    base_src = ('n"c4 e4 g4 a4" |> '
                'osc("sin", @.freq) * ar(@.trig, 0.001, 0.01) |> out(@)')
    comp_src = ('fast(slow(n"c4 e4 g4 a4", 2), 3) |> '
                'osc("sin", @.freq) * ar(@.trig, 0.001, 0.01) |> out(@)')

    base_wav = render("base_4notes_comp", base_src, RENDER_SECONDS)
    comp_wav = render("comp_fastslow", comp_src, RENDER_SECONDS)

    db = load_wav(base_wav)
    dc = load_wav(comp_wav)
    mb = db[:, 0] if db.ndim == 2 else db
    mc = dc[:, 0] if dc.ndim == 2 else dc

    if not (check_finite_nonsilent("base", db) and
            check_finite_nonsilent("comp", dc)):
        return False

    base_onsets = count_onsets(mb)
    comp_onsets = count_onsets(mc)
    ratio = comp_onsets / float(base_onsets)

    print(f"  base onsets: {base_onsets}, comp onsets: {comp_onsets}, "
          f"ratio: {ratio:.3f}")

    if not (1.35 <= ratio <= 1.65):
        print(f"  ✗ FAIL: fast(slow(_,2),3) onset ratio {ratio:.3f}, "
              f"expected ~1.5")
        return False
    print(f"  ✓ PASS: composition ratio {ratio:.3f}")
    return True


def main():
    print(f"Output directory: {OUT}")
    tests = [
        ("constant fast(p, 2) doubles event rate", test_constant_fast_doubles_event_rate),
        ("constant slow(p, 2) halves event rate", test_constant_slow_halves_event_rate),
        ("signal-rate factor renders stably", test_signal_rate_factor_stable),
        ("composition fast(slow(_, 2), 3)", test_composition_fast_slow),
    ]
    results = []
    for name, fn in tests:
        try:
            results.append((name, fn()))
        except Exception as e:
            print(f"  ✗ ERROR: {name}: {e}")
            results.append((name, False))

    print("\n=== summary ===")
    passed = sum(1 for _, ok in results if ok)
    for name, ok in results:
        mark = "✓" if ok else "✗"
        print(f"  {mark} {name}")
    print(f"\n{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
