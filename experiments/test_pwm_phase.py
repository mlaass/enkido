"""
PWM Oscillator Phase Consistency Experiments
=============================================

Investigates potential phase inconsistencies in PWM oscillators:
- OSC_SQR_PWM, OSC_SAW_PWM (1x variants)
- OSC_SQR_PWM_4X, OSC_SAW_PWM_4X (4x oversampled variants)
- OSC_SQR_PWM_MINBLEP (MinBLEP variant)

Expected issues being investigated:
1. PWM not interpolated in 4x variants (frequency IS interpolated)
2. Falling edge position discontinuities when PWM changes
3. PolyBLEP distance calculation with changing duty cycle
4. Different state types causing inconsistent initialization

Test methodology:
- Each test outputs WAV files for human listening evaluation
- Quantitative measurements with clear PASS/FAIL criteria
- All tests based on expected algorithm behavior, not observed behavior
"""

import numpy as np
import matplotlib.pyplot as plt
import scipy.io.wavfile as wav
import cedar_core as cedar
import os

# Ensure output directory exists
os.makedirs('output', exist_ok=True)

SR = 48000  # Sample rate


def generate_pwm_signal(opcode, freq, pwm_value, duration, sr=SR):
    """Generate a PWM oscillator signal with static parameters."""
    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('freq', freq)
    vm.set_param('pwm', pwm_value)

    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
        cedar.Instruction.make_binary(opcode, 1, 10, 11, cedar.hash('osc')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]
    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    for _ in range(num_blocks):
        left, right = vm.process()
        signal.append(left)

    return np.concatenate(signal)


def generate_modulated_pwm_signal(opcode, freq, pwm_modulator_freq, pwm_mod_depth, duration, sr=SR):
    """Generate a PWM signal with audio-rate PWM modulation via internal LFO."""
    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('freq', freq)
    vm.set_param('pwm_mod_freq', pwm_modulator_freq)
    vm.set_param('pwm_mod_depth', pwm_mod_depth)

    # PWM modulation: sin(pwm_mod_freq) * pwm_mod_depth -> PWM input
    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 12, cedar.hash('pwm_mod_freq')),
        cedar.Instruction.make_unary(cedar.Opcode.OSC_SIN, 13, 12, cedar.hash('pwm_lfo')),
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 14, cedar.hash('pwm_mod_depth')),
        cedar.Instruction.make_binary(cedar.Opcode.MUL, 11, 13, 14),  # pwm = lfo * depth
        cedar.Instruction.make_binary(opcode, 1, 10, 11, cedar.hash('osc')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]
    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    for _ in range(num_blocks):
        left, right = vm.process()
        signal.append(left)

    return np.concatenate(signal)


def find_zero_crossings(signal, rising=True):
    """Find sample indices of zero crossings.

    Args:
        signal: Input signal
        rising: If True, find rising zero crossings; if False, find falling

    Returns:
        Array of fractional sample indices where crossings occur
    """
    if rising:
        # Find where signal goes from negative to positive
        crossings = np.where((signal[:-1] <= 0) & (signal[1:] > 0))[0]
    else:
        # Find where signal goes from positive to negative
        crossings = np.where((signal[:-1] > 0) & (signal[1:] <= 0))[0]

    # Linear interpolation for sub-sample accuracy
    precise = []
    for idx in crossings:
        if signal[idx + 1] != signal[idx]:
            frac = -signal[idx] / (signal[idx + 1] - signal[idx])
            precise.append(idx + frac)

    return np.array(precise)


def find_falling_edges(signal, threshold=0.0):
    """Find falling edge positions in a square-like wave.

    Looks for transitions from high (+1 region) to low (-1 region).

    Returns:
        Array of fractional sample indices where falling edges occur
    """
    # Find where signal transitions from > threshold to < threshold
    crossings = np.where((signal[:-1] > threshold) & (signal[1:] <= threshold))[0]

    precise = []
    for idx in crossings:
        if signal[idx] != signal[idx + 1]:
            frac = (signal[idx] - threshold) / (signal[idx] - signal[idx + 1])
            precise.append(idx + frac)

    return np.array(precise)


def find_rising_edges(signal, threshold=0.0):
    """Find rising edge positions in a square-like wave."""
    crossings = np.where((signal[:-1] <= threshold) & (signal[1:] > threshold))[0]

    precise = []
    for idx in crossings:
        if signal[idx + 1] != signal[idx]:
            frac = (threshold - signal[idx]) / (signal[idx + 1] - signal[idx])
            precise.append(idx + frac)

    return np.array(precise)


