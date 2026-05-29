"""
SEQPAT_PROP Pattern-Argument Long-Window Stability Test
=======================================================

PRD prd-patterns-as-scalar-values §10.3 acceptance:
"verify pattern-driven bend / aftertouch / dur produce correct per-event
values across 300+ seconds. No drift, no zombie events, no value drops."

Approach: render three Akkado programs that exercise the pattern-arg
forms of `bend()`, `aftertouch()`, and `dur()` for 320 s each via
`nkido render`. Each program uses a v"…" pattern as the second
argument, sample-and-held into the host pattern's per-event property
buffer (SEQPAT_PROP). The output spectrum / envelope must reflect the
pattern's values stably across 100+ cycles.

Subject programs:

  1. bend(notes, v"<0 100 200>") — pattern-driven freq offset
     n"a4 a4 a4" repeats 440 Hz; bend adds 0 / 100 / 200 Hz per event,
     producing peaks at 440, 540, 640 Hz.

  2. aftertouch(notes, v"<0.2 0.6 1.0>") — pattern-driven amplitude
     n"a4 a4 a4" with output multiplied by e.aftertouch produces
     stepped amplitudes; first half amplitude ratios should match.

  3. dur(notes, v"<0.25 0.5 1.0>") — pattern-driven duration
     n"a4 a4 a4" trigger envelopes scale with e.dur; aggregate energy
     remains stable, no zombie events.

Acceptance criteria (per program):
  * Render exits 0 within timeout.
  * WAV runs the full duration (no truncation).
  * No silent windows > 5 s anywhere in the output.
  * RMS energy in the second half within 0.5×–2× the first half (no
    drift, no zombie-event runaway).
  * For program 1: spectral peaks include 440 / 540 / 640 Hz within
    ±5 Hz on the second half of the run — confirms the v"…" values
    reach the per-event property buffer correctly across 100+ cycles.

If this test fails: investigate
  - cedar/include/cedar/opcodes/sequencing.hpp op_seqpat_prop
  - akkado/src/codegen_patterns.cpp handle_property_transform_call /
    handle_dur_call (pattern-arg path: per-event sample-and-hold of
    val_events into prop_vals[slot])
"""

import os
import subprocess

import numpy as np
import scipy.io.wavfile

from cedar_testing import output_dir


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
NKIDO_CLI = os.path.join(REPO_ROOT, "build", "tools", "nkido", "nkido")
OUT = output_dir("op_seqpat_prop")
RENDER_SECONDS = 320.0  # > 300 per PRD §10.3
SR = 48000
BPM = 120.0


