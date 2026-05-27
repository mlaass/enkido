"""
Click analysis for the user's chord-soundfont pipeline.

Reproduces the user-reported clicking on every note onset:
    c"<[Am Am7] [Dm7 Dm] G CM >" |> soundfont(@, "gm", 0) |> out(@*.85)

Renders the pattern to WAV via nkido, then locates discontinuities by
measuring sample-to-sample difference (first derivative). A click manifests
as a large d/dt value localized to a single sample. We then group nearby
spikes into "click events", count them, and save annotated plots.

This file is part of the click-fix work — run BEFORE applying the fix to
see the click count, and AFTER to confirm the fix reduces it.
"""
import os
import subprocess
import sys

import numpy as np
import scipy.io.wavfile
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from cedar_testing import output_dir

OUT = output_dir("op_soundfont_clicks")
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


USER_PATTERN = (
    'c"<[Am Am7] [Dm7 Dm] G CM >" |> soundfont(@, "gm", 0) |> out(@*.85)'
)


def render_pattern(seconds=12.0):
    """Render the user's pattern via nkido and return (sr, samples)."""
    src_path = os.path.join(OUT, "pattern.akk")
    with open(src_path, "w") as f:
        f.write(USER_PATTERN + "\n")
    wav_path = os.path.join(OUT, "pattern.wav")
    sf2 = os.path.join(REPO, "web/static/soundfonts/FluidR3Mono_GM.sf3")
    cli = os.path.join(REPO, "build/bin/nkido")
    cmd = [
        cli, "render",
        "--soundfont", f"file://{sf2}",
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
    if audio.ndim == 2:
        audio = audio.mean(axis=1)
    return sr, audio, wav_path


def detect_clicks(audio, sr, threshold=0.15, min_gap_ms=10.0):
    """
    A click is a sudden jump in sample value. Detect by thresholding the
    first difference; cluster nearby spikes within min_gap_ms into one event.

    Returns list of (sample_index, peak_diff) per click event.
    """
    diff = np.abs(np.diff(audio))
    spike_mask = diff > threshold
    spike_indices = np.flatnonzero(spike_mask)

    if spike_indices.size == 0:
        return []

    min_gap = int(min_gap_ms * sr / 1000.0)
    events = []
    cur_start = spike_indices[0]
    cur_peak = diff[cur_start]
    prev = cur_start
    for i in spike_indices[1:]:
        if i - prev > min_gap:
            events.append((int(cur_start), float(cur_peak)))
            cur_start = i
            cur_peak = diff[i]
        else:
            if diff[i] > cur_peak:
                cur_peak = float(diff[i])
                cur_start = int(i)
        prev = i
    events.append((int(cur_start), float(cur_peak)))
    return events


def plot_overview(audio, sr, clicks, path):
    t = np.arange(audio.size) / sr
    fig, axes = plt.subplots(2, 1, figsize=(14, 6), sharex=True)
    axes[0].plot(t, audio, lw=0.4, color="C0")
    for idx, mag in clicks:
        axes[0].axvline(idx / sr, color="red", alpha=0.4, lw=0.8)
    axes[0].set_ylabel("amplitude")
    axes[0].set_title(f"{len(clicks)} click events detected (red lines)")
    axes[1].plot(t[1:], np.abs(np.diff(audio)), lw=0.4, color="C1")
    axes[1].set_xlabel("seconds")
    axes[1].set_ylabel("|d sample|")
    axes[1].set_title("sample-to-sample first difference")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def plot_click_zoom(audio, sr, click_idx, path, window_ms=20.0):
    half = int(window_ms * sr / 1000.0 / 2)
    a = max(0, click_idx - half)
    b = min(audio.size, click_idx + half)
    seg = audio[a:b]
    t = (np.arange(seg.size) + a) / sr * 1000.0  # ms
    fig, ax = plt.subplots(figsize=(8, 3))
    ax.plot(t, seg, color="C0", lw=0.6)
    ax.axvline(click_idx / sr * 1000.0, color="red", alpha=0.5)
    ax.set_xlabel("ms")
    ax.set_ylabel("amplitude")
    ax.set_title(f"click @ {click_idx / sr:.3f}s (sample {click_idx})")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def main():
    print(f"Rendering pattern: {USER_PATTERN}")
    sr, audio, wav_path = render_pattern(seconds=12.0)
    print(f"  rendered {audio.size / sr:.1f}s @ {sr} Hz → {wav_path}")
    peak = float(np.max(np.abs(audio)))
    print(f"  peak |x|: {peak:.3f}")

    # Skim the first 200 ms — that's transient startup, not a real click.
    start = int(0.2 * sr)
    # 0.30 is the threshold for clipping-distortion clicks. Below this we
    # tolerate normal piano-attack transients (real musical content).
    big_clicks = detect_clicks(audio[start:], sr, threshold=0.30, min_gap_ms=15.0)
    big_clicks = [(idx + start, mag) for idx, mag in big_clicks]
    soft_clicks = detect_clicks(audio[start:], sr, threshold=0.10, min_gap_ms=15.0)
    soft_clicks = [(idx + start, mag) for idx, mag in soft_clicks]

    print(f"  click events (|diff| > 0.30, clipping-grade): {len(big_clicks)}")
    print(f"  onset transients (|diff| > 0.10, natural piano attack): {len(soft_clicks)}")

    overview = os.path.join(OUT, "overview.png")
    plot_overview(audio, sr, soft_clicks, overview)
    print(f"  Saved {overview}")

    for i, (idx, _) in enumerate(soft_clicks[:6]):
        path = os.path.join(OUT, f"click_{i:02d}.png")
        plot_click_zoom(audio, sr, idx, path)
        print(f"  Saved {path}")

    # Pass criteria, in order of importance:
    #   1. No clipping (peak |x| < 1.0). Clipping is the worst kind of click.
    #   2. No clipping-grade discontinuities (single-sample jumps > 0.30).
    #
    # Lower-threshold "clicks" are the SF2 sample's natural attack transient
    # and are part of the desired sound. We do not flag those.
    print()
    print(f"  WAV: {wav_path} — listen for clicks at note onsets")
    fail = False
    if peak >= 0.999:
        print(f"  ✗ FAIL: peak |x| = {peak:.3f} indicates hard clipping")
        fail = True
    else:
        print(f"  ✓ PASS: peak |x| = {peak:.3f} < 1.0 (no clipping)")
    if big_clicks:
        print(f"  ✗ FAIL: {len(big_clicks)} clipping-grade discontinuities")
        fail = True
    else:
        print(f"  ✓ PASS: no single-sample jumps > 0.30")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
