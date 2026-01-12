"""Comprehensive FM aliasing tests for oversampled oscillators.

These tests use aggressive parameters to clearly demonstrate the difference
between 1x, 2x, and 4x oversampled oscillators for FM synthesis.
"""

import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar
from scipy.signal import spectrogram
from scipy.io import wavfile
import os


def export_wav(filename, signal, sr=48000):
    """Export signal as 16-bit WAV file."""
    # Normalize to prevent clipping
    peak = np.max(np.abs(signal))
    if peak > 0:
        signal = signal / peak * 0.9

    # Convert to 16-bit integer
    signal_int = (signal * 32767).astype(np.int16)
    wavfile.write(filename, sr, signal_int)
    print(f"  Exported: {filename}")


def generate_fm_signal(carrier_freq, mod_freq, mod_index, duration, sr, osc_opcode):
    """Generate FM signal using specified oscillator."""
    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('carrier', carrier_freq)
    vm.set_param('mod_freq', mod_freq)
    vm.set_param('mod_index', mod_index * mod_freq)  # Deviation in Hz

    # FM synthesis: carrier + (modulator * mod_index) -> oscillator
    program = [
        # Modulator oscillator
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('mod_freq')),
        cedar.Instruction.make_unary(osc_opcode, 11, 10, cedar.hash('mod_osc')),

        # Scale by mod index
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 12, cedar.hash('mod_index')),
        cedar.Instruction.make_binary(cedar.Opcode.MUL, 13, 11, 12),

        # Add to carrier frequency
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 14, cedar.hash('carrier')),
        cedar.Instruction.make_binary(cedar.Opcode.ADD, 15, 14, 13),

        # Carrier oscillator with FM'd frequency
        cedar.Instruction.make_unary(osc_opcode, 1, 15, cedar.hash('carrier_osc')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]

    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    for _ in range(num_blocks):
        left, right = vm.process()
        signal.append(left)

    return np.concatenate(signal)


def analyze_spectrum(signal, sr, title="", ax=None):
    """Compute FFT and optionally plot."""
    fft_freqs = np.fft.rfftfreq(len(signal), 1/sr)
    fft_mag = np.abs(np.fft.rfft(signal))
    fft_db = 20 * np.log10(fft_mag / np.max(fft_mag) + 1e-10)

    if ax:
        ax.plot(fft_freqs, fft_db, linewidth=0.5)
        ax.set_title(title)
        ax.set_xlim(0, sr/2)
        ax.set_ylim(-100, 0)
        ax.set_ylabel('Magnitude (dB)')
        ax.axhline(y=-60, color='r', linestyle='--', alpha=0.3, label='-60dB')
        ax.axhline(y=-80, color='orange', linestyle='--', alpha=0.3, label='-80dB')

    return fft_freqs, fft_db


def measure_noise_floor(fft_db, fft_freqs, signal_freqs, bandwidth=100):
    """Measure noise floor in regions without expected signal."""
    # Find bins that should be noise (not near signal frequencies)
    noise_mask = np.ones(len(fft_freqs), dtype=bool)

    for freq in signal_freqs:
        noise_mask &= (np.abs(fft_freqs - freq) > bandwidth)

    if not np.any(noise_mask):
        return np.nan

    # Median of noise bins (more robust than mean)
    return np.median(fft_db[noise_mask])


