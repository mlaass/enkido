"""Test PWM oscillator implementation."""

import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar

def test_pwm_duty_cycles():
    """Test PWM at different duty cycles."""
    print("\n=== PWM Duty Cycle Test ===")
    sr = 48000
    freq = 440.0

    # Test different PWM values
    pwm_values = [-0.8, -0.5, 0.0, 0.5, 0.8]  # Maps to duty cycles: 0.1, 0.25, 0.5, 0.75, 0.9

    fig, axes = plt.subplots(len(pwm_values), 1, figsize=(12, 2.5*len(pwm_values)))

    for idx, pwm_val in enumerate(pwm_values):
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('freq', freq)
        vm.set_param('pwm', pwm_val)

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
            cedar.Instruction.make_binary(cedar.Opcode.OSC_SQR_PWM, 1, 10, 11, cedar.hash('sqr_pwm')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]

        vm.load_program(program)

        # Generate a few cycles
        signal = []
        num_blocks = 5
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        # Calculate actual duty cycle from signal
        high_samples = np.sum(signal > 0)
        total_samples = len(signal)
        actual_duty = high_samples / total_samples
        expected_duty = 0.5 + pwm_val * 0.5

        print(f"PWM={pwm_val:+.1f}: Expected duty={expected_duty:.2f}, Actual={actual_duty:.2f}")

        # Plot
        ax = axes[idx]
        time_ms = np.arange(len(signal)) / sr * 1000
        ax.plot(time_ms, signal)
        ax.set_title(f'PWM={pwm_val:+.1f} (duty={expected_duty:.0%})')
        ax.set_xlim(0, 5)  # Show first 5ms
        ax.set_ylim(-1.2, 1.2)
        ax.set_ylabel('Amplitude')
        ax.axhline(y=0, color='gray', linestyle='--', alpha=0.5)

    axes[-1].set_xlabel('Time (ms)')
    plt.tight_layout()
    plt.savefig('output/pwm_duty_cycles.png', dpi=150)
    print("Saved: output/pwm_duty_cycles.png")


def test_pwm_modulation():
    """Test PWM with swept modulation."""
    print("\n=== PWM Modulation Sweep Test ===")
    sr = 48000
    freq = 110.0
    duration = 1.0

    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('freq', freq)

    # We'll manually modulate PWM from Python
    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
        cedar.Instruction.make_binary(cedar.Opcode.OSC_SQR_PWM, 1, 10, 11, cedar.hash('sqr_pwm')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]

    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)

    for block_idx in range(num_blocks):
        # Sweep PWM from -0.9 to 0.9
        t = block_idx / num_blocks
        pwm = -0.9 + 1.8 * t
        vm.set_param('pwm', pwm)

        left, right = vm.process()
        signal.append(left)

    signal = np.concatenate(signal)

    # Plot spectrogram to see PWM effect
    fig, ax = plt.subplots(figsize=(12, 4))
    from scipy.signal import spectrogram
    f, t, Sxx = spectrogram(signal, fs=sr, nperseg=1024, noverlap=768)
    ax.pcolormesh(t, f, 10*np.log10(Sxx + 1e-10), shading='gouraud', cmap='magma')
    ax.set_ylabel('Frequency (Hz)')
    ax.set_xlabel('Time (s)')
    ax.set_ylim(0, 5000)
    ax.set_title('PWM Sweep: -0.9 to +0.9 over 1 second')
    plt.colorbar(ax.collections[0], ax=ax, label='dB')
    plt.tight_layout()
    plt.savefig('output/pwm_sweep_spectrogram.png', dpi=150)
    print("Saved: output/pwm_sweep_spectrogram.png")


def test_saw_pwm():
    """Test SAW_PWM (variable slope saw)."""
    print("\n=== SAW PWM (Variable Slope) Test ===")
    sr = 48000
    freq = 440.0

    pwm_values = [-0.9, -0.5, 0.0, 0.5, 0.9]
    labels = ['Rising ramp', 'Skewed saw', 'Triangle', 'Skewed tri', 'Falling ramp']

    fig, axes = plt.subplots(len(pwm_values), 1, figsize=(12, 2.5*len(pwm_values)))

    for idx, (pwm_val, label) in enumerate(zip(pwm_values, labels)):
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('freq', freq)
        vm.set_param('pwm', pwm_val)

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm')),
            cedar.Instruction.make_binary(cedar.Opcode.OSC_SAW_PWM, 1, 10, 11, cedar.hash('saw_pwm')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]

        vm.load_program(program)

        signal = []
        for _ in range(5):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        ax = axes[idx]
        time_ms = np.arange(len(signal)) / sr * 1000
        ax.plot(time_ms, signal)
        ax.set_title(f'PWM={pwm_val:+.1f}: {label}')
        ax.set_xlim(0, 5)
        ax.set_ylim(-1.2, 1.2)
        ax.set_ylabel('Amplitude')
        ax.axhline(y=0, color='gray', linestyle='--', alpha=0.5)

    axes[-1].set_xlabel('Time (ms)')
    plt.tight_layout()
    plt.savefig('output/saw_pwm_shapes.png', dpi=150)
    print("Saved: output/saw_pwm_shapes.png")


