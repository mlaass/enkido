"""
EVENT_MAP / EVENT_FILTER Runtime Event-Transform Tests (Cedar Engine)
=====================================================================
Exercises both the Phase-1 packed EVENT_MAP path (transpose / velocity
modifiers) and the Phase-2a closure EVENT_MAP / EVENT_FILTER path (the
event_map / event_filter closure-taking builtins) end-to-end through the
real VM, with ≥300 s rendered audio per CLAUDE.md.

Each patch is rendered for >=300 seconds of simulated audio (per the CLAUDE.md
sequenced-opcode rule — pattern/transform bugs surface only over a long
window) and checked for: non-silence, no NaN/Inf, a stable block-RMS profile,
and — for the transpose patches — the correct dominant frequency via FFT.

A shorter WAV is also written for human listening — the ear is the final judge.

If a test fails, do NOT loosen the threshold. Investigate:
  - cedar/include/cedar/opcodes/event_transforms.hpp (op_event_map)
  - akkado/src/codegen_patterns.cpp (compile_pattern_query_only / readout)
"""

import os
import subprocess
import wave
from pathlib import Path

import numpy as np

from cedar_testing import output_dir

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido" / "nkido"

OUT = output_dir("op_event_map")

RENDER_SECONDS = 300.0  # >=300s simulated audio per CLAUDE.md
LISTEN_SECONDS = 8.0    # short WAV for human evaluation
BPM = 120.0
BLOCK = 128
SR = 48000

# c4 = MIDI 60.
C4_HZ = 261.6256
# Equal-tempered ratios.
OCTAVE = 2.0
FIFTH = 2.0 ** (7.0 / 12.0)


