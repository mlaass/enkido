#!/usr/bin/env python3
"""
test_chord_pipeline.py
======================
Audio-level regression test for the chord-pattern → soundfont pipeline.

Renders the four user-reported pipelines through `nkido-cli render` and
inspects the resulting WAVs to verify that:

  1. `c"CM Am Dm G" |> soundfont(@, ...) |> out(@, @)` produces audible
     chord triads (≥3 distinct spectral peaks per chord, not just a single
     root tone).
  2. `.voicing(...)` does not break the multi-voice path.
  3. `.transpose(0)` preserves all chord voices (not collapsed to root).

If any test fails, the WAVs in `output/chord_pipeline/` are kept for
human review — open them in an audio editor to hear the chord vs. the
collapsed-root single-tone.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import scipy.io.wavfile

REPO_ROOT = Path(__file__).resolve().parents[1]
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido-cli" / "nkido-cli"
SOUNDFONT = REPO_ROOT / "web" / "static" / "soundfonts" / "FluidR3Mono_GM.sf3"
OUT = Path(__file__).resolve().parent / "output" / "chord_pipeline"
OUT.mkdir(parents=True, exist_ok=True)

SECONDS = 8.0
SR = 48000
# At 30 BPM, 1 beat = 2 seconds, default 1 cycle = 4 beats = 8 seconds.
# A 4-chord pattern in one cycle gives 2 s/chord — long enough for FFT
# analysis and to outlive the soundfont sample's attack envelope.
BPM = 30.0


def render(name: str, source: str) -> Path:
    """Render `source` to WAV via nkido-cli. Returns the WAV path."""
    wav = OUT / f"{name}.wav"
    if wav.exists():
        wav.unlink()
    cmd = [
        str(NKIDO_CLI), "render",
        "--source", source,
        "--soundfont", f"file://{SOUNDFONT}",
        "--seconds", str(SECONDS),
        "--bpm", str(BPM),
        "--output", str(wav),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if proc.returncode != 0 or not wav.exists():
        print(f"  ✗ render failed for {name}")
        print(f"    cmd: {' '.join(cmd)}")
        print(f"    stdout: {proc.stdout}")
        print(f"    stderr: {proc.stderr}")
        return None
    return wav


def load_mono(wav_path: Path) -> tuple[int, np.ndarray]:
    sr, data = scipy.io.wavfile.read(wav_path)
    if data.ndim == 2:
        data = data.mean(axis=1)
    if data.dtype.kind == "i":
        data = data.astype(np.float32) / np.iinfo(data.dtype).max
    return sr, data.astype(np.float32)


def chord_window(audio: np.ndarray, sr: int,
                 chord_idx: int, total_chords: int) -> np.ndarray:
    """Slice the chord-N sustain region (skip attack and release tails)."""
    chord_len = len(audio) // total_chords
    start = chord_idx * chord_len + chord_len // 4
    end = chord_idx * chord_len + 3 * chord_len // 4
    return audio[start:end]


def magnitude_at(freq_hz: float, audio_window: np.ndarray, sr: int) -> float:
    """Average FFT magnitude in a narrow band around `freq_hz` (±10 Hz)."""
    n = len(audio_window)
    win = audio_window * np.hanning(n)
    fft = np.fft.rfft(win)
    mag = np.abs(fft) / (n / 2 + 1)
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    band = (freqs >= freq_hz - 10.0) & (freqs <= freq_hz + 10.0)
    if not band.any():
        return 0.0
    return float(mag[band].mean())


def assert_specific_voices(name: str, wav: Path,
                           expected_voices_per_chord: list[list[float]]) -> bool:
    """Verify each named chord-voice fundamental is energetically present.

    `expected_voices_per_chord[i]` is a list of fundamental frequencies that
    must show up as significant energy in chord i's sustain window. The
    threshold is calibrated so that voice fundamentals (driven by note-on)
    pass while harmonic leakage from other notes (much weaker than the
    fundamental) does not.
    """
    sr, audio = load_mono(wav)
    if audio.max() == 0.0:
        print(f"  ✗ {name}: silent render")
        return False

    all_ok = True
    for c, voices in enumerate(expected_voices_per_chord):
        win = chord_window(audio, sr, c, total_chords=len(expected_voices_per_chord))
        peak_voice_mag = max(magnitude_at(v, win, sr) for v in voices)
        # A voice fundamental is "present" if it's at least 25% as loud as
        # the loudest fundamental in this chord. Harmonic leakage from other
        # notes typically lands well below this threshold for the brief
        # sustain windows of soundfont samples.
        threshold = 0.25 * peak_voice_mag
        per_voice = []
        for v in voices:
            m = magnitude_at(v, win, sr)
            present = m >= threshold and m > 1e-5
            per_voice.append((v, m, present))
        chord_ok = all(p for _, _, p in per_voice)
        all_ok = all_ok and chord_ok
        status = "✓" if chord_ok else "✗"
        details = ", ".join(
            f"{int(v)}Hz={'present' if p else 'MISSING'}" for v, _, p in per_voice)
        print(f"  {status} {name} chord{c}: {details}")
    return all_ok


# Chord-voice fundamentals for c"CM Am Dm G". chord_parser anchors every
# root at MIDI octave 4 (akkado/src/chord_parser.cpp:143), so:
#   CM = [60, 64, 67]  = C4 E4 G4
#   Am = [69, 72, 76]  = A4 C5 E5
#   Dm = [62, 65, 69]  = D4 F4 A4
#   G  = [67, 71, 74]  = G4 B4 D5
# These are the frequencies a bare `c"…"` pipeline carries to soundfont
# (no voicing optimizer in the path).
CHORD_VOICES_RAW = [
    [261.626, 329.628, 391.995],   # CM  = C4 E4 G4
    [440.000, 523.251, 659.255],   # Am  = A4 C5 E5
    [293.665, 349.228, 440.000],   # Dm  = D4 F4 A4
    [391.995, 493.883, 587.330],   # G   = G4 B4 D5
]

# When voicing(...) is applied, apply_voicing() runs the events through
# voice_chords() which picks an inversion. With default anchor=c4 (MIDI 60)
# Below mode, the optimizer drops upper voices an octave to fit `hi <= 60`,
# producing a more grounded sound. These are the voicings observed
# empirically — re-derive if voicing.cpp's algorithm changes.
CHORD_VOICES_DEFAULT = [
    [164.814, 195.998, 261.626],   # CM  → [E3 G3 C4]
    [164.814, 220.000, 261.626],   # Am  → [E3 A3 C4]
    [146.832, 174.614, 220.000],   # Dm  → [D3 F3 A3]
    [146.832, 195.998, 246.942],   # G   → [D3 G3 B3]
]


def main() -> int:
    if not NKIDO_CLI.exists():
        print(f"error: nkido-cli not built — run `cmake --build {REPO_ROOT}/build --target nkido-cli`")
        return 2
    if not SOUNDFONT.exists():
        print(f"error: soundfont not found at {SOUNDFONT}")
        return 2

    sf_name = SOUNDFONT.stem  # "FluidR3Mono_GM"

    cases = [
        ("direct",
         f'c"CM Am Dm G" |> soundfont(@, "{sf_name}", 0) |> out(@, @)'),
        ("voicing_close",
         f'c"CM Am Dm G" .voicing("close") |> soundfont(@, "{sf_name}", 0) |> out(@, @)'),
        ("voicing_open",
         f'c"CM Am Dm G" .voicing("open")  |> soundfont(@, "{sf_name}", 0) |> out(@, @)'),
        ("transpose_0",
         f'c"CM Am Dm G" .transpose(0) |> soundfont(@, "{sf_name}", 0) |> out(@, @)'),
        ("note_pattern_baseline",
         f'n"c4 a4 d4 g4" |> soundfont(@, "{sf_name}", 0) |> out(@, @)'),
    ]

    print(f"Rendering {len(cases)} cases through nkido-cli (8s each)...")
    wavs = {}
    for name, src in cases:
        wav = render(name, src)
        if wav is None:
            print(f"FAIL: render failed for {name}")
            return 1
        wavs[name] = wav

    print("\nSpectral analysis:")
    print("  Each named chord-voice fundamental must be ≥25% as loud as the")
    print("  loudest voice in that chord — i.e. all chord notes are sounding,")
    print("  not just the root.\n")

    all_ok = True
    # `direct` and `transpose(0)` go through paths where the chord_parser's
    # raw intervals reach soundfont (no voicing optimizer applied). voicing
    # cases run through apply_voicing which picks an inversion under the
    # default anchor.
    case_voices = {
        "direct":         CHORD_VOICES_RAW,
        "transpose_0":    CHORD_VOICES_RAW,
        "voicing_close":  CHORD_VOICES_DEFAULT,
        "voicing_open":   CHORD_VOICES_DEFAULT,
    }
    for name, voices in case_voices.items():
        ok = assert_specific_voices(name, wavs[name], voices)
        if not ok:
            all_ok = False

    # Sanity: the n"c4 a4 d4 g4" baseline plays one note at a time. For
    # chord 0 (c4 alone), only ~261 Hz should be a strong fundamental;
    # E4 (~330) and G4 (~392) must NOT be present. If they are, the
    # presence detector is too lenient and the chord-pipeline pass
    # results are false positives.
    sr, audio = load_mono(wavs["note_pattern_baseline"])
    win = chord_window(audio, sr, 0, 4)
    c4_mag = magnitude_at(261.626, win, sr)
    e4_mag = magnitude_at(329.628, win, sr)
    g4_mag = magnitude_at(392.0, win, sr)
    e4_ratio = e4_mag / c4_mag if c4_mag > 0 else 0.0
    g4_ratio = g4_mag / c4_mag if c4_mag > 0 else 0.0
    print(f"\n  baseline n\"c4 …\" chord0: c4={c4_mag:.4f} "
          f"e4={e4_mag:.4f}({e4_ratio:.1%}) g4={g4_mag:.4f}({g4_ratio:.1%})")
    if e4_ratio >= 0.25 or g4_ratio >= 0.25:
        print("  ⚠ baseline detector too lenient: chord-pipeline tests may be"
              " false positives.")
        all_ok = False
    else:
        print("  ✓ baseline correctly shows c4 alone, e4/g4 absent.")

    print()
    if all_ok:
        print(f"All chord pipelines audibly polyphonic — WAVs in {OUT}")
        return 0
    else:
        print(f"FAIL — listen to WAVs in {OUT} to hear the bug")
        return 1


if __name__ == "__main__":
    sys.exit(main())
