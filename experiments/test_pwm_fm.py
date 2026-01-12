"""PWM + FM synthesis experiments.

Tests comparing 1x vs 4x oversampled PWM oscillators with FM modulation.
Exports WAV files for listening comparison.
"""

import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar
from scipy.io import wavfile
import os


def export_wav(filename, signal, sr=48000):
    """Export signal as 16-bit WAV file."""
    peak = np.max(np.abs(signal))
    if peak > 0:
        signal = signal / peak * 0.9
    signal_int = (signal * 32767).astype(np.int16)
    wavfile.write(filename, sr, signal_int)
    print(f"  Exported: {filename}")


def generate_pwm_fm_sqr(carrier_freq, mod_freq, mod_index, pwm_val, duration, sr, use_4x=False):
    """Generate FM signal using PWM square oscillator."""
    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('carrier', carrier_freq)
    vm.set_param('mod_freq', mod_freq)
    vm.set_param('mod_index', mod_index * mod_freq)
    vm.set_param('pwm', pwm_val)

    osc_opcode = cedar.Opcode.OSC_SQR_PWM_4X if use_4x else cedar.Opcode.OSC_SQR_PWM

    program = [
        # Modulator oscillator (sine for clean FM)
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('mod_freq')),
        cedar.Instruction.make_unary(cedar.Opcode.OSC_SIN, 11, 10, cedar.hash('mod_osc')),

        # Scale by mod index
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 12, cedar.hash('mod_index')),
        cedar.Instruction.make_binary(cedar.Opcode.MUL, 13, 11, 12),

        # Add to carrier frequency
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 14, cedar.hash('carrier')),
        cedar.Instruction.make_binary(cedar.Opcode.ADD, 15, 14, 13),

        # PWM value
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 16, cedar.hash('pwm')),

        # PWM square carrier with FM'd frequency
        cedar.Instruction.make_binary(osc_opcode, 1, 15, 16, cedar.hash('carrier_osc')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]

    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    for _ in range(num_blocks):
        left, right = vm.process()
        signal.append(left)

    return np.concatenate(signal)


def generate_pwm_fm_saw(carrier_freq, mod_freq, mod_index, pwm_val, duration, sr, use_4x=False):
    """Generate FM signal using PWM saw oscillator (variable slope)."""
    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('carrier', carrier_freq)
    vm.set_param('mod_freq', mod_freq)
    vm.set_param('mod_index', mod_index * mod_freq)
    vm.set_param('pwm', pwm_val)

    osc_opcode = cedar.Opcode.OSC_SAW_PWM_4X if use_4x else cedar.Opcode.OSC_SAW_PWM

    program = [
        # Modulator oscillator (sine for clean FM)
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('mod_freq')),
        cedar.Instruction.make_unary(cedar.Opcode.OSC_SIN, 11, 10, cedar.hash('mod_osc')),

        # Scale by mod index
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 12, cedar.hash('mod_index')),
        cedar.Instruction.make_binary(cedar.Opcode.MUL, 13, 11, 12),

        # Add to carrier frequency
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 14, cedar.hash('carrier')),
        cedar.Instruction.make_binary(cedar.Opcode.ADD, 15, 14, 13),

        # PWM value (controls slope)
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 16, cedar.hash('pwm')),

        # PWM saw carrier with FM'd frequency
        cedar.Instruction.make_binary(osc_opcode, 1, 15, 16, cedar.hash('carrier_osc')),
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
        ax.axhline(y=-60, color='r', linestyle='--', alpha=0.3)

    return fft_freqs, fft_db


