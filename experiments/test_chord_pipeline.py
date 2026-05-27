#!/usr/bin/env python3
"""
test_chord_pipeline.py
======================
Audio-level regression test for the chord-pattern → soundfont pipeline.

Renders the four user-reported pipelines through `nkido render` and
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
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido" / "nkido"
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
    """Render `source` to WAV via nkido. Returns the WAV path."""
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


def count_distinct_pitch_classes(audio_window: np.ndarray, sr: int,
                                  min_freq: float = 80.0,
                                  max_freq: float = 4000.0,
                                  rel_threshold: float = 0.20) -> tuple[int, set[int]]:
    """Count distinct pitch classes that have a spectral peak above the
    relative threshold. Used to verify a chord has N voices without needing
    to predict exactly which octave each one lands in.

    Returns (count, pitch_class_set).
    """
    n = len(audio_window)
    win = audio_window * np.hanning(n)
    fft = np.fft.rfft(win)
    mag = np.abs(fft) / (n / 2 + 1)
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    # Mask to musical band
    band = (freqs >= min_freq) & (freqs <= max_freq)
    if not band.any() or mag[band].max() == 0:
        return 0, set()
    threshold = rel_threshold * mag[band].max()

    # Local-peak detection: a bin is a peak if it exceeds both neighbors and
    # crosses the threshold.
    peaks: list[int] = []
    for i in range(1, len(mag) - 1):
        if not band[i]:
            continue
        if mag[i] > threshold and mag[i] > mag[i - 1] and mag[i] > mag[i + 1]:
            peaks.append(i)

    # Map each peak's frequency to a MIDI note → pitch class.
    pcs: set[int] = set()
    for p in peaks:
        f = freqs[p]
        if f <= 0:
            continue
        midi = 69 + 12 * np.log2(f / 440.0)
        pc = int(round(midi)) % 12
        pcs.add(pc)
    return len(pcs), pcs


def assert_distinct_pcs(name: str, wav: Path,
                         expected_pcs_per_chord: list[set[int]],
                         min_count_per_chord: list[int]) -> bool:
    """Verify each chord has at least `min_count` distinct pitch classes
    present, drawn from the expected set. PC-based assertion sidesteps the
    optimizer's octave choices — we don't need to predict whether voice 4
    lands in octave 3 or 4.
    """
    sr, audio = load_mono(wav)
    if audio.max() == 0.0:
        print(f"  ✗ {name}: silent render")
        return False

    all_ok = True
    for c, (expected, min_count) in enumerate(
            zip(expected_pcs_per_chord, min_count_per_chord)):
        win = chord_window(audio, sr, c, total_chords=len(expected_pcs_per_chord))
        count, found_pcs = count_distinct_pitch_classes(win, sr)
        # Restrict to the expected pitch classes (filter out spurious peaks).
        matched = found_pcs & expected
        chord_ok = len(matched) >= min_count
        all_ok = all_ok and chord_ok
        status = "✓" if chord_ok else "✗"
        # Render PCs as note names for human readability.
        pc_names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
        matched_str = ",".join(pc_names[p] for p in sorted(matched))
        expected_str = ",".join(pc_names[p] for p in sorted(expected))
        print(f"  {status} {name} chord{c}: {len(matched)}/{min_count} expected PCs "
              f"({matched_str}) of {{{expected_str}}}")
    return all_ok


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

def main() -> int:
    if not NKIDO_CLI.exists():
        print(f"error: nkido not built — run `cmake --build {REPO_ROOT}/build --target nkido`")
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
        ("custom_dict_jazz5",
         # addVoicings({M:[0,4,7,11,14], m:[0,3,7,10,14]}) — 5-note dict.
         # Should expand CM/Am/Dm to 5-voice chords; G (empty quality) falls
         # back to the chord parser's intrinsic 3-voice triad.
         f'addVoicings("jazz5", {{M: [0, 4, 7, 11, 14], m: [0, 3, 7, 10, 14]}})\n'
         f'c"CM Am Dm G" .voicing("jazz5") |> soundfont(@, "{sf_name}", 0) |> out(@, @)'),
        ("note_pattern_baseline",
         f'n"c4 a4 d4 g4" |> soundfont(@, "{sf_name}", 0) |> out(@, @)'),
    ]

    print(f"Rendering {len(cases)} cases through nkido (8s each)...")
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
    # `direct` and `transpose_0` go through paths where the chord_parser's
    # raw intervals reach soundfont (no voicing optimizer applied) — exact
    # frequencies are stable. The voicing cases use PC-based checks below
    # because the algorithm's octave choices are not part of the user-visible
    # contract (it's free to evolve to better-sounding voicings).
    case_voices = {
        "direct":         CHORD_VOICES_RAW,
        "transpose_0":    CHORD_VOICES_RAW,
    }
    for name, voices in case_voices.items():
        ok = assert_specific_voices(name, wavs[name], voices)
        if not ok:
            all_ok = False

    # voicing_close / voicing_open: verify each chord's pitch classes are
    # present (regardless of octave). Specific-frequency checks would over-
    # specify the voicing optimizer's output and break on every algorithm
    # tweak. The close-vs-open spectral comparison below confirms they
    # actually differ.
    voicing_pcs = [
        {0, 4, 7},      # CM = {C, E, G}
        {9, 0, 4},      # Am = {A, C, E}
        {2, 5, 9},      # Dm = {D, F, A}
        {7, 11, 2},     # G  = {G, B, D}
    ]
    voicing_min = [3, 3, 3, 3]
    for name in ("voicing_close", "voicing_open"):
        if not assert_distinct_pcs(name, wavs[name], voicing_pcs, voicing_min):
            all_ok = False

    # custom_dict_jazz5: dict overrides expand CM/Am/Dm to 5-note voicings,
    # G falls back to the 3-note major triad. Pitch classes (regardless of
    # octave) must include all dict-defined intervals, so we don't need to
    # predict which inversion/octave the optimizer chooses.
    #
    #   CM with M=[0,4,7,11,14] → PCs {C,E,G,B,D} = {0,4,7,11,2}
    #   Am with m=[0,3,7,10,14] (root=A=9) → PCs {A,C,E,G,B} = {9,0,4,7,11}
    #   Dm with m=[0,3,7,10,14] (root=D=2) → PCs {D,F,A,C,E} = {2,5,9,0,4}
    #   G  (empty quality, intrinsic [0,4,7], root=G=7) → PCs {G,B,D} = {7,11,2}
    print("\nCustom dict (jazz5) chord-voice expansion:")
    jazz5_pcs = [
        {0, 4, 7, 11, 2},   # CM with [0,4,7,11,14]
        {9, 0, 4, 7, 11},   # Am with [0,3,7,10,14]
        {2, 5, 9, 0, 4},    # Dm with [0,3,7,10,14]
        {7, 11, 2},         # G fallback to triad
    ]
    jazz5_min = [4, 4, 4, 2]  # tolerance for octave-fold collisions
    if not assert_distinct_pcs("custom_dict_jazz5", wavs["custom_dict_jazz5"],
                                jazz5_pcs, jazz5_min):
        all_ok = False

    # close vs open spectral signature — they must be audibly different.
    # open's bass-drop transform (apply_builtin runs after inversion now)
    # pushes energy into a lower band than close. We compare the ratio of
    # sub-bass (40–150 Hz) vs mid-band (150–800 Hz) energy: open should
    # have more sub-bass relative to mid-band than close.
    sr_c, audio_c = load_mono(wavs["voicing_close"])
    sr_o, audio_o = load_mono(wavs["voicing_open"])

    def band_ratio(audio: np.ndarray, sr: int) -> float:
        n = len(audio)
        win = audio * np.hanning(n)
        fft = np.fft.rfft(win)
        mag2 = (np.abs(fft) ** 2)
        freqs = np.fft.rfftfreq(n, d=1.0 / sr)
        sub = mag2[(freqs >= 40.0) & (freqs <= 150.0)].sum()
        mid = mag2[(freqs >= 150.0) & (freqs <= 800.0)].sum()
        return sub / mid if mid > 0 else 0.0

    close_ratio = band_ratio(audio_c, sr_c)
    open_ratio = band_ratio(audio_o, sr_o)
    print(f"\nClose vs open spectral signature:")
    print(f"  close sub/mid ratio = {close_ratio:.3f}")
    print(f"  open  sub/mid ratio = {open_ratio:.3f}")
    if open_ratio > close_ratio * 1.2:
        print(f"  ✓ open has ≥20% more sub-bass relative to mid — voicings differ")
    else:
        print(f"  ✗ close and open look spectrally similar — fix may have regressed")
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
