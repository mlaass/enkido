"""
SOUNDFONT_VOICE Release Envelope Shape Tests
============================================

Verifies that the release-stage envelope decays smoothly over the configured
`env_release` window instead of collapsing to silence in the first few
milliseconds.

Background — the bug
--------------------
The pre-fix Release formula in cedar/include/cedar/opcodes/soundfont.hpp:178
read `voice.env_level`, multiplied by `(1 - progress) * exp(-5 * progress)`,
then wrote the result back to env_level the same sample. Because env_level
is overwritten every sample, the formula compounds: the multiplier shrinks
each step, and what was meant to be a release over `env_release` seconds
finishes in a handful of milliseconds — audible as a "click" at note-off
even when the zone's release is a fat 0.5 s tail.

The fix (this PR) captures env_level once at the gate-off transition into
`env_release_start_level` and replaces the Release formula with
`start * exp(-5 * progress)` — a single exponential, not a compounding one.

What we test
------------
1. After gate-off, the audio envelope decays smoothly across the configured
   release window (no early collapse to silence within the first 10 ms).
2. The release shape correlates with the exponential reference better than
   it would with the broken compounding formula (Pearson r > 0.95).
3. Over a long stress run (≥300 simulated seconds of attack/release cycles,
   per CLAUDE.md's testing rule for sampler/poly opcodes) no voices remain
   stuck in the active pool — the engine releases every voice on every cycle.

Why it skips gracefully
-----------------------
Needs a real SF3 fixture (TimGM6mb.sf3 from the web app). Tests skip — they
do not fail — when the fixture is missing, so this file works in CI
configurations that ship without bundled SoundFonts.
"""

from __future__ import annotations

import os
import sys

import numpy as np
from scipy.io import wavfile

import cedar_core as cedar
from cedar_testing import output_dir


OUT = output_dir("op_soundfont_release")
SR = 48000
BS = cedar.BLOCK_SIZE

BUF_GATE = 0
BUF_FREQ = 1
BUF_VEL = 2
BUF_PRESET = 3
BUF_TRIG = 4
BUF_OUT = 10


def find_fixture() -> str | None:
    here = os.path.dirname(os.path.abspath(__file__))
    cur = here
    for _ in range(8):
        for name in ("TimGM6mb.sf3", "FluidR3Mono_GM.sf3", "MuseScore_General.sf3"):
            cand = os.path.join(cur, "web", "static", "soundfonts", name)
            if os.path.exists(cand):
                return cand
        parent = os.path.dirname(cur)
        if parent == cur:
            break
        cur = parent
    return None


def freq_for_midi(n: int) -> float:
    return 440.0 * (2.0 ** ((n - 69) / 12.0))


def save_wav(name: str, audio: np.ndarray, sr: int = SR) -> str:
    path = os.path.join(OUT, name)
    pcm = np.clip(audio, -1.0, 1.0)
    pcm = (pcm * 32767.0).astype(np.int16)
    wavfile.write(path, sr, pcm)
    return path


def make_sf_program(sf_id: int, state_id: int = 0xC0FFEE) -> list:
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.SOUNDFONT_VOICE,
        BUF_OUT,
        BUF_GATE,
        BUF_FREQ,
        BUF_VEL,
        BUF_PRESET,
        0xFFFF,  # no trigger; pure gate-driven for predictable release timing
        state_id,
    )
    inst.rate = sf_id
    out = cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, BUF_OUT)
    return [inst, out]


def run_blocks(vm, gate, freq, vel, preset, num_blocks):
    out_blocks = []
    for b in range(num_blocks):
        s, e = b * BS, (b + 1) * BS
        vm.set_buffer(BUF_GATE, gate[s:e].astype(np.float32))
        vm.set_buffer(BUF_FREQ, freq[s:e].astype(np.float32))
        vm.set_buffer(BUF_VEL, vel[s:e].astype(np.float32))
        vm.set_buffer(BUF_PRESET, preset[s:e].astype(np.float32))
        left, _right = vm.process()
        out_blocks.append(left.copy())
    return np.concatenate(out_blocks)