def test_pwm_sqr_fm_comparison():
    """Compare 1x vs 4x PWM square with FM modulation."""
    print("\n=== PWM Square + FM: 1x vs 4x Comparison ===")

    sr = 48000
    duration = 2.0

    # Aggressive FM parameters to expose aliasing
    carrier = 440
    mod_freq = 110
    mod_index = 8  # High modulation index
    pwm = 0.3  # 30% duty cycle (asymmetric)

    print(f"  Carrier: {carrier} Hz, Mod: {mod_freq} Hz, Index: {mod_index}, PWM: {pwm}")

    sig_1x = generate_pwm_fm_sqr(carrier, mod_freq, mod_index, pwm, duration, sr, use_4x=False)
    sig_4x = generate_pwm_fm_sqr(carrier, mod_freq, mod_index, pwm, duration, sr, use_4x=True)

    os.makedirs("output", exist_ok=True)
    export_wav("output/pwm_sqr_fm_1x.wav", sig_1x, sr)
    export_wav("output/pwm_sqr_fm_4x.wav", sig_4x, sr)

    # Spectrum comparison
    fig, axes = plt.subplots(2, 1, figsize=(12, 8))
    analyze_spectrum(sig_1x, sr, "PWM Square FM - 1x (basic)", axes[0])
    analyze_spectrum(sig_4x, sr, "PWM Square FM - 4x (oversampled)", axes[1])
    axes[1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig("output/pwm_sqr_fm_spectrum.png", dpi=150)
    print("  Saved spectrum: output/pwm_sqr_fm_spectrum.png")
    plt.close()


def test_pwm_saw_fm_comparison():
    """Compare 1x vs 4x PWM saw with FM modulation."""
    print("\n=== PWM Saw + FM: 1x vs 4x Comparison ===")

    sr = 48000
    duration = 2.0

    carrier = 220
    mod_freq = 55
    mod_index = 10
    pwm = 0.7  # More saw-like (asymmetric triangle)

    print(f"  Carrier: {carrier} Hz, Mod: {mod_freq} Hz, Index: {mod_index}, PWM: {pwm}")

    sig_1x = generate_pwm_fm_saw(carrier, mod_freq, mod_index, pwm, duration, sr, use_4x=False)
    sig_4x = generate_pwm_fm_saw(carrier, mod_freq, mod_index, pwm, duration, sr, use_4x=True)

    os.makedirs("output", exist_ok=True)
    export_wav("output/pwm_saw_fm_1x.wav", sig_1x, sr)
    export_wav("output/pwm_saw_fm_4x.wav", sig_4x, sr)

    fig, axes = plt.subplots(2, 1, figsize=(12, 8))
    analyze_spectrum(sig_1x, sr, "PWM Saw FM - 1x (basic)", axes[0])
    analyze_spectrum(sig_4x, sr, "PWM Saw FM - 4x (oversampled)", axes[1])
    axes[1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig("output/pwm_saw_fm_spectrum.png", dpi=150)
    print("  Saved spectrum: output/pwm_saw_fm_spectrum.png")
    plt.close()


def test_extreme_pwm_fm():
    """Extreme FM with PWM - bass growl style."""
    print("\n=== Extreme PWM FM (Bass Growl) ===")

    sr = 48000
    duration = 3.0

    # Dubstep-style bass parameters
    carrier = 55  # Low bass
    mod_freq = 27.5  # Sub-bass modulator
    mod_index = 20  # Extreme modulation
    pwm = 0.2  # Narrow pulse

    print(f"  Carrier: {carrier} Hz, Mod: {mod_freq} Hz, Index: {mod_index}, PWM: {pwm}")

    sig_1x = generate_pwm_fm_sqr(carrier, mod_freq, mod_index, pwm, duration, sr, use_4x=False)
    sig_4x = generate_pwm_fm_sqr(carrier, mod_freq, mod_index, pwm, duration, sr, use_4x=True)

    os.makedirs("output", exist_ok=True)
    export_wav("output/pwm_extreme_1x.wav", sig_1x, sr)
    export_wav("output/pwm_extreme_4x.wav", sig_4x, sr)

    fig, axes = plt.subplots(2, 1, figsize=(12, 8))
    analyze_spectrum(sig_1x, sr, "Extreme PWM FM - 1x", axes[0])
    analyze_spectrum(sig_4x, sr, "Extreme PWM FM - 4x", axes[1])
    axes[1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig("output/pwm_extreme_spectrum.png", dpi=150)
    print("  Saved spectrum: output/pwm_extreme_spectrum.png")
    plt.close()


def test_pwm_sweep_fm():
    """PWM value sweeping during FM synthesis."""
    print("\n=== PWM Sweep + FM ===")

    sr = 48000
    duration = 4.0
    num_samples = int(duration * sr)

    carrier = 330
    mod_freq = 82.5
    mod_index = 6

    # We'll generate in smaller chunks to update PWM
    vm_1x = cedar.VM()
    vm_1x.set_sample_rate(sr)

    vm_4x = cedar.VM()
    vm_4x.set_sample_rate(sr)

    # Setup both VMs
    for vm, use_4x in [(vm_1x, False), (vm_4x, True)]:
        vm.set_param('carrier', carrier)
        vm.set_param('mod_freq', mod_freq)
        vm.set_param('mod_index', mod_index * mod_freq)
        vm.set_param('pwm', 0.0)

        osc_opcode = cedar.Opcode.OSC_SQR_PWM_4X if use_4x else cedar.Opcode.OSC_SQR_PWM

        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('mod_freq')),
            cedar.Instruction.make_unary(cedar.Opcode.OSC_SIN, 11, 10, cedar.hash('mod_osc')),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 12, cedar.hash('mod_index')),
            cedar.Instruction.make_binary(cedar.Opcode.MUL, 13, 11, 12),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 14, cedar.hash('carrier')),
            cedar.Instruction.make_binary(cedar.Opcode.ADD, 15, 14, 13),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 16, cedar.hash('pwm')),
            cedar.Instruction.make_binary(osc_opcode, 1, 15, 16, cedar.hash('carrier_osc')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]
        vm.load_program(program)

    # Generate with PWM sweep
    sig_1x = []
    sig_4x = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)

    for i in range(num_blocks):
        # Sweep PWM from -0.8 to 0.8 over duration
        t = i / num_blocks
        pwm = -0.8 + 1.6 * t

        vm_1x.set_param('pwm', pwm)
        vm_4x.set_param('pwm', pwm)

        left_1x, _ = vm_1x.process()
        left_4x, _ = vm_4x.process()

        sig_1x.append(left_1x)
        sig_4x.append(left_4x)

    sig_1x = np.concatenate(sig_1x)
    sig_4x = np.concatenate(sig_4x)

    os.makedirs("output", exist_ok=True)
    export_wav("output/pwm_sweep_fm_1x.wav", sig_1x, sr)
    export_wav("output/pwm_sweep_fm_4x.wav", sig_4x, sr)
    print("  PWM sweeps from -0.8 to 0.8 during FM synthesis")