def test_oversampled_oscillators():
    """Test oversampled oscillators for FM."""
    print("\n=== Oversampled Oscillator Test ===")
    sr = 48000
    duration = 0.5

    # Compare standard vs 2x vs 4x sine at high frequency
    freq = 10000.0  # 10 kHz - high frequency where aliasing is more visible

    fig, axes = plt.subplots(3, 1, figsize=(12, 6))
    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    for idx, (name, opcode) in enumerate(oscillators):
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('freq', freq)

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
            cedar.Instruction.make_unary(opcode, 1, 10, cedar.hash('osc')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]

        vm.load_program(program)

        signal = []
        num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        ax = axes[idx]
        time_ms = np.arange(len(signal)) / sr * 1000
        ax.plot(time_ms[:200], signal[:200])  # Show first 200 samples
        ax.set_title(f'{name} at {freq/1000:.0f} kHz')
        ax.set_ylabel('Amplitude')

    axes[-1].set_xlabel('Time (ms)')
    plt.tight_layout()
    plt.savefig('output/oversampled_comparison.png', dpi=150)
    print("Saved: output/oversampled_comparison.png")


def test_fm_aliasing():
    """Test FM synthesis aliasing with different oscillators."""
    print("\n=== FM Aliasing Comparison Test ===")
    sr = 48000
    duration = 1.0
    carrier_freq = 440.0
    mod_freq = 440.0
    mod_index = 5.0  # High modulation index = more aliasing

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(len(oscillators), 1, figsize=(12, 3*len(oscillators)))

    for idx, (name, opcode) in enumerate(oscillators):
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('carrier', carrier_freq)
        vm.set_param('mod_freq', mod_freq)
        vm.set_param('mod_index', mod_index * mod_freq)  # Deviation in Hz

        # FM synthesis: carrier + (modulator * mod_index) -> oscillator
        program = [
            # Modulator oscillator
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('mod_freq')),
            cedar.Instruction.make_unary(opcode, 11, 10, cedar.hash('mod_osc')),

            # Scale by mod index
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 12, cedar.hash('mod_index')),
            cedar.Instruction.make_binary(cedar.Opcode.MUL, 13, 11, 12),

            # Add to carrier frequency
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 14, cedar.hash('carrier')),
            cedar.Instruction.make_binary(cedar.Opcode.ADD, 15, 14, 13),

            # Carrier oscillator with FM'd frequency
            cedar.Instruction.make_unary(opcode, 1, 15, cedar.hash('carrier_osc')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]

        vm.load_program(program)

        signal = []
        num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        # FFT analysis
        fft_freqs = np.fft.rfftfreq(len(signal), 1/sr)
        fft_mag = np.abs(np.fft.rfft(signal))
        fft_db = 20 * np.log10(fft_mag / np.max(fft_mag) + 1e-10)

        ax = axes[idx]
        ax.plot(fft_freqs, fft_db)
        ax.set_title(f'FM Synthesis with {name} (carrier={carrier_freq}Hz, mod={mod_freq}Hz, index={mod_index})')
        ax.set_xlim(0, sr/2)
        ax.set_ylim(-80, 0)
        ax.set_ylabel('Magnitude (dB)')
        ax.axvline(x=sr/2 - carrier_freq, color='r', linestyle='--', alpha=0.5, label='Nyquist - carrier')

    axes[-1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig('output/fm_aliasing_comparison.png', dpi=150)
    print("Saved: output/fm_aliasing_comparison.png")


if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)

    test_pwm_duty_cycles()
    test_pwm_modulation()
    test_saw_pwm()
    test_oversampled_oscillators()
    test_fm_aliasing()

    print("\n=== All tests complete ===")