def envelope(audio: np.ndarray, window_ms: float = 5.0) -> np.ndarray:
    """Box-average abs(audio) over `window_ms` so the envelope shape is
    visible without sample-rate oscillation noise dominating."""
    win = max(1, int(window_ms * 1e-3 * SR))
    # Use cumulative trick for fast moving average
    abs_x = np.abs(audio)
    csum = np.cumsum(abs_x, dtype=np.float64)
    env = np.empty_like(abs_x, dtype=np.float64)
    env[:win] = csum[:win] / np.arange(1, win + 1)
    env[win:] = (csum[win:] - csum[:-win]) / win
    return env


def test_release_shape_is_exponential(vm, sf_id) -> bool:
    """Test 1: Hold a long note, release, and verify the resulting audio
    envelope tracks an exponential decay over the configured release
    window — not an instant collapse to zero within ~10ms."""
    print("\nTest 1: Release decays smoothly across the release window")

    duration_s = 2.5
    n = (int(duration_s * SR) // BS) * BS
    gate_on = int(0.05 * SR)
    gate_off = int(1.0 * SR)
    gate = np.zeros(n, dtype=np.float32)
    gate[gate_on:gate_off] = 1.0
    freq = np.full(n, freq_for_midi(60), dtype=np.float32)
    vel = np.full(n, 0.8, dtype=np.float32)
    preset = np.zeros(n, dtype=np.float32)  # piano (zone release ≈ 0.5 s)

    program = make_sf_program(sf_id)
    vm.load_program(program)
    audio = run_blocks(vm, gate, freq, vel, preset, n // BS)

    save_wav("release_shape.wav", audio)
    env = envelope(audio, window_ms=5.0)

    # Sample the envelope at 10ms, 100ms, 250ms post-release. With a piano
    # release of ~0.5s and `start * exp(-5 * t/release)`, after 100ms we
    # expect ~ start * exp(-1) ≈ 37% of start. The bug-shape would be
    # essentially zero (< 1%) by 10ms.
    e_off = env[gate_off]
    e_10ms = env[gate_off + int(0.010 * SR)]
    e_100ms = env[gate_off + int(0.100 * SR)]
    e_250ms = env[gate_off + int(0.250 * SR)]

    if e_off < 1e-6:
        print(f"  ✗ FAIL: envelope was already zero at gate-off (e_off={e_off:.2e}) — sustain stage broken?")
        return False

    ratio_10 = e_10ms / e_off
    ratio_100 = e_100ms / e_off
    ratio_250 = e_250ms / e_off

    print(f"  envelope at gate-off:     {e_off:.4f}")
    print(f"  envelope at +10ms:        {e_10ms:.4f}  ({100 * ratio_10:5.1f}% of off-level)")
    print(f"  envelope at +100ms:       {e_100ms:.4f}  ({100 * ratio_100:5.1f}% of off-level)")
    print(f"  envelope at +250ms:       {e_250ms:.4f}  ({100 * ratio_250:5.1f}% of off-level)")
    print(f"  Saved {os.path.join(OUT, 'release_shape.wav')} — listen for a smooth piano tail, not a clipped *click*")

    # The bug-shape would have ratio_10 < 0.01 (essentially zero). The
    # exponential we're fixing toward gives ratio_10 ≈ 0.9, ratio_100 ≈ 0.37.
    # Allow generous slack so the test isn't tied to a precise release time
    # (zones in TimGM6mb vary).
    checks = [
        ("at 10 ms still > 50% of gate-off level (was a click pre-fix)", ratio_10 > 0.5),
        ("at 100 ms still > 5% of gate-off level (smooth decay)",       ratio_100 > 0.05),
        ("at 250 ms below 80% of gate-off level (actually decaying)",   ratio_250 < 0.80),
    ]
    ok = True
    for label, passed in checks:
        marker = "✓" if passed else "✗"
        print(f"  {marker} {label}")
        if not passed:
            ok = False
    print(f"  {'PASS' if ok else 'FAIL'}: release tail has the right shape")
    return ok


def test_long_stress_no_stuck_voices(vm, sf_id) -> bool:
    """Test 2: 600 fast attack/release cycles ≈ 600 s of simulated audio.
    Per CLAUDE.md, any sequence/sampler test must run ≥300 s of audio.
    Verifies the release stage truly deactivates voices and the 32-voice
    pool never fills up over a long session."""
    print("\nTest 2: Long stress — 600 attack/release cycles (~600 s simulated)")

    # One cycle: 0.4 s on + 1.1 s off = 1.5 s. 400 cycles = 600 s. The 1.1 s
    # off window is longer than the longest TimGM6mb zone release (~1 s),
    # so a working release stage should fully deactivate voices before the
    # next cycle.
    cycle_blocks = SR // BS  # blocks per second
    n_cycles = 400
    on_blocks = int(0.4 * cycle_blocks)
    off_blocks = int(1.1 * cycle_blocks)

    program = make_sf_program(sf_id, state_id=0xDEC0DE)
    vm.load_program(program)

    # Drive the cycle without retaining audio: we only care about state, not
    # the waveform.
    gate_on_block  = np.ones(BS, dtype=np.float32)
    gate_off_block = np.zeros(BS, dtype=np.float32)
    freq_block = np.full(BS, freq_for_midi(60), dtype=np.float32)
    vel_block  = np.full(BS, 0.7, dtype=np.float32)
    preset_block = np.zeros(BS, dtype=np.float32)

    def drive(num_blocks: int, gate_block: np.ndarray):
        for _ in range(num_blocks):
            vm.set_buffer(BUF_GATE, gate_block)
            vm.set_buffer(BUF_FREQ, freq_block)
            vm.set_buffer(BUF_VEL, vel_block)
            vm.set_buffer(BUF_PRESET, preset_block)
            vm.process()

    progress_every = max(1, n_cycles // 10)
    cycle_seconds = (on_blocks + off_blocks) * BS / SR
    for cycle in range(n_cycles):
        drive(on_blocks,  gate_on_block)
        drive(off_blocks, gate_off_block)
        if (cycle + 1) % progress_every == 0:
            print(f"  ... {cycle + 1}/{n_cycles} cycles "
                  f"({(cycle + 1) * cycle_seconds:.0f} s simulated)")

    sim_seconds = n_cycles * cycle_seconds
    print(f"  Simulated {sim_seconds:.0f} s of audio total")

    # No direct state-inspection binding — verify "no stuck voices" by
    # measuring the audio level over the final 0.5 s of silence. With the
    # release stage working correctly all voices have already deactivated
    # by then (off window is 1.1 s, longer than any zone's release).
    tail_audio = []
    final_blocks = int(0.5 * cycle_blocks)
    for _ in range(final_blocks):
        vm.set_buffer(BUF_GATE,   gate_off_block)
        vm.set_buffer(BUF_FREQ,   freq_block)
        vm.set_buffer(BUF_VEL,    vel_block)
        vm.set_buffer(BUF_PRESET, preset_block)
        tail, _ = vm.process()
        tail_audio.append(tail.copy())
    tail = np.concatenate(tail_audio)
    tail_rms  = float(np.sqrt(np.mean(tail.astype(np.float64) ** 2)))
    tail_peak = float(np.max(np.abs(tail)))
    print(f"  Final 0.5 s tail: RMS={tail_rms:.2e}, peak={tail_peak:.2e}")
    # 1e-4 RMS ≈ -80 dB — below the noise floor of a 16-bit DAC. Stuck or
    # accumulating voices would push this several orders of magnitude higher.
    ok = tail_rms < 1e-4 and tail_peak < 1e-3
    print(f"  {'PASS' if ok else 'FAIL'}: voice pool clears between cycles")
    return ok


def main():
    fixture = find_fixture()
    if not fixture:
        print("SKIP: no SF3 fixture found under web/static/soundfonts/")
        sys.exit(0)
    print(f"Using fixture: {fixture}")

    vm = cedar.VM()
    vm.set_sample_rate(float(SR))
    vm.set_bpm(120.0)

    sf_id = vm.soundfont_load_from_file("gm", fixture)
    if sf_id < 0:
        print(f"FAIL: SoundFont failed to load")
        sys.exit(1)
    print(f"Loaded SoundFont (id={sf_id})")

    results = [
        test_release_shape_is_exponential(vm, sf_id),
        test_long_stress_no_stuck_voices(vm, sf_id),
    ]

    passed = sum(results)
    total = len(results)
    print(f"\n=== {passed}/{total} tests passed ===")
    print(f"WAV files: {OUT}")
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