def test_bright_pwm_fm():
    """High frequency PWM + FM for maximum aliasing exposure."""
    print("\n=== Bright PWM FM (High Frequency) ===")

    sr = 48000
    duration = 2.0

    carrier = 2000  # High carrier
    mod_freq = 500  # High modulator
    mod_index = 4
    pwm = 0.4

    print(f"  Carrier: {carrier} Hz, Mod: {mod_freq} Hz, Index: {mod_index}, PWM: {pwm}")

    sig_1x = generate_pwm_fm_sqr(carrier, mod_freq, mod_index, pwm, duration, sr, use_4x=False)
    sig_4x = generate_pwm_fm_sqr(carrier, mod_freq, mod_index, pwm, duration, sr, use_4x=True)

    os.makedirs("output", exist_ok=True)
    export_wav("output/pwm_bright_fm_1x.wav", sig_1x, sr)
    export_wav("output/pwm_bright_fm_4x.wav", sig_4x, sr)

    fig, axes = plt.subplots(2, 1, figsize=(12, 8))
    analyze_spectrum(sig_1x, sr, "Bright PWM FM - 1x", axes[0])
    analyze_spectrum(sig_4x, sr, "Bright PWM FM - 4x", axes[1])
    axes[1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig("output/pwm_bright_fm_spectrum.png", dpi=150)
    print("  Saved spectrum: output/pwm_bright_fm_spectrum.png")
    plt.close()


def generate_pwm_mod_signal(carrier_freq, pwm_rate, duration, sr, use_4x=False):
    """Generate signal with audio-rate PWM modulation (no FM)."""
    vm = cedar.VM()
    vm.set_sample_rate(sr)
    vm.set_param('carrier', carrier_freq)
    vm.set_param('pwm_rate', pwm_rate)

    osc_opcode = cedar.Opcode.OSC_SQR_PWM_4X if use_4x else cedar.Opcode.OSC_SQR_PWM

    program = [
        # Carrier frequency (constant - no FM)
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('carrier')),

        # PWM modulator (audio rate oscillator -> PWM input)
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash('pwm_rate')),
        cedar.Instruction.make_unary(cedar.Opcode.OSC_SIN, 12, 11, cedar.hash('pwm_mod')),

        # PWM square with modulated PWM (sin output is -1 to 1, maps to duty 0-100%)
        cedar.Instruction.make_binary(osc_opcode, 1, 10, 12, cedar.hash('carrier_osc')),
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
    ]

    vm.load_program(program)

    signal = []
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    for _ in range(num_blocks):
        left, right = vm.process()
        signal.append(left)

    return np.concatenate(signal)


