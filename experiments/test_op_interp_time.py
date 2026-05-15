"""
INTERP_TIME Opcode Quality Test (Cedar Engine)
==============================================
Validates the time-based interpolator's timing accuracy, curve shapes,
stereo independence, and edge cases (NaN target, time=0 passthrough,
first-sample init, mid-ramp retarget).

Per PRD docs/prd-glide-interp.md §9.1 — kernel test phase.
"""

import json
import math
import os
import sys

import numpy as np
import matplotlib.pyplot as plt
import scipy.io.wavfile

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder
from visualize import save_figure

OUT = output_dir("op_interp_time")
SR = 48000


# =============================================================================
# Helpers
# =============================================================================

def _build_mono_host(time_sec: float, rate: int, state_name: str) -> CedarTestHost:
    """Wire INTERP_TIME with a constant `time_sec`. Input target arrives via
    buffer 0 (process()'s input). Output is OUTPUT-routed to stereo output_left
    (we read .process()'s L return)."""
    host = CedarTestHost(SR)
    buf_time = host.set_param("time", time_sec)
    buf_in = 0
    buf_out = 2  # writes to 2 (L) and 3 (R)
    inst = cedar.Instruction.make_binary(
        cedar.Opcode.INTERP_TIME, buf_out, buf_in, buf_time, cedar.hash(state_name)
    )
    inst.rate = rate
    host.load_instruction(inst)
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )
    return host


def _build_stereo_host(time_sec: float, rate: int, state_name: str) -> CedarTestHost:
    """Stereo: target L in buffer 0, target R in buffer 1. STEREO_INPUT flag
    on INTERP_TIME tells the opcode to read inputs[0]+1 as the R target."""
    host = CedarTestHost(SR)
    buf_time = host.set_param("time", time_sec)
    buf_in = 0  # also implicitly reads buf 1 for R
    buf_out = 2
    inst = cedar.Instruction.make_binary(
        cedar.Opcode.INTERP_TIME, buf_out, buf_in, buf_time, cedar.hash(state_name)
    )
    inst.rate = rate
    inst.flags = cedar.STEREO_INPUT_FLAG
    host.load_instruction(inst)
    host.load_instruction(
        cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, buf_out, buf_out + 1)
    )
    return host


def _save_wav(name: str, sig: np.ndarray) -> str:
    """Save mono WAV (normalize-free; signal expected in plausible range)."""
    path = os.path.join(OUT, name)
    scipy.io.wavfile.write(path, SR, sig.astype(np.float32))
    return path


def _step_input(num_samples: int, step_at: int, lo: float, hi: float) -> np.ndarray:
    a = np.full(num_samples, lo, dtype=np.float32)
    a[step_at:] = hi
    return a


# Curve shape references (must match op_interp_time SHAPE expressions).
def _shape_linear(u):    return u
def _shape_ease_in(u):   return u * u
def _shape_ease_out(u):  return 1.0 - (1.0 - u) * (1.0 - u)
def _shape_cosine(u):    return 0.5 * (1.0 - np.cos(np.pi * u))


# =============================================================================
# Sub-tests
# =============================================================================

def test_linear_ramp_duration():
    """#1: Linear ramp duration.
    Step 0→1 with time=0.1s should take exactly 4800 samples (±1) to reach 1.0
    at sr=48k. The first sample after the change emits start+0*(end-start)=0;
    sample N=4800 after the change emits state.end exactly.
    """
    print("\n  #1: Linear ramp duration (0→1, time=0.1s)")
    host = _build_mono_host(time_sec=0.1, rate=0, state_name="t1_lin_dur")
    n = 8000
    step_at = 100
    inp = _step_input(n, step_at, 0.0, 1.0)
    out = host.process(inp)
    _save_wav("test1_linear_ramp.wav", out)

    # Find first sample after step_at where output == 1.0 (held).
    target_idx = None
    for i in range(step_at, n):
        if out[i] >= 1.0 - 1e-7:
            target_idx = i
            break
    samples_to_target = (target_idx - step_at) if target_idx is not None else -1
    expected = 4800
    ok = target_idx is not None and abs(samples_to_target - expected) <= 1
    status = "✓" if ok else "✗"
    print(f"    {status} reached 1.0 at sample {target_idx} "
          f"(= {samples_to_target} after step; expected {expected} ±1)")
    return {"name": "linear_ramp_duration", "passed": bool(ok),
            "samples_to_target": int(samples_to_target), "expected": expected}


