"""
Python opcode-level fuzz: TRIGGER + AR envelope under repeated program loads.

Companion to the C++ rapid-recompile fuzz in
akkado/tests/test_fuzz_recompile_audio.cpp. The C++ tests exercise the
real hot-swap path with realistic akkado programs; this script complements
them at the opcode level using cedar_core's Python bindings.

Limitation: cedar_core currently only exposes load_program_immediate,
which fully wipes state. So this test demonstrates the *worst-case*
behavior — what happens if state preservation is completely absent — and
verifies the artifact_metrics library catches numerical anomalies.

A follow-up will expose vm.load_program() (the real hot-swap path) to
Python so the same metrics can attack hot-swap continuity directly.

Run:
    cd experiments && uv run python test_fuzz_trigger_recompile.py
"""

import os
import numpy as np
import cedar_core as cedar
from cedar_testing import output_dir
from utils import save_wav
from artifact_metrics import (
    detect_clicks,
    scan_sanity,
    count_rising_edges,
    block_rms,
    find_energy_anomaly,
)

OUT = output_dir("fuzz_trigger_recompile")


def trigger_program(division: float) -> tuple[list, cedar.VM]:
    """Build a 'TRIGGER at given division → OUTPUT' program."""
    vm = cedar.VM()
    vm.set_sample_rate(48000)
    vm.set_bpm(120.0)
    vm.set_param("division", division)
    prog = [
        cedar.Instruction.make_nullary(
            cedar.Opcode.ENV_GET, 1, cedar.hash("division")
        ),
        cedar.Instruction.make_unary(cedar.Opcode.TRIGGER, 10, 1, state=42),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10),
    ]
    return prog, vm


def render_blocks(vm: cedar.VM, n_blocks: int) -> np.ndarray:
    """Render n blocks, return L channel concatenated."""
    out = []
    for _ in range(n_blocks):
        l, _ = vm.process()
        out.append(l)
    return np.concatenate(out)


# ---------------------------------------------------------------------------
# Test 1 — baseline: single load, render long, verify trigger count + sanity.
# ---------------------------------------------------------------------------


def test_baseline_trigger_count():
    """Establish baseline: 4 triggers/beat × 4 beats/cycle × cycles in render."""
    prog, vm = trigger_program(division=4.0)
    vm.load_program(prog)
    seconds = 4.0
    n_blocks = int(seconds * 48000 / cedar.BLOCK_SIZE)
    sig = render_blocks(vm, n_blocks)

    wav_path = os.path.join(OUT, "baseline_trigger.wav")
    save_wav(wav_path, sig, 48000)

    sanity = scan_sanity(sig, clip_threshold=2.0)
    edges = count_rising_edges(sig, threshold=0.5)

    # 120 BPM → 2 beats/sec → division=4 → 8 trigs/sec → ~32 over 4 s
    expected_edges = int(round(seconds * (120.0 / 60.0) * 4.0))
    print(f"  Baseline: {edges} edges (expected ~{expected_edges})")
    print(f"  Sanity: NaN={sanity.nan_count} Inf={sanity.inf_count} "
          f"clip={sanity.clip_count} peak={sanity.peak:.3f}")
    print(f"  Saved {wav_path}")

    if sanity.nan_count or sanity.inf_count or sanity.clip_count:
        print(f"  ✗ FAIL: sanity issue (first bad @ {sanity.first_bad_index}: "
              f"{sanity.first_bad_reason})")
        return False
    if abs(edges - expected_edges) > 1:
        print(f"  ✗ FAIL: edge count off by {edges - expected_edges}")
        return False
    print(f"  ✓ PASS")
    return True


# ---------------------------------------------------------------------------
# Test 2 — recompile-stress with state wipe (worst-case lower bound).
# ---------------------------------------------------------------------------