def test_moderate_fm():
    """Test 1: Moderate FM showing clear aliasing differences.

    Carrier: 4000 Hz, Modulator: 2000 Hz, Index: 7
    Sidebands extend to 4000 + 14*2000 = 32000 Hz (beyond Nyquist at 24kHz)
    """
    print("\n=== Test 1: Moderate FM (carrier=4kHz, mod=2kHz, index=7) ===")
    sr = 48000
    carrier = 4000.0
    mod = 2000.0
    index = 7.0
    duration = 1.0

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(3, 1, figsize=(14, 9))
    noise_floors = []

    # Expected sideband frequencies (for analysis)
    expected_sidebands = [carrier + n * mod for n in range(-15, 16)]
    expected_sidebands = [f for f in expected_sidebands if 0 < f < sr/2]

    for idx, (name, opcode) in enumerate(oscillators):
        signal = generate_fm_signal(carrier, mod, index, duration, sr, opcode)
        fft_freqs, fft_db = analyze_spectrum(signal, sr, f'{name}', axes[idx])

        noise = measure_noise_floor(fft_db, fft_freqs, expected_sidebands)
        noise_floors.append(noise)
        print(f"  {name}: noise floor = {noise:.1f} dB")

    axes[-1].set_xlabel('Frequency (Hz)')
    fig.suptitle(f'FM Synthesis: Carrier={carrier}Hz, Mod={mod}Hz, Index={index}')
    plt.tight_layout()
    plt.savefig('output/fm_moderate.png', dpi=150)
    print("Saved: output/fm_moderate.png")

    return noise_floors


def test_extreme_fm():
    """Test 2: Extreme FM showing dramatic aliasing differences.

    Carrier: 8000 Hz, Modulator: 8000 Hz, Index: 10
    Sidebands extend to 8000 + 80000 = 88000 Hz (massive aliasing at 1x)
    """
    print("\n=== Test 2: Extreme FM (carrier=8kHz, mod=8kHz, index=10) ===")
    sr = 48000
    carrier = 8000.0
    mod = 8000.0
    index = 10.0
    duration = 1.0

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(3, 1, figsize=(14, 9))
    noise_floors = []

    expected_sidebands = [carrier + n * mod for n in range(-12, 13)]
    expected_sidebands = [f for f in expected_sidebands if 0 < f < sr/2]

    for idx, (name, opcode) in enumerate(oscillators):
        signal = generate_fm_signal(carrier, mod, index, duration, sr, opcode)
        fft_freqs, fft_db = analyze_spectrum(signal, sr, f'{name}', axes[idx])

        noise = measure_noise_floor(fft_db, fft_freqs, expected_sidebands)
        noise_floors.append(noise)
        print(f"  {name}: noise floor = {noise:.1f} dB")

    axes[-1].set_xlabel('Frequency (Hz)')
    fig.suptitle(f'FM Synthesis: Carrier={carrier}Hz, Mod={mod}Hz, Index={index}')
    plt.tight_layout()
    plt.savefig('output/fm_extreme.png', dpi=150)
    print("Saved: output/fm_extreme.png")

    return noise_floors


def test_classic_bell():
    """Test 3: Classic FM bell sound.

    Carrier: 500 Hz, Modulator: 1000 Hz (2:1 ratio)
    Index: 15 (rich metallic tone)
    """
    print("\n=== Test 3: Classic Bell FM (carrier=500Hz, mod=1kHz, index=15) ===")
    sr = 48000
    carrier = 500.0
    mod = 1000.0
    index = 15.0
    duration = 1.0

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(3, 1, figsize=(14, 9))
    noise_floors = []

    # Bell has many sidebands
    expected_sidebands = [carrier + n * mod for n in range(-20, 21)]
    expected_sidebands = [f for f in expected_sidebands if 0 < f < sr/2]

    for idx, (name, opcode) in enumerate(oscillators):
        signal = generate_fm_signal(carrier, mod, index, duration, sr, opcode)
        fft_freqs, fft_db = analyze_spectrum(signal, sr, f'{name}', axes[idx])

        noise = measure_noise_floor(fft_db, fft_freqs, expected_sidebands)
        noise_floors.append(noise)
        print(f"  {name}: noise floor = {noise:.1f} dB")

    axes[-1].set_xlabel('Frequency (Hz)')
    fig.suptitle(f'FM Bell: Carrier={carrier}Hz, Mod={mod}Hz, Index={index}')
    plt.tight_layout()
    plt.savefig('output/fm_bell.png', dpi=150)
    print("Saved: output/fm_bell.png")

    return noise_floors


