"""
Test: PAN_STEREO (opcode 176) — equal-power stereo balance
==========================================================
Verifies the DAW-style stereo balance opcode dispatched by the overloaded
`pan(stereo, pos)` Akkado builtin (handle_pan_call in codegen_stereo.cpp).

Expected behavior (per cedar/include/cedar/opcodes/stereo.hpp:147-162):
- inputs[0] = L, inputs[1] = R, inputs[2] = pos in [-1, +1]
- angle = (pos + 1) * π/4   →  maps [-1, +1] to [0, π/2]
- L_out = L_in * cos(angle)
- R_out = R_in * sin(angle)
- At pos = -1: cos=1, sin=0  →  R silenced, L passes through unity.
- At pos =  0: cos=sin=√½    →  both channels at ~0.7071 (-3 dB).
- At pos = +1: cos=0, sin=1  →  L silenced, R passes through unity.
- Per-channel-input-RMS power preservation: L_out_rms² + R_out_rms² ≈
  L_in_rms² (assuming L_in == R_in) — i.e. the gain pair traces the unit
  circle as pos sweeps from -1 to +1.

If this test fails, check op_pan_stereo() in
cedar/include/cedar/opcodes/stereo.hpp:147.
"""

import os
import numpy as np

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav
from visualize import save_figure
import matplotlib.pyplot as plt

OUT = output_dir("op_pan_stereo")
SR = 48000


def _build_pan_program(host: CedarTestHost) -> None:
    """Load a one-instruction PAN_STEREO program.

    Buffer plan:
      buf 4 = L input, buf 5 = R input, buf 6 = pos.
      out_buffer = 8 → writes L' at 8, R' at 9 (implicit adjacency).
    """
    pan_inst = cedar.Instruction.make_ternary(
        cedar.Opcode.PAN_STEREO, 8, 4, 5, 6, 0
    )
    program = [
        pan_inst,
        cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 8, 9),
    ]
    host.vm.load_program(program)