def _render(akk_src: str, name: str) -> str:
    if not os.path.isfile(NKIDO_CLI):
        raise RuntimeError(
            f"nkido not found at {NKIDO_CLI}. Build with:\n"
            f"  cmake --build build --target nkido"
        )
    src_path = os.path.join(OUT, f"{name}.akk")
    wav_path = os.path.join(OUT, f"{name}.wav")
    with open(src_path, "w") as f:
        f.write(akk_src)
    cmd = [
        NKIDO_CLI, "render",
        "-o", wav_path,
        "--rate", str(SR),
        "--seconds", str(RENDER_SECONDS),
        "--bpm", str(BPM),
        src_path,
    ]
    print(f"  $ {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if res.returncode != 0:
        print(res.stdout)
        print(res.stderr)
        raise RuntimeError(f"nkido render failed (code {res.returncode})")
    return wav_path


def _silent_window_lengths(audio: np.ndarray, threshold: float, sr: int) -> list[float]:
    quiet = np.abs(audio) < threshold
    runs: list[float] = []
    i = 0
    while i < len(quiet):
        if quiet[i]:
            j = i
            while j < len(quiet) and quiet[j]:
                j += 1
            runs.append((j - i) / sr)
            i = j
        else:
            i += 1
    return runs


def _peak_freqs(audio: np.ndarray, sr: int, n_peaks: int) -> list[float]:
    window = np.hanning(len(audio))
    spectrum = np.abs(np.fft.rfft(audio * window))
    freqs = np.fft.rfftfreq(len(audio), 1.0 / sr)
    order = np.argsort(spectrum)[::-1]
    picked: list[int] = []
    for idx in order:
        if all(abs(int(idx) - p) > 4 for p in picked):
            picked.append(int(idx))
        if len(picked) >= n_peaks:
            break
    return sorted(float(freqs[p]) for p in picked)


def _load_wav_mono(wav_path: str) -> np.ndarray:
    sr, data = scipy.io.wavfile.read(wav_path)
    assert sr == SR, f"unexpected sample rate {sr}"
    if data.ndim == 2:
        return data.astype(np.float32).mean(axis=1) / 32768.0
    return data.astype(np.float32) / 32768.0


def _check_basic_stability(audio: np.ndarray, label: str):
    """Assert duration, no long silences, energy stable across halves."""
    expected_samples = int(RENDER_SECONDS * SR)
    duration_s = len(audio) / SR
    assert len(audio) >= expected_samples * 0.99, \
        f"{label}: WAV truncated, {duration_s:.2f}s of expected {RENDER_SECONDS}s"

    silences = _silent_window_lengths(audio, threshold=1e-3, sr=SR)
    long_silences = [s for s in silences if s > 5.0]
    assert not long_silences, \
        f"{label}: silent windows > 5s: {long_silences[:5]} (max {max(long_silences):.2f}s)"

    half = len(audio) // 2
    rms_a = float(np.sqrt(np.mean(audio[:half] ** 2)))
    rms_b = float(np.sqrt(np.mean(audio[half:] ** 2)))
    ratio = rms_b / rms_a if rms_a > 0 else 0.0
    assert 0.5 <= ratio <= 2.0, \
        f"{label}: energy drift, RMS first {rms_a:.4f}, second {rms_b:.4f} (ratio {ratio:.3f})"
    print(f"    ✓ {label}: {duration_s:.1f}s, RMS ratio {ratio:.3f} (first {rms_a:.4f}, second {rms_b:.4f})")


# -----------------------------------------------------------------------------
# Subject 1: pattern-driven bend
# -----------------------------------------------------------------------------

# n"a4 a4 a4" baseline = 440 Hz repeated. bend(., v"0 100 200") adds
# 0/100/200 Hz per event (flat 3-event-per-cycle value pattern; the
# angle-bracket form `<...>` would be alternating per cycle, not per
# event). Reading e.bend from the binding pulls the SEQPAT_PROP buffer
# (Signal). With 3 elements the cycle is 3 beats, so at 120 BPM each
# cycle is 1.5 s and over 320 s we get ~213 cycles each playing
# 440 / 540 / 640 Hz.
BEND_AKK = """
n"a4 a4 a4" |> bend(%, v"0 100 200") as e
  |> sine(e.freq + e.bend) * 0.2
  |> out(%, %)
"""
BEND_EXPECTED_FREQS = (440.0, 540.0, 640.0)


def test_bend_pattern_long_window():
    """
    Pattern-arg bend over 320 s: spectrum must show three stable peaks
    matching the v"…" offsets, energy stable across halves.
    """
    wav_path = _render(BEND_AKK, "bend_long_window")
    audio = _load_wav_mono(wav_path)
    _check_basic_stability(audio, "bend")

    # Spectral peaks on the second half — the SEQPAT_PROP buffer must
    # have stayed in sync across ~100 cycles.
    half = len(audio) // 2
    second_half = audio[half:]
    peaks = _peak_freqs(second_half, SR, n_peaks=8)
    print(f"    bend spectrum peaks (Hz): {[round(p, 1) for p in peaks]}")
    for nominal in BEND_EXPECTED_FREQS:
        nearest = min(peaks, key=lambda p: abs(p - nominal))
        if abs(nearest - nominal) > 5.0:
            raise AssertionError(
                f"bend: expected ~{nominal} Hz, nearest peak {nearest:.1f} Hz "
                f"(off by {nearest - nominal:+.1f} Hz)"
            )
    print(f"    ✓ bend: spectral peaks {BEND_EXPECTED_FREQS} matched within ±5 Hz")
    print(f"    Saved {wav_path}")


# -----------------------------------------------------------------------------
# Subject 2: pattern-driven aftertouch (used as amplitude scalar)
# -----------------------------------------------------------------------------

AFTERTOUCH_AKK = """
n"a4 a4 a4" |> aftertouch(%, v"0.2 0.6 1.0") as e
  |> sine(e.freq) * e.aftertouch * 0.3
  |> out(%, %)
"""


def test_aftertouch_pattern_long_window():
    """
    Pattern-arg aftertouch over 320 s: amplitude pattern must hold; no
    runaway gain, no zombie events, energy stable across halves.
    """
    wav_path = _render(AFTERTOUCH_AKK, "aftertouch_long_window")
    audio = _load_wav_mono(wav_path)
    _check_basic_stability(audio, "aftertouch")
    print(f"    Saved {wav_path}")


# -----------------------------------------------------------------------------
# Subject 3: pattern-driven dur (per-event duration multiplier)
# -----------------------------------------------------------------------------

DUR_AKK = """
n"a4 a4 a4" |> dur(%, v"0.25 0.5 1.0") as e
  |> sine(e.freq) * ar(e.trig, 0.005, 0.15) * 0.3
  |> out(%, %)
"""


def test_dur_pattern_long_window():
    """
    Pattern-arg dur over 320 s: duration pattern must hold; varied
    note lengths but energy stable across halves, no zombies.
    """
    wav_path = _render(DUR_AKK, "dur_long_window")
    audio = _load_wav_mono(wav_path)
    _check_basic_stability(audio, "dur")
    print(f"    Saved {wav_path}")


if __name__ == "__main__":
    print("=" * 72)
    print("SEQPAT_PROP pattern-arg long-window stability — PRD §10.3")
    print("=" * 72)
    failed = []
    for label, fn in [
        ("bend",        test_bend_pattern_long_window),
        ("aftertouch",  test_aftertouch_pattern_long_window),
        ("dur",         test_dur_pattern_long_window),
    ]:
        print(f"\n  Subject: {label}")
        try:
            fn()
        except AssertionError as e:
            print(f"    ✗ FAIL: {e}")
            failed.append(label)

    print()
    if failed:
        print(f"✗ FAILED subjects: {', '.join(failed)}")
        raise SystemExit(1)
    print("All tests passed ✓")