def test_recompile_state_wipe():
    """Render in segments, calling load_program() between each.

    With load_program_immediate (Python's only path today), state is wiped
    on every load. We DON'T assert trigger-count parity — instead we check
    that the audio stays numerically sane (no NaN/Inf/extreme clip) and
    that nothing pathological happens at the load boundaries.

    This catches: load() leaving the VM in an inconsistent state, a
    swap-path bug that lets stale state through, output buffer not being
    properly zeroed between programs, etc.
    """
    prog, vm = trigger_program(division=4.0)
    vm.load_program(prog)

    segments = []
    n_swaps = 20
    blocks_per_segment = 30  # ~80 ms each

    for s in range(n_swaps + 1):
        seg = render_blocks(vm, blocks_per_segment)
        segments.append(seg)
        if s < n_swaps:
            # Rebuild the same program and load it again. Different state_id
            # would force full state replacement; same state_id reuses the
            # state (which load_program_immediate wipes anyway).
            prog2, _ = trigger_program(division=4.0)
            vm.load_program(prog2)

    sig = np.concatenate(segments)
    wav_path = os.path.join(OUT, "recompile_state_wipe.wav")
    save_wav(wav_path, sig, 48000)

    sanity = scan_sanity(sig, clip_threshold=2.0)
    # Per-block RMS over the full render — energy should stay roughly stable.
    rms = block_rms(sig, cedar.BLOCK_SIZE)
    anomaly = find_energy_anomaly(rms, first_block=blocks_per_segment,
                                  ratio_threshold=100.0, floor=1e-5)

    print(f"  Recompile×{n_swaps}: NaN={sanity.nan_count} Inf={sanity.inf_count} "
          f"clip={sanity.clip_count} peak={sanity.peak:.3f}")
    print(f"  Energy: anomaly={anomaly.found} ({anomaly.kind} @ block "
          f"{anomaly.block_idx}, ratio={anomaly.ratio:.2f})")
    print(f"  Saved {wav_path}")

    if sanity.nan_count or sanity.inf_count:
        print(f"  ✗ FAIL: non-finite samples (first @ {sanity.first_bad_index}: "
              f"{sanity.first_bad_reason})")
        return False
    if sanity.peak > 2.0:
        print(f"  ✗ FAIL: peak {sanity.peak:.3f} > 2.0 — load() may not be "
              "clearing output buffer cleanly")
        return False
    if anomaly.found and anomaly.kind == "burst":
        print(f"  ✗ FAIL: energy burst at block {anomaly.block_idx} "
              f"(ratio {anomaly.ratio:.1f})")
        return False
    print(f"  ✓ PASS")
    return True


# ---------------------------------------------------------------------------
# Test 3 — random-division stress.
# ---------------------------------------------------------------------------


def test_random_division_stress():
    """Repeatedly change division parameter — exercises set_param under
    a sequencer-driven opcode. Verifies no NaN/Inf/clip and the program
    remains stable.
    """
    rng = np.random.default_rng(seed=0xA1B2C3D4)
    prog, vm = trigger_program(division=1.0)
    vm.load_program(prog)

    segments = []
    n_param_changes = 40
    blocks_per_segment = 25

    for _ in range(n_param_changes):
        # Cover a wide range: very slow (0.25 trigs/beat) to very fast (16).
        division = float(rng.uniform(0.25, 16.0))
        vm.set_param("division", division)
        segments.append(render_blocks(vm, blocks_per_segment))

    sig = np.concatenate(segments)
    wav_path = os.path.join(OUT, "random_division.wav")
    save_wav(wav_path, sig, 48000)

    sanity = scan_sanity(sig, clip_threshold=2.0)
    print(f"  Random division×{n_param_changes}: NaN={sanity.nan_count} "
          f"Inf={sanity.inf_count} clip={sanity.clip_count} peak={sanity.peak:.3f}")
    print(f"  Saved {wav_path}")
    if not (sanity.nan_count == 0 and sanity.inf_count == 0 and sanity.peak <= 2.0):
        print(f"  ✗ FAIL: sanity issue")
        return False
    print(f"  ✓ PASS")
    return True


if __name__ == "__main__":
    print("Test 1: baseline trigger count")
    ok1 = test_baseline_trigger_count()
    print()
    print("Test 2: repeated load_program (state-wipe path)")
    ok2 = test_recompile_state_wipe()
    print()
    print("Test 3: random division parameter stress")
    ok3 = test_random_division_stress()
    print()
    if ok1 and ok2 and ok3:
        print("All tests PASSED")
    else:
        raise SystemExit(1)
