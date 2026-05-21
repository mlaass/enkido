"""
POLY Opcode Quality Tests (Cedar Engine)
=========================================
Tests POLY voice allocation against the user-reported "incomplete chord" /
"hiccups every few bars" symptom.

These tests drive the user's actual `chord("C Em Am G") |> poly(@, stab) |> out(@)`
patch through the offline render pipeline (`nkido-cli render`), then analyze:

1. Voice trace (JSONL): every block, list of active voices with freq, gate, etc.
   - Detect "firing voice" count > 3 (more chord notes than expected)
   - Detect dropped chord notes (firing voice count < 3 mid-chord)
   - Detect voice leaks (active voices accumulating beyond expected)

2. WAV audio: detect "hiccups" — sudden level spikes, sudden silences, DC offsets.

3. Save WAV for human listening — the ear is the final judge.

If a test fails, do NOT loosen the threshold. Investigate the implementation in:
  - cedar/include/cedar/opcodes/dsp_state.hpp (PolyAllocState voice allocation)
  - cedar/src/vm/vm.cpp (execute_poly_block event processing)
"""

import json
import os
import shutil
import subprocess
import sys
import wave
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal as scipy_signal

# Locate repo root and binaries
THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent
NKIDO_CLI = REPO_ROOT / "build" / "tools" / "nkido-cli" / "nkido-cli"
PATCH_PATH = REPO_ROOT / "web" / "static" / "patches" / "chord-stab.akk"

OUT_DIR = THIS_DIR / "output" / "op_poly"
OUT_DIR.mkdir(parents=True, exist_ok=True)


