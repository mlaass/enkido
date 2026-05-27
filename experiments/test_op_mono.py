"""
mono() Voice Manager Quality Tests (Phase 4, prd-poly-callback-event-record)
============================================================================
`mono()` is `poly()` with mode=1: a single voice (slot 0) reused for every
note, the envelope re-attacking on each onset. mono/legato route through the
same `handle_poly_call` codegen and the same `PolyAllocState` as poly, so
every Phase 1-3 callback shape and the per-voice field bank apply unchanged.

These tests drive the mono voice manager through the offline render pipeline
(`nkido render`) for >=300s and verify:

1. The poly voice trace never shows more than one voice — mono is
   single-voice by construction (PolyAllocState mode==1 always uses slot 0).
2. The render is clean: no NaN/Inf, no DC offset.
3. RMS is stable across the >=300s render — no voice leak / state drift.
4. Every Phase 1-3 callback shape (positional, destructure, rest, custom
   record-suffix field) compiles and renders through mono.

If a test fails, do NOT loosen the threshold. Investigate:
  - cedar/include/cedar/opcodes/dsp_state.hpp — PolyAllocState::allocate_voice,
    the `mode == 1` branch
  - cedar/src/vm/vm.cpp — run_voice_pool field-bank fill
  - akkado/src/codegen_functions.cpp — handle_poly_call
"""

import json
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido" / "nkido"

OUT_DIR = THIS_DIR / "output" / "op_mono"
OUT_DIR.mkdir(parents=True, exist_ok=True)

RENDER_SECONDS = 300.0  # >=300s simulated audio per CLAUDE.md sequenced-opcode rule
RENDER_BPM = 120.0