def test_comparison_overlay():
    """Overlay all three oscillator variants on same plot for direct comparison."""
    print("\n=== Comparison Overlay Test ===")
    sr = 48000
    carrier = 6000.0
    mod = 3000.0
    index = 8.0
    duration = 1.0

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN, 'red'),
        ('SIN_2X', cedar.Opcode.OSC_SIN_2X, 'blue'),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X, 'green'),
    ]

    fig, ax = plt.subplots(figsize=(14, 6))

    for name, opcode, color in oscillators:
        signal = generate_fm_signal(carrier, mod, index, duration, sr, opcode)
        fft_freqs, fft_db = analyze_spectrum(signal, sr)
        ax.plot(fft_freqs, fft_db, linewidth=0.5, label=name, color=color, alpha=0.7)

    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_title(f'FM Comparison: Carrier={carrier}Hz, Mod={mod}Hz, Index={index}')
    ax.set_xlim(0, sr/2)
    ax.set_ylim(-100, 0)
    ax.legend()
    ax.axhline(y=-60, color='gray', linestyle='--', alpha=0.3)
    ax.axhline(y=-80, color='gray', linestyle='--', alpha=0.3)

    plt.tight_layout()
    plt.savefig('output/fm_overlay_comparison.png', dpi=150)
    print("Saved: output/fm_overlay_comparison.png")


def test_spectrogram_sweep():
    """Spectrogram showing FM with swept modulation index."""
    print("\n=== Spectrogram Sweep Test ===")
    sr = 48000
    carrier = 4000.0
    mod = 2000.0
    duration = 2.0

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(2, 1, figsize=(14, 8))

    for idx, (name, opcode) in enumerate(oscillators):
        # Sweep modulation index from 0 to 12
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('carrier', carrier)
        vm.set_param('mod_freq', mod)

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('mod_freq')),
            cedar.Instruction.make_unary(opcode, 11, 10, cedar.hash('mod_osc')),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 12, cedar.hash('mod_index')),
            cedar.Instruction.make_binary(cedar.Opcode.MUL, 13, 11, 12),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 14, cedar.hash('carrier')),
            cedar.Instruction.make_binary(cedar.Opcode.ADD, 15, 14, 13),
            cedar.Instruction.make_unary(opcode, 1, 15, cedar.hash('carrier_osc')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]

        vm.load_program(program)

        signal = []
        num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)

        for block_idx in range(num_blocks):
            t = block_idx / num_blocks
            mod_index = t * 12.0  # Sweep from 0 to 12
            vm.set_param('mod_index', mod_index * mod)

            left, right = vm.process()
            signal.append(left)

        signal = np.concatenate(signal)

        # Spectrogram
        f, t, Sxx = spectrogram(signal, fs=sr, nperseg=1024, noverlap=768)
        axes[idx].pcolormesh(t, f, 10*np.log10(Sxx + 1e-10), shading='gouraud', cmap='magma', vmin=-80, vmax=0)
        axes[idx].set_ylabel('Frequency (Hz)')
        axes[idx].set_ylim(0, sr/2)
        axes[idx].set_title(f'{name}: Index sweep 0→12')

    axes[-1].set_xlabel('Time (s)')
    plt.tight_layout()
    plt.savefig('output/fm_spectrogram_sweep.png', dpi=150)
    print("Saved: output/fm_spectrogram_sweep.png")