def render(source_path: str, wav_path: str, trace_path: str,
           seconds: float = 8.0, bpm: float = 110.0) -> dict:
    """
    Run `nkido-cli render` on the source file. Returns a dict with paths.
    Raises if the renderer fails.
    """
    if not NKIDO_CLI.exists():
        raise FileNotFoundError(
            f"nkido-cli not found at {NKIDO_CLI}. "
            f"Build it with: cmake --build {REPO_ROOT}/build --target nkido-cli"
        )

    cmd = [
        str(NKIDO_CLI), "render", str(source_path),
        "-o", str(wav_path),
        "--seconds", str(seconds),
        "--bpm", str(bpm),
        "--trace-poly", str(trace_path),
        "-v",
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        raise RuntimeError(
            f"nkido-cli render failed (exit {result.returncode}):\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return {"wav": wav_path, "trace": trace_path}


def load_wav_int16_to_float(path: str) -> tuple[np.ndarray, int]:
    """Load a 16-bit stereo WAV as a float32 numpy array shape (frames, 2)."""
    with wave.open(path, "rb") as f:
        frames = f.getnframes()
        sr = f.getframerate()
        channels = f.getnchannels()
        sampwidth = f.getsampwidth()
        raw = f.readframes(frames)
    assert sampwidth == 2, f"Expected 16-bit WAV, got {sampwidth*8}-bit"
    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    if channels == 2:
        samples = samples.reshape(-1, 2)
    return samples, sr


def load_voice_trace(path: str) -> list[dict]:
    """Load the JSONL voice trace, one record per block per poly state."""
    records = []
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            records.append(json.loads(line))
    return records


def voice_is_firing(voice: dict) -> bool:
    """A voice is 'firing' if it's actively producing audio (not releasing)."""
    return (
        voice.get("releasing") is False
        and voice.get("gate", 0.0) > 0.5
    )


# =============================================================================
# Test 1: chord("C Em Am G") -- voice grid sanity
# =============================================================================

RENDER_SECONDS = 300.0  # ≥300s simulated audio per CLAUDE.md sequenced-opcode rule
RENDER_BPM = 110.0

# Frequencies the chord("C Em Am G") parser produces (validated by the
# matching C++ test in akkado/tests/test_codegen.cpp [chord-completeness]).
# Default octave is 4; quality intervals are added on top.
CHORD_NAMES = ["C", "Em", "Am", "G"]
CHORD_FREQS = [
    (261.626, 329.628, 391.995),  # C  = C4 E4 G4
    (329.628, 391.995, 493.883),  # Em = E4 G4 B4
    (440.000, 523.251, 659.255),  # Am = A4 C5 E5
    (391.995, 493.883, 587.330),  # G  = G4 B4 D5
]


def test_chord_stab_voice_grid():
    """
    Run the user's chord-stab patch and check that:
    - At all times, no more than 3 voices are firing simultaneously
      (one per chord note; chord("C Em Am G") has 3 notes per chord).
    - Each chord onset eventually has exactly 3 firing voices.
    - Active voice count stays bounded (no voice leaks).
    """
    print("\nTest 1: chord-stab voice grid")
    print("=" * 60)

    # Copy source into output dir for traceability
    src_copy = OUT_DIR / "chord_stab_input.akk"
    shutil.copy(PATCH_PATH, src_copy)

    wav_path = OUT_DIR / "chord_stab.wav"
    trace_path = OUT_DIR / "chord_stab_voices.jsonl"

    render(str(src_copy), str(wav_path), str(trace_path),
           seconds=RENDER_SECONDS, bpm=RENDER_BPM)

    trace = load_voice_trace(str(trace_path))
    print(f"  Loaded {len(trace)} block records from trace")
    assert len(trace) > 0, "No voice trace records!"

    # Per-block analysis
    firing_counts = []
    active_counts = []
    over_3_firing_blocks = []
    under_3_firing_blocks = []
    for rec in trace:
        voices = rec["voices"]
        firing = sum(1 for v in voices if voice_is_firing(v))
        active = len(voices)
        firing_counts.append(firing)
        active_counts.append(active)
        if firing > 3:
            over_3_firing_blocks.append({
                "block": rec["block"],
                "firing": firing,
                "voices": voices,
            })
        if firing < 3:
            under_3_firing_blocks.append({
                "block": rec["block"],
                "firing": firing,
                "voices": voices,
            })

    fc = np.array(firing_counts)
    ac = np.array(active_counts)

    print(f"  Firing voices: min={fc.min()} max={fc.max()} mean={fc.mean():.2f}")
    print(f"  Active voices: min={ac.min()} max={ac.max()} mean={ac.mean():.2f}")

    if over_3_firing_blocks:
        print(f"  ✗ FAIL: {len(over_3_firing_blocks)} blocks with >3 firing voices")
        # Show the first few violations
        for v in over_3_firing_blocks[:5]:
            print(f"    block={v['block']} firing={v['firing']}: "
                  f"{[round(x['freq'], 1) for x in v['voices'] if voice_is_firing(x)]}")
        first_bad = over_3_firing_blocks[0]
        # Reproduces the user's "more than 3 voices active for at least a frame/block"
        assert False, (
            f"More than 3 voices firing at block {first_bad['block']}. "
            f"This reproduces the user's bug."
        )
    else:
        print(f"  ✓ Never more than 3 firing voices simultaneously")

    # Every chord("C Em Am G") event has exactly 3 notes, and the events tile
    # the cycle with no gaps — so once the patch starts running, there should
    # ALWAYS be exactly 3 firing voices, every block. A drop to <3 means a
    # voice gap at a chord transition (e.g. the cycle-boundary alignment bug
    # at BPM 110 where every 11 cycles == 9000 blocks exactly).
    if under_3_firing_blocks:
        print(f"  ✗ FAIL: {len(under_3_firing_blocks)} blocks with <3 firing voices")
        for v in under_3_firing_blocks[:5]:
            print(f"    block={v['block']} firing={v['firing']}: "
                  f"voices={[(round(x['freq'], 1), x.get('gate'), x.get('releasing')) for x in v['voices']]}")
        first_bad = under_3_firing_blocks[0]
        assert False, (
            f"Voice gap at block {first_bad['block']}: only "
            f"{first_bad['firing']} voices firing (expected 3). "
            f"This indicates a chord-transition gap."
        )
    else:
        print(f"  ✓ Always exactly 3 firing voices (no transition gaps)")

    # Save WAV for human listening
    print(f"  Saved {wav_path} - Listen for hiccups, missing notes, level spikes")
    print(f"  Saved {trace_path} - Per-block voice grid (JSONL)")


# =============================================================================
# Test 2: chord-stab WAV — detect hiccups and glitches
# =============================================================================

def test_chord_stab_audio_quality():
    """
    Analyze the rendered WAV for audio glitches:
    - Sudden level spikes (peak above expected envelope)
    - Sudden zero-crossings (clicks)
    - DC offset accumulation
    """
    print("\nTest 2: chord-stab audio quality")
    print("=" * 60)

    wav_path = OUT_DIR / "chord_stab.wav"
    if not wav_path.exists():
        # Render if missing
        trace_path = OUT_DIR / "chord_stab_voices.jsonl"
        render(str(PATCH_PATH), str(wav_path), str(trace_path),
               seconds=16.0, bpm=110.0)

    samples, sr = load_wav_int16_to_float(str(wav_path))
    if samples.ndim == 2:
        # Mix to mono for analysis
        mono = samples.mean(axis=1)
    else:
        mono = samples
    print(f"  Loaded {len(mono)} samples at {sr} Hz "
          f"({len(mono)/sr:.2f}s)")

    # Peak / RMS overview
    peak = float(np.max(np.abs(mono)))
    rms = float(np.sqrt(np.mean(mono * mono)))
    print(f"  Peak: {peak:.4f}  RMS: {rms:.4f}")

    # Detect "hiccups" by looking at sudden RMS jumps in a sliding window
    win_ms = 5.0  # 5 ms windows
    win = max(1, int(sr * win_ms / 1000.0))
    n_wins = len(mono) // win
    if n_wins < 4:
        print("  ⚠ Audio too short for window analysis")
        return
    win_rms = np.array([
        np.sqrt(np.mean(mono[i*win:(i+1)*win] ** 2))
        for i in range(n_wins)
    ])
    # Discard the first 100ms (start transient)
    skip_wins = int(0.1 * 1000.0 / win_ms)
    if skip_wins < n_wins:
        steady = win_rms[skip_wins:]
    else:
        steady = win_rms
    if len(steady) < 4:
        print("  ⚠ Not enough steady windows for analysis")
        return
    median_rms = float(np.median(steady))
    print(f"  Median 5ms-window RMS (steady): {median_rms:.4f}")

    # Look for windows where RMS spikes >2x the median (hiccup candidates)
    if median_rms > 1e-5:
        spikes = np.where(steady > median_rms * 2.5)[0]
        print(f"  Windows with RMS > 2.5x median: {len(spikes)} / {len(steady)}")
    else:
        spikes = np.array([])
        print("  Median RMS too low to detect spikes")

    # Click detection: very large sample-to-sample deltas
    deltas = np.abs(np.diff(mono))
    click_threshold = 0.5  # half the full-scale range in one sample
    clicks = np.where(deltas > click_threshold)[0]
    print(f"  Clicks (Δ>0.5 sample-to-sample): {len(clicks)}")

    # DC offset
    dc = float(np.mean(mono))
    print(f"  DC offset: {dc:.6f}")

    # Report — this is informational; many spikes are normal for AR-attack edges
    if len(clicks) > 0:
        print(f"  ⚠ Detected {len(clicks)} clicks — listen to {wav_path} to verify")
    else:
        print("  ✓ No sample-to-sample clicks")

    if abs(dc) > 0.01:
        print(f"  ⚠ DC offset {dc:.4f} is non-trivial")
    else:
        print("  ✓ DC offset within tolerance")

    # Per-chord RMS stability: every chord in chord("C Em Am G") repeats every
    # cycle and should sound the same each time. A drop on certain cycles
    # (e.g., when chord transitions land on exact block boundaries) means an
    # envelope failed to retrigger. Flag any chord whose min cycle-RMS drops
    # below 70% of its mean — that's the symptom of the
    # "shared-frequency retrigger at exact alignment" bug.
    spb = 60.0 / 110.0 * sr
    n_cycles = int(len(mono) / (4 * spb))
    chord_names = ["C", "Em", "Am", "G"]
    rms_per = [[0.0] * 4 for _ in range(n_cycles)]
    skip = int(0.05 * sr)
    for c in range(n_cycles):
        for ch in range(4):
            start = int((c * 4 + ch) * spb)
            end = int((c * 4 + ch + 1) * spb)
            if end > len(mono):
                continue
            seg = mono[start + skip:end - skip]
            if len(seg) > 0:
                rms_per[c][ch] = float(np.sqrt(np.mean(seg ** 2)))
    rms_arr = np.array(rms_per)
    print(f"  Per-chord RMS over {n_cycles} cycles "
          f"(checking for retrigger drop-outs):")
    bad_chords = []
    for ch_idx, ch_name in enumerate(chord_names):
        col = rms_arr[:, ch_idx]
        col = col[col > 0]
        if len(col) < 4:
            continue
        mean = float(col.mean())
        mn = float(col.min())
        ratio = mn / mean if mean > 0 else 1.0
        marker = "✓" if ratio >= 0.7 else "✗"
        print(f"    {marker} {ch_name}: mean={mean:.4f} min={mn:.4f} "
              f"ratio={ratio:.3f}")
        if ratio < 0.7:
            bad_chords.append((ch_name, mean, mn, ratio))
    if bad_chords:
        first = bad_chords[0]
        assert False, (
            f"Chord {first[0]} dropped to {first[2]:.4f} RMS (ratio "
            f"{first[3]:.2f} of mean {first[1]:.4f}) — envelope failed "
            f"to retrigger on at least one cycle. Reproduces the "
            f"shared-frequency retrigger-at-alignment bug."
        )


# =============================================================================
# Test 3: chord progression voice churn
# =============================================================================

def test_chord_progression_voice_churn():
    """
    Verify that during chord transitions, the voice count behavior matches
    expectations:
    - Briefly, active voices may exceed 3 (releasing voices from previous chord
      + firing voices from new chord) — this is normal.
    - However, FIRING voices (active && !releasing && gate>0.5) should never
      exceed 3 at any block.
    - Active voices should decay back to 3 within RELEASE_TIMEOUT (4 blocks)
      after each chord change.
    """
    print("\nTest 3: voice churn during chord transitions")
    print("=" * 60)

    trace_path = OUT_DIR / "chord_stab_voices.jsonl"
    if not trace_path.exists():
        wav_path = OUT_DIR / "chord_stab.wav"
        render(str(PATCH_PATH), str(wav_path), str(trace_path),
               seconds=16.0, bpm=110.0)

    trace = load_voice_trace(str(trace_path))

    # Find blocks where active count drops back to a steady value
    # At 110 BPM, 1 cycle = 4 beats * (60/110) = 2.18s = ~819 blocks at 48k/128
    # Each chord = ~205 blocks
    # After a chord change, releasing voices should clear in <= 4 blocks (timeout)
    # so by block N+10 after a chord change, active should equal firing
    settle_blocks = 12
    settle_failures = []

    # Walk through the trace and find chord transitions
    last_chord_event = None
    transition_blocks = []
    for rec in trace:
        firing_voices = [v for v in rec["voices"] if voice_is_firing(v)]
        # Use the dominant event_index of firing voices as the chord identity
        if not firing_voices:
            continue
        events = [v["event"] for v in firing_voices]
        # Mode of events
        most_common_event = max(set(events), key=events.count)
        if last_chord_event is None:
            last_chord_event = most_common_event
            continue
        if most_common_event != last_chord_event:
            transition_blocks.append(rec["block"])
            last_chord_event = most_common_event

    print(f"  Detected {len(transition_blocks)} chord transitions")

    # For each transition, check that within `settle_blocks` blocks,
    # active count == firing count (no lingering releasing voices)
    for trans_block in transition_blocks:
        # Find the trace record `settle_blocks` blocks after the transition
        target_block = trans_block + settle_blocks
        for rec in trace:
            if rec["block"] == target_block:
                voices = rec["voices"]
                active = len(voices)
                firing = sum(1 for v in voices if voice_is_firing(v))
                if active != firing:
                    settle_failures.append({
                        "transition": trans_block,
                        "checked_at": target_block,
                        "active": active,
                        "firing": firing,
                    })
                break

    if settle_failures:
        print(f"  ⚠ {len(settle_failures)} transitions still had releasing voices "
              f"after {settle_blocks} blocks")
        for f in settle_failures[:3]:
            print(f"    transition={f['transition']} at +{settle_blocks}: "
                  f"active={f['active']} firing={f['firing']}")
    else:
        print(f"  ✓ All transitions settled to active==firing within "
              f"{settle_blocks} blocks")


# =============================================================================
# Test 4: Phase 2 — per-voice event-record field bank
# =============================================================================

def _rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))