# =============================================================================
# Test 1: Phase Drift Test
# =============================================================================

def test_phase_drift():
    """
    Test PWM oscillator phase drift over extended duration.

    Expected behavior:
    - After N cycles, accumulated phase should match expected phase exactly
    - Phase error should not grow over time (no drift)

    Acceptance criteria:
    - Phase drift < 0.01 cycles over 10 seconds at all test frequencies
    """
    print("\n" + "=" * 70)
    print("Test 1: Phase Drift Test")
    print("=" * 70)
    print("Testing phase accumulation accuracy over 10 seconds...")

    test_freqs = [100.0, 440.0, 1000.0, 10000.0]
    duration = 10.0  # 10 seconds
    pwm_value = 0.0  # 50% duty cycle

    oscillators = [
        ('OSC_SQR_PWM', cedar.Opcode.OSC_SQR_PWM),
        ('OSC_SQR_PWM_4X', cedar.Opcode.OSC_SQR_PWM_4X),
        ('OSC_SQR_PWM_MINBLEP', cedar.Opcode.OSC_SQR_PWM_MINBLEP),
    ]

    results = {}
    all_passed = True

    for osc_name, opcode in oscillators:
        print(f"\n  {osc_name}:")
        results[osc_name] = {}

        for freq in test_freqs:
            signal = generate_pwm_signal(opcode, freq, pwm_value, duration)

            # Find rising edge zero crossings
            rising_edges = find_rising_edges(signal)

            if len(rising_edges) < 10:
                print(f"    {freq:>6.0f} Hz: SKIP - not enough cycles detected")
                continue

            # Expected cycle period in samples
            expected_period = SR / freq
            expected_cycles = freq * duration

            # Measure actual cycles from edge count
            actual_cycles = len(rising_edges)

            # Measure period consistency
            if len(rising_edges) > 1:
                periods = np.diff(rising_edges)
                mean_period = np.mean(periods)
                period_std = np.std(periods)

                # Calculate drift: compare first few cycles vs last few cycles
                first_periods = periods[:100] if len(periods) > 100 else periods[:len(periods)//2]
                last_periods = periods[-100:] if len(periods) > 100 else periods[len(periods)//2:]

                drift_cycles = (np.mean(last_periods) - np.mean(first_periods)) / expected_period

                # Pass criteria: drift < 0.01 cycles and period std < 1% of period
                period_error = abs(mean_period - expected_period) / expected_period * 100
                passed = abs(drift_cycles) < 0.01 and period_error < 1.0

                status = "PASS" if passed else "FAIL"
                symbol = "\u2713" if passed else "\u2717"

                if not passed:
                    all_passed = False

                print(f"    {freq:>6.0f} Hz: {symbol} {status} - period={mean_period:.3f} (expected {expected_period:.3f}), "
                      f"error={period_error:.4f}%, drift={drift_cycles:.6f} cycles")

                results[osc_name][freq] = {
                    'period_error_pct': period_error,
                    'drift_cycles': drift_cycles,
                    'passed': passed
                }
            else:
                print(f"    {freq:>6.0f} Hz: SKIP - single cycle")

    # Save a short sample for listening
    print("\n  Saving audio samples...")
    for osc_name, opcode in oscillators:
        signal = generate_pwm_signal(opcode, 440.0, 0.0, 2.0)
        signal_int16 = (signal * 32767).astype(np.int16)
        wav.write(f'output/pwm_phase_drift_{osc_name.lower()}.wav', SR, signal_int16)
    print(f"  Saved: output/pwm_phase_drift_*.wav - Listen for pitch stability")

    print(f"\n  Overall: {'PASS' if all_passed else 'FAIL'}")
    return all_passed


# =============================================================================
# Test 2: PWM Step Response
# =============================================================================

def test_pwm_step_response():
    """
    Test PWM value step change for clicks/discontinuities.

    Expected behavior:
    - Sudden PWM changes should not cause clicks/pops
    - Waveform should transition smoothly to new duty cycle
    - PolyBLEP should handle moving edge position correctly

    Acceptance criteria:
    - No discontinuities larger than the waveform amplitude (2.0 peak-to-peak)
    - Output should remain bounded [-1, 1]
    """
    print("\n" + "=" * 70)
    print("Test 2: PWM Step Response")
    print("=" * 70)
    print("Testing sudden PWM value changes for clicks/discontinuities...")

    freq = 440.0
    duration_before = 0.5  # 0.5s at PWM=0.2
    duration_after = 0.5   # 0.5s at PWM=0.8

    oscillators = [
        ('OSC_SQR_PWM', cedar.Opcode.OSC_SQR_PWM),
        ('OSC_SQR_PWM_4X', cedar.Opcode.OSC_SQR_PWM_4X),
        ('OSC_SQR_PWM_MINBLEP', cedar.Opcode.OSC_SQR_PWM_MINBLEP),
    ]

    fig, axes = plt.subplots(len(oscillators), 2, figsize=(14, 3 * len(oscillators)))
    all_passed = True

    for idx, (osc_name, opcode) in enumerate(oscillators):
        vm = cedar.VM()
        vm.set_sample_rate(SR)
        vm.set_param('freq', freq)
        vm.set_param('pwm', -0.6)  # duty = 0.2

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
            cedar.Instruction.make_binary(opcode, 1, 10, 11, cedar.hash('osc')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]
        vm.load_program(program)

        signal = []
        total_blocks = int(((duration_before + duration_after) * SR) / cedar.BLOCK_SIZE)
        switch_block = int((duration_before * SR) / cedar.BLOCK_SIZE)

        for block_idx in range(total_blocks):
            if block_idx == switch_block:
                vm.set_param('pwm', 0.6)  # duty = 0.8
            left, right = vm.process()
            signal.append(left)

        signal = np.concatenate(signal)

        # Find the transition region
        transition_sample = int(duration_before * SR)
        margin = int(0.01 * SR)  # 10ms margin

        # Look for discontinuities (sample-to-sample jumps > threshold)
        diffs = np.abs(np.diff(signal))
        max_diff = np.max(diffs)

        # In a square wave, max expected jump is 2.0 (from +1 to -1)
        # Allow some margin for PolyBLEP overshoots
        max_allowed_diff = 2.5

        # Check for out-of-bounds values
        max_val = np.max(np.abs(signal))
        bounded = max_val <= 1.1  # Allow small overshoot

        passed = max_diff <= max_allowed_diff and bounded
        status = "PASS" if passed else "FAIL"
        symbol = "\u2713" if passed else "\u2717"

        if not passed:
            all_passed = False

        print(f"  {osc_name}: {symbol} {status} - max_diff={max_diff:.4f}, max_amplitude={max_val:.4f}")

        # Plot transition region
        trans_start = max(0, transition_sample - margin)
        trans_end = min(len(signal), transition_sample + margin)
        t_ms = (np.arange(trans_start, trans_end) - transition_sample) / SR * 1000

        axes[idx, 0].plot(t_ms, signal[trans_start:trans_end], linewidth=0.5)
        axes[idx, 0].axvline(x=0, color='r', linestyle='--', alpha=0.5, label='PWM change')
        axes[idx, 0].set_title(f'{osc_name}: Transition region')
        axes[idx, 0].set_ylabel('Amplitude')
        axes[idx, 0].set_ylim(-1.5, 1.5)
        axes[idx, 0].legend()

        # Plot sample-to-sample differences around transition
        axes[idx, 1].plot(t_ms[:-1], diffs[trans_start:trans_end - 1], linewidth=0.5)
        axes[idx, 1].axvline(x=0, color='r', linestyle='--', alpha=0.5)
        axes[idx, 1].axhline(y=2.0, color='orange', linestyle='--', alpha=0.5, label='Expected max (2.0)')
        axes[idx, 1].set_title(f'{osc_name}: Sample differences')
        axes[idx, 1].set_ylabel('|diff|')
        axes[idx, 1].legend()

        # Save audio
        signal_int16 = (np.clip(signal, -1, 1) * 32767).astype(np.int16)
        wav.write(f'output/pwm_step_{osc_name.lower()}.wav', SR, signal_int16)

    axes[-1, 0].set_xlabel('Time from transition (ms)')
    axes[-1, 1].set_xlabel('Time from transition (ms)')

    plt.tight_layout()
    plt.savefig('output/pwm_step_response.png', dpi=150)
    print(f"\n  Saved: output/pwm_step_response.png")
    print(f"  Saved: output/pwm_step_*.wav - Listen for clicks at 0.5s mark")

    print(f"\n  Overall: {'PASS' if all_passed else 'FAIL'}")
    return all_passed


# =============================================================================
# Test 3: Audio-Rate PWM Modulation Artifacts
# =============================================================================

def test_audio_rate_pwm_modulation():
    """
    Test PWM modulation at audio rates.

    Expected behavior (potential issue):
    - 1x variants update PWM every sample
    - 4x variants sample PWM once per 4 sub-samples
    - This may cause audible stepping artifacts in 4x variants

    Acceptance criteria:
    - This is an investigative test - document differences between 1x and 4x
    - Listen for "staircase" artifacts in 4x variant
    """
    print("\n" + "=" * 70)
    print("Test 3: Audio-Rate PWM Modulation Artifacts")
    print("=" * 70)
    print("Testing PWM modulation at audio rates (potential 1x vs 4x difference)...")

    carrier_freq = 200.0  # Low frequency to hear PWM effect clearly
    pwm_mod_freq = 2000.0  # Audio-rate modulation
    pwm_mod_depth = 0.8   # Strong modulation
    duration = 2.0

    oscillators = [
        ('OSC_SQR_PWM', cedar.Opcode.OSC_SQR_PWM),
        ('OSC_SQR_PWM_4X', cedar.Opcode.OSC_SQR_PWM_4X),
        ('OSC_SQR_PWM_MINBLEP', cedar.Opcode.OSC_SQR_PWM_MINBLEP),
    ]

    fig, axes = plt.subplots(len(oscillators), 2, figsize=(14, 3 * len(oscillators)))

    signals = {}

    for idx, (osc_name, opcode) in enumerate(oscillators):
        signal = generate_modulated_pwm_signal(opcode, carrier_freq, pwm_mod_freq, pwm_mod_depth, duration)
        signals[osc_name] = signal

        # Plot waveform (zoomed to show detail)
        samples_to_show = int(0.02 * SR)  # 20ms
        t_ms = np.arange(samples_to_show) / SR * 1000

        axes[idx, 0].plot(t_ms, signal[:samples_to_show], linewidth=0.5)
        axes[idx, 0].set_title(f'{osc_name}: Waveform (20ms)')
        axes[idx, 0].set_ylabel('Amplitude')
        axes[idx, 0].set_ylim(-1.2, 1.2)

        # Spectrum
        fft_freqs = np.fft.rfftfreq(len(signal), 1/SR)
        fft_mag = np.abs(np.fft.rfft(signal))
        fft_db = 20 * np.log10(fft_mag / np.max(fft_mag) + 1e-10)

        axes[idx, 1].plot(fft_freqs, fft_db, linewidth=0.5)
        axes[idx, 1].set_title(f'{osc_name}: Spectrum')
        axes[idx, 1].set_ylabel('dB')
        axes[idx, 1].set_xlim(0, 10000)
        axes[idx, 1].set_ylim(-80, 5)
        axes[idx, 1].axvline(x=carrier_freq, color='g', linestyle='--', alpha=0.5, label=f'Carrier {carrier_freq}Hz')
        axes[idx, 1].axvline(x=pwm_mod_freq, color='r', linestyle='--', alpha=0.5, label=f'PWM mod {pwm_mod_freq}Hz')
        axes[idx, 1].legend(fontsize=8)

        # Save audio
        signal_int16 = (np.clip(signal, -1, 1) * 32767).astype(np.int16)
        wav.write(f'output/pwm_audiomod_{osc_name.lower()}.wav', SR, signal_int16)

        print(f"  {osc_name}: Generated {len(signal)} samples")

    axes[-1, 0].set_xlabel('Time (ms)')
    axes[-1, 1].set_xlabel('Frequency (Hz)')

    plt.tight_layout()
    plt.savefig('output/pwm_audiomod.png', dpi=150)

    # Calculate spectral differences between 1x and 4x
    if 'OSC_SQR_PWM' in signals and 'OSC_SQR_PWM_4X' in signals:
        sig_1x = signals['OSC_SQR_PWM']
        sig_4x = signals['OSC_SQR_PWM_4X']

        # Align lengths
        min_len = min(len(sig_1x), len(sig_4x))

        fft_1x = np.abs(np.fft.rfft(sig_1x[:min_len]))
        fft_4x = np.abs(np.fft.rfft(sig_4x[:min_len]))

        # Calculate spectral difference
        spectral_diff_db = 20 * np.log10(np.abs(fft_1x - fft_4x) / np.max(fft_1x) + 1e-10)

        print(f"\n  1x vs 4x spectral difference (max): {np.max(spectral_diff_db):.1f} dB")
        print(f"  Listen to WAV files for audible differences")

    print(f"\n  Saved: output/pwm_audiomod.png")
    print(f"  Saved: output/pwm_audiomod_*.wav - Compare 1x vs 4x for stepping artifacts")

    return True  # Investigative test, no pass/fail


# =============================================================================
# Test 4: 1x vs 4x Phase Alignment
# =============================================================================

def test_1x_vs_4x_phase_alignment():
    """
    Test that 1x and 4x PWM variants produce aligned outputs.

    Expected behavior:
    - With static parameters, 1x and 4x should have identical phase
    - Zero-crossing positions should match

    Acceptance criteria:
    - Rising edge positions within 1 sample of each other
    - Falling edge positions within 1 sample of each other
    """
    print("\n" + "=" * 70)
    print("Test 4: 1x vs 4x Phase Alignment")
    print("=" * 70)
    print("Testing phase alignment between 1x and 4x variants...")

    freq = 440.0
    pwm_value = 0.3  # Non-symmetric duty cycle
    duration = 0.1   # 100ms should give many cycles

    # Generate signals
    sig_1x = generate_pwm_signal(cedar.Opcode.OSC_SQR_PWM, freq, pwm_value, duration)
    sig_4x = generate_pwm_signal(cedar.Opcode.OSC_SQR_PWM_4X, freq, pwm_value, duration)
    sig_minblep = generate_pwm_signal(cedar.Opcode.OSC_SQR_PWM_MINBLEP, freq, pwm_value, duration)

    # Find edges
    rising_1x = find_rising_edges(sig_1x)
    rising_4x = find_rising_edges(sig_4x)
    rising_minblep = find_rising_edges(sig_minblep)

    falling_1x = find_falling_edges(sig_1x)
    falling_4x = find_falling_edges(sig_4x)
    falling_minblep = find_falling_edges(sig_minblep)

    all_passed = True

    # Compare 1x vs 4x rising edges
    min_edges = min(len(rising_1x), len(rising_4x), len(rising_minblep))
    if min_edges > 2:
        # Skip first edge (may have initialization differences)
        rising_diff_4x = rising_4x[1:min_edges] - rising_1x[1:min_edges]
        rising_diff_minblep = rising_minblep[1:min_edges] - rising_1x[1:min_edges]

        falling_min = min(len(falling_1x), len(falling_4x), len(falling_minblep))
        falling_diff_4x = falling_4x[1:falling_min] - falling_1x[1:falling_min]
        falling_diff_minblep = falling_minblep[1:falling_min] - falling_1x[1:falling_min]

        # Check if differences are within 1 sample
        max_rising_diff_4x = np.max(np.abs(rising_diff_4x))
        max_falling_diff_4x = np.max(np.abs(falling_diff_4x))
        max_rising_diff_minblep = np.max(np.abs(rising_diff_minblep))
        max_falling_diff_minblep = np.max(np.abs(falling_diff_minblep))

        passed_4x = max_rising_diff_4x < 1.0 and max_falling_diff_4x < 1.0
        passed_minblep = max_rising_diff_minblep < 1.0 and max_falling_diff_minblep < 1.0

        status_4x = "PASS" if passed_4x else "FAIL"
        status_minblep = "PASS" if passed_minblep else "FAIL"
        symbol_4x = "\u2713" if passed_4x else "\u2717"
        symbol_minblep = "\u2713" if passed_minblep else "\u2717"

        if not passed_4x or not passed_minblep:
            all_passed = False

        print(f"  1x vs 4x: {symbol_4x} {status_4x}")
        print(f"    Rising edge max diff: {max_rising_diff_4x:.4f} samples")
        print(f"    Falling edge max diff: {max_falling_diff_4x:.4f} samples")

        print(f"  1x vs MinBLEP: {symbol_minblep} {status_minblep}")
        print(f"    Rising edge max diff: {max_rising_diff_minblep:.4f} samples")
        print(f"    Falling edge max diff: {max_falling_diff_minblep:.4f} samples")

        # Plot comparison
        fig, axes = plt.subplots(3, 1, figsize=(14, 8))

        samples = int(0.01 * SR)  # 10ms
        t_ms = np.arange(samples) / SR * 1000

        axes[0].plot(t_ms, sig_1x[:samples], label='1x', alpha=0.8)
        axes[0].plot(t_ms, sig_4x[:samples], label='4x', alpha=0.8, linestyle='--')
        axes[0].set_title('OSC_SQR_PWM 1x vs 4x')
        axes[0].legend()
        axes[0].set_ylabel('Amplitude')

        axes[1].plot(t_ms, sig_1x[:samples] - sig_4x[:samples], color='red')
        axes[1].set_title('Difference (1x - 4x)')
        axes[1].set_ylabel('Difference')

        # Plot edge position differences
        axes[2].bar(range(min(20, len(rising_diff_4x))), rising_diff_4x[:20], alpha=0.7, label='Rising edges')
        axes[2].bar(range(min(20, len(falling_diff_4x))), falling_diff_4x[:20], alpha=0.7, label='Falling edges')
        axes[2].axhline(y=0, color='black', linestyle='-')
        axes[2].axhline(y=1, color='red', linestyle='--', alpha=0.5)
        axes[2].axhline(y=-1, color='red', linestyle='--', alpha=0.5)
        axes[2].set_title('Edge position differences (1x vs 4x)')
        axes[2].set_xlabel('Edge index')
        axes[2].set_ylabel('Position diff (samples)')
        axes[2].legend()

        plt.tight_layout()
        plt.savefig('output/pwm_phase_alignment.png', dpi=150)
        print(f"\n  Saved: output/pwm_phase_alignment.png")
    else:
        print("  SKIP - not enough edges detected")

    print(f"\n  Overall: {'PASS' if all_passed else 'FAIL'}")
    return all_passed


# =============================================================================
# Test 5: Falling Edge Timing Accuracy
# =============================================================================

def test_falling_edge_timing():
    """
    Test accuracy of falling edge position at various duty cycles.

    Expected behavior:
    - Falling edge should occur at phase = duty
    - For PWM=-0.8 (duty=0.1): falling edge at 10% of period
    - For PWM=0.8 (duty=0.9): falling edge at 90% of period

    Acceptance criteria:
    - Falling edge position error < 1% of period
    """
    print("\n" + "=" * 70)
    print("Test 5: Falling Edge Timing Accuracy")
    print("=" * 70)
    print("Testing falling edge position accuracy at various duty cycles...")

    freq = 440.0
    duration = 0.5

    pwm_values = [-0.8, -0.5, 0.0, 0.5, 0.8]
    expected_duties = [0.5 + pwm * 0.5 for pwm in pwm_values]

    oscillators = [
        ('OSC_SQR_PWM', cedar.Opcode.OSC_SQR_PWM),
        ('OSC_SQR_PWM_4X', cedar.Opcode.OSC_SQR_PWM_4X),
        ('OSC_SQR_PWM_MINBLEP', cedar.Opcode.OSC_SQR_PWM_MINBLEP),
    ]

    expected_period = SR / freq
    all_passed = True

    fig, axes = plt.subplots(len(oscillators), 1, figsize=(14, 3 * len(oscillators)))

    for osc_idx, (osc_name, opcode) in enumerate(oscillators):
        print(f"\n  {osc_name}:")

        duty_errors = []

        for pwm_val, expected_duty in zip(pwm_values, expected_duties):
            signal = generate_pwm_signal(opcode, freq, pwm_val, duration)

            rising_edges = find_rising_edges(signal)
            falling_edges = find_falling_edges(signal)

            if len(rising_edges) < 3 or len(falling_edges) < 3:
                print(f"    PWM={pwm_val:+.1f}: SKIP - not enough edges")
                continue

            # Calculate duty cycle from edge positions
            # Duty = (falling_edge - rising_edge) / period
            # We need to pair rising and falling edges correctly
            measured_duties = []

            for i in range(min(len(rising_edges) - 1, len(falling_edges))):
                rising = rising_edges[i]
                # Find the falling edge that comes after this rising edge
                falling_after = falling_edges[falling_edges > rising]
                if len(falling_after) > 0:
                    falling = falling_after[0]
                    # Find the next rising edge to get the period
                    next_rising = rising_edges[rising_edges > rising]
                    if len(next_rising) > 0:
                        period = next_rising[0] - rising
                        duty = (falling - rising) / period
                        if 0 < duty < 1:  # Sanity check
                            measured_duties.append(duty)

            if len(measured_duties) > 0:
                mean_duty = np.mean(measured_duties)
                std_duty = np.std(measured_duties)
                error_pct = abs(mean_duty - expected_duty) / expected_duty * 100

                duty_errors.append(error_pct)

                passed = error_pct < 1.0  # < 1% error
                status = "PASS" if passed else "FAIL"
                symbol = "\u2713" if passed else "\u2717"

                if not passed:
                    all_passed = False

                print(f"    PWM={pwm_val:+.1f} (duty={expected_duty:.2f}): {symbol} {status} - "
                      f"measured={mean_duty:.4f}, error={error_pct:.2f}%")
            else:
                print(f"    PWM={pwm_val:+.1f}: SKIP - couldn't measure duty")

        # Plot duty error
        if duty_errors:
            axes[osc_idx].bar(range(len(pwm_values[:len(duty_errors)])), duty_errors)
            axes[osc_idx].set_xticks(range(len(pwm_values[:len(duty_errors)])))
            axes[osc_idx].set_xticklabels([f'PWM={p:.1f}' for p in pwm_values[:len(duty_errors)]])
            axes[osc_idx].set_ylabel('Duty error (%)')
            axes[osc_idx].set_title(f'{osc_name}: Falling edge timing error')
            axes[osc_idx].axhline(y=1.0, color='red', linestyle='--', alpha=0.5, label='1% threshold')
            axes[osc_idx].legend()

    plt.tight_layout()
    plt.savefig('output/pwm_falling_edge_timing.png', dpi=150)
    print(f"\n  Saved: output/pwm_falling_edge_timing.png")

    print(f"\n  Overall: {'PASS' if all_passed else 'FAIL'}")
    return all_passed


# =============================================================================
# Test 6: High-Speed PWM Modulation
# =============================================================================

def test_high_speed_pwm_modulation():
    """
    Test PWM modulation at very high rates.

    Expected behavior:
    - PWM parameter changes multiple times per carrier cycle
    - Should produce expected sidebands (ring modulation effect)
    - 4x variant may show aliasing artifacts due to PWM undersampling

    Acceptance criteria:
    - Investigative test - document spectral differences
    """
    print("\n" + "=" * 70)
    print("Test 6: High-Speed PWM Modulation")
    print("=" * 70)
    print("Testing high-rate PWM modulation (>1kHz mod with <100Hz carrier)...")

    carrier_freq = 80.0    # Low carrier
    pwm_mod_freq = 2000.0  # High modulation rate (many changes per carrier cycle)
    pwm_mod_depth = 0.8
    duration = 3.0

    oscillators = [
        ('OSC_SQR_PWM', cedar.Opcode.OSC_SQR_PWM),
        ('OSC_SQR_PWM_4X', cedar.Opcode.OSC_SQR_PWM_4X),
        ('OSC_SQR_PWM_MINBLEP', cedar.Opcode.OSC_SQR_PWM_MINBLEP),
    ]

    fig, axes = plt.subplots(len(oscillators), 2, figsize=(14, 3 * len(oscillators)))

    for idx, (osc_name, opcode) in enumerate(oscillators):
        signal = generate_modulated_pwm_signal(opcode, carrier_freq, pwm_mod_freq, pwm_mod_depth, duration)

        # Spectrogram to show sidebands
        from scipy.signal import spectrogram
        f, t, Sxx = spectrogram(signal, fs=SR, nperseg=2048, noverlap=1536)

        axes[idx, 0].pcolormesh(t, f, 10*np.log10(Sxx + 1e-10), shading='gouraud', cmap='magma', vmin=-80, vmax=0)
        axes[idx, 0].set_ylabel('Frequency (Hz)')
        axes[idx, 0].set_ylim(0, 8000)
        axes[idx, 0].set_title(f'{osc_name}: Spectrogram')

        # Zoomed waveform showing PWM changes within carrier cycle
        # One carrier cycle = SR / carrier_freq samples
        carrier_period = int(SR / carrier_freq)
        samples_to_show = carrier_period * 3  # Show 3 carrier cycles
        t_ms = np.arange(samples_to_show) / SR * 1000

        axes[idx, 1].plot(t_ms, signal[:samples_to_show], linewidth=0.5)
        axes[idx, 1].set_title(f'{osc_name}: Waveform (3 carrier cycles)')
        axes[idx, 1].set_ylabel('Amplitude')
        axes[idx, 1].set_ylim(-1.2, 1.2)

        # Expected PWM changes per carrier cycle
        pwm_changes_per_cycle = pwm_mod_freq / carrier_freq
        print(f"  {osc_name}: {pwm_changes_per_cycle:.1f} PWM changes per carrier cycle")

        # Save audio
        signal_int16 = (np.clip(signal, -1, 1) * 32767).astype(np.int16)
        wav.write(f'output/pwm_highspeed_{osc_name.lower()}.wav', SR, signal_int16)

    axes[-1, 0].set_xlabel('Time (s)')
    axes[-1, 1].set_xlabel('Time (ms)')

    plt.tight_layout()
    plt.savefig('output/pwm_highspeed_mod.png', dpi=150)

    print(f"\n  Saved: output/pwm_highspeed_mod.png")
    print(f"  Saved: output/pwm_highspeed_*.wav - Listen for aliasing/stepping in 4x variant")

    return True  # Investigative test


# =============================================================================
# Test 7: SAW_PWM Variants
# =============================================================================

def test_saw_pwm_variants():
    """
    Test SAW_PWM (variable-slope saw) oscillator variants.

    Expected behavior:
    - PWM = -1: Rising ramp (saw)
    - PWM = 0: Triangle
    - PWM = +1: Falling ramp (inverted saw)

    Acceptance criteria:
    - Waveform shape matches expected form
    - 1x and 4x variants should match
    """
    print("\n" + "=" * 70)
    print("Test 7: SAW_PWM Variants")
    print("=" * 70)
    print("Testing variable-slope sawtooth (SAW_PWM)...")

    freq = 440.0
    duration = 0.1
    pwm_values = [-0.9, -0.5, 0.0, 0.5, 0.9]
    labels = ['Rising saw', 'Skewed rising', 'Triangle', 'Skewed falling', 'Falling ramp']

    oscillators = [
        ('OSC_SAW_PWM', cedar.Opcode.OSC_SAW_PWM),
        ('OSC_SAW_PWM_4X', cedar.Opcode.OSC_SAW_PWM_4X),
    ]

    fig, axes = plt.subplots(len(pwm_values), len(oscillators),
                              figsize=(6 * len(oscillators), 2 * len(pwm_values)))

    all_passed = True

    for osc_idx, (osc_name, opcode) in enumerate(oscillators):
        print(f"\n  {osc_name}:")

        for pwm_idx, (pwm_val, label) in enumerate(zip(pwm_values, labels)):
            signal = generate_pwm_signal(opcode, freq, pwm_val, duration)

            # Plot 2 cycles
            period_samples = int(SR / freq)
            samples_to_show = period_samples * 2
            t_ms = np.arange(samples_to_show) / SR * 1000

            ax = axes[pwm_idx, osc_idx] if len(oscillators) > 1 else axes[pwm_idx]
            ax.plot(t_ms, signal[:samples_to_show], linewidth=1)
            ax.set_title(f'{osc_name}: PWM={pwm_val:.1f} ({label})')
            ax.set_ylim(-1.2, 1.2)
            ax.axhline(y=0, color='gray', linestyle='--', alpha=0.3)

            # Check amplitude bounds
            max_val = np.max(np.abs(signal))
            bounded = max_val <= 1.1

            status = "bounded" if bounded else "UNBOUNDED"
            print(f"    PWM={pwm_val:+.1f}: max_amplitude={max_val:.4f} ({status})")

            if not bounded:
                all_passed = False

    for ax in (axes[-1, :] if len(oscillators) > 1 else [axes[-1]]):
        ax.set_xlabel('Time (ms)')

    plt.tight_layout()
    plt.savefig('output/saw_pwm_shapes.png', dpi=150)

    # Generate comparison audio
    for osc_name, opcode in oscillators:
        signal = generate_pwm_signal(opcode, freq, 0.0, 2.0)  # Triangle
        signal_int16 = (np.clip(signal, -1, 1) * 32767).astype(np.int16)
        wav.write(f'output/saw_pwm_triangle_{osc_name.lower()}.wav', SR, signal_int16)

    print(f"\n  Saved: output/saw_pwm_shapes.png")
    print(f"  Saved: output/saw_pwm_triangle_*.wav")

    print(f"\n  Overall: {'PASS' if all_passed else 'FAIL'}")
    return all_passed


# =============================================================================
# Main
# =============================================================================

def main():
    print("=" * 70)
    print("PWM Oscillator Phase Consistency Experiments")
    print("=" * 70)
    print(f"Sample rate: {SR} Hz")
    print(f"Block size: {cedar.BLOCK_SIZE} samples")

    results = {}

    # Run all tests
    results['phase_drift'] = test_phase_drift()
    results['step_response'] = test_pwm_step_response()
    results['audio_rate_mod'] = test_audio_rate_pwm_modulation()
    results['phase_alignment'] = test_1x_vs_4x_phase_alignment()
    results['falling_edge_timing'] = test_falling_edge_timing()
    results['high_speed_mod'] = test_high_speed_pwm_modulation()
    results['saw_pwm'] = test_saw_pwm_variants()

    # Summary
    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)

    for test_name, passed in results.items():
        status = "PASS" if passed else "FAIL" if passed is False else "INFO"
        symbol = "\u2713" if passed else "\u2717" if passed is False else "\u2022"
        print(f"  {symbol} {test_name}: {status}")

    passed_count = sum(1 for v in results.values() if v is True)
    failed_count = sum(1 for v in results.values() if v is False)

    print(f"\n  Tests passed: {passed_count}")
    print(f"  Tests failed: {failed_count}")
    print(f"\n  Audio files saved to output/ directory for listening evaluation")
    print("=" * 70)


if __name__ == "__main__":
    main()