def test_hold_at_target():
    """#2: After the ramp completes, output holds at target indefinitely
    (no drift, no overshoot, no oscillation).
    """
    print("\n  #2: Hold at target after ramp completes")
    host = _build_mono_host(time_sec=0.1, rate=0, state_name="t2_hold")
    n = 10000
    inp = _step_input(n, 100, 0.0, 1.0)
    out = host.process(inp)
    # Inspect samples well past the ramp end (4900 + slack to 10000).
    hold_region = out[6000:]
    max_dev = float(np.max(np.abs(hold_region - 1.0)))
    ok = max_dev <= 1e-7
    status = "✓" if ok else "✗"
    print(f"    {status} max deviation from 1.0 in hold region = {max_dev:.3e}")
    return {"name": "hold_at_target", "passed": bool(ok), "max_deviation": max_dev}


def test_mid_ramp_retarget():
    """#3: Re-anchor mid-ramp. Step 0→1 with time=0.2s (= 9600 samples),
    then at sample 4900 (50% into ramp) re-step to 0. Expected: the new ramp
    starts from the value the opcode was currently emitting (≈ 0.5) and
    descends monotonically (no jump).
    """
    print("\n  #3: Mid-ramp retarget at 50% (0→1→0, time=0.2s)")
    host = _build_mono_host(time_sec=0.2, rate=0, state_name="t3_mid")
    n = 20000
    inp = np.zeros(n, dtype=np.float32)
    inp[100:4900] = 1.0       # rising target
    inp[4900:] = 0.0          # falling target (re-step mid-ramp)
    # The above leaves inp[100..4899]=1.0 and inp[4900..]=0.0 — that's the
    # retarget edge at sample 4900 (4800 samples after the first step).
    out = host.process(inp)
    _save_wav("test3_mid_retarget.wav", out)

    # Output at the retarget moment should be ≈ 0.5 (half-way through the
    # 9600-sample ramp). Allow ±0.01 slack for fractional progress accumulation.
    value_at_retarget = float(out[4900])
    descent_ok = True
    last = value_at_retarget
    for i in range(4901, 4900 + 9600):
        if out[i] > last + 1e-6:  # strictly non-increasing (allow tiny float noise)
            descent_ok = False
            break
        last = out[i]
    val_ok = abs(value_at_retarget - 0.5) <= 0.01
    ok = val_ok and descent_ok
    status = "✓" if ok else "✗"
    print(f"    {status} value at retarget = {value_at_retarget:.4f} "
          f"(expected ≈ 0.5); monotonic descent = {descent_ok}")
    return {"name": "mid_ramp_retarget", "passed": bool(ok),
            "value_at_retarget": value_at_retarget, "descent_monotonic": descent_ok}


def test_curve_shape(rate: int, shape_fn, name: str):
    """#4-7: Curve shape matches expected formula to 1e-5 across the ramp."""
    print(f"\n  #{4 + rate}: {name} curve shape (rate={rate})")
    host = _build_mono_host(time_sec=0.1, rate=rate, state_name=f"curve_{name}")
    n = 8000
    step_at = 100
    inp = _step_input(n, step_at, 0.0, 1.0)
    out = host.process(inp)
    _save_wav(f"test{4 + rate}_{name}.wav", out)

    total = 4800
    max_err = 0.0
    for k in range(total):
        u = k / float(total)
        expected = shape_fn(u)
        actual = out[step_at + k]
        err = abs(actual - expected)
        if err > max_err:
            max_err = err
    ok = max_err <= 1e-5
    status = "✓" if ok else "✗"
    print(f"    {status} max abs error vs reference shape = {max_err:.3e}")
    return {"name": f"curve_{name}", "rate": rate, "passed": bool(ok),
            "max_error": max_err}