def test_pwm_mod_only():
    """Test audio-rate PWM modulation (no FM) across multiple frequency ranges."""
    print("\n=== Audio-Rate PWM Modulation (No FM) - Frequency Sweep ===")

    sr = 48000
    duration = 2.0

    # Test cases: (carrier_freq, pwm_mod_rate, name)
    # Higher frequencies and faster PWM rates are more likely to alias
    test_cases = [
        (220, 110, "low"),           # Low: 220Hz carrier, 110Hz PWM mod
        (440, 220, "mid"),           # Mid: 440Hz carrier, 220Hz PWM mod
        (880, 440, "high"),          # High: 880Hz carrier, 440Hz PWM mod
        (1760, 880, "very_high"),    # Very high: 1760Hz carrier, 880Hz PWM mod
        (3520, 1760, "extreme"),     # Extreme: 3520Hz carrier, 1760Hz PWM mod
        (440, 1000, "fast_pwm"),     # Fast PWM: 440Hz carrier, 1000Hz PWM mod
        (200, 2000, "very_fast_pwm"), # Very fast PWM mod on low carrier
    ]

    os.makedirs("output", exist_ok=True)

    # Create figure for all spectra
    fig, axes = plt.subplots(len(test_cases), 2, figsize=(14, 3 * len(test_cases)))

    for idx, (carrier, pwm_rate, name) in enumerate(test_cases):
        print(f"  {name}: Carrier {carrier} Hz, PWM mod {pwm_rate} Hz")

        sig_1x = generate_pwm_mod_signal(carrier, pwm_rate, duration, sr, use_4x=False)
        sig_4x = generate_pwm_mod_signal(carrier, pwm_rate, duration, sr, use_4x=True)

        export_wav(f"output/pwm_mod_{name}_1x.wav", sig_1x, sr)
        export_wav(f"output/pwm_mod_{name}_4x.wav", sig_4x, sr)

        # Spectrum analysis
        analyze_spectrum(sig_1x, sr, f"{name} 1x (c={carrier}, pwm={pwm_rate})", axes[idx, 0])
        analyze_spectrum(sig_4x, sr, f"{name} 4x (c={carrier}, pwm={pwm_rate})", axes[idx, 1])

    axes[-1, 0].set_xlabel('Frequency (Hz)')
    axes[-1, 1].set_xlabel('Frequency (Hz)')
    plt.tight_layout()
    plt.savefig("output/pwm_mod_spectrum_comparison.png", dpi=150)
    print("  Saved spectrum comparison: output/pwm_mod_spectrum_comparison.png")
    plt.close()

    print("\n  Key question: Does audio-rate PWM modulation benefit from 4x oversampling?")
    print("  Listen to pairs and check spectrum for aliasing artifacts.")


if __name__ == "__main__":
    print("=" * 60)
    print("PWM + FM Synthesis Experiments")
    print("=" * 60)

    test_pwm_sqr_fm_comparison()
    test_pwm_saw_fm_comparison()
    test_extreme_pwm_fm()
    test_pwm_sweep_fm()
    test_bright_pwm_fm()
    test_pwm_mod_only()

    print("\n" + "=" * 60)
    print("All experiments complete! WAV files in ./output/")
    print("=" * 60)
    print("\nListen and compare:")
    print("  - pwm_sqr_fm_1x.wav vs pwm_sqr_fm_4x.wav")
    print("  - pwm_saw_fm_1x.wav vs pwm_saw_fm_4x.wav")
    print("  - pwm_extreme_1x.wav vs pwm_extreme_4x.wav")
    print("  - pwm_sweep_fm_1x.wav vs pwm_sweep_fm_4x.wav")
    print("  - pwm_bright_fm_1x.wav vs pwm_bright_fm_4x.wav")
    print("  - pwm_mod_only_1x.wav vs pwm_mod_only_4x.wav")