def test_zoomed_detail():
    """Zoomed view showing detail of aliasing artifacts."""
    print("\n=== Zoomed Detail Test ===")
    sr = 48000
    carrier = 8000.0
    mod = 4000.0
    index = 10.0
    duration = 1.0

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(2, 2, figsize=(14, 8))

    for idx, (name, opcode) in enumerate(oscillators):
        signal = generate_fm_signal(carrier, mod, index, duration, sr, opcode)
        fft_freqs, fft_db = analyze_spectrum(signal, sr)

        # Full spectrum
        axes[idx, 0].plot(fft_freqs, fft_db, linewidth=0.5)
        axes[idx, 0].set_xlim(0, sr/2)
        axes[idx, 0].set_ylim(-100, 0)
        axes[idx, 0].set_title(f'{name}: Full Spectrum')
        axes[idx, 0].set_ylabel('Magnitude (dB)')

        # Zoomed to 18-24 kHz (where aliased content appears)
        mask = (fft_freqs >= 18000) & (fft_freqs <= 24000)
        axes[idx, 1].plot(fft_freqs[mask], fft_db[mask], linewidth=0.5)
        axes[idx, 1].set_xlim(18000, 24000)
        axes[idx, 1].set_ylim(-100, 0)
        axes[idx, 1].set_title(f'{name}: Zoomed 18-24kHz (aliasing region)')
        axes[idx, 1].axhline(y=-60, color='r', linestyle='--', alpha=0.3)

    axes[-1, 0].set_xlabel('Frequency (Hz)')
    axes[-1, 1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig('output/fm_zoomed_detail.png', dpi=150)
    print("Saved: output/fm_zoomed_detail.png")


def test_irrational_ratio_fm():
    """Test with irrational frequency ratio to show clear aliasing.

    Using golden ratio multiplier so aliased content doesn't land on sidebands.
    Carrier: 5000 Hz, Modulator: 5000 * phi = 8090.17 Hz, Index: 8
    """
    print("\n=== Test: Irrational Ratio FM (clear aliasing demonstration) ===")
    sr = 48000
    phi = 1.6180339887  # Golden ratio
    carrier = 5000.0
    mod = carrier * phi  # ~8090 Hz
    index = 8.0
    duration = 1.0

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_2X', cedar.Opcode.OSC_SIN_2X),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    fig, axes = plt.subplots(3, 1, figsize=(14, 9))

    for idx, (name, opcode) in enumerate(oscillators):
        signal = generate_fm_signal(carrier, mod, index, duration, sr, opcode)
        fft_freqs, fft_db = analyze_spectrum(signal, sr, f'{name}', axes[idx])

        # Measure energy in specific aliasing region (near Nyquist)
        mask = (fft_freqs >= 20000) & (fft_freqs <= 24000)
        nyquist_energy = np.mean(fft_db[mask])
        print(f"  {name}: energy 20-24kHz = {nyquist_energy:.1f} dB")

    axes[-1].set_xlabel('Frequency (Hz)')
    fig.suptitle(f'FM Synthesis with Irrational Ratio: Carrier={carrier}Hz, Mod={mod:.1f}Hz, Index={index}')
    plt.tight_layout()
    plt.savefig('output/fm_irrational_ratio.png', dpi=150)
    print("Saved: output/fm_irrational_ratio.png")


def test_aliasing_visualization():
    """Direct visualization of aliasing artifacts.

    Shows high-frequency region where aliased content appears.
    """
    print("\n=== Aliasing Visualization Test ===")
    sr = 48000
    carrier = 10000.0  # High carrier
    mod = 6000.0       # Creates sidebands up to ~70kHz with index=10
    index = 10.0
    duration = 0.5

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN, 'red'),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X, 'green'),
    ]

    fig, axes = plt.subplots(2, 2, figsize=(14, 8))

    for idx, (name, opcode, color) in enumerate(oscillators):
        signal = generate_fm_signal(carrier, mod, index, duration, sr, opcode)
        fft_freqs, fft_db = analyze_spectrum(signal, sr)

        # Full spectrum
        axes[0, idx].plot(fft_freqs, fft_db, linewidth=0.5, color=color)
        axes[0, idx].set_xlim(0, sr/2)
        axes[0, idx].set_ylim(-100, 0)
        axes[0, idx].set_title(f'{name}: Full Spectrum')
        axes[0, idx].set_ylabel('Magnitude (dB)')
        axes[0, idx].set_xlabel('Frequency (Hz)')

        # Time domain waveform (zoom to a few cycles)
        samples_per_cycle = int(sr / carrier)
        num_samples = samples_per_cycle * 4
        t = np.arange(num_samples) / sr * 1000
        axes[1, idx].plot(t, signal[:num_samples], color=color, linewidth=0.5)
        axes[1, idx].set_title(f'{name}: Waveform (4 cycles)')
        axes[1, idx].set_ylabel('Amplitude')
        axes[1, idx].set_xlabel('Time (ms)')
        axes[1, idx].set_ylim(-1.5, 1.5)

    fig.suptitle(f'Aliasing Comparison: Carrier={carrier}Hz, Mod={mod}Hz, Index={index}')
    plt.tight_layout()
    plt.savefig('output/fm_aliasing_visual.png', dpi=150)
    print("Saved: output/fm_aliasing_visual.png")


