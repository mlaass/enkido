"""
SF_VOICE — single-voice SoundFont player used as a poly() instrument.

PRD prd-soundfont-poly-unification.md Phase 1. SF_VOICE is the per-voice inner
loop of SOUNDFONT_VOICE (zone lookup, sample interpolation, per-zone SVF
filter, DAHDSR envelope) with NO voice management — poly() owns voices, so a
soundfont voice slots into poly() like any other (freq, gate, vel) instrument.

Expected behavior (per implementation):
- `poly(@, (f,g,v) -> sf_voice(...))` renders stereo, non-silent audio.
- Each pattern note retriggers a fresh voice — distinct note onsets are
  visible as attack transients (regression guard for the retrigger fix).
- A 300 s render (CLAUDE.md ≥300 s rule) is glitch-free: no NaN/Inf, no
  runaway level, no voice/state leak (RMS stays bounded, returns toward the
  noise floor between notes).

If this test fails, check op_sf_voice in
cedar/include/cedar/opcodes/soundfont.hpp and the SF_VOICE codegen handler
handle_sf_voice_call in akkado/src/codegen_patterns.cpp.
"""
import os
import subprocess

import numpy as np
import scipy.io.wavfile
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from cedar_testing import output_dir

OUT = output_dir("op_sf_voice")
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

CLI = os.path.join(REPO, "build/bin/nkido")
SF2 = os.path.join(REPO, "web/static/soundfonts/TimGM6mb.sf3")

# SF_VOICE driven through poly() — the Phase 1 goal line.
PATTERN = (
    'n"c4 e4 g4" |> poly(@, (f,g,v) -> sf_voice("gm", 0, f, g, v)) |> out(@, @)'
)


