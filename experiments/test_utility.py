"""
Utility Opcode Quality Tests (Cedar Engine)
============================================
Tests for NOISE, MTOF, SLEW, and SAH opcodes.
Validates noise distribution, frequency conversion accuracy,
slew rate limiting, and sample-and-hold timing.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost
from visualize import save_figure
from utils import midi_to_freq


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


# =============================================================================
# 1. NOISE Test - Distribution and Spectral Flatness
# =============================================================================

def test_noise_distribution():
    """
    Test white noise generator for:
    - Uniform distribution in [-1, 1]
    - Mean near 0
    - Standard deviation near 0.577 (uniform distribution std)
    - Spectral flatness (no peaks)
    """
    print("Test 1: NOISE Distribution and Spectral Flatness")
    print("=" * 60)

    sr = 48000
    duration = 10.0  # 10 seconds of noise for good statistics
    num_samples = int(duration * sr)

    host = CedarTestHost(sr)

    # NOISE opcode: out = noise generator
    # Most noise opcodes are nullary (no inputs)
    buf_out = 1
    host.load_instruction(
        cedar.Instruction.make_nullary(cedar.Opcode.NOISE, buf_out, cedar.hash("noise") & 0xFFFF)
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    # Generate silence as input (noise is self-generating)
    silence = np.zeros(num_samples, dtype=np.float32)
    noise_output = host.process(silence)

    results = {'sample_rate': sr, 'duration': duration, 'tests': []}

    # Statistics analysis
    mean = float(np.mean(noise_output))
    std = float(np.std(noise_output))
    min_val = float(np.min(noise_output))
    max_val = float(np.max(noise_output))

    # For uniform distribution in [-1, 1]: mean=0, std=1/sqrt(3)≈0.577
    expected_std = 1.0 / np.sqrt(3)
    std_error = abs(std - expected_std) / expected_std * 100

    results['tests'].append({
        'name': 'Distribution statistics',
        'mean': mean,
        'std': std,
        'expected_std': expected_std,
        'std_error_pct': std_error,
        'min': min_val,
        'max': max_val
    })

    print(f"\n  Distribution Statistics:")
    print(f"    Mean:     {mean:.6f} (expected: ~0)")
    print(f"    Std Dev:  {std:.4f} (expected: {expected_std:.4f}, error: {std_error:.1f}%)")
    print(f"    Range:    [{min_val:.4f}, {max_val:.4f}] (expected: [-1, 1])")

    mean_ok = abs(mean) < 0.01  # Mean within 1% of range
    std_ok = std_error < 10  # Std within 10%
    range_ok = min_val >= -1.05 and max_val <= 1.05  # Allow 5% tolerance

    print(f"    Mean OK:  {'PASS' if mean_ok else 'FAIL'}")
    print(f"    Std OK:   {'PASS' if std_ok else 'FAIL'}")
    print(f"    Range OK: {'PASS' if range_ok else 'FAIL'}")

    # Spectral analysis - should be flat
    print("\n  Spectral Analysis:")
    # Use 1 second of noise for FFT
    fft_samples = sr
    fft_data = noise_output[:fft_samples]
    freqs = np.fft.rfftfreq(fft_samples, 1/sr)
    spectrum = np.abs(np.fft.rfft(fft_data))
    spectrum_db = 20 * np.log10(spectrum + 1e-10)

    # Check flatness by looking at variance across frequency bands
    # Divide spectrum into octave bands and check uniformity
    bands = [(20, 100), (100, 500), (500, 2000), (2000, 8000), (8000, 20000)]
    band_levels = []

    for low, high in bands:
        mask = (freqs >= low) & (freqs < high)
        if np.any(mask):
            band_avg = np.mean(spectrum_db[mask])
            band_levels.append(band_avg)

    band_variance = np.var(band_levels)
    spectral_flatness_ok = band_variance < 20  # dB variance

    results['tests'].append({
        'name': 'Spectral flatness',
        'band_levels_db': band_levels,
        'band_variance_db': float(band_variance),
        'passed': spectral_flatness_ok
    })

    print(f"    Band levels (dB): {[f'{l:.1f}' for l in band_levels]}")
    print(f"    Band variance: {band_variance:.2f} dB")
    print(f"    Flatness OK: {'PASS' if spectral_flatness_ok else 'FAIL'}")

    # Create visualization
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Histogram
    ax1 = axes[0, 0]
    ax1.hist(noise_output, bins=100, density=True, alpha=0.7, edgecolor='black')
    ax1.axvline(0, color='red', linestyle='--', alpha=0.5)
    # Theoretical uniform distribution
    x_uniform = np.linspace(-1, 1, 100)
    ax1.plot(x_uniform, np.ones_like(x_uniform) * 0.5, 'g-', linewidth=2, label='Uniform PDF')
    ax1.set_xlabel('Value')
    ax1.set_ylabel('Density')
    ax1.set_title(f'Noise Distribution (mean={mean:.4f}, std={std:.4f})')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Spectrum
    ax2 = axes[0, 1]
    ax2.semilogx(freqs[1:], spectrum_db[1:], linewidth=0.5, alpha=0.7)
    ax2.set_xlabel('Frequency (Hz)')
    ax2.set_ylabel('Magnitude (dB)')
    ax2.set_title('Noise Spectrum')
    ax2.set_xlim(20, sr/2)
    ax2.grid(True, which='both', alpha=0.3)

    # Waveform snippet
    ax3 = axes[1, 0]
    time_ms = np.arange(2000) / sr * 1000
    ax3.plot(time_ms, noise_output[:2000], linewidth=0.5)
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Amplitude')
    ax3.set_title('Noise Waveform (first 2000 samples)')
    ax3.grid(True, alpha=0.3)

    # Autocorrelation (should be near-delta for white noise)
    ax4 = axes[1, 1]
    autocorr_samples = 1000
    autocorr = np.correlate(noise_output[:autocorr_samples],
                            noise_output[:autocorr_samples], mode='full')
    autocorr = autocorr[autocorr_samples-1:autocorr_samples+100]
    autocorr = autocorr / autocorr[0]  # Normalize
    ax4.plot(np.arange(len(autocorr)), autocorr, linewidth=1)
    ax4.set_xlabel('Lag (samples)')
    ax4.set_ylabel('Autocorrelation')
    ax4.set_title('Autocorrelation (should be ~0 except at lag 0)')
    ax4.axhline(0, color='gray', linestyle='--', alpha=0.5)
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/noise_distribution.png')
    print(f"\n  Saved: output/noise_distribution.png")

    with open('output/noise_distribution.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/noise_distribution.json")

    return results


# =============================================================================
# 2. MTOF Test - MIDI to Frequency Accuracy
# =============================================================================

def test_mtof_accuracy():
    """
    Test MIDI to frequency conversion accuracy.
    - Standard A4=69 -> 440Hz
    - Full MIDI range 0-127
    - Tolerance: <0.1% frequency error
    """
    print("\nTest 2: MTOF (MIDI to Frequency) Accuracy")
    print("=" * 60)

    sr = 48000

    # Reference test cases
    reference_notes = [
        (69, 440.0, "A4"),      # A440
        (60, 261.626, "C4"),    # Middle C
        (57, 220.0, "A3"),      # A3
        (81, 880.0, "A5"),      # A5
        (48, 130.813, "C3"),    # C3
        (36, 65.406, "C2"),     # C2
        (84, 1046.50, "C6"),    # C6
        (21, 27.5, "A0"),       # A0 (low piano)
        (108, 4186.01, "C8"),   # C8 (high piano)
    ]

    results = {'sample_rate': sr, 'tests': [], 'full_range': []}

    print("\n  Reference Note Tests:")

    all_passed = True
    for midi_note, expected_freq, name in reference_notes:
        host = CedarTestHost(sr)

        # Set MIDI note as input
        buf_midi = host.set_param("midi", float(midi_note))
        buf_out = 1

        # MTOF: out = mtof(midi_note)
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.MTOF, buf_out, buf_midi, cedar.hash("mtof") & 0xFFFF)
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Process one block
        silence = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
        output = host.process(silence)

        measured_freq = float(output[0])
        error_pct = abs(measured_freq - expected_freq) / expected_freq * 100
        passed = error_pct < 0.1

        if not passed:
            all_passed = False

        results['tests'].append({
            'name': name,
            'midi_note': midi_note,
            'expected_hz': expected_freq,
            'measured_hz': measured_freq,
            'error_pct': error_pct,
            'passed': passed
        })

        status = "PASS" if passed else "FAIL"
        print(f"    {name:4s} (MIDI {midi_note:3d}): expected={expected_freq:8.2f}Hz, "
              f"measured={measured_freq:8.2f}Hz, error={error_pct:.4f}% [{status}]")

    # Full range test
    print("\n  Full MIDI Range Test (0-127):")
    max_error = 0
    errors = []

    for midi_note in range(128):
        expected_freq = 440.0 * (2 ** ((midi_note - 69) / 12))

        host = CedarTestHost(sr)
        buf_midi = host.set_param("midi", float(midi_note))
        buf_out = 1

        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.MTOF, buf_out, buf_midi, cedar.hash("mtof") & 0xFFFF)
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        silence = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)
        output = host.process(silence)
        measured_freq = float(output[0])

        error_pct = abs(measured_freq - expected_freq) / expected_freq * 100
        errors.append(error_pct)
        max_error = max(max_error, error_pct)

        results['full_range'].append({
            'midi_note': midi_note,
            'expected_hz': expected_freq,
            'measured_hz': measured_freq,
            'error_pct': error_pct
        })

    avg_error = np.mean(errors)
    range_passed = max_error < 0.1

    print(f"    Max error: {max_error:.6f}%")
    print(f"    Avg error: {avg_error:.6f}%")
    print(f"    Range test: {'PASS' if range_passed else 'FAIL'}")

    results['summary'] = {
        'max_error_pct': max_error,
        'avg_error_pct': avg_error,
        'all_reference_passed': all_passed,
        'full_range_passed': range_passed
    }

    # Visualization
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # MIDI to Frequency curve
    ax1 = axes[0, 0]
    midi_notes = np.arange(128)
    measured_freqs = [r['measured_hz'] for r in results['full_range']]
    expected_freqs = [r['expected_hz'] for r in results['full_range']]
    ax1.semilogy(midi_notes, expected_freqs, 'b-', linewidth=2, label='Expected')
    ax1.semilogy(midi_notes, measured_freqs, 'r--', linewidth=1, label='Measured')
    ax1.set_xlabel('MIDI Note')
    ax1.set_ylabel('Frequency (Hz)')
    ax1.set_title('MIDI to Frequency Conversion')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Error across range
    ax2 = axes[0, 1]
    ax2.plot(midi_notes, errors, 'b-', linewidth=1)
    ax2.axhline(0.1, color='red', linestyle='--', alpha=0.5, label='0.1% threshold')
    ax2.set_xlabel('MIDI Note')
    ax2.set_ylabel('Error (%)')
    ax2.set_title('Conversion Error Across MIDI Range')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(0, max(0.2, max_error * 1.1))

    # Reference notes bar chart
    ax3 = axes[1, 0]
    ref_names = [r['name'] for r in results['tests']]
    ref_errors = [r['error_pct'] for r in results['tests']]
    colors = ['green' if e < 0.1 else 'red' for e in ref_errors]
    ax3.bar(ref_names, ref_errors, color=colors)
    ax3.axhline(0.1, color='red', linestyle='--', alpha=0.5)
    ax3.set_xlabel('Note')
    ax3.set_ylabel('Error (%)')
    ax3.set_title('Reference Note Errors')
    ax3.grid(True, alpha=0.3)

    # Error histogram
    ax4 = axes[1, 1]
    ax4.hist(errors, bins=50, edgecolor='black', alpha=0.7)
    ax4.axvline(0.1, color='red', linestyle='--', alpha=0.7, label='0.1% threshold')
    ax4.set_xlabel('Error (%)')
    ax4.set_ylabel('Count')
    ax4.set_title('Error Distribution')
    ax4.legend()
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/mtof_accuracy.png')
    print(f"\n  Saved: output/mtof_accuracy.png")

    with open('output/mtof_accuracy.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/mtof_accuracy.json")

    return results


# =============================================================================
# 3. SLEW Test - Rise/Fall Time Accuracy
# =============================================================================

def test_slew_timing():
    """
    Test slew rate limiter rise/fall time.
    - Step input 0->1, measure time to reach 0.99
    - Step input 1->0, measure time to reach 0.01
    - Verify matches configured rate within 1%
    """
    print("\nTest 3: SLEW (Slew Rate Limiter) Timing")
    print("=" * 60)

    sr = 48000

    # Test various slew rates (in units per second)
    slew_rates = [
        (10.0, "Fast (10/s)"),      # 100ms for full range
        (5.0, "Medium (5/s)"),      # 200ms for full range
        (2.0, "Slow (2/s)"),        # 500ms for full range
        (1.0, "Very slow (1/s)"),   # 1000ms for full range
    ]

    results = {'sample_rate': sr, 'tests': []}

    fig, axes = plt.subplots(len(slew_rates), 2, figsize=(14, 4 * len(slew_rates)))

    for idx, (rate, name) in enumerate(slew_rates):
        print(f"\n  Testing {name}:")

        # Expected time to traverse 0->1 (full range)
        full_time = 1.0 / rate
        # But we measure to 99% threshold, so expected time is 99% of full time
        expected_time = 0.99 / rate
        expected_samples = int(expected_time * sr)
        duration = full_time * 2.0  # Give enough time
        num_samples = int(duration * sr)

        host = CedarTestHost(sr)

        # Set slew rate
        buf_rate = host.set_param("rate", rate)
        buf_in = 0
        buf_out = 1

        # SLEW: out = slew(in, rate)
        host.load_instruction(
            cedar.Instruction.make_binary(
                cedar.Opcode.SLEW, buf_out, buf_in, buf_rate, cedar.hash("slew") & 0xFFFF
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Create step input: 0 for first 10%, then 1
        step_start = int(0.1 * num_samples)
        step_input = np.zeros(num_samples, dtype=np.float32)
        step_input[step_start:] = 1.0

        output = host.process(step_input)

        # Measure rise time (time from step to reaching 0.99)
        rise_idx = None
        for i in range(step_start, len(output)):
            if output[i] >= 0.99:
                rise_idx = i
                break

        if rise_idx is not None:
            measured_rise_samples = rise_idx - step_start
            measured_rise_time = measured_rise_samples / sr
            rise_error_pct = (measured_rise_time - expected_time) / expected_time * 100
        else:
            measured_rise_samples = -1
            measured_rise_time = float('nan')
            rise_error_pct = float('nan')

        # Test fall time: now create 1->0 step
        host2 = CedarTestHost(sr)
        buf_rate2 = host2.set_param("rate", rate)
        buf_out2 = 1

        host2.load_instruction(
            cedar.Instruction.make_binary(
                cedar.Opcode.SLEW, buf_out2, 0, buf_rate2, cedar.hash("slew2") & 0xFFFF
            )
        )
        host2.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out2)
        )

        # Start at 1, then step to 0
        step_input2 = np.ones(num_samples, dtype=np.float32)
        step_input2[step_start:] = 0.0

        output2 = host2.process(step_input2)

        # Measure fall time
        fall_idx = None
        for i in range(step_start, len(output2)):
            if output2[i] <= 0.01:
                fall_idx = i
                break

        if fall_idx is not None:
            measured_fall_samples = fall_idx - step_start
            measured_fall_time = measured_fall_samples / sr
            fall_error_pct = (measured_fall_time - expected_time) / expected_time * 100
        else:
            measured_fall_samples = -1
            measured_fall_time = float('nan')
            fall_error_pct = float('nan')

        # Check if passed
        tolerance = max(1.0, 5 / expected_samples * 100)  # 1% or ±5 samples
        rise_passed = not np.isnan(rise_error_pct) and abs(rise_error_pct) < tolerance
        fall_passed = not np.isnan(fall_error_pct) and abs(fall_error_pct) < tolerance

        test_result = {
            'name': name,
            'rate': rate,
            'expected_time_ms': expected_time * 1000,
            'rise_measured_ms': measured_rise_time * 1000 if not np.isnan(measured_rise_time) else None,
            'rise_error_pct': rise_error_pct if not np.isnan(rise_error_pct) else None,
            'rise_passed': rise_passed,
            'fall_measured_ms': measured_fall_time * 1000 if not np.isnan(measured_fall_time) else None,
            'fall_error_pct': fall_error_pct if not np.isnan(fall_error_pct) else None,
            'fall_passed': fall_passed
        }
        results['tests'].append(test_result)

        rise_status = "PASS" if rise_passed else "FAIL"
        fall_status = "PASS" if fall_passed else "FAIL"
        print(f"    Rise: expected={expected_time*1000:.1f}ms, measured={measured_rise_time*1000:.1f}ms, "
              f"error={rise_error_pct:.1f}% [{rise_status}]")
        print(f"    Fall: expected={expected_time*1000:.1f}ms, measured={measured_fall_time*1000:.1f}ms, "
              f"error={fall_error_pct:.1f}% [{fall_status}]")

        # Plot rise
        ax1 = axes[idx, 0]
        time_ms = np.arange(len(output)) / sr * 1000
        ax1.plot(time_ms, output, 'b-', linewidth=1, label='Output')
        ax1.plot(time_ms, step_input, 'g--', linewidth=0.5, alpha=0.5, label='Input')
        ax1.axhline(0.99, color='red', linestyle=':', alpha=0.5, label='99% threshold')
        if rise_idx:
            ax1.axvline(rise_idx / sr * 1000, color='red', linestyle=':', alpha=0.7)
        ax1.set_xlabel('Time (ms)')
        ax1.set_ylabel('Level')
        ax1.set_title(f'{name} - Rise Response')
        ax1.legend(fontsize=8)
        ax1.grid(True, alpha=0.3)

        # Plot fall
        ax2 = axes[idx, 1]
        ax2.plot(time_ms, output2, 'b-', linewidth=1, label='Output')
        ax2.plot(time_ms, step_input2, 'g--', linewidth=0.5, alpha=0.5, label='Input')
        ax2.axhline(0.01, color='red', linestyle=':', alpha=0.5, label='1% threshold')
        if fall_idx:
            ax2.axvline(fall_idx / sr * 1000, color='red', linestyle=':', alpha=0.7)
        ax2.set_xlabel('Time (ms)')
        ax2.set_ylabel('Level')
        ax2.set_title(f'{name} - Fall Response')
        ax2.legend(fontsize=8)
        ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/slew_timing.png')
    print(f"\n  Saved: output/slew_timing.png")

    with open('output/slew_timing.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/slew_timing.json")

    return results


# =============================================================================
# 4. SAH Test - Sample-and-Hold Timing
# =============================================================================

def test_sah_timing():
    """
    Test sample-and-hold captures value on rising edge.
    - Ramp input with trigger at known times
    - Verify held value matches input at trigger moment
    """
    print("\nTest 4: SAH (Sample and Hold) Timing")
    print("=" * 60)

    sr = 48000
    duration = 1.0
    num_samples = int(duration * sr)

    results = {'sample_rate': sr, 'tests': []}

    # Create ramp input (linear 0 to 1 over duration)
    ramp = np.linspace(0, 1, num_samples, dtype=np.float32)

    # Test with triggers at various times
    trigger_times_ms = [100, 250, 500, 750]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    for idx, trigger_ms in enumerate(trigger_times_ms):
        print(f"\n  Trigger at {trigger_ms}ms:")

        host = CedarTestHost(sr)

        # Create trigger signal (single sample pulse)
        trigger_sample = int(trigger_ms / 1000 * sr)
        trigger = np.zeros(num_samples, dtype=np.float32)
        trigger[trigger_sample] = 1.0

        # Expected held value = ramp value at trigger time
        expected_value = ramp[trigger_sample]

        buf_in = 0  # Ramp input
        buf_trig = 1  # Trigger input
        buf_out = 2

        # Set trigger to buffer 1
        # We need to inject both ramp and trigger - this is tricky with the current harness
        # SAH: out = sah(input, trigger)
        # We'll process block by block with both inputs

        # For simplicity, let's create a combined processing approach
        host.load_instruction(
            cedar.Instruction.make_binary(
                cedar.Opcode.SAH, buf_out, buf_in, buf_trig, cedar.hash("sah") & 0xFFFF
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Process manually with both inputs
        host.vm.load_program(host.program)
        n_blocks = (num_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
        padded_len = n_blocks * cedar.BLOCK_SIZE

        ramp_padded = np.zeros(padded_len, dtype=np.float32)
        ramp_padded[:num_samples] = ramp
        trigger_padded = np.zeros(padded_len, dtype=np.float32)
        trigger_padded[:num_samples] = trigger

        output = []
        for i in range(n_blocks):
            start = i * cedar.BLOCK_SIZE
            end = start + cedar.BLOCK_SIZE
            host.vm.set_buffer(0, ramp_padded[start:end])
            host.vm.set_buffer(1, trigger_padded[start:end])
            l, r = host.vm.process()
            output.append(l)

        output = np.concatenate(output)[:num_samples]

        # Verify: after trigger, output should hold the value from trigger moment
        # Check a few samples after trigger
        check_idx = trigger_sample + 100  # 100 samples after trigger
        if check_idx < len(output):
            measured_value = output[check_idx]
            error = abs(measured_value - expected_value)
            # Allow for block boundary quantization
            passed = error < 0.01 or abs(measured_value - expected_value) < cedar.BLOCK_SIZE / num_samples

            # Also check that value is stable (held)
            stability_region = output[trigger_sample + 10:trigger_sample + 500]
            stability_std = np.std(stability_region) if len(stability_region) > 0 else 0
            stable = stability_std < 0.001
        else:
            measured_value = float('nan')
            error = float('nan')
            passed = False
            stable = False

        test_result = {
            'trigger_ms': trigger_ms,
            'trigger_sample': trigger_sample,
            'expected_value': float(expected_value),
            'measured_value': float(measured_value),
            'error': float(error) if not np.isnan(error) else None,
            'passed': passed,
            'stable': stable
        }
        results['tests'].append(test_result)

        status = "PASS" if passed else "FAIL"
        stable_str = "stable" if stable else "unstable"
        print(f"    Expected hold: {expected_value:.4f}")
        print(f"    Measured hold: {measured_value:.4f}")
        print(f"    Error: {error:.6f} [{status}]")
        print(f"    Hold stability: {stable_str}")

        # Plot
        ax = axes[idx // 2, idx % 2]
        time_ms = np.arange(len(output)) / sr * 1000
        ax.plot(time_ms, ramp[:len(output)], 'g--', linewidth=0.5, alpha=0.5, label='Input (ramp)')
        ax.plot(time_ms, output, 'b-', linewidth=1, label='S&H Output')
        ax.axvline(trigger_ms, color='red', linestyle=':', alpha=0.7, label='Trigger')
        ax.axhline(expected_value, color='orange', linestyle='--', alpha=0.5,
                   label=f'Expected hold={expected_value:.3f}')
        ax.set_xlabel('Time (ms)')
        ax.set_ylabel('Level')
        ax.set_title(f'S&H Trigger at {trigger_ms}ms')
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sah_timing.png')
    print(f"\n  Saved: output/sah_timing.png")

    with open('output/sah_timing.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/sah_timing.json")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    os.makedirs('output', exist_ok=True)

    print("Cedar Utility Opcode Quality Tests")
    print("=" * 60)
    print()

    test_noise_distribution()
    test_mtof_accuracy()
    test_slew_timing()
    test_sah_timing()

    print()
    print("=" * 60)
    print("All utility tests complete. Results saved to output/")