def test_sideband_accuracy():
    """Test accuracy of individual sideband amplitudes.

    FM theory predicts sideband amplitudes follow Bessel functions.
    Aliasing causes errors in these amplitudes.
    """
    print("\n=== Sideband Accuracy Test ===")
    from scipy.special import jv  # Bessel function

    sr = 48000
    carrier = 1000.0  # Low carrier for clear sidebands
    mod = 200.0       # Low mod ratio for well-separated sidebands
    index = 5.0       # Moderate index
    duration = 2.0    # Longer for better frequency resolution

    oscillators = [
        ('SIN (1x)', cedar.Opcode.OSC_SIN),
        ('SIN_4X', cedar.Opcode.OSC_SIN_4X),
    ]

    # Theoretical sideband amplitudes (Bessel functions)
    n_sidebands = 10
    theoretical = [np.abs(jv(n, index)) for n in range(-n_sidebands, n_sidebands+1)]
    sideband_freqs = [carrier + n * mod for n in range(-n_sidebands, n_sidebands+1)]

    print(f"  Theoretical sidebands at: {[f for f in sideband_freqs if 0 < f < sr/2][:5]}... Hz")

    fig, axes = plt.subplots(2, 1, figsize=(14, 6))

    for idx, (name, opcode) in enumerate(oscillators):
        signal = generate_fm_signal(carrier, mod, index, duration, sr, opcode)
        fft_freqs = np.fft.rfftfreq(len(signal), 1/sr)
        fft_mag = np.abs(np.fft.rfft(signal))

        # Find peaks at expected sideband frequencies
        measured = []
        for freq in sideband_freqs:
            if 0 < freq < sr/2:
                # Find nearest FFT bin
                bin_idx = np.argmin(np.abs(fft_freqs - freq))
                measured.append(fft_mag[bin_idx])
            else:
                measured.append(0)

        # Normalize to carrier amplitude
        carrier_idx = sideband_freqs.index(carrier) if carrier in sideband_freqs else n_sidebands
        if measured[carrier_idx] > 0:
            measured = [m / measured[carrier_idx] for m in measured]

        # Plot comparison
        x = list(range(-n_sidebands, n_sidebands+1))
        valid_mask = [(0 < f < sr/2) for f in sideband_freqs]

        ax = axes[idx]
        ax.bar([xi - 0.2 for xi, v in zip(x, valid_mask) if v],
               [t for t, v in zip(theoretical, valid_mask) if v],
               width=0.4, label='Theory (Bessel)', alpha=0.7, color='blue')
        ax.bar([xi + 0.2 for xi, v in zip(x, valid_mask) if v],
               [m for m, v in zip(measured, valid_mask) if v],
               width=0.4, label=f'{name}', alpha=0.7, color='orange')
        ax.set_xlabel('Sideband Number')
        ax.set_ylabel('Relative Amplitude')
        ax.set_title(f'{name}: Sideband Accuracy')
        ax.legend()
        ax.set_xlim(-n_sidebands-1, n_sidebands+1)

    fig.suptitle(f'FM Sideband Accuracy: Carrier={carrier}Hz, Mod={mod}Hz, Index={index}')
    plt.tight_layout()
    plt.savefig('output/fm_sideband_accuracy.png', dpi=150)
    print("Saved: output/fm_sideband_accuracy.png")


