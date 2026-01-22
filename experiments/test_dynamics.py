"""
Dynamics Processor Quality Tests (Cedar Engine)
================================================
Tests for DYNAMICS_COMP, DYNAMICS_LIMITER, and DYNAMICS_GATE opcodes.
Validates threshold accuracy, ratio, attack/release timing, and gain reduction.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost
from visualize import save_figure
from utils import db_to_linear, linear_to_db


class NumpyEncoder(json.JSONEncoder):
    """JSON encoder that handles numpy types."""
    def default(self, obj):
        if isinstance(obj, np.integer):
            return int(obj)
        if isinstance(obj, np.floating):
            return float(obj)
        if isinstance(obj, np.ndarray):
            return obj.tolist()
        if isinstance(obj, np.bool_):
            return bool(obj)
        return super().default(obj)


def gen_test_tone(freq, duration, sr, amplitude=1.0):
    """Generate a test sine tone."""
    t = np.arange(int(duration * sr)) / sr
    return (np.sin(2 * np.pi * freq * t) * amplitude).astype(np.float32)


def gen_level_sweep(duration, sr, start_db=-60, end_db=0, freq=1000):
    """Generate a tone with linearly increasing level in dB."""
    t = np.arange(int(duration * sr)) / sr
    # Linear ramp in dB space
    db_levels = np.linspace(start_db, end_db, len(t))
    amplitudes = db_to_linear(db_levels)
    return (np.sin(2 * np.pi * freq * t) * amplitudes).astype(np.float32)


def measure_rms_blocks(signal, sr, block_ms=50):
    """Measure RMS level in blocks."""
    block_samples = int(block_ms / 1000 * sr)
    num_blocks = len(signal) // block_samples
    rms_values = []
    for i in range(num_blocks):
        start = i * block_samples
        end = start + block_samples
        block = signal[start:end]
        rms = np.sqrt(np.mean(block ** 2))
        rms_values.append(rms)
    return np.array(rms_values)


# =============================================================================
# 1. DYNAMICS_COMP Test - Compressor Ratio and Threshold
# =============================================================================

def test_compressor_ratio():
    """
    Test compressor reduces gain above threshold by ratio.
    - Input: 1kHz tone at varying levels (-60dB to 0dB)
    - Settings: threshold=-20dB, ratio=4:1
    - Above threshold, +4dB in yields +1dB out
    """
    print("Test 1: DYNAMICS_COMP (Compressor) Ratio and Threshold")
    print("=" * 60)

    sr = 48000
    duration = 5.0
    freq = 1000

    # Compressor settings
    threshold_db = -20
    ratio = 4.0
    attack_ms = 10
    release_ms = 100

    results = {'sample_rate': sr, 'tests': [], 'transfer_curve': []}

    # Test transfer curve: measure output for various input levels
    test_levels_db = np.arange(-50, 1, 2)  # -50dB to 0dB in 2dB steps

    print("\n  Transfer Curve Measurement:")
    print(f"    Threshold: {threshold_db}dB, Ratio: {ratio}:1")
    print(f"    Attack: {attack_ms}ms, Release: {release_ms}ms")

    input_rms_db = []
    output_rms_db = []

    for level_db in test_levels_db:
        host = CedarTestHost(sr)

        amplitude = db_to_linear(level_db)
        test_signal = gen_test_tone(freq, 0.5, sr, amplitude)

        # Set compressor parameters
        buf_thresh = host.set_param("threshold", threshold_db)
        buf_ratio = host.set_param("ratio", ratio)
        buf_in = 0
        buf_out = 1

        # Pack attack/release into rate parameter
        # rate = (attack_idx << 4) | release_idx
        # Convert ms to 0-15 index (assumes specific mapping in DSP)
        attack_idx = min(15, int(attack_ms / 10))
        release_idx = min(15, int(release_ms / 50))
        rate = (attack_idx << 4) | release_idx

        # DYNAMICS_COMP: out = comp(in, threshold, ratio)
        inst = cedar.Instruction.make_ternary(
            cedar.Opcode.DYNAMICS_COMP, buf_out, buf_in, buf_thresh, buf_ratio, cedar.hash("comp") & 0xFFFF
        )
        inst.rate = rate
        host.load_instruction(inst)
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        output = host.process(test_signal)

        # Measure RMS of steady-state portion (skip attack)
        steady_start = int(0.2 * sr)  # Skip first 200ms
        in_rms = np.sqrt(np.mean(test_signal[steady_start:] ** 2))
        out_rms = np.sqrt(np.mean(output[steady_start:] ** 2))

        in_db = linear_to_db(in_rms)
        out_db = linear_to_db(out_rms)

        input_rms_db.append(in_db)
        output_rms_db.append(out_db)

        results['transfer_curve'].append({
            'input_db': float(in_db),
            'output_db': float(out_db)
        })

    # Calculate expected transfer curve
    expected_output_db = []
    for in_db in input_rms_db:
        if in_db <= threshold_db:
            expected_output_db.append(in_db)
        else:
            # Above threshold: out = threshold + (in - threshold) / ratio
            expected = threshold_db + (in_db - threshold_db) / ratio
            expected_output_db.append(expected)

    # Calculate error
    errors = np.abs(np.array(output_rms_db) - np.array(expected_output_db))
    max_error = np.max(errors)
    avg_error = np.mean(errors)

    # Check gain reduction above threshold
    above_thresh_mask = np.array(input_rms_db) > threshold_db
    if np.any(above_thresh_mask):
        gr_errors = errors[above_thresh_mask]
        gr_max_error = np.max(gr_errors)
        gr_passed = gr_max_error < 3.0  # Allow 3dB tolerance for GR
    else:
        gr_passed = True
        gr_max_error = 0

    results['tests'].append({
        'name': 'Transfer curve',
        'max_error_db': float(max_error),
        'avg_error_db': float(avg_error),
        'gain_reduction_max_error': float(gr_max_error),
        'passed': gr_passed
    })

    status = "PASS" if gr_passed else "FAIL"
    print(f"\n    Max error: {max_error:.2f}dB")
    print(f"    Avg error: {avg_error:.2f}dB")
    print(f"    GR accuracy: {gr_max_error:.2f}dB [{status}]")

    # Test attack/release timing
    print("\n  Attack/Release Timing:")

    # Create signal with sudden level change for timing test
    timing_dur = 1.0
    timing_signal = np.zeros(int(timing_dur * sr), dtype=np.float32)
    # Low level for first 0.3s, then high level
    low_amp = db_to_linear(-40)
    high_amp = db_to_linear(-6)  # Well above threshold
    transition_sample = int(0.3 * sr)

    t_low = np.arange(transition_sample) / sr
    t_high = np.arange(int(timing_dur * sr) - transition_sample) / sr
    timing_signal[:transition_sample] = np.sin(2 * np.pi * freq * t_low) * low_amp
    timing_signal[transition_sample:] = np.sin(2 * np.pi * freq * t_high) * high_amp

    host2 = CedarTestHost(sr)
    buf_thresh2 = host2.set_param("threshold", threshold_db)
    buf_ratio2 = host2.set_param("ratio", ratio)
    buf_out2 = 1

    inst2 = cedar.Instruction.make_ternary(
        cedar.Opcode.DYNAMICS_COMP, buf_out2, 0, buf_thresh2, buf_ratio2, cedar.hash("comp2") & 0xFFFF
    )
    inst2.rate = rate
    host2.load_instruction(inst2)
    host2.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out2)
    )

    timing_output = host2.process(timing_signal)

    # Measure envelope of output
    env_in = np.abs(timing_signal)
    env_out = np.abs(timing_output)

    # Visualization
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Transfer curve
    ax1 = axes[0, 0]
    ax1.plot(input_rms_db, input_rms_db, 'k--', alpha=0.3, label='Unity (no compression)')
    ax1.plot(input_rms_db, expected_output_db, 'g-', linewidth=2, label='Expected')
    ax1.plot(input_rms_db, output_rms_db, 'b.', markersize=8, label='Measured')
    ax1.axvline(threshold_db, color='red', linestyle=':', alpha=0.5, label=f'Threshold={threshold_db}dB')
    ax1.axhline(threshold_db, color='red', linestyle=':', alpha=0.5)
    ax1.set_xlabel('Input Level (dB)')
    ax1.set_ylabel('Output Level (dB)')
    ax1.set_title(f'Compressor Transfer Curve (Ratio {ratio}:1)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    ax1.set_xlim(-55, 5)
    ax1.set_ylim(-55, 5)

    # Error plot
    ax2 = axes[0, 1]
    ax2.plot(input_rms_db, errors, 'b-', linewidth=1)
    ax2.axhline(3.0, color='red', linestyle='--', alpha=0.5, label='3dB tolerance')
    ax2.axvline(threshold_db, color='orange', linestyle=':', alpha=0.5, label='Threshold')
    ax2.set_xlabel('Input Level (dB)')
    ax2.set_ylabel('Error (dB)')
    ax2.set_title('Transfer Curve Error')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    # Timing waveform
    ax3 = axes[1, 0]
    time_ms = np.arange(len(timing_signal)) / sr * 1000
    ax3.plot(time_ms, timing_signal, 'g-', linewidth=0.3, alpha=0.5, label='Input')
    ax3.plot(time_ms, timing_output, 'b-', linewidth=0.3, alpha=0.7, label='Output')
    ax3.axvline(transition_sample / sr * 1000, color='red', linestyle=':', alpha=0.7, label='Level change')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Amplitude')
    ax3.set_title('Attack/Release Response')
    ax3.legend()
    ax3.grid(True, alpha=0.3)

    # Gain reduction over time
    ax4 = axes[1, 1]
    # Calculate instantaneous GR (smoothed)
    window = int(0.01 * sr)  # 10ms window
    if window > 0:
        env_in_smooth = np.convolve(np.abs(timing_signal), np.ones(window)/window, mode='same')
        env_out_smooth = np.convolve(np.abs(timing_output), np.ones(window)/window, mode='same')
        gr_db = linear_to_db(env_out_smooth + 1e-10) - linear_to_db(env_in_smooth + 1e-10)
        ax4.plot(time_ms, gr_db, 'b-', linewidth=1)
        ax4.axhline(0, color='gray', linestyle='--', alpha=0.5)
        ax4.axvline(transition_sample / sr * 1000, color='red', linestyle=':', alpha=0.7)
    ax4.set_xlabel('Time (ms)')
    ax4.set_ylabel('Gain Reduction (dB)')
    ax4.set_title('Gain Reduction vs Time')
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/compressor_curve.png')
    print(f"\n  Saved: output/compressor_curve.png")

    with open('output/compressor_curve.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/compressor_curve.json")

    return results


# =============================================================================
# 2. DYNAMICS_LIMITER Test - Ceiling and Peak Prevention
# =============================================================================

def test_limiter_ceiling():
    """
    Test limiter prevents output from exceeding ceiling.
    - Input: sine with peaks at +6dB
    - Settings: ceiling=-3dB
    - Output should never exceed ceiling
    """
    print("\nTest 2: DYNAMICS_LIMITER (Limiter) Ceiling")
    print("=" * 60)

    sr = 48000

    # Limiter settings
    ceiling_db = -3
    ceiling_linear = db_to_linear(ceiling_db)
    release_ms = 100

    results = {'sample_rate': sr, 'tests': []}

    # Test with various input levels above ceiling
    test_cases = [
        (-6, "6dB above ceiling"),
        (0, "3dB above ceiling"),
        (-3, "At ceiling"),
        (-10, "Below ceiling"),
    ]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    for idx, (input_db, name) in enumerate(test_cases):
        print(f"\n  {name} (input peak: {input_db}dB, ceiling: {ceiling_db}dB):")

        host = CedarTestHost(sr)

        # Generate test signal
        amplitude = db_to_linear(input_db)
        duration = 0.5
        freq = 1000
        test_signal = gen_test_tone(freq, duration, sr, amplitude)

        # Set limiter parameters
        buf_ceiling = host.set_param("ceiling", ceiling_db)
        buf_release = host.set_param("release", release_ms)
        buf_in = 0
        buf_out = 1

        # DYNAMICS_LIMITER: out = limiter(in, ceiling, release)
        host.load_instruction(
            cedar.Instruction.make_ternary(
                cedar.Opcode.DYNAMICS_LIMITER, buf_out, buf_in, buf_ceiling, buf_release,
                cedar.hash("limiter") & 0xFFFF
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        output = host.process(test_signal)

        # Measure peak output
        peak_out = np.max(np.abs(output))
        peak_out_db = linear_to_db(peak_out)

        # Check if ceiling is respected
        # Allow small overshoot (0.5dB) for non-lookahead limiters
        overshoot_tolerance = 0.5
        ceiling_respected = peak_out_db <= ceiling_db + overshoot_tolerance

        # Calculate actual overshoot
        overshoot_db = max(0, peak_out_db - ceiling_db)

        test_result = {
            'name': name,
            'input_peak_db': input_db,
            'output_peak_db': float(peak_out_db),
            'overshoot_db': float(overshoot_db),
            'ceiling_respected': ceiling_respected
        }
        results['tests'].append(test_result)

        status = "PASS" if ceiling_respected else "FAIL"
        print(f"    Input peak:  {input_db:.1f}dB")
        print(f"    Output peak: {peak_out_db:.2f}dB")
        print(f"    Overshoot:   {overshoot_db:.2f}dB [{status}]")

        # Plot
        ax = axes[idx // 2, idx % 2]
        time_ms = np.arange(len(output)) / sr * 1000
        ax.plot(time_ms, test_signal, 'g-', linewidth=0.5, alpha=0.5, label='Input')
        ax.plot(time_ms, output, 'b-', linewidth=0.5, alpha=0.8, label='Output')
        ax.axhline(ceiling_linear, color='red', linestyle='--', alpha=0.7,
                   label=f'Ceiling={ceiling_db}dB')
        ax.axhline(-ceiling_linear, color='red', linestyle='--', alpha=0.7)
        ax.set_xlabel('Time (ms)')
        ax.set_ylabel('Amplitude')
        ax.set_title(f'{name}')
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/limiter_ceiling.png')
    print(f"\n  Saved: output/limiter_ceiling.png")

    # Test transient response
    print("\n  Transient Response Test:")

    host2 = CedarTestHost(sr)

    # Create signal with sudden transient
    transient_dur = 0.5
    transient_signal = np.zeros(int(transient_dur * sr), dtype=np.float32)
    t = np.arange(len(transient_signal)) / sr

    # Low level background with high transient peaks
    background_amp = db_to_linear(-20)
    peak_amp = db_to_linear(0)  # 3dB above ceiling

    # Background tone
    transient_signal = np.sin(2 * np.pi * 1000 * t) * background_amp

    # Add transient peaks
    for peak_time in [0.1, 0.2, 0.3, 0.4]:
        peak_sample = int(peak_time * sr)
        # Sharp attack, slow decay transient
        env_len = int(0.01 * sr)
        for i in range(env_len):
            if peak_sample + i < len(transient_signal):
                env = np.exp(-i / (env_len / 5))
                transient_signal[peak_sample + i] += peak_amp * env * np.sin(2 * np.pi * 2000 * (i / sr))

    buf_ceiling2 = host2.set_param("ceiling", ceiling_db)
    buf_release2 = host2.set_param("release", release_ms)
    buf_out2 = 1

    host2.load_instruction(
        cedar.Instruction.make_ternary(
            cedar.Opcode.DYNAMICS_LIMITER, buf_out2, 0, buf_ceiling2, buf_release2,
            cedar.hash("limiter2") & 0xFFFF
        )
    )
    host2.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out2)
    )

    transient_output = host2.process(transient_signal)

    peak_transient_out = np.max(np.abs(transient_output))
    peak_transient_db = linear_to_db(peak_transient_out)
    transient_passed = peak_transient_db <= ceiling_db + 1.0  # Allow 1dB for transients

    results['tests'].append({
        'name': 'Transient handling',
        'peak_transient_db': float(peak_transient_db),
        'passed': transient_passed
    })

    print(f"    Peak with transients: {peak_transient_db:.2f}dB")
    print(f"    Transient test: {'PASS' if transient_passed else 'FAIL'}")

    with open('output/limiter_ceiling.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/limiter_ceiling.json")

    return results


# =============================================================================
# 3. DYNAMICS_GATE Test - Threshold and Attenuation
# =============================================================================

def test_gate_threshold():
    """
    Test gate attenuates signal below threshold.
    - Input: tone with varying levels
    - Settings: threshold=-40dB, range=-80dB (full gate)
    - Signal below threshold should be attenuated by range amount
    """
    print("\nTest 3: DYNAMICS_GATE (Gate) Threshold")
    print("=" * 60)

    sr = 48000

    # Gate settings
    threshold_db = -40
    range_db = -80  # Full gate
    attack_ms = 1
    hold_ms = 50
    release_ms = 100

    results = {'sample_rate': sr, 'tests': []}

    print(f"\n  Gate Settings:")
    print(f"    Threshold: {threshold_db}dB")
    print(f"    Range: {range_db}dB")
    print(f"    Attack: {attack_ms}ms, Hold: {hold_ms}ms, Release: {release_ms}ms")

    # Test with bursts of signal above and below threshold
    duration = 2.0
    num_samples = int(duration * sr)
    t = np.arange(num_samples) / sr

    # Create test signal: alternating loud and quiet sections
    test_signal = np.zeros(num_samples, dtype=np.float32)
    freq = 1000

    # Loud bursts (above threshold)
    loud_amp = db_to_linear(-20)  # 20dB above threshold
    quiet_amp = db_to_linear(-50)  # 10dB below threshold

    burst_times = [
        (0.1, 0.3, loud_amp, "loud"),
        (0.4, 0.5, quiet_amp, "quiet"),
        (0.6, 0.8, loud_amp, "loud"),
        (0.9, 1.0, quiet_amp, "quiet"),
        (1.1, 1.5, loud_amp, "loud"),
        (1.6, 1.9, quiet_amp, "quiet"),
    ]

    for start, end, amp, _ in burst_times:
        start_sample = int(start * sr)
        end_sample = int(end * sr)
        t_burst = np.arange(end_sample - start_sample) / sr
        test_signal[start_sample:end_sample] = np.sin(2 * np.pi * freq * t_burst) * amp

    host = CedarTestHost(sr)

    # Set gate parameters
    buf_thresh = host.set_param("threshold", threshold_db)
    buf_range = host.set_param("range", range_db)
    buf_in = 0
    buf_out = 1

    # Pack timing into rate parameter
    # rate = (attack << 6) | (hold << 4) | release (simplified encoding)
    attack_idx = min(3, attack_ms // 5)
    hold_idx = min(3, hold_ms // 50)
    release_idx = min(15, release_ms // 25)
    rate = (attack_idx << 6) | (hold_idx << 4) | release_idx

    # DYNAMICS_GATE: out = gate(in, threshold, range)
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.DYNAMICS_GATE, buf_out, buf_in, buf_thresh, buf_range,
        cedar.hash("gate") & 0xFFFF
    )
    inst.rate = rate
    host.load_instruction(inst)
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    output = host.process(test_signal)

    # Analyze each burst region
    print("\n  Burst Analysis:")

    for start, end, amp, burst_type in burst_times:
        start_sample = int((start + 0.05) * sr)  # Skip attack transient
        end_sample = int((end - 0.05) * sr)  # Skip release

        if start_sample >= end_sample:
            continue

        in_rms = np.sqrt(np.mean(test_signal[start_sample:end_sample] ** 2))
        out_rms = np.sqrt(np.mean(output[start_sample:end_sample] ** 2))

        in_db = linear_to_db(in_rms)
        out_db = linear_to_db(out_rms)
        attenuation = in_db - out_db

        if burst_type == "loud":
            # Should pass through with minimal attenuation
            expected_atten = 0
            tolerance = 3.0  # Allow 3dB
            passed = attenuation < tolerance
        else:
            # Should be attenuated by range amount
            expected_atten = -range_db  # Note: range_db is negative
            tolerance = 10.0  # Allow 10dB tolerance for full gate
            passed = attenuation > expected_atten - tolerance

        results['tests'].append({
            'burst_type': burst_type,
            'time_range': f'{start:.1f}-{end:.1f}s',
            'input_db': float(in_db),
            'output_db': float(out_db),
            'attenuation_db': float(attenuation),
            'passed': passed
        })

        status = "PASS" if passed else "FAIL"
        print(f"    {burst_type:5s} burst ({start:.1f}-{end:.1f}s): "
              f"in={in_db:.1f}dB, out={out_db:.1f}dB, atten={attenuation:.1f}dB [{status}]")

    # Visualization
    fig, axes = plt.subplots(3, 1, figsize=(14, 12))

    time_ms = np.arange(len(output)) / sr * 1000

    # Input and output waveforms
    ax1 = axes[0]
    ax1.plot(time_ms, test_signal, 'g-', linewidth=0.3, alpha=0.5, label='Input')
    ax1.plot(time_ms, output, 'b-', linewidth=0.3, alpha=0.8, label='Output')
    ax1.axhline(db_to_linear(threshold_db), color='red', linestyle='--', alpha=0.5,
                label=f'Threshold={threshold_db}dB')
    ax1.axhline(-db_to_linear(threshold_db), color='red', linestyle='--', alpha=0.5)
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title('Gate Input/Output Waveforms')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Envelope comparison
    ax2 = axes[1]
    window = int(0.01 * sr)
    env_in = np.convolve(np.abs(test_signal), np.ones(window)/window, mode='same')
    env_out = np.convolve(np.abs(output), np.ones(window)/window, mode='same')
    env_in_db = linear_to_db(env_in + 1e-10)
    env_out_db = linear_to_db(env_out + 1e-10)

    ax2.plot(time_ms, env_in_db, 'g-', linewidth=1, alpha=0.7, label='Input envelope')
    ax2.plot(time_ms, env_out_db, 'b-', linewidth=1, alpha=0.9, label='Output envelope')
    ax2.axhline(threshold_db, color='red', linestyle='--', alpha=0.5, label='Threshold')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Level (dB)')
    ax2.set_title('Gate Envelope Response')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(-100, 0)

    # Gate gain (1 = open, 0 = closed)
    ax3 = axes[2]
    # Calculate approximate gate gain from envelopes
    gate_gain = (env_out + 1e-10) / (env_in + 1e-10)
    gate_gain = np.clip(gate_gain, 0, 1)
    ax3.plot(time_ms, gate_gain, 'b-', linewidth=1)
    ax3.axhline(1.0, color='green', linestyle='--', alpha=0.5, label='Open')
    ax3.axhline(0.0, color='red', linestyle='--', alpha=0.5, label='Closed')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Gate Gain')
    ax3.set_title('Gate State')
    ax3.legend()
    ax3.grid(True, alpha=0.3)
    ax3.set_ylim(-0.1, 1.1)

    plt.tight_layout()
    save_figure(fig, 'output/gate_response.png')
    print(f"\n  Saved: output/gate_response.png")

    # Test hysteresis (if implemented)
    print("\n  Hysteresis Test (chatter prevention):")

    # Create signal hovering around threshold
    hover_signal = gen_test_tone(freq, 1.0, sr, db_to_linear(threshold_db))
    # Add small random amplitude modulation
    noise = np.random.uniform(-0.1, 0.1, len(hover_signal)).astype(np.float32)
    hover_signal = hover_signal * (1 + noise * 0.5)

    host2 = CedarTestHost(sr)
    buf_thresh2 = host2.set_param("threshold", threshold_db)
    buf_range2 = host2.set_param("range", range_db)
    buf_out2 = 1

    inst2 = cedar.Instruction.make_ternary(
        cedar.Opcode.DYNAMICS_GATE, buf_out2, 0, buf_thresh2, buf_range2,
        cedar.hash("gate2") & 0xFFFF
    )
    inst2.rate = rate
    host2.load_instruction(inst2)
    host2.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out2)
    )

    hover_output = host2.process(hover_signal)

    # Count zero crossings of gate state (excessive = chatter)
    window2 = int(0.005 * sr)
    env_hover = np.convolve(np.abs(hover_output), np.ones(window2)/window2, mode='same')
    gate_state = (env_hover > db_to_linear(-60)).astype(int)
    state_changes = np.sum(np.abs(np.diff(gate_state)))

    # Reasonable: a few state changes, excessive: hundreds
    hysteresis_ok = state_changes < 20

    results['tests'].append({
        'name': 'Hysteresis/chatter',
        'state_changes': int(state_changes),
        'passed': hysteresis_ok
    })

    print(f"    State changes around threshold: {state_changes}")
    print(f"    Hysteresis test: {'PASS' if hysteresis_ok else 'FAIL (chattering)'}")

    with open('output/gate_response.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/gate_response.json")

    return results


# =============================================================================
# 4. DYNAMICS_GATE Attenuation Speed Test
# =============================================================================

def test_gate_attenuation_speed():
    """
    Test that range attenuation is applied quickly when gate closes.
    Verifies fix for: "Range attenuation not applied to quiet signals"

    The gate should reach full attenuation within ~50ms of the signal dropping
    below threshold, regardless of the configured release time.
    """
    print("\nTest 4: DYNAMICS_GATE Attenuation Speed")
    print("=" * 60)

    sr = 48000

    # Gate settings
    threshold_db = -30
    range_db = -60  # Expect 60dB attenuation when closed
    attack_ms = 1
    hold_ms = 0  # No hold for this test
    release_ms = 500  # Slow release - but gate close should be fast!

    results = {'sample_rate': sr, 'tests': []}

    print(f"\n  Gate Settings:")
    print(f"    Threshold: {threshold_db}dB")
    print(f"    Range: {range_db}dB (expect {-range_db}dB attenuation)")
    print(f"    Release: {release_ms}ms (should NOT affect gate close speed)")

    # Create signal: 200ms loud (-10dB), then 400ms quiet (-50dB)
    duration = 0.7
    num_samples = int(duration * sr)
    freq = 1000

    loud_amp = db_to_linear(-10)   # Well above threshold
    quiet_amp = db_to_linear(-50)  # Well below threshold

    transition_time = 0.2  # 200ms
    transition_sample = int(transition_time * sr)

    test_signal = np.zeros(num_samples, dtype=np.float32)
    t_loud = np.arange(transition_sample) / sr
    t_quiet = np.arange(num_samples - transition_sample) / sr
    test_signal[:transition_sample] = np.sin(2 * np.pi * freq * t_loud) * loud_amp
    test_signal[transition_sample:] = np.sin(2 * np.pi * freq * t_quiet) * quiet_amp

    host = CedarTestHost(sr)

    # Set gate parameters
    buf_thresh = host.set_param("threshold", threshold_db)
    buf_range = host.set_param("range", range_db)
    buf_in = 0
    buf_out = 1

    # Pack timing into rate parameter (no hold)
    attack_idx = 0
    hold_idx = 0
    release_idx = min(15, release_ms // 25)  # Maps to ~500ms
    rate = (attack_idx << 6) | (hold_idx << 4) | release_idx

    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.DYNAMICS_GATE, buf_out, buf_in, buf_thresh, buf_range,
        cedar.hash("gate_speed") & 0xFFFF
    )
    inst.rate = rate
    host.load_instruction(inst)
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    output = host.process(test_signal)

    # Analyze attenuation at various times after the signal drops
    # Note: The envelope follower has ~50ms time constant (release/10), so it takes
    # ~150ms for the envelope to drop below threshold and trigger gate close.
    # After gate closes, the gain transition uses a fast 5ms coefficient.
    # Total expected time to full attenuation: ~175-200ms
    check_times_ms = [50, 100, 150, 200, 250, 300]

    print("\n  Attenuation Timing Analysis:")
    print(f"    Signal drops at {transition_time * 1000:.0f}ms")
    print(f"    Envelope time constant: ~50ms (needs ~150ms to drop below threshold)")
    print(f"    Gain transition: 5ms (fast close coefficient)")

    window_ms = 10  # Use 10ms RMS windows
    window_samples = int(window_ms / 1000 * sr)

    for check_ms in check_times_ms:
        check_sample = transition_sample + int(check_ms / 1000 * sr)

        if check_sample + window_samples > len(output):
            continue

        # Measure input and output RMS in window
        in_rms = np.sqrt(np.mean(test_signal[check_sample:check_sample + window_samples] ** 2))
        out_rms = np.sqrt(np.mean(output[check_sample:check_sample + window_samples] ** 2))

        in_db = linear_to_db(in_rms + 1e-10)
        out_db = linear_to_db(out_rms + 1e-10)
        attenuation = in_db - out_db

        # Expected attenuation timing:
        # - 0-150ms: envelope still above threshold, gate open, minimal attenuation
        # - 150-175ms: envelope drops, gate closes, gain transitions (fast 5ms)
        # - 200ms+: full attenuation (~60dB)
        # Note: The envelope needs ~150ms to drop below threshold (ln(20) * 50ms)
        expected_attenuation = {
            50: 0,     # Too early, envelope still high
            100: 0,    # Envelope dropping but gate still open
            150: 0,    # Just at threshold crossing, minimal attenuation
            200: 50,   # Gate should be mostly closed
            250: 55,   # Should be near full attenuation
            300: 58,   # Should be at full attenuation
        }

        min_expected = expected_attenuation.get(check_ms, 0)
        passed = attenuation >= min_expected

        test_result = {
            'time_after_drop_ms': check_ms,
            'input_db': float(in_db),
            'output_db': float(out_db),
            'attenuation_db': float(attenuation),
            'min_expected_db': min_expected,
            'passed': passed
        }
        results['tests'].append(test_result)

        status = "PASS" if passed else "FAIL"
        print(f"    +{check_ms:3d}ms: attenuation={attenuation:.1f}dB (need >{min_expected}dB) [{status}]")

    # Also verify the loud section passes through cleanly
    loud_start = int(0.05 * sr)  # Skip first 50ms
    loud_end = transition_sample - int(0.02 * sr)  # Skip last 20ms

    in_rms_loud = np.sqrt(np.mean(test_signal[loud_start:loud_end] ** 2))
    out_rms_loud = np.sqrt(np.mean(output[loud_start:loud_end] ** 2))
    loud_attenuation = linear_to_db(in_rms_loud) - linear_to_db(out_rms_loud)

    loud_passed = loud_attenuation < 3.0  # Should pass through with <3dB loss

    results['tests'].append({
        'name': 'Loud passthrough',
        'attenuation_db': float(loud_attenuation),
        'passed': loud_passed
    })

    print(f"\n    Loud signal passthrough: {loud_attenuation:.1f}dB loss [{'PASS' if loud_passed else 'FAIL'}]")

    # Visualization
    fig, axes = plt.subplots(2, 1, figsize=(14, 8))

    time_ms = np.arange(len(output)) / sr * 1000

    # Waveform
    ax1 = axes[0]
    ax1.plot(time_ms, test_signal, 'g-', linewidth=0.3, alpha=0.5, label='Input')
    ax1.plot(time_ms, output, 'b-', linewidth=0.3, alpha=0.8, label='Output')
    ax1.axvline(transition_time * 1000, color='red', linestyle='--', alpha=0.7, label='Signal drop')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title('Gate Attenuation Speed Test')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Envelope in dB
    ax2 = axes[1]
    window = int(0.005 * sr)  # 5ms window
    env_in = np.convolve(np.abs(test_signal), np.ones(window)/window, mode='same')
    env_out = np.convolve(np.abs(output), np.ones(window)/window, mode='same')
    env_in_db = linear_to_db(env_in + 1e-10)
    env_out_db = linear_to_db(env_out + 1e-10)

    ax2.plot(time_ms, env_in_db, 'g-', linewidth=1, alpha=0.7, label='Input envelope')
    ax2.plot(time_ms, env_out_db, 'b-', linewidth=1, alpha=0.9, label='Output envelope')
    ax2.axvline(transition_time * 1000, color='red', linestyle='--', alpha=0.7, label='Signal drop')
    ax2.axhline(threshold_db, color='orange', linestyle=':', alpha=0.5, label=f'Threshold={threshold_db}dB')

    # Mark check points
    for check_ms in check_times_ms:
        ax2.axvline(transition_time * 1000 + check_ms, color='gray', linestyle=':', alpha=0.3)

    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Level (dB)')
    ax2.set_title('Envelope Response (should drop quickly after signal drops)')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(-100, 0)

    plt.tight_layout()
    save_figure(fig, 'output/gate_attenuation_speed.png')
    print(f"\n  Saved: output/gate_attenuation_speed.png")

    with open('output/gate_attenuation_speed.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/gate_attenuation_speed.json")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    os.makedirs('output', exist_ok=True)

    print("Cedar Dynamics Processor Quality Tests")
    print("=" * 60)
    print()

    test_compressor_ratio()
    test_limiter_ceiling()
    test_gate_threshold()
    test_gate_attenuation_speed()

    print()
    print("=" * 60)
    print("All dynamics tests complete. Results saved to output/")
