"""Basic oscillator test to verify oversampling implementation."""

import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar

def test_basic_sine_outputs():
    """Test that basic sine oscillators produce correct waveforms."""
    print("\n=== Basic Sine Oscillator Test ===")
    sr = 48000
    freq = 1000.0  # 1kHz - 48 samples per cycle
    duration = 0.01  # 10ms

    oscillators = [
        ('OSC_SIN', cedar.Opcode.OSC_SIN),
        ('OSC_SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('OSC_SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(len(oscillators), 2, figsize=(14, 2.5*len(oscillators)))

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
        num_blocks = max(1, int((duration * sr) / cedar.BLOCK_SIZE))
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        # Time domain
        samples = min(200, len(signal))
        t = np.arange(samples) / sr * 1000
        axes[idx, 0].plot(t, signal[:samples], linewidth=0.8)
        axes[idx, 0].set_title(f'{name}: Waveform')
        axes[idx, 0].set_ylabel('Amplitude')
        axes[idx, 0].set_ylim(-1.2, 1.2)

        # FFT
        fft_freqs = np.fft.rfftfreq(len(signal), 1/sr)
        fft_db = 20 * np.log10(np.abs(np.fft.rfft(signal)) + 1e-10)
        fft_db -= np.max(fft_db)  # Normalize
        axes[idx, 1].plot(fft_freqs, fft_db, linewidth=0.5)
        axes[idx, 1].set_title(f'{name}: Spectrum')
        axes[idx, 1].set_ylabel('dB')
        axes[idx, 1].set_xlim(0, 5000)
        axes[idx, 1].set_ylim(-100, 5)

        # Measure THD (total harmonic distortion)
        fundamental_bin = np.argmin(np.abs(fft_freqs - freq))
        fund_power = np.abs(np.fft.rfft(signal)[fundamental_bin])**2
        total_power = np.sum(np.abs(np.fft.rfft(signal))**2)
        thd = np.sqrt((total_power - fund_power) / fund_power) * 100
        print(f"  {name}: THD = {thd:.4f}%")

    axes[-1, 0].set_xlabel('Time (ms)')
    axes[-1, 1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig('output/osc_basic_sine.png', dpi=150)
    print("Saved: output/osc_basic_sine.png")


def test_high_frequency_sine():
    """Test sine at high frequency to check aliasing."""
    print("\n=== High Frequency Sine Test (20kHz) ===")
    sr = 48000
    freq = 20000.0  # Near Nyquist
    duration = 0.05

    oscillators = [
        ('OSC_SIN', cedar.Opcode.OSC_SIN),
        ('OSC_SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('OSC_SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(len(oscillators), 1, figsize=(14, 2.5*len(oscillators)))

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
        num_blocks = max(1, int((duration * sr) / cedar.BLOCK_SIZE))
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        # FFT
        fft_freqs = np.fft.rfftfreq(len(signal), 1/sr)
        fft_db = 20 * np.log10(np.abs(np.fft.rfft(signal)) + 1e-10)
        fft_db -= np.max(fft_db)

        axes[idx].plot(fft_freqs, fft_db, linewidth=0.5)
        axes[idx].set_title(f'{name} at {freq/1000:.0f}kHz')
        axes[idx].set_ylabel('dB')
        axes[idx].set_xlim(0, sr/2)
        axes[idx].set_ylim(-100, 5)
        axes[idx].axvline(x=freq, color='r', linestyle='--', alpha=0.5)

    axes[-1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig('output/osc_high_freq.png', dpi=150)
    print("Saved: output/osc_high_freq.png")


def test_frequency_sweep():
    """Test frequency sweep to check filter response."""
    print("\n=== Frequency Sweep Test ===")
    sr = 48000
    duration = 1.0

    oscillators = [
        ('OSC_SIN', cedar.Opcode.OSC_SIN),
        ('OSC_SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(2, 1, figsize=(14, 6))

    for idx, (name, opcode) in enumerate(oscillators):
        vm = cedar.VM()
        vm.set_sample_rate(sr)

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
            cedar.Instruction.make_unary(opcode, 1, 10, cedar.hash('osc')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]
        vm.load_program(program)

        signal = []
        num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)

        for block_idx in range(num_blocks):
            # Exponential sweep from 100 Hz to 20 kHz
            t = block_idx / num_blocks
            freq = 100.0 * (200.0 ** t)  # 100 Hz to 20 kHz
            vm.set_param('freq', freq)

            left, right = vm.process()
            signal.append(left)

        signal = np.concatenate(signal)

        # Spectrogram
        from scipy.signal import spectrogram
        f, t, Sxx = spectrogram(signal, fs=sr, nperseg=512, noverlap=384)
        axes[idx].pcolormesh(t, f, 10*np.log10(Sxx + 1e-10), shading='gouraud', cmap='magma', vmin=-80, vmax=0)
        axes[idx].set_ylabel('Frequency (Hz)')
        axes[idx].set_ylim(0, sr/2)
        axes[idx].set_title(f'{name}: Frequency Sweep 100Hz → 20kHz')

    axes[-1].set_xlabel('Time (s)')
    plt.tight_layout()
    plt.savefig('output/osc_freq_sweep.png', dpi=150)
    print("Saved: output/osc_freq_sweep.png")


def test_simple_fm():
    """Simple FM test with constant modulation."""
    print("\n=== Simple FM Test ===")
    sr = 48000
    carrier = 440.0
    mod_depth = 440.0  # ±440 Hz deviation
    mod_freq = 100.0
    duration = 0.1

    oscillators = [
        ('OSC_SIN', cedar.Opcode.OSC_SIN),
        ('OSC_SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(len(oscillators), 2, figsize=(14, 2.5*len(oscillators)))

    for idx, (name, opcode) in enumerate(oscillators):
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('carrier', carrier)
        vm.set_param('mod_freq', mod_freq)
        vm.set_param('mod_depth', mod_depth)

        # Modulator -> scale -> add to carrier -> oscillator
        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('mod_freq')),
            cedar.Instruction.make_unary(opcode, 11, 10, cedar.hash('mod_osc')),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 12, cedar.hash('mod_depth')),
            cedar.Instruction.make_binary(cedar.Opcode.MUL, 13, 11, 12),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 14, cedar.hash('carrier')),
            cedar.Instruction.make_binary(cedar.Opcode.ADD, 15, 14, 13),
            cedar.Instruction.make_unary(opcode, 1, 15, cedar.hash('carrier_osc')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]
        vm.load_program(program)

        signal = []
        num_blocks = max(1, int((duration * sr) / cedar.BLOCK_SIZE))
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)

        # Waveform
        samples = min(500, len(signal))
        t = np.arange(samples) / sr * 1000
        axes[idx, 0].plot(t, signal[:samples], linewidth=0.5)
        axes[idx, 0].set_title(f'{name}: Waveform')
        axes[idx, 0].set_ylabel('Amplitude')

        # Spectrum
        fft_freqs = np.fft.rfftfreq(len(signal), 1/sr)
        fft_db = 20 * np.log10(np.abs(np.fft.rfft(signal)) + 1e-10)
        fft_db -= np.max(fft_db)
        axes[idx, 1].plot(fft_freqs, fft_db, linewidth=0.5)
        axes[idx, 1].set_title(f'{name}: Spectrum')
        axes[idx, 1].set_xlim(0, 3000)
        axes[idx, 1].set_ylim(-80, 5)

    axes[-1, 0].set_xlabel('Time (ms)')
    axes[-1, 1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig('output/osc_simple_fm.png', dpi=150)
    print("Saved: output/osc_simple_fm.png")


def test_compare_with_reference():
    """Compare oscillator output with numpy reference."""
    print("\n=== Reference Comparison Test ===")
    sr = 48000
    freq = 1000.0
    duration = 0.01

    # Generate numpy reference
    num_samples = int(duration * sr)
    t = np.arange(num_samples) / sr
    reference = np.sin(2 * np.pi * freq * t)

    oscillators = [
        ('OSC_SIN', cedar.Opcode.OSC_SIN),
        ('OSC_SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('OSC_SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    print(f"  Reference (numpy): {num_samples} samples")

    for name, opcode in oscillators:
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
        num_blocks = max(1, int((duration * sr) / cedar.BLOCK_SIZE))
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)[:num_samples]

        # Calculate error
        error = signal - reference
        rms_error = np.sqrt(np.mean(error**2))
        max_error = np.max(np.abs(error))
        print(f"  {name}: RMS error = {rms_error:.6f}, Max error = {max_error:.6f}")


if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)

    test_basic_sine_outputs()
    test_high_frequency_sine()
    test_frequency_sweep()
    test_simple_fm()
    test_compare_with_reference()

    print("\n=== Basic oscillator tests complete ===")