def export_fm_wav_files():
    """Export WAV files for listening comparison."""
    print("\n=== Exporting WAV Files for Listening Test ===")
    sr = 48000
    duration = 4.0  # 4 seconds per sample

    os.makedirs('output/wav', exist_ok=True)

    # Test configurations
    tests = [
        {
            'name': 'moderate',
            'carrier': 4000.0,
            'mod': 2000.0,
            'index': 7.0,
            'description': 'Moderate FM (4kHz carrier, 2kHz mod, index=7)'
        },
        {
            'name': 'extreme',
            'carrier': 8000.0,
            'mod': 8000.0,
            'index': 10.0,
            'description': 'Extreme FM (8kHz carrier, 8kHz mod, index=10)'
        },
        {
            'name': 'bell',
            'carrier': 500.0,
            'mod': 1000.0,
            'index': 15.0,
            'description': 'Classic Bell (500Hz carrier, 1kHz mod, index=15)'
        },
        {
            'name': 'bass',
            'carrier': 100.0,
            'mod': 200.0,
            'index': 8.0,
            'description': 'FM Bass (100Hz carrier, 200Hz mod, index=8)'
        },
        {
            'name': 'bright',
            'carrier': 2000.0,
            'mod': 3000.0,
            'index': 6.0,
            'description': 'Bright FM (2kHz carrier, 3kHz mod, index=6)'
        },
    ]

    oscillators = [
        ('1x', cedar.Opcode.OSC_SIN),
        ('2x', cedar.Opcode.OSC_SIN_2X),
        ('4x', cedar.Opcode.OSC_SIN_4X),
    ]

    for test in tests:
        print(f"\n  {test['description']}:")
        for osc_name, opcode in oscillators:
            signal = generate_fm_signal(
                test['carrier'], test['mod'], test['index'],
                duration, sr, opcode
            )
            filename = f"output/wav/fm_{test['name']}_{osc_name}.wav"
            export_wav(filename, signal, sr)

    # Also create comparison files with all three variants in sequence
    print("\n  Creating comparison files (1x -> 2x -> 4x sequences):")
    for test in tests:
        signals = []
        silence = np.zeros(int(0.5 * sr))  # 0.5s silence between variants

        for osc_name, opcode in oscillators:
            signal = generate_fm_signal(
                test['carrier'], test['mod'], test['index'],
                duration, sr, opcode
            )
            signals.append(signal)
            signals.append(silence)

        combined = np.concatenate(signals[:-1])  # Remove trailing silence
        filename = f"output/wav/fm_{test['name']}_comparison.wav"
        export_wav(filename, combined, sr)

    print("\n  WAV files exported to output/wav/")
    print("  Individual files: fm_<test>_<1x|2x|4x>.wav")
    print("  Comparison files: fm_<test>_comparison.wav (plays 1x, 2x, 4x in sequence)")


def summarize_results(test_results):
    """Print summary of all test results."""
    print("\n" + "="*60)
    print("SUMMARY: Noise Floor Measurements")
    print("(Note: With harmonic ratios, aliased content may land on")
    print(" expected sidebands, making noise floor misleading.)")
    print("="*60)

    test_names = ['Moderate FM', 'Extreme FM', 'Classic Bell']
    osc_names = ['SIN (1x)', 'SIN_2X', 'SIN_4X']

    print(f"\n{'Test':<20} | {'SIN (1x)':<12} | {'SIN_2X':<12} | {'SIN_4X':<12}")
    print("-" * 60)

    for test_name, noise_floors in zip(test_names, test_results):
        if noise_floors:
            row = f"{test_name:<20}"
            for nf in noise_floors:
                row += f" | {nf:>10.1f} dB"
            print(row)

    print("\n" + "="*60)
    print("See output images for visual comparison of aliasing artifacts.")
    print("="*60)


if __name__ == "__main__":
    os.makedirs('output', exist_ok=True)

    # Run all tests
    results = []
    results.append(test_moderate_fm())
    results.append(test_extreme_fm())
    results.append(test_classic_bell())

    test_comparison_overlay()
    test_spectrogram_sweep()
    test_zoomed_detail()

    # New tests with better aliasing demonstration
    test_irrational_ratio_fm()
    test_aliasing_visualization()
    test_sideband_accuracy()

    # Export WAV files for listening tests
    export_fm_wav_files()

    # Print summary
    summarize_results(results)

    print("\n=== All FM aliasing tests complete ===")