def test_stereo_independence():
    """#8: Stereo independence — L re-targets at sample 100, R at sample 5000.
    Each lane should ramp on its own schedule. We assert: at sample 4900, L is
    at target (1.0) and R is still at its initial 0.0 (not retargeted yet).
    """
    print("\n  #8: Stereo channel independence")
    host = _build_stereo_host(time_sec=0.1, rate=0, state_name="t8_stereo")
    n = 12000
    left_in = _step_input(n, 100, 0.0, 1.0)
    right_in = _step_input(n, 5000, 0.0, 1.0)
    out_l, out_r = host.process_stereo(left_in, right_in)
    _save_wav("test8_stereo_L.wav", out_l)
    _save_wav("test8_stereo_R.wav", out_r)

    # At sample 4900: L has finished its 4800-sample ramp (value = 1.0);
    # R is still holding at 0 (its ramp doesn't start until sample 5000).
    l_at = float(out_l[4900])
    r_at = float(out_r[4900])
    l_ok = abs(l_at - 1.0) <= 1e-6
    r_ok = abs(r_at - 0.0) <= 1e-6
    # Also verify R hits 1.0 at sample 5000 + 4800 = 9800.
    r_done = float(out_r[9800])
    r_done_ok = abs(r_done - 1.0) <= 1e-6
    ok = l_ok and r_ok and r_done_ok
    status = "✓" if ok else "✗"
    print(f"    {status} L@4900={l_at:.6f} (exp 1.0), R@4900={r_at:.6f} (exp 0.0), "
          f"R@9800={r_done:.6f} (exp 1.0)")
    return {"name": "stereo_independence", "passed": bool(ok),
            "L_at_4900": l_at, "R_at_4900": r_at, "R_at_9800": r_done}