def render(name: str, source: str, seconds: float) -> str:
    """Render a source string to a WAV via `nkido render`. Returns the path."""
    if not NKIDO_CLI.exists():
        raise FileNotFoundError(
            f"nkido not found at {NKIDO_CLI}. Build it with: "
            f"cmake --build {REPO_ROOT}/build --target nkido"
        )
    src_path = os.path.join(OUT, f"{name}.akkado")
    wav_path = os.path.join(OUT, f"{name}_{int(seconds)}s.wav")
    with open(src_path, "w") as f:
        f.write(source + "\n")
    cmd = [
        str(NKIDO_CLI), "render", src_path,
        "-o", wav_path, "--seconds", str(seconds), "--bpm", str(BPM),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if result.returncode != 0:
        raise RuntimeError(
            f"render failed for {name} (exit {result.returncode}):\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return wav_path


def load_wav(path: str) -> np.ndarray:
    """Load a 16-bit WAV as float in [-1, 1], shape (frames,) or (frames, 2)."""
    with wave.open(path, "rb") as f:
        frames = f.getnframes()
        channels = f.getnchannels()
        raw = f.readframes(frames)
    d = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    if channels == 2:
        d = d.reshape(-1, 2)
    return d


def dominant_hz(mono: np.ndarray) -> float:
    """Dominant frequency of a 1s slice taken from the middle of the signal."""
    mid = len(mono) // 2
    win = mono[mid:mid + SR]
    if len(win) < SR:
        win = mono[:SR]
    spec = np.abs(np.fft.rfft(win * np.hanning(len(win))))
    freqs = np.fft.rfftfreq(len(win), 1.0 / SR)
    return float(freqs[int(np.argmax(spec))])


def block_rms_profile(mono: np.ndarray):
    n_blocks = len(mono) // BLOCK
    block_rms = np.array([
        np.sqrt((mono[i * BLOCK:(i + 1) * BLOCK] ** 2).mean())
        for i in range(n_blocks)
    ])
    head = block_rms[: max(1, n_blocks // 10)]
    tail = block_rms[-max(1, n_blocks // 10):]
    return float(head.mean()), float(tail.mean())


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


def test_transpose_frequency() -> bool:
    """transpose() must shift the rendered fundamental by the exact ratio."""
    print("\n=== transpose: dominant frequency ===")
    ok = True
    cases = [
        ("ref",       'n"c4" |> sine(@.freq) |> out(@)',           C4_HZ),
        ("octave",    'n"c4".transpose(12) |> sine(@.freq) |> out(@)',
                                                                  C4_HZ * OCTAVE),
        ("fifth",     'n"c4".transpose(7) |> sine(@.freq) |> out(@)',
                                                                  C4_HZ * FIFTH),
        ("down_oct",  'n"c4".transpose(-12) |> sine(@.freq) |> out(@)',
                                                                  C4_HZ / OCTAVE),
    ]
    for name, src, expect_hz in cases:
        wav = render(f"transpose_{name}", src, RENDER_SECONDS)
        d = load_wav(wav)
        mono = d[:, 0] if d.ndim == 2 else d
        if not check_finite_nonsilent(name, d):
            ok = False
            continue
        got = dominant_hz(mono)
        # 3 Hz tolerance covers FFT bin resolution + master-bus soft-clip skew.
        if abs(got - expect_hz) > 3.0:
            print(f"  ✗ FAIL: {name} dominant {got:.1f} Hz, "
                  f"expected {expect_hz:.1f} Hz")
            ok = False
        else:
            print(f"  ✓ PASS: {name} dominant {got:.1f} Hz "
                  f"(expected {expect_hz:.1f} Hz)")
    listen = render("transpose_octave_listen",
                    'n"c4 e4 g4 a4".transpose(12) |> sine(@.freq) |> out(@)',
                    LISTEN_SECONDS)
    print(f"  Saved {listen} - Listen for: a 4-note melody an octave above c4")
    return ok


def test_velocity_scaling() -> bool:
    """velocity() must scale rendered amplitude by the multiplier."""
    print("\n=== velocity: amplitude scaling ===")
    # Keep both renders well under the master-bus soft-clip threshold (0.9)
    # so the RMS ratio reflects velocity() alone, not clipping compression.
    full = render("velocity_full",
                   'n"c4" |> sine(@.freq) * @.vel * 0.4 |> out(@)',
                   RENDER_SECONDS)
    half = render("velocity_half",
                   'n"c4".velocity(0.5) |> sine(@.freq) * @.vel * 0.4 |> out(@)',
                   RENDER_SECONDS)
    df = load_wav(full)
    dh = load_wav(half)
    if not (check_finite_nonsilent("full", df) and
            check_finite_nonsilent("half", dh)):
        return False
    mf = df[:, 0] if df.ndim == 2 else df
    mh = dh[:, 0] if dh.ndim == 2 else dh
    rms_f = float(np.sqrt((mf ** 2).mean()))
    rms_h = float(np.sqrt((mh ** 2).mean()))
    ratio = rms_h / rms_f if rms_f > 1e-9 else 0.0
    if abs(ratio - 0.5) > 0.05:
        print(f"  ✗ FAIL: velocity(0.5) RMS ratio {ratio:.3f}, expected ~0.5")
        return False
    print(f"  ✓ PASS: velocity(0.5) RMS ratio {ratio:.3f} (expected ~0.5)")
    return True


def test_chained_stability() -> bool:
    """A chained velocity().transpose() must render stably over 300s."""
    print("\n=== chained velocity().transpose(): stability ===")
    src = ('n"c4 e4 g4 a4 g4 e4".velocity(0.8).transpose(7) |> '
           'saw(@.freq) * @.vel * 0.3 |> out(@)')
    wav = render("chained", src, RENDER_SECONDS)
    d = load_wav(wav)
    mono = d[:, 0] if d.ndim == 2 else d
    if not check_finite_nonsilent("chained", d):
        return False
    head_rms, tail_rms = block_rms_profile(mono)
    growth = (tail_rms / head_rms) if head_rms > 1e-6 else 0.0
    listen = render("chained_listen", src, LISTEN_SECONDS)
    print(f"  Saved {listen} - Listen for: a 6-note saw line, a fifth up, "
          f"attenuated")
    if head_rms > 1e-6 and growth > 4.0:
        print(f"  ✗ FAIL: RMS grows {growth:.1f}x head->tail — runaway/leak")
        return False
    print(f"  ✓ PASS: {RENDER_SECONDS:.0f}s render stable "
          f"(growth={growth:.2f}x)")
    return True


def test_event_map_closure_frequency() -> bool:
    """Closure event_map((e) -> {note: e.note + N}) must shift the dominant
    frequency by the same ratio as the Phase-1 transpose() modifier, proving
    the closure subprogram dispatches correctly every event."""
    print("\n=== event_map closure: dominant frequency ===")
    ok = True
    cases = [
        ("octave",   'event_map(n"c4", (e) -> {note: e.note + 12}) |> '
                     'sine(@.freq) |> out(@)',           C4_HZ * OCTAVE),
        ("fifth",    'event_map(n"c4", (e) -> {note: e.note + 7}) |> '
                     'sine(@.freq) |> out(@)',           C4_HZ * FIFTH),
        ("down_oct", 'event_map(n"c4", (e) -> {note: e.note - 12}) |> '
                     'sine(@.freq) |> out(@)',           C4_HZ / OCTAVE),
    ]
    for name, src, expect_hz in cases:
        wav = render(f"closure_{name}", src, RENDER_SECONDS)
        d = load_wav(wav)
        mono = d[:, 0] if d.ndim == 2 else d
        if not check_finite_nonsilent(name, d):
            ok = False
            continue
        got = dominant_hz(mono)
        if abs(got - expect_hz) > 3.0:
            print(f"  ✗ FAIL: {name} dominant {got:.1f} Hz, "
                  f"expected {expect_hz:.1f} Hz")
            ok = False
        else:
            print(f"  ✓ PASS: {name} dominant {got:.1f} Hz "
                  f"(expected {expect_hz:.1f} Hz)")
    listen = render("closure_chord_listen",
                    'event_map(c"CM", (e) -> {note: e.note + 12}) |> '
                    'poly(@, (f, g, v) -> sine(f) * v * 0.3, 6) |> out(@)',
                    LISTEN_SECONDS)
    print(f"  Saved {listen} - Listen for: a C major chord, one octave up")
    return ok


def test_event_filter_drops_events() -> bool:
    """event_filter((e) -> e.note > N) must keep only events whose note
    field passes the predicate — verified by FFT energy at the kept pitches
    and silence near the dropped ones."""
    print("\n=== event_filter: predicate drops events ===")
    # n"c4 e4 g4" plays one note per cycle (top-level alternation). Filter
    # to e.note > 63 keeps e4(64) and g4(67) and drops c4(60). After 300 s
    # the spectrum must concentrate at the kept frequencies, not at c4.
    src = ('event_filter(n"c4 e4 g4", (e) -> e.note > 63) |> '
           'sine(@.freq) * @.vel * 0.3 |> out(@)')
    wav = render("filter_keep_high", src, RENDER_SECONDS)
    d = load_wav(wav)
    mono = d[:, 0] if d.ndim == 2 else d
    if not check_finite_nonsilent("filter", d):
        return False
    # Energy near each candidate pitch, summed across a 1 s slice taken from
    # a region likely to contain a sounded note.
    spec = np.abs(np.fft.rfft(mono[SR:2*SR] * np.hanning(SR)))
    freqs = np.fft.rfftfreq(SR, 1.0 / SR)
    def band(hz: float) -> float:
        lo, hi = 0.95 * hz, 1.05 * hz
        idx = (freqs >= lo) & (freqs <= hi)
        return float(spec[idx].sum())
    e_c4 = band(C4_HZ)
    e_e4 = band(C4_HZ * 2.0 ** (4.0 / 12.0))
    e_g4 = band(C4_HZ * 2.0 ** (7.0 / 12.0))
    total_kept = e_e4 + e_g4
    listen = render("filter_listen", src, LISTEN_SECONDS)
    print(f"  Saved {listen} - Listen for: only e4 and g4, never c4")
    # The dropped c4 band may carry a residual from a near oscillator tail —
    # require it to be a small fraction of the kept energy, not zero.
    if total_kept <= 0.0 or e_c4 > 0.25 * total_kept:
        print(f"  ✗ FAIL: c4 band has {e_c4:.2f}, kept total {total_kept:.2f} "
              "— predicate did not drop c4")
        return False
    print(f"  ✓ PASS: kept e4+g4={total_kept:.2f}, dropped c4={e_c4:.2f}")
    return True


def test_closure_chained_stability() -> bool:
    """Two stacked closures must render stably over 300 s — exercises the
    chained EVENT_MAP closure path under load."""
    print("\n=== chained event_map closures: stability ===")
    src = (
        'event_map(event_map(n"c4 e4 g4 a4 g4 e4", (e) -> {vel: e.vel * 0.8}), '
        '          (e) -> {note: e.note + 7}) |> '
        'saw(@.freq) * @.vel * 0.3 |> out(@)')
    wav = render("closure_chained", src, RENDER_SECONDS)
    d = load_wav(wav)
    mono = d[:, 0] if d.ndim == 2 else d
    if not check_finite_nonsilent("closure_chained", d):
        return False
    head_rms, tail_rms = block_rms_profile(mono)
    growth = (tail_rms / head_rms) if head_rms > 1e-6 else 0.0
    listen = render("closure_chained_listen", src, LISTEN_SECONDS)
    print(f"  Saved {listen} - Listen for: a 6-note saw line, a fifth up, "
          f"attenuated")
    if head_rms > 1e-6 and growth > 4.0:
        print(f"  ✗ FAIL: RMS grows {growth:.1f}x head→tail — runaway")
        return False
    print(f"  ✓ PASS: {RENDER_SECONDS:.0f}s render stable "
          f"(growth={growth:.2f}x)")
    return True


def main() -> int:
    print(f"EVENT_MAP runtime event-transforms — {RENDER_SECONDS:.0f}s renders")
    results = []
    for fn in (test_transpose_frequency, test_velocity_scaling,
               test_chained_stability,
               test_event_map_closure_frequency,
               test_event_filter_drops_events,
               test_closure_chained_stability):
        try:
            results.append((fn.__name__, fn()))
        except Exception as e:  # noqa: BLE001
            print(f"  ✗ FAIL: {fn.__name__} raised {type(e).__name__}: {e}")
            results.append((fn.__name__, False))

    print("\n" + "=" * 60)
    passed = sum(1 for _, ok in results if ok)
    for name, ok in results:
        print(f"  {'✓' if ok else '✗'} {name}")
    print(f"{passed}/{len(results)} passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