def render(seconds, name):
    """Render PATTERN via nkido and return (sr, stereo float audio, path)."""
    src_path = os.path.join(OUT, f"{name}.akk")
    with open(src_path, "w") as f:
        f.write(PATTERN + "\n")
    wav_path = os.path.join(OUT, f"{name}.wav")
    cmd = [
        CLI, "render",
        "--soundfont-alias", f"gm={SF2}",
        "--seconds", str(seconds),
        "-o", wav_path,
        src_path,
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print("nkido stderr:", res.stderr)
        raise RuntimeError(f"nkido render failed: {res.returncode}")
    sr, audio = scipy.io.wavfile.read(wav_path)
    if audio.dtype == np.int16:
        audio = audio.astype(np.float32) / 32768.0
    return sr, audio, wav_path


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if x.size else 0.0


def test_stereo_and_onsets():
    """
    Short render: verify stereo output, non-zero per-channel RMS, and at
    least three distinct note onsets (retrigger works through poly).
    """
    print("\n=== SF_VOICE: stereo output + note onsets ===")
    sr, audio, wav_path = render(8.0, "sf_voice_short")

    ok = True

    if audio.ndim != 2 or audio.shape[1] != 2:
        print(f"  ✗ FAIL: expected stereo output, got shape {audio.shape}")
        return False
    print("  ✓ stereo output (2 channels)")

    left, right = audio[:, 0], audio[:, 1]
    l_rms, r_rms = rms(left), rms(right)
    print(f"  L rms={l_rms:.4f}  R rms={r_rms:.4f}")
    if l_rms > 1e-3 and r_rms > 1e-3:
        print("  ✓ both channels non-silent")
    else:
        print("  ✗ FAIL: a channel is silent")
        ok = False

    if np.any(~np.isfinite(audio)):
        print("  ✗ FAIL: NaN/Inf in output")
        ok = False
    else:
        print("  ✓ no NaN/Inf")

    # Onset detection: each note onset is a prominent peak in the smoothed
    # amplitude envelope. find_peaks with a 150 ms minimum spacing and a
    # prominence floor ignores intra-note tremolo/beating. n"c4 e4 g4"
    # alternates one note per cycle, so an 8 s render must contain several
    # distinct onsets — a broken retrigger (one sustained gate) would show
    # essentially a single attack instead.
    import scipy.signal
    mono = audio.mean(axis=1)
    win = max(1, sr // 200)  # 5 ms smoothing
    env = np.convolve(np.abs(mono), np.ones(win) / win, mode="same")
    peak = float(env.max())
    onset_idx, _ = scipy.signal.find_peaks(
        env, distance=int(0.15 * sr), prominence=peak * 0.15)
    onsets = int(onset_idx.size)
    print(f"  detected {onsets} note onsets (env peak={peak:.3f})")
    if onsets >= 3:
        print("  ✓ PASS: ≥3 distinct note onsets — retrigger works through poly")
    else:
        print("  ✗ FAIL: fewer than 3 onsets — retrigger may be broken")
        ok = False

    # Plot for human inspection.
    t = np.arange(mono.size) / sr
    plt.figure(figsize=(12, 4))
    plt.plot(t, mono, lw=0.4, color="steelblue")
    plt.plot(t, env, lw=0.8, color="darkorange", label="envelope")
    plt.plot(onset_idx / sr, env[onset_idx], "rv", ms=6, label="onsets")
    plt.title("SF_VOICE via poly() — n\"c4 e4 g4\"")
    plt.xlabel("time (s)")
    plt.legend()
    plt.tight_layout()
    plt.savefig(os.path.join(OUT, "sf_voice_onsets.png"), dpi=90)
    plt.close()

    print(f"  Saved {wav_path} - Listen for three clean piano notes, "
          f"no clicks at note boundaries.")
    return ok


def test_long_run_stability():
    """
    Long render (≥300 s simulated audio, CLAUDE.md rule): the SF_VOICE/poly
    path must stay glitch-free over a long window — no NaN/Inf, no runaway
    level, no voice/state leak.
    """
    print("\n=== SF_VOICE: 300 s long-run stability ===")
    sr, audio, wav_path = render(300.0, "sf_voice_long")
    mono = audio.mean(axis=1) if audio.ndim == 2 else audio

    ok = True

    if np.any(~np.isfinite(audio)):
        print("  ✗ FAIL: NaN/Inf appeared during the 300 s render")
        return False
    print("  ✓ no NaN/Inf across 300 s")

    peak = float(np.max(np.abs(mono)))
    print(f"  absolute peak = {peak:.3f}")
    if peak <= 1.001:
        print("  ✓ output stays within [-1, 1] — no runaway level")
    else:
        print(f"  ✗ FAIL: output exceeds full scale (peak={peak:.3f})")
        ok = False

    # Compare RMS of the first vs last 10 s — a voice/state leak would make
    # the tail drift markedly louder than the head.
    span = int(10 * sr)
    head_rms = rms(mono[:span])
    tail_rms = rms(mono[-span:])
    print(f"  head rms={head_rms:.4f}  tail rms={tail_rms:.4f}")
    if head_rms > 1e-3 and tail_rms > 1e-3 and tail_rms < head_rms * 3.0:
        print("  ✓ PASS: level stable head-to-tail — no voice/state leak")
    else:
        print("  ✗ FAIL: level drift suggests a voice or state leak")
        ok = False

    print(f"  Saved {wav_path}")
    return ok


if __name__ == "__main__":
    if not os.path.exists(CLI):
        raise SystemExit(f"nkido not built at {CLI} — run cmake --build build")
    if not os.path.exists(SF2):
        raise SystemExit(f"test SoundFont missing at {SF2}")

    results = [
        ("stereo + onsets", test_stereo_and_onsets()),
        ("300 s stability", test_long_run_stability()),
    ]
    print("\n=== SUMMARY ===")
    for name, passed in results:
        print(f"  {'✓ PASS' if passed else '✗ FAIL'}  {name}")
    raise SystemExit(0 if all(p for _, p in results) else 1)