def test_time_zero_passthrough():
    """#9: time = 0 → output equals target sample-exact."""
    print("\n  #9: time=0 passthrough (square target)")
    host = _build_mono_host(time_sec=0.0, rate=0, state_name="t9_zero")
    n = 4000
    inp = np.zeros(n, dtype=np.float32)
    for i in range(n):
        inp[i] = 1.0 if (i // 256) % 2 == 0 else -1.0
    out = host.process(inp)
    _save_wav("test9_passthrough_zero.wav", out)
    max_diff = float(np.max(np.abs(out - inp)))
    ok = max_diff <= 1e-7
    status = "✓" if ok else "✗"
    print(f"    {status} max |out - target| = {max_diff:.3e}")
    return {"name": "time_zero_passthrough", "passed": bool(ok),
            "max_diff": max_diff}


def test_nan_target_hold():
    """#10: NaN target → hold last good `end`. Then resume ramping when target
    becomes finite again.
    """
    print("\n  #10: NaN target holds, then resumes")
    host = _build_mono_host(time_sec=0.1, rate=0, state_name="t10_nan")
    n = 16000
    inp = np.zeros(n, dtype=np.float32)
    inp[100:5000] = 1.0          # ramp 0→1, then hold
    inp[5000:5100] = np.nan      # 100 NaN samples
    inp[5100:] = 2.0             # resume with new target
    out = host.process(inp)

    # During NaN window: output should hold at 1.0 (no NaN propagation).
    nan_region = out[5000:5100]
    hold_ok = (not np.any(np.isnan(nan_region))) and \
              (float(np.max(np.abs(nan_region - 1.0))) <= 1e-6)
    # After NaN window: ramp resumes 1→2 over 4800 samples.
    # At sample 5100 + 4800 = 9900, output should be ≈ 2.0.
    resume_val = float(out[9900])
    resume_ok = abs(resume_val - 2.0) <= 1e-6
    ok = hold_ok and resume_ok
    status = "✓" if ok else "✗"
    print(f"    {status} hold-during-NaN={hold_ok}, resume_val@9900={resume_val:.6f} "
          f"(expected 2.0)")
    return {"name": "nan_target_hold", "passed": bool(ok),
            "hold_during_nan": hold_ok, "resume_value": resume_val}


def test_long_stability():
    """Long-stability loop (per CLAUDE.md / PRD §9.1): simulate ≥ 300 s of
    audio looping the "100 ms ramp" event. Catches cumulative drift, NaN
    propagation, and state-pool churn. No WAV is written (file size); summary
    stats only.
    """
    print("\n  #11: Long-stability loop (≥ 300 s simulated audio)")
    duration_sec = 305.0
    n = int(duration_sec * SR)
    # Build an input that toggles between 0 and 1 every 4800 samples (= 100 ms).
    # This means every ramp completes exactly before the next retarget — covers
    # both the "ramp ongoing" and "ramp completed → next event" code paths.
    period = 4800
    inp = np.empty(n, dtype=np.float32)
    for i in range(n):
        inp[i] = 1.0 if (i // period) % 2 == 0 else 0.0
    host = _build_mono_host(time_sec=0.1, rate=0, state_name="t11_long")
    out = host.process(inp)

    nan_count = int(np.sum(np.isnan(out)))
    inf_count = int(np.sum(np.isinf(out)))
    max_val = float(np.max(out)) if out.size else 0.0
    min_val = float(np.min(out)) if out.size else 0.0
    # Output must stay in [0, 1] (modulo tiny float noise — but with linear
    # interp between 0 and 1 there should be NO overshoot).
    bounds_ok = (max_val <= 1.0 + 1e-6) and (min_val >= -1e-6)
    ok = (nan_count == 0) and (inf_count == 0) and bounds_ok
    status = "✓" if ok else "✗"
    print(f"    {status} {duration_sec:.0f}s simulated; samples={n:,}; "
          f"NaN={nan_count}, inf={inf_count}, range=[{min_val:.6f}, {max_val:.6f}]")
    return {"name": "long_stability", "passed": bool(ok),
            "duration_sec": duration_sec, "samples": n,
            "nan_count": nan_count, "inf_count": inf_count,
            "min": min_val, "max": max_val}


# =============================================================================
# Plot panel
# =============================================================================

def render_timing_panel():
    """Render a 2x2 panel showing the four curve shapes side-by-side over a
    100ms 0→1 ramp. Visual sanity check + reference image."""
    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    curves = [
        (0, "linear", axes[0, 0]),
        (1, "ease_in", axes[0, 1]),
        (2, "ease_out", axes[1, 0]),
        (3, "cosine", axes[1, 1]),
    ]
    for rate, name, ax in curves:
        host = _build_mono_host(time_sec=0.1, rate=rate, state_name=f"plot_{name}")
        n = 8000
        inp = _step_input(n, 100, 0.0, 1.0)
        out = host.process(inp)
        t_ms = np.arange(n) / SR * 1000.0
        ax.plot(t_ms, inp, "g--", linewidth=0.8, alpha=0.5, label="target")
        ax.plot(t_ms, out, "b-", linewidth=1.2, label="output")
        ax.set_title(f"{name} (rate={rate})")
        ax.set_xlabel("Time (ms)")
        ax.set_ylabel("Level")
        ax.set_xlim(0, 150)
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=8)
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "timing.png"))
    print(f"\n  Saved: {os.path.join(OUT, 'timing.png')}")


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar INTERP_TIME Opcode Quality Test")
    print("=" * 60)

    results = []
    results.append(test_linear_ramp_duration())
    results.append(test_hold_at_target())
    results.append(test_mid_ramp_retarget())
    results.append(test_curve_shape(0, _shape_linear,  "linear"))
    results.append(test_curve_shape(1, _shape_ease_in, "ease_in"))
    results.append(test_curve_shape(2, _shape_ease_out, "ease_out"))
    results.append(test_curve_shape(3, _shape_cosine,  "cosine"))
    results.append(test_stereo_independence())
    results.append(test_time_zero_passthrough())
    results.append(test_nan_target_hold())
    results.append(test_long_stability())

    render_timing_panel()

    with open(os.path.join(OUT, "timing.json"), "w") as f:
        json.dump({"sample_rate": SR, "tests": results}, f,
                  indent=2, cls=NumpyEncoder)

    print("\n" + "=" * 60)
    passed = sum(1 for r in results if r["passed"])
    total = len(results)
    print(f"INTERP_TIME test complete: {passed}/{total} sub-tests passed")
    print(f"  Saved: {os.path.join(OUT, 'timing.json')}")
    sys.exit(0 if passed == total else 1)