def _run_constant_pos(pos: float, n_blocks: int = 8) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Run PAN_STEREO with identical sine on L and R and a fixed `pos`.

    Returns (input_lr_signal, out_left, out_right).
    """
    host = CedarTestHost(SR)
    _build_pan_program(host)

    t = np.arange(cedar.BLOCK_SIZE) / SR
    # Frequency well above DC, picked so a few blocks contain whole cycles.
    sine_block = np.sin(2 * np.pi * 440.0 * t).astype(np.float32)
    pos_block = np.full(cedar.BLOCK_SIZE, pos, dtype=np.float32)

    in_acc, l_acc, r_acc = [], [], []
    for _ in range(n_blocks):
        host.vm.set_buffer(4, sine_block)
        host.vm.set_buffer(5, sine_block)
        host.vm.set_buffer(6, pos_block)
        l, r = host.vm.process()
        in_acc.append(sine_block.copy())
        l_acc.append(l.copy())
        r_acc.append(r.copy())

    return (
        np.concatenate(in_acc),
        np.concatenate(l_acc),
        np.concatenate(r_acc),
    )


def _rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))


def test_endpoint_spot_check():
    """Pan at -1, 0, +1 — verify silencing and centre attenuation."""
    print("Test 1: endpoint spot check (pos = -1, 0, +1)")
    pass_count = 0

    for pos, expected_l_gain, expected_r_gain, label in [
        (-1.0, 1.0, 0.0, "hard left"),
        ( 0.0, np.cos(np.pi / 4), np.sin(np.pi / 4), "centre (-3 dB)"),
        (+1.0, 0.0, 1.0, "hard right"),
    ]:
        in_sig, out_l, out_r = _run_constant_pos(pos)
        in_rms = _rms(in_sig)
        l_rms = _rms(out_l)
        r_rms = _rms(out_r)
        l_gain = l_rms / in_rms if in_rms > 0 else 0.0
        r_gain = r_rms / in_rms if in_rms > 0 else 0.0

        # Tolerances: 0.5% on the active channel, float-eps on the silenced one.
        l_ok = (abs(l_gain - expected_l_gain) < 0.005
                if expected_l_gain > 0
                else l_rms < 1e-6)
        r_ok = (abs(r_gain - expected_r_gain) < 0.005
                if expected_r_gain > 0
                else r_rms < 1e-6)

        status = "✓ PASS" if (l_ok and r_ok) else "✗ FAIL"
        print(f"  pos={pos:+.1f} ({label}): "
              f"L gain {l_gain:.4f} (expect {expected_l_gain:.4f})  "
              f"R gain {r_gain:.4f} (expect {expected_r_gain:.4f})  {status}")
        if l_ok and r_ok:
            pass_count += 1

    if pass_count == 3:
        print("  ✓ PASS: all three endpoints match equal-power law")
    else:
        print(f"  ✗ FAIL: {3 - pass_count}/3 endpoints diverged — check op_pan_stereo()")


def test_equal_power_law_sweep():
    """21 positions across [-1, +1]: assert L²+R² ≈ const within ±0.5 dB."""
    print("Test 2: equal-power law sweep (21 positions, identical L=R input)")
    positions = np.linspace(-1.0, +1.0, 21)
    measured_l = np.zeros_like(positions)
    measured_r = np.zeros_like(positions)
    input_rms_ref = None

    for i, pos in enumerate(positions):
        in_sig, out_l, out_r = _run_constant_pos(float(pos))
        if input_rms_ref is None:
            input_rms_ref = _rms(in_sig)
        measured_l[i] = _rms(out_l) / input_rms_ref
        measured_r[i] = _rms(out_r) / input_rms_ref

    expected_l = np.cos((positions + 1.0) * np.pi / 4.0)
    expected_r = np.sin((positions + 1.0) * np.pi / 4.0)

    # Power-sum: should be ≈ 1.0 at every position (cos² + sin² = 1).
    power_sum = measured_l ** 2 + measured_r ** 2
    # ±0.5 dB tolerance → linear factor between 10^(-0.05) ≈ 0.944 and 10^(0.05) ≈ 1.059.
    tol_lin = 10 ** (0.5 / 20.0)
    sum_ok = np.all((power_sum > 1.0 / tol_lin) & (power_sum < tol_lin))

    # Curve match: ±0.005 absolute on each channel.
    curve_ok = (np.max(np.abs(measured_l - expected_l)) < 0.005
                and np.max(np.abs(measured_r - expected_r)) < 0.005)

    sum_min, sum_max = float(np.min(power_sum)), float(np.max(power_sum))
    print(f"  L²+R² range: [{sum_min:.4f}, {sum_max:.4f}]  "
          f"(target ≈ 1.000, tol ±0.5 dB)")
    print(f"  max |L − cos(angle)| = {np.max(np.abs(measured_l - expected_l)):.4f}")
    print(f"  max |R − sin(angle)| = {np.max(np.abs(measured_r - expected_r)):.4f}")

    if sum_ok and curve_ok:
        print("  ✓ PASS: equal-power law holds across full pan range")
    else:
        bad_what = []
        if not sum_ok:
            bad_what.append("power-sum drift")
        if not curve_ok:
            bad_what.append("gain-curve mismatch")
        print(f"  ✗ FAIL: {', '.join(bad_what)} — check op_pan_stereo()")

    # Plot measured vs reference cos/sin curves.
    fig, (ax_gain, ax_pow) = plt.subplots(2, 1, figsize=(10, 8), sharex=True)
    pos_dense = np.linspace(-1, 1, 401)
    ax_gain.plot(pos_dense, np.cos((pos_dense + 1) * np.pi / 4), 'C0--',
                 alpha=0.5, label='cos((pos+1)·π/4) reference')
    ax_gain.plot(pos_dense, np.sin((pos_dense + 1) * np.pi / 4), 'C1--',
                 alpha=0.5, label='sin((pos+1)·π/4) reference')
    ax_gain.plot(positions, measured_l, 'C0o', label='measured L gain')
    ax_gain.plot(positions, measured_r, 'C1o', label='measured R gain')
    ax_gain.set_ylabel('gain (linear)')
    ax_gain.set_title('PAN_STEREO equal-power law')
    ax_gain.legend()
    ax_gain.set_ylim(-0.05, 1.1)

    ax_pow.axhline(1.0, color='k', linestyle='--', alpha=0.4, label='unity power')
    ax_pow.axhline(1.0 / tol_lin, color='r', linestyle=':', alpha=0.4,
                   label='±0.5 dB tolerance')
    ax_pow.axhline(tol_lin, color='r', linestyle=':', alpha=0.4)
    ax_pow.plot(positions, power_sum, 'C2o-', label='measured L²+R²')
    ax_pow.set_xlabel('pos')
    ax_pow.set_ylabel('power sum')
    ax_pow.set_ylim(0.9, 1.1)
    ax_pow.legend()

    fig_path = os.path.join(OUT, "equal_power_law.png")
    save_figure(fig, fig_path)
    print(f"  Saved {fig_path}")


def test_audible_position_sweep_wav():
    """Render 5 s of audio with pos ramping -1 → +1 for human listening."""
    print("Test 3: audible position sweep WAV (pos ramps -1 → +1 over 5 s)")
    duration_s = 5.0
    n_samples = int(duration_s * SR)
    n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    padded_len = n_blocks * cedar.BLOCK_SIZE

    # Source: dual-mono pink-ish noise so the balance is easy to hear.
    rng = np.random.default_rng(0xCAFE)
    src = rng.uniform(-0.4, 0.4, padded_len).astype(np.float32)
    pos_ramp = np.linspace(-1.0, +1.0, padded_len, dtype=np.float32)

    host = CedarTestHost(SR)
    _build_pan_program(host)

    out_l_acc, out_r_acc = [], []
    for i in range(n_blocks):
        s = i * cedar.BLOCK_SIZE
        e = s + cedar.BLOCK_SIZE
        host.vm.set_buffer(4, src[s:e])
        host.vm.set_buffer(5, src[s:e])
        host.vm.set_buffer(6, pos_ramp[s:e])
        l, r = host.vm.process()
        out_l_acc.append(l.copy())
        out_r_acc.append(r.copy())

    out_l = np.concatenate(out_l_acc)[:n_samples]
    out_r = np.concatenate(out_r_acc)[:n_samples]
    stereo = np.column_stack([out_l, out_r])

    wav_path = os.path.join(OUT, "pos_sweep.wav")
    save_wav(wav_path, stereo, SR)
    print(f"  Saved {wav_path} - Listen for smooth equal-power balance pan "
          f"from left to right with no clicks at endpoints.")


if __name__ == "__main__":
    print("=" * 60)
    print("PAN_STEREO (opcode 176) TESTS")
    print("=" * 60)
    print()
    test_endpoint_spot_check()
    print()
    test_equal_power_law_sweep()
    print()
    test_audible_position_sweep_wav()
    print()
    print("=" * 60)
    print("PAN_STEREO TESTS COMPLETE")
    print("=" * 60)