def render(source_path, wav_path, trace_path, seconds=RENDER_SECONDS,
           bpm=RENDER_BPM):
    """Run `nkido render` on the source file. Raises if it fails."""
    if not NKIDO_CLI.exists():
        raise FileNotFoundError(
            f"nkido not found at {NKIDO_CLI}. Build it with: "
            f"cmake --build {REPO_ROOT}/build --target nkido")
    cmd = [
        str(NKIDO_CLI), "render", str(source_path),
        "-o", str(wav_path),
        "--seconds", str(seconds),
        "--bpm", str(bpm),
        "--trace-poly", str(trace_path),
        "-v",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if result.returncode != 0:
        raise RuntimeError(
            f"nkido render failed (exit {result.returncode}):\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}")


def load_wav(path):
    """Load a 16-bit WAV as float32, shape (frames,) for L channel."""
    with wave.open(str(path), "rb") as f:
        frames = f.getnframes()
        channels = f.getnchannels()
        sampwidth = f.getsampwidth()
        raw = f.readframes(frames)
    assert sampwidth == 2, f"Expected 16-bit WAV, got {sampwidth*8}-bit"
    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if channels == 2:
        samples = samples.reshape(-1, 2)[:, 0]
    return samples


def load_trace(path):
    """Load the JSONL poly voice trace, one record per block."""
    records = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


def _rms(x):
    return float(np.sqrt(np.mean(np.square(x)))) if len(x) else 0.0


def check_render_clean(sig, label):
    """Assert a render is finite and DC-free. Returns RMS."""
    assert np.all(np.isfinite(sig)), f"{label}: render produced NaN/Inf"
    dc = abs(float(np.mean(sig)))
    assert dc < 0.01, f"{label}: DC offset {dc:.4f} (expected < 0.01)"
    rms = _rms(sig)
    assert rms > 0.01, f"{label}: render is silent (rms {rms:.4f})"
    return rms


def check_single_voice(trace, label):
    """Assert no block ever traced more than one voice (mono invariant)."""
    assert len(trace) > 0, f"{label}: empty voice trace"
    max_voices = 0
    worst_block = -1
    for rec in trace:
        n = len(rec.get("voices", []))
        if n > max_voices:
            max_voices = n
            worst_block = rec.get("block", -1)
    assert max_voices <= 1, (
        f"{label}: block {worst_block} traced {max_voices} voices — "
        f"mono must never exceed 1")
    return max_voices


def check_no_drift(sig, label):
    """Assert RMS is stable across the render (voice-leak / drift guard)."""
    thirds = np.array_split(sig, 3)
    rms_thirds = [_rms(t) for t in thirds]
    lo, hi = min(rms_thirds), max(rms_thirds)
    assert lo > 0.0 and hi / lo < 1.5, (
        f"{label}: RMS drifts across render: thirds "
        f"{[round(r, 4) for r in rms_thirds]} — possible voice leak")
    return rms_thirds


# =============================================================================
# Test 1: mono single-voice stability over >=300s
# =============================================================================

def test_mono_single_voice_stable():
    """
    Render a destructure-callback mono patch for >=300s and verify mono stays
    single-voice, clean, and drift-free for the whole render.

    If this fails, mono is leaking voices or its state is drifting — check
    PolyAllocState::allocate_voice (mode==1) and the release path.
    """
    print("\n[test_mono_single_voice_stable]")
    src = OUT_DIR / "mono_stable.akk"
    src.write_text(
        'n"c2 e2 g2 c3" |> mono(@, ({freq, gate, vel}) ->\n'
        '    saw(freq) * adsr(gate, 0.01, 0.1, 0.7, 0.2) * vel * 0.4)\n'
        '    |> out(@)\n')
    wav = OUT_DIR / "mono_stable.wav"
    trace_path = OUT_DIR / "mono_stable.jsonl"
    render(src, wav, trace_path)

    trace = load_trace(trace_path)
    max_voices = check_single_voice(trace, "mono_stable")
    sig = load_wav(wav)
    rms = check_render_clean(sig, "mono_stable")
    rms_thirds = check_no_drift(sig, "mono_stable")

    print(f"  {len(trace)} blocks traced, max voices = {max_voices}")
    print(f"  rms {rms:.4f}, RMS thirds {[round(r, 4) for r in rms_thirds]}")
    print(f"  ✓ PASS: mono single-voice, clean, no drift over {RENDER_SECONDS}s")
    print(f"  Saved {wav} — listen for an even mono saw line, even level")


# =============================================================================
# Test 2: every callback shape compiles and renders through mono
# =============================================================================

def test_mono_callback_shapes():
    """
    mono() must accept every Phase 1-3 instrument callback shape. Render each
    shape (short) and assert the output is finite and non-silent — proving the
    shape both compiles and feeds audio through the mono voice.

    Shapes: positional-1, rest param, and a custom record-suffix field
    (the cutoff value modulates the filter, so a stuck-at-0 field would
    silence the patch).
    """
    print("\n[test_mono_callback_shapes]")
    shapes = {
        "positional1": 'n"c2 e2 g2 c3" |> mono(@, (freq) ->\n'
                       '    saw(freq) * 0.3) |> out(@)\n',
        "rest": 'n"c2 e2 g2 c3" |> mono(@, (...e) ->\n'
                '    saw(e.freq) * e.gate * 0.3) |> out(@)\n',
        "custom_field":
            'n"c2{cutoff:0.9} e2{cutoff:0.4} g2{cutoff:0.7} c3{cutoff:0.5}"\n'
            '    |> mono(@, ({freq, gate, cutoff}) ->\n'
            '        saw(freq) |> lp(@, cutoff * 4000)\n'
            '        |> @ * adsr(gate, 0.01, 0.1, 0.7, 0.2) * 0.4)\n'
            '    |> out(@)\n',
    }
    for name, code in shapes.items():
        src = OUT_DIR / f"mono_shape_{name}.akk"
        src.write_text(code)
        wav = OUT_DIR / f"mono_shape_{name}.wav"
        trace_path = OUT_DIR / f"mono_shape_{name}.jsonl"
        # Shorter render — this is a compile+render-path check, the 300s
        # stability guarantee is covered by test 1.
        render(src, wav, trace_path, seconds=16.0)
        trace = load_trace(trace_path)
        check_single_voice(trace, f"mono_shape_{name}")
        sig = load_wav(wav)
        rms = check_render_clean(sig, f"mono_shape_{name}")
        print(f"  ✓ {name}: compiles + renders (rms {rms:.4f})")
    print(f"  ✓ PASS: mono accepts positional, rest, and custom-field "
          f"callbacks")
    print(f"  Saved {OUT_DIR}/mono_shape_custom_field.wav — listen for the "
          f"filter tracking each note's cutoff")


def main():
    print("mono() Voice Manager Quality Tests")
    print("=" * 60)
    print(f"Renderer: {NKIDO_CLI}")
    print(f"Output:   {OUT_DIR}")

    if not NKIDO_CLI.exists():
        print(f"\n✗ FAIL: nkido not found at {NKIDO_CLI}")
        print(f"  Build with: cmake --build {REPO_ROOT}/build --target "
              f"nkido")
        sys.exit(1)

    failures = []
    for test in [test_mono_single_voice_stable, test_mono_callback_shapes]:
        try:
            test()
        except AssertionError as e:
            failures.append((test.__name__, str(e)))
            print(f"  ✗ {test.__name__} FAILED: {e}")
        except Exception as e:
            failures.append((test.__name__, f"{type(e).__name__}: {e}"))
            print(f"  ✗ {test.__name__} ERRORED: {e}")

    print("\n" + "=" * 60)
    if failures:
        print(f"FAILED: {len(failures)} test(s)")
        for name, msg in failures:
            print(f"  ✗ {name}: {msg}")
        sys.exit(1)
    print("All tests passed.")


if __name__ == "__main__":
    main()