def test_field_bank_phase_plumbing():
    """
    Verify the Phase-2 per-voice field bank (prd-poly-callback-event-record):
    a callback that destructures {freq, note, dur, phase} and multiplies the
    voice by (1 - phase) must produce an audibly attenuated, decaying signal.

    If `phase` were stuck at the old constant-0 buffer, (1 - phase) == 1 and
    the phase patch would be bit-identical to the plain patch. So:
    - plain  = osc(freq) * 0.3
    - phased = osc(freq) * 0.3 * (1 - phase), destructuring all four fields.
    Expect: phased RMS is clearly below plain RMS (phase ramps 0->1), but not
    silent (phase never pinned at 1). Renders >=300s; asserts no NaN/DC and
    no RMS drift across the render (voice-leak guard).

    If this fails, check VM::run_voice_pool field-bank fill (cedar/src/vm/vm.cpp)
    and handle_poly_call bank wiring (akkado/src/codegen_functions.cpp).
    """
    print("\n[test_field_bank_phase_plumbing]")

    plain_src = OUT_DIR / "phase2_plain.akk"
    phased_src = OUT_DIR / "phase2_phased.akk"
    plain_src.write_text(
        'n"c4 e4 g4 b4" |> poly(@, ({freq}) -> osc("sin", freq) * 0.3) |> out(@)\n'
    )
    phased_src.write_text(
        'n"c4 e4 g4 b4" |> poly(@, ({freq, note, dur, phase}) ->\n'
        '    osc("sin", freq) * 0.3 * (1 - phase)) |> out(@)\n'
    )

    plain_wav = str(OUT_DIR / "phase2_plain.wav")
    phased_wav = str(OUT_DIR / "phase2_phased.wav")
    render(str(plain_src), plain_wav, str(OUT_DIR / "phase2_plain.jsonl"),
           seconds=RENDER_SECONDS, bpm=RENDER_BPM)
    render(str(phased_src), phased_wav, str(OUT_DIR / "phase2_phased.jsonl"),
           seconds=RENDER_SECONDS, bpm=RENDER_BPM)

    plain, _ = load_wav_int16_to_float(plain_wav)
    phased, sr = load_wav_int16_to_float(phased_wav)
    plain_l = plain[:, 0]
    phased_l = phased[:, 0]

    assert np.all(np.isfinite(phased_l)), "phased render produced NaN/Inf"
    dc = abs(float(np.mean(phased_l)))
    assert dc < 0.01, f"phased render has DC offset {dc:.4f}"

    plain_rms = _rms(plain_l)
    phased_rms = _rms(phased_l)
    assert plain_rms > 0.01, f"plain render is silent (rms {plain_rms:.4f})"
    # phase ramps 0->1: (1-phase) averages ~0.5, so phased must be clearly
    # below plain. Stuck-at-0 phase would make them equal.
    assert phased_rms < plain_rms * 0.85, (
        f"phased rms {phased_rms:.4f} not below plain rms {plain_rms:.4f} — "
        f"`phase` field not plumbed (still constant 0?)"
    )
    # ...but not pinned at 1 (which would silence the voice).
    assert phased_rms > plain_rms * 0.15, (
        f"phased rms {phased_rms:.4f} far too low vs plain {plain_rms:.4f} — "
        f"`phase` may be pinned at 1"
    )

    # Voice-leak / drift guard: RMS stable across the >=300s render.
    thirds = np.array_split(phased_l, 3)
    rms_thirds = [_rms(t) for t in thirds]
    lo, hi = min(rms_thirds), max(rms_thirds)
    assert lo > 0.0 and hi / lo < 1.5, (
        f"phased RMS drifts across render: thirds {rms_thirds} — possible "
        f"voice leak"
    )

    print(f"  plain rms {plain_rms:.4f}, phased rms {phased_rms:.4f} "
          f"(ratio {phased_rms / plain_rms:.2f})")
    print(f"  RMS thirds {[round(r, 4) for r in rms_thirds]}")
    print(f"  ✓ PASS: per-voice `phase` plumbed; no NaN/DC/drift")
    print(f"  Saved {phased_wav} — listen for each note ramping down in level")


def main():
    print("POLY Opcode Quality Tests")
    print("=" * 60)
    print(f"Renderer: {NKIDO_CLI}")
    print(f"Source:   {PATCH_PATH}")
    print(f"Output:   {OUT_DIR}")

    if not NKIDO_CLI.exists():
        print(f"\n✗ FAIL: nkido-cli not found at {NKIDO_CLI}")
        print(f"  Build with: cmake --build {REPO_ROOT}/build --target nkido-cli")
        sys.exit(1)
    if not PATCH_PATH.exists():
        print(f"\n✗ FAIL: patch source not found at {PATCH_PATH}")
        sys.exit(1)

    failures = []
    for test in [
        test_chord_stab_voice_grid,
        test_chord_stab_audio_quality,
        test_chord_progression_voice_churn,
        test_field_bank_phase_plumbing,
    ]:
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
    else:
        print("All tests passed.")


if __name__ == "__main__":
    main()
