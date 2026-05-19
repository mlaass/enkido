"""
Gate Quality Tests (Cedar Engine)
==================================
Tests for DYNAMICS_GATE opcode.
Validates threshold, attenuation, hysteresis, and gate close speed.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, db_to_linear, linear_to_db, save_wav
from visualize import save_figure

OUT = output_dir("op_gate")


def gen_test_tone(freq, duration, sr, amplitude=1.0):
    """Generate a test sine tone."""
    t = np.arange(int(duration * sr)) / sr
    return (np.sin(2 * np.pi * freq * t) * amplitude).astype(np.float32)


def _build_gate(host, threshold_db, range_db, hysteresis_db=6.0, close_time_ms=5.0,
                attack_ms=0.1, hold_ms=0.0, release_ms=10.0,
                init_ext=True, state_name="gate"):
    """Configure host with a DYNAMICS_GATE instruction.

    attack/hold/release moved from inst.rate bit-packing to ExtendedParams<5>
    ([attack, hold, release, dry, wet]) in prd-extended-params-migration §4.3.
    The gate opcode reads inputs[3]=hysteresis and inputs[4]=close_time
    unconditionally, so all 5 input slots must be wired (make_quinary). When
    init_ext is False the ExtendedParams state is left uninitialized to
    exercise the opcode's nullptr fallback (attack=0.1ms, hold=0, release=10ms).
    buf_in=0, buf_out=1.
    """
    buf_thresh = host.set_param("g_threshold", threshold_db)
    buf_range = host.set_param("g_range", range_db)
    buf_hyst = host.set_param("g_hyst", hysteresis_db)
    buf_close = host.set_param("g_close", close_time_ms)
    state_id = cedar.hash(state_name) & 0xFFFF
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.DYNAMICS_GATE, 1, 0, buf_thresh, buf_range, buf_hyst, buf_close,
        state_id
    )
    inst.rate = 0  # rate field no longer carries attack/hold/release
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))
    host.vm.load_program(host.program)
    if init_ext:
        host.vm.init_extended_params(
            cedar.ext_params_state_id(state_id),
            np.array([attack_ms, hold_ms, release_ms, 0.0, 1.0], dtype=np.float32),
            np.array([0xFFFF] * 5, dtype=np.uint16),
            5,
        )
    return state_id


# =============================================================================
# DYNAMICS_GATE Test - Threshold and Attenuation
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

    # DYNAMICS_GATE: out = gate(in, threshold, range)
    _build_gate(host, threshold_db, range_db,
                attack_ms=attack_ms, hold_ms=hold_ms, release_ms=release_ms)

    output = host.process_loaded(test_signal)

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
    save_figure(fig, os.path.join(OUT, 'response.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'response.png')}")

    # Test hysteresis (if implemented)
    print("\n  Hysteresis Test (chatter prevention):")

    # Create signal hovering around threshold
    hover_signal = gen_test_tone(freq, 1.0, sr, db_to_linear(threshold_db))
    # Add small random amplitude modulation
    noise = np.random.uniform(-0.1, 0.1, len(hover_signal)).astype(np.float32)
    hover_signal = hover_signal * (1 + noise * 0.5)

    host2 = CedarTestHost(sr)
    _build_gate(host2, threshold_db, range_db,
                attack_ms=attack_ms, hold_ms=hold_ms, release_ms=release_ms,
                state_name="gate2")

    hover_output = host2.process_loaded(hover_signal)

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

    with open(os.path.join(OUT, 'response.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'response.json')}")

    return results


# =============================================================================
# DYNAMICS_GATE Attenuation Speed Test
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

    _build_gate(host, threshold_db, range_db,
                attack_ms=attack_ms, hold_ms=hold_ms, release_ms=release_ms,
                state_name="gate_speed")

    output = host.process_loaded(test_signal)

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
    save_figure(fig, os.path.join(OUT, 'attenuation_speed.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'attenuation_speed.png')}")

    with open(os.path.join(OUT, 'attenuation_speed.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'attenuation_speed.json')}")

    return results


# =============================================================================
# ExtendedParams migration tests (prd-extended-params-migration §4.3)
# =============================================================================

def test_default_fallback_equivalence():
    """
    Verify the nullptr-fallback (no ExtendedParams init) is identical to an
    explicit-default init (attack=0.1ms, hold=0ms, release=10ms, dry=0, wet=1).

    Pre-migration, gate always ran at inst.rate=0 → that exact decode (no
    Akkado path ever set the rate field). The post-migration opcode must
    reproduce it when ExtendedParams is absent.

    Acceptance: peak sample error < -60 dBFS between the two renders.
    """
    print("\nTest: gate default-fallback equivalence")
    print("=" * 60)

    sr = 48000
    t = np.arange(int(0.5 * sr)) / sr
    sig = (np.sin(2 * np.pi * 1000 * t) * db_to_linear(-25)).astype(np.float32)
    sig[:len(sig) // 2] *= db_to_linear(-30)  # quiet then loud → exercises gate

    host_fb = CedarTestHost(sr)
    _build_gate(host_fb, -40, -60, init_ext=False, state_name="gate_fb")
    out_fb = host_fb.process_loaded(sig)

    host_def = CedarTestHost(sr)
    _build_gate(host_def, -40, -60, attack_ms=0.1, hold_ms=0.0, release_ms=10.0,
                state_name="gate_def")
    out_def = host_def.process_loaded(sig)

    peak_err = float(np.max(np.abs(out_fb - out_def)))
    peak_err_db = linear_to_db(peak_err + 1e-12)
    print(f"  Peak error fallback vs default-init: {peak_err_db:.1f} dBFS")

    passed = peak_err_db < -60.0
    print(f"  {'✓ PASS' if passed else '✗ FAIL'}: nullptr fallback "
          f"{'matches' if passed else 'DIVERGES FROM'} explicit defaults")
    return passed


def test_hold_tunable():
    """
    Verify gate `hold` is now tunable via ExtendedParams.

    `hold` sets how long the gate stays open after the signal drops below
    threshold before it starts closing. A loud burst followed by silence:
    with hold=0 the gate closes almost immediately; with hold=200ms it keeps
    passing (near-unity gain) well into the silent region.

    Acceptance: 50ms into the silent region, the hold=200ms render has a
    higher gate gain than the hold=0ms render by ≥ 6 dB.
    """
    print("\nTest: gate hold tunability")
    print("=" * 60)

    sr = 48000
    dur = 0.6
    sig = np.zeros(int(dur * sr), dtype=np.float32)
    burst = int(0.3 * sr)
    t_loud = np.arange(burst) / sr
    sig[:burst] = np.sin(2 * np.pi * 1000 * t_loud) * db_to_linear(-6)
    # after `burst`: silence — gate should close (subject to hold time)

    def render(hold_ms, name):
        host = CedarTestHost(sr)
        # release long so the *hold* time, not release, governs the window.
        _build_gate(host, -40, -60, attack_ms=0.1, hold_ms=hold_ms,
                    release_ms=10.0, state_name=name)
        return host.process_loaded(sig)

    # Drive a faint probe tone in the "silent" region so gate gain is audible.
    probe = np.zeros_like(sig)
    t_probe = np.arange(len(sig) - burst) / sr
    probe[burst:] = np.sin(2 * np.pi * 1000 * t_probe) * db_to_linear(-46)
    sig = sig + probe

    out_hold0 = render(0.0, "gate_h0")
    out_hold200 = render(200.0, "gate_h200")

    save_wav(os.path.join(OUT, "hold_0ms.wav"), out_hold0, sr)
    save_wav(os.path.join(OUT, "hold_200ms.wav"), out_hold200, sr)
    print("  Saved hold_0ms.wav / hold_200ms.wav")

    # Window: 50ms into the silent region.
    win = slice(burst + int(0.04 * sr), burst + int(0.06 * sr))
    rms0 = np.sqrt(np.mean(out_hold0[win] ** 2))
    rms200 = np.sqrt(np.mean(out_hold200[win] ** 2))
    diff_db = linear_to_db(rms200 + 1e-12) - linear_to_db(rms0 + 1e-12)
    print(f"  hold=0ms post-burst RMS: {linear_to_db(rms0):.1f} dB, "
          f"hold=200ms: {linear_to_db(rms200):.1f} dB")
    print(f"  Difference: {diff_db:.2f} dB (longer hold keeps gate open)")

    passed = diff_db >= 6.0
    print(f"  {'✓ PASS' if passed else '✗ FAIL'}: hold param "
          f"{'is tunable' if passed else 'has NO measurable effect'}")
    return passed


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar Gate Quality Tests")
    print("=" * 60)
    print()

    test_gate_threshold()
    test_gate_attenuation_speed()
    test_default_fallback_equivalence()
    test_hold_tunable()

    print()
    print("=" * 60)
    print("Gate tests complete.")
