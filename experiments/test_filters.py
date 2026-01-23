import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar
from cedar_testing import CedarTestHost
from visualize import plot_frequency_response, save_figure
import scipy.signal
import scipy.io.wavfile

def get_impulse(duration_sec, sample_rate):
    """Generate a unit impulse signal."""
    n = int(duration_sec * sample_rate)
    x = np.zeros(n, dtype=np.float32)
    x[0] = 1.0
    return x

def analyze_filter(filter_op, cutoff, res, filter_type_name):
    """
    Runs an impulse through the specified filter opcode and plots frequency response.
    """
    sr = 48000
    host = CedarTestHost(sr)

    # 1. Setup Parameters
    buf_in = 0  # Input is injected here
    buf_freq = host.set_param("cutoff", cutoff)
    buf_res = host.set_param("res", res)
    buf_out = 1

    # 2. Add Filter Instruction
    # Opcode signature: out, in, freq, res
    state_id = cedar.hash(f"{filter_type_name}_state") & 0xFFFF
    host.load_instruction(
        cedar.Instruction.make_ternary(filter_op, buf_out, buf_in, buf_freq, buf_res, state_id)
    )

    # 3. Route to Output
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    # 4. Run Analysis
    # Use Impulse Response -> FFT to get Bode plot
    impulse = get_impulse(0.1, sr) # 100ms is enough for IR
    response = host.process(impulse)

    # Calculate Frequency Response
    freqs, mag_db = get_bode_data(response, sr)

    return freqs, mag_db

def get_bode_data(impulse_response, sr):
    """Convert IR to Magnitude (dB) vs Frequency."""
    # FFT
    H = np.fft.rfft(impulse_response)
    freqs = np.fft.rfftfreq(len(impulse_response), 1/sr)

    # Magnitude in dB
    mag = 20 * np.log10(np.abs(H) + 1e-10)
    return freqs, mag

def test_svf_comparison():
    print("Test: SVF Filter Response Comparison")

    # Compare LP, HP, BP at same settings
    cutoff = 1000.0
    q = 2.0  # Mild resonance

    filters = [
        (cedar.Opcode.FILTER_SVF_LP, "Lowpass"),
        (cedar.Opcode.FILTER_SVF_HP, "Highpass"),
        (cedar.Opcode.FILTER_SVF_BP, "Bandpass")
    ]

    plt.figure(figsize=(12, 6))

    for op, name in filters:
        freqs, mag = analyze_filter(op, cutoff, q, name)
        plt.semilogx(freqs, mag, label=name)

    plt.title(f'SVF Topology Comparison (Fc={cutoff}Hz, Q={q})')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 10)
    plt.xlim(20, 20000)

    save_figure(plt.gcf(), "output/filter_svf_comparison.png")
    print("  Saved output/filter_svf_comparison.png")

def test_moog_resonance():
    print("Test: Moog Ladder Resonance Sweeps")

    cutoff = 2000.0
    resonance_values = [0.0, 1.0, 2.0, 3.0, 3.8] # 4.0 is self-oscillation

    plt.figure(figsize=(12, 6))

    for res in resonance_values:
        freqs, mag = analyze_filter(cedar.Opcode.FILTER_MOOG, cutoff, res, "Moog")
        plt.semilogx(freqs, mag, label=f'Resonance {res}')

    plt.title(f'Moog Ladder Resonance (Fc={cutoff}Hz)')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 20)
    plt.xlim(20, 20000)

    save_figure(plt.gcf(), "output/filter_moog_resonance.png")
    print("  Saved output/filter_moog_resonance.png")

def analyze_diode_filter(cutoff, res, vt=0.026, fb_gain=10.0, filter_name="Diode"):
    """
    Runs an impulse through FILTER_DIODE with tunable vt/fb_gain parameters.
    """
    sr = 48000
    host = CedarTestHost(sr)

    buf_in = 0
    buf_freq = host.set_param("cutoff", cutoff)
    buf_res = host.set_param("res", res)
    buf_vt = host.set_param("vt", vt)
    buf_fb_gain = host.set_param("fb_gain", fb_gain)
    buf_out = 1

    state_id = cedar.hash(f"{filter_name}_state") & 0xFFFF
    host.load_instruction(
        cedar.Instruction.make_quinary(
            cedar.Opcode.FILTER_DIODE, buf_out, buf_in,
            buf_freq, buf_res, buf_vt, buf_fb_gain, state_id
        )
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    impulse = get_impulse(0.1, sr)
    response = host.process(impulse)
    freqs, mag_db = get_bode_data(response, sr)
    return freqs, mag_db


def test_diode_frequency_response():
    """Test FILTER_DIODE frequency response and resonance sweep."""
    print("Test: Diode Ladder Filter Frequency Response")

    cutoff = 1000.0
    resonance_values = [0.0, 1.0, 2.0, 3.0, 3.5]  # 3.5+ = self-oscillation

    plt.figure(figsize=(12, 6))

    for res in resonance_values:
        freqs, mag = analyze_diode_filter(cutoff, res, vt=0.026, fb_gain=10.0)
        plt.semilogx(freqs, mag, label=f'Resonance {res}')

    plt.title(f'Diode Ladder Filter (Fc={cutoff}Hz)')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 30)
    plt.xlim(20, 20000)

    # Mark cutoff frequency
    plt.axvline(cutoff, color='gray', linestyle='--', alpha=0.5, label='Cutoff')

    save_figure(plt.gcf(), "output/filter_diode_response.png")
    print("  Saved output/filter_diode_response.png")


def test_diode_self_oscillation():
    """
    Test FILTER_DIODE self-oscillation at high resonance with different VT/FB_GAIN configs.

    Expected behavior (per implementation comments):
    - Self-oscillation should occur at resonance ~3.5+ with proper VT/FB_GAIN tuning
    - Oscillation frequency should match cutoff frequency within 5%

    Configurations tested:
    - Original (VT=0.026, FB_GAIN=1.0): No oscillation expected
    - A (VT=0.026, FB_GAIN=10.0): Self-oscillation expected
    - B (VT=0.05, FB_GAIN=5.0): Middle ground
    - C (VT=0.1, FB_GAIN=2.5): Softer character

    If this test fails, check the implementation in cedar/include/cedar/opcodes/filters.hpp
    """
    print("Test: Diode Ladder Self-Oscillation (VT/FB_GAIN comparison)")

    sr = 48000
    duration = 0.5  # 500ms
    cutoff = 1000.0
    resonance = 3.8  # Should self-oscillate per implementation comments

    # Test configurations: (name, vt, fb_gain, expect_oscillation)
    configs = [
        ("Original", 0.026, 1.0, False),   # No compensation - should NOT oscillate
        ("A_fb10", 0.026, 10.0, True),     # High feedback gain - should oscillate
        ("B_mid", 0.05, 5.0, True),        # Middle ground
        ("C_soft", 0.1, 2.5, True),        # Softer character
    ]

    fig, axes = plt.subplots(len(configs), 2, figsize=(14, 12))
    fig.suptitle(f'Diode Ladder Self-Oscillation: VT/FB_GAIN Comparison (Cutoff={cutoff}Hz, Res={resonance})')

    results = []

    for idx, (name, vt, fb_gain, expect_osc) in enumerate(configs):
        host = CedarTestHost(sr)

        buf_in = 0
        buf_freq = host.set_param("cutoff", cutoff)
        buf_res = host.set_param("res", resonance)
        buf_vt = host.set_param("vt", vt)
        buf_fb_gain = host.set_param("fb_gain", fb_gain)
        buf_out = 1

        state_id = cedar.hash(f"diode_osc_{name}") & 0xFFFF
        host.load_instruction(
            cedar.Instruction.make_quinary(
                cedar.Opcode.FILTER_DIODE, buf_out, buf_in,
                buf_freq, buf_res, buf_vt, buf_fb_gain, state_id
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Feed short noise burst to excite oscillation, then silence
        signal = np.zeros(int(duration * sr), dtype=np.float32)
        signal[:100] = np.random.uniform(-0.5, 0.5, 100).astype(np.float32)
        output = host.process(signal)

        # Save WAV for human evaluation
        wav_path = f"output/filter_diode_selfoscillation_{name}.wav"
        scipy.io.wavfile.write(wav_path, sr, output)

        # Analyze steady-state (after 100ms)
        steady = output[int(0.1 * sr):]

        # Time domain plot
        time_ms = np.arange(len(steady[:2000])) / sr * 1000
        axes[idx, 0].plot(time_ms, steady[:2000])
        axes[idx, 0].set_xlabel('Time (ms)')
        axes[idx, 0].set_ylabel('Amplitude')
        axes[idx, 0].set_title(f'{name}: VT={vt}, FB_GAIN={fb_gain}')
        axes[idx, 0].grid(True, alpha=0.3)

        # Check if oscillating
        max_amp = np.max(np.abs(steady))
        is_oscillating = max_amp > 0.01

        if is_oscillating:
            fft_size = 8192
            freqs = np.fft.rfftfreq(fft_size, 1/sr)
            spectrum = np.abs(np.fft.rfft(steady[:fft_size]))
            spectrum_db = 20 * np.log10(spectrum + 1e-10)

            peak_idx = np.argmax(spectrum)
            peak_freq = freqs[peak_idx]
            freq_error = abs(peak_freq - cutoff) / cutoff * 100

            axes[idx, 1].plot(freqs, spectrum_db)
            axes[idx, 1].axvline(cutoff, color='red', linestyle='--', alpha=0.7, label=f'Expected {cutoff}Hz')
            axes[idx, 1].axvline(peak_freq, color='green', linestyle=':', alpha=0.7, label=f'Actual {peak_freq:.0f}Hz')
            axes[idx, 1].set_xlabel('Frequency (Hz)')
            axes[idx, 1].set_ylabel('Magnitude (dB)')
            axes[idx, 1].set_title(f'Spectrum (error: {freq_error:.1f}%)')
            axes[idx, 1].set_xlim(0, cutoff * 3)
            axes[idx, 1].legend()
            axes[idx, 1].grid(True, alpha=0.3)

            # Check if result matches expectation
            if expect_osc:
                status = "✓ PASS" if freq_error < 5 else "⚠ FREQ ERROR"
            else:
                status = "⚠ UNEXPECTED OSCILLATION"
            print(f"  {name} (VT={vt}, FB={fb_gain}): oscillates at {peak_freq:.1f}Hz (error: {freq_error:.1f}%) {status}")
            results.append((name, True, freq_error < 5 if expect_osc else False))
        else:
            axes[idx, 1].text(0.5, 0.5, 'NO OSCILLATION',
                            ha='center', va='center', transform=axes[idx, 1].transAxes,
                            fontsize=12, color='orange' if not expect_osc else 'red')
            axes[idx, 1].set_xlim(0, cutoff * 3)
            axes[idx, 1].set_xlabel('Frequency (Hz)')
            axes[idx, 1].set_ylabel('Magnitude (dB)')
            axes[idx, 1].grid(True, alpha=0.3)

            if expect_osc:
                status = "✗ FAIL - No oscillation"
                results.append((name, False, False))
            else:
                status = "✓ PASS - No oscillation (as expected)"
                results.append((name, False, True))
            print(f"  {name} (VT={vt}, FB={fb_gain}): {status} (max amp: {max_amp:.6f})")

        print(f"    Saved {wav_path} - Listen for sine-like tone at {cutoff}Hz")

    plt.tight_layout()
    save_figure(fig, "output/filter_diode_selfoscillation.png")
    print("  Saved output/filter_diode_selfoscillation.png")

    # Summary
    passed = sum(1 for _, _, ok in results if ok)
    total = len(results)
    print(f"\n  Summary: {passed}/{total} configurations behaved as expected")
    if passed < total:
        print("  ⚠ WARNING: Some configurations didn't match expectations. Review results.")


def test_diode_vs_moog():
    """Compare FILTER_DIODE and FILTER_MOOG character."""
    print("Test: Diode vs Moog Character Comparison")

    cutoff = 1000.0
    resonance = 3.0  # High but not self-oscillating

    plt.figure(figsize=(12, 6))

    # Use default VT/FB_GAIN for diode filter
    freqs_diode, mag_diode = analyze_diode_filter(cutoff, resonance, vt=0.026, fb_gain=10.0)
    freqs_moog, mag_moog = analyze_filter(cedar.Opcode.FILTER_MOOG, cutoff, resonance, "Moog")

    plt.semilogx(freqs_diode, mag_diode, label='Diode Ladder (TB-303)', linewidth=2)
    plt.semilogx(freqs_moog, mag_moog, label='Moog Ladder', linewidth=2, linestyle='--')

    plt.title(f'Diode vs Moog Ladder Comparison (Fc={cutoff}Hz, Res={resonance})')
    plt.xlabel('Frequency (Hz)')
    plt.ylabel('Magnitude (dB)')
    plt.grid(True, which='both', alpha=0.3)
    plt.legend()
    plt.ylim(-60, 30)
    plt.xlim(20, 20000)
    plt.axvline(cutoff, color='gray', linestyle='--', alpha=0.5)

    save_figure(plt.gcf(), "output/filter_diode_vs_moog.png")
    print("  Saved output/filter_diode_vs_moog.png")


def test_formant_vowels():
    """
    Test FILTER_FORMANT vowel accuracy.

    Expected behavior:
    - Each vowel should have spectral peaks at the documented formant frequencies
    - F1, F2, F3 accuracy should be within ±10% of target

    WAV files are saved for human listening evaluation.
    """
    print("Test: Formant Filter Vowel Response")

    sr = 48000
    duration = 1.0  # Longer for better audio evaluation

    # Vowel table (from C++ implementation)
    vowel_table = {
        0: ("A", [650, 1100, 2860]),
        1: ("I", [300, 2300, 3000]),
        2: ("U", [300, 870, 2240]),
        3: ("E", [400, 2000, 2550]),
        4: ("O", [400, 800, 2600]),
    }

    fig, axes = plt.subplots(3, 2, figsize=(14, 12))
    fig.suptitle('Formant Filter - Vowel Frequency Response')

    for vowel_idx in range(5):
        host = CedarTestHost(sr)

        # White noise input for spectral analysis
        noise = np.random.uniform(-0.5, 0.5, int(duration * sr)).astype(np.float32)

        buf_in = 0
        buf_vowel_a = host.set_param("vowel_a", float(vowel_idx))
        buf_vowel_b = host.set_param("vowel_b", float(vowel_idx))  # Same vowel
        buf_morph = host.set_param("morph", 0.0)  # No morphing
        buf_q = host.set_param("q", 10.0)  # Moderate resonance
        buf_out = 1

        state_id = cedar.hash(f"formant_{vowel_idx}") & 0xFFFF
        host.load_instruction(
            cedar.Instruction.make_quinary(
                cedar.Opcode.FILTER_FORMANT, buf_out, buf_in,
                buf_vowel_a, buf_vowel_b, buf_morph, buf_q, state_id
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        output = host.process(noise)

        # Save WAV for human evaluation
        vowel_name = vowel_table[vowel_idx][0]
        wav_path = f"output/filter_formant_vowel_{vowel_name}.wav"
        scipy.io.wavfile.write(wav_path, sr, output)

        # Analyze spectrum
        fft_size = 8192
        freqs = np.fft.rfftfreq(fft_size, 1/sr)
        spectrum = np.abs(np.fft.rfft(output[:fft_size]))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        # Smooth spectrum for visualization
        from scipy.ndimage import gaussian_filter1d
        spectrum_smooth = gaussian_filter1d(spectrum_db, sigma=10)

        ax = axes[vowel_idx // 2, vowel_idx % 2]
        vowel_name, expected_formants = vowel_table[vowel_idx]

        ax.plot(freqs, spectrum_smooth, linewidth=1, label='Response')

        # Mark expected formant frequencies
        colors = ['red', 'green', 'blue']
        for f_idx, f_expected in enumerate(expected_formants):
            ax.axvline(f_expected, color=colors[f_idx], linestyle='--', alpha=0.7,
                      label=f'F{f_idx+1}={f_expected}Hz')

        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.set_title(f'Vowel "{vowel_name}" (Index {vowel_idx})')
        ax.set_xlim(0, 5000)
        ax.legend(loc='upper right', fontsize=8)
        ax.grid(True, alpha=0.3)

        # Find actual peaks and report accuracy
        peaks_found = []
        for f_expected in expected_formants:
            # Search in window around expected
            window = (freqs > f_expected * 0.7) & (freqs < f_expected * 1.3)
            if np.any(window):
                local_peak_idx = np.argmax(spectrum[window])
                actual_freq = freqs[window][local_peak_idx]
                error = abs(actual_freq - f_expected) / f_expected * 100
                peaks_found.append((f_expected, actual_freq, error))

        # Check if any formant is off by more than 10%
        all_ok = all(err <= 10 for _, _, err in peaks_found)
        status = "✓" if all_ok else "⚠"

        print(f"  {status} Vowel '{vowel_name}': ", end="")
        for exp, act, err in peaks_found:
            print(f"F={exp}Hz→{act:.0f}Hz ({err:.1f}%), ", end="")
        print(f"[{wav_path}]")

    # Leave last subplot empty or add summary
    axes[2, 1].axis('off')
    axes[2, 1].text(0.5, 0.5, 'Vowel formants based on\naverage male voice frequencies',
                   ha='center', va='center', fontsize=12)

    plt.tight_layout()
    save_figure(fig, "output/filter_formant_vowels.png")
    print("  Saved output/filter_formant_vowels.png")


def test_formant_morph():
    """Test FILTER_FORMANT morph smoothness."""
    print("Test: Formant Filter Morph Smoothness")

    sr = 48000
    duration = 4.0  # Long duration to see morph

    host = CedarTestHost(sr)

    # White noise input
    noise = np.random.uniform(-0.3, 0.3, int(duration * sr)).astype(np.float32)

    # Morph from A (0) to I (1) over time
    # We'll create a time-varying morph by processing in chunks
    buf_in = 0
    buf_vowel_a = host.set_param("vowel_a", 0.0)  # A
    buf_vowel_b = host.set_param("vowel_b", 1.0)  # I
    buf_morph = host.set_param("morph", 0.0)  # Will be updated
    buf_q = host.set_param("q", 8.0)
    buf_out = 1

    state_id = cedar.hash("formant_morph") & 0xFFFF
    host.load_instruction(
        cedar.Instruction.make_quinary(
            cedar.Opcode.FILTER_FORMANT, buf_out, buf_in,
            buf_vowel_a, buf_vowel_b, buf_morph, buf_q, state_id
        )
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    # Process in chunks, varying morph
    n_samples = len(noise)
    n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    output = []

    # Linear morph over entire duration
    for i in range(n_blocks):
        morph_val = i / n_blocks  # 0 to 1
        host.vm.set_param("morph", morph_val)

        start = i * cedar.BLOCK_SIZE
        end = min(start + cedar.BLOCK_SIZE, n_samples)
        block_in = noise[start:end]

        # Pad if needed
        if len(block_in) < cedar.BLOCK_SIZE:
            block_in = np.pad(block_in, (0, cedar.BLOCK_SIZE - len(block_in)))

        host.vm.set_buffer(0, block_in.astype(np.float32))
        l, r = host.vm.process()
        output.append(l[:end-start])

    output = np.concatenate(output)

    # Save WAV for human evaluation of morph smoothness
    wav_path = "output/filter_formant_morph_A_to_I.wav"
    scipy.io.wavfile.write(wav_path, sr, output.astype(np.float32))

    # Create spectrogram
    fig, ax = plt.subplots(figsize=(14, 6))
    ax.specgram(output, NFFT=2048, Fs=sr, noverlap=1024, cmap='magma')
    ax.set_ylabel('Frequency (Hz)')
    ax.set_xlabel('Time (s)')
    ax.set_title('Formant Morph: A → I (morph 0→1)')
    ax.set_ylim(0, 5000)

    # Mark expected formant transitions
    # A: F1=650, F2=1100, F3=2860
    # I: F1=300, F2=2300, F3=3000
    ax.axhline(650, color='white', linestyle='--', alpha=0.5, linewidth=0.5)
    ax.axhline(300, color='white', linestyle='--', alpha=0.5, linewidth=0.5)
    ax.axhline(1100, color='cyan', linestyle='--', alpha=0.5, linewidth=0.5)
    ax.axhline(2300, color='cyan', linestyle='--', alpha=0.5, linewidth=0.5)

    save_figure(fig, "output/filter_formant_morph.png")
    print(f"  Saved output/filter_formant_morph.png")
    print(f"  Saved {wav_path} - Listen for smooth formant transitions")


def test_sallenkey_modes():
    """Test FILTER_SALLENKEY lowpass and highpass modes."""
    print("Test: Sallen-Key Filter LP/HP Modes")

    sr = 48000
    cutoff = 1000.0
    resonance_values = [0.5, 1.5, 2.5, 3.5]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    for mode_idx, mode in enumerate([0.0, 1.0]):  # 0=LP, 1=HP
        mode_name = "Lowpass" if mode == 0.0 else "Highpass"

        for res in resonance_values:
            host = CedarTestHost(sr)

            buf_in = 0
            buf_freq = host.set_param("cutoff", cutoff)
            buf_res = host.set_param("res", res)
            buf_mode = host.set_param("mode", mode)
            buf_out = 1

            state_id = cedar.hash(f"sallenkey_{mode}_{res}") & 0xFFFF
            host.load_instruction(
                cedar.Instruction.make_quaternary(
                    cedar.Opcode.FILTER_SALLENKEY, buf_out, buf_in,
                    buf_freq, buf_res, buf_mode, state_id
                )
            )
            host.load_instruction(
                cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
            )

            # Impulse response
            impulse = get_impulse(0.1, sr)
            response = host.process(impulse)

            # FFT
            freqs, mag_db = get_bode_data(response, sr)

            axes[mode_idx, 0].semilogx(freqs, mag_db, label=f'Res={res}')

        axes[mode_idx, 0].set_title(f'Sallen-Key {mode_name} (Fc={cutoff}Hz)')
        axes[mode_idx, 0].set_xlabel('Frequency (Hz)')
        axes[mode_idx, 0].set_ylabel('Magnitude (dB)')
        axes[mode_idx, 0].grid(True, which='both', alpha=0.3)
        axes[mode_idx, 0].legend()
        axes[mode_idx, 0].set_ylim(-60, 30)
        axes[mode_idx, 0].set_xlim(20, 20000)
        axes[mode_idx, 0].axvline(cutoff, color='gray', linestyle='--', alpha=0.5)

    # Self-oscillation test
    print("  Testing self-oscillation...")
    for mode_idx, mode in enumerate([0.0, 1.0]):
        host = CedarTestHost(sr)
        cutoff_osc = 800.0
        res_osc = 3.8

        buf_in = 0
        buf_freq = host.set_param("cutoff", cutoff_osc)
        buf_res = host.set_param("res", res_osc)
        buf_mode = host.set_param("mode", mode)
        buf_out = 1

        state_id = cedar.hash(f"sallenkey_osc_{mode}") & 0xFFFF
        host.load_instruction(
            cedar.Instruction.make_quaternary(
                cedar.Opcode.FILTER_SALLENKEY, buf_out, buf_in,
                buf_freq, buf_res, buf_mode, state_id
            )
        )
        host.load_instruction(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
        )

        # Silence with kick to test self-oscillation
        silence = np.zeros(int(0.5 * sr), dtype=np.float32)
        silence[:100] = np.random.uniform(-0.3, 0.3, 100).astype(np.float32)
        output = host.process(silence)

        # Save WAV for human evaluation
        mode_name = "LP" if mode == 0.0 else "HP"
        wav_path = f"output/filter_sallenkey_selfoscillation_{mode_name}.wav"
        scipy.io.wavfile.write(wav_path, sr, output)
        print(f"    Saved {wav_path}")

        # Time domain
        time_ms = np.arange(len(output[:4000])) / sr * 1000
        axes[mode_idx, 1].plot(time_ms, output[:4000])
        axes[mode_idx, 1].set_title(f'Sallen-Key {mode_name} Self-Oscillation (Res={res_osc})')
        axes[mode_idx, 1].set_xlabel('Time (ms)')
        axes[mode_idx, 1].set_ylabel('Amplitude')
        axes[mode_idx, 1].grid(True, alpha=0.3)

        # Check if oscillating
        steady = output[int(0.1 * sr):]
        max_amp = np.max(np.abs(steady))
        if max_amp > 0.01:
            print(f"    {mode_name}: ✓ Oscillating (max amp: {max_amp:.3f})")
        else:
            print(f"    {mode_name}: ⚠ No oscillation detected (max amp: {max_amp:.6f})")

    plt.tight_layout()
    save_figure(fig, "output/filter_sallenkey_response.png")
    print("  Saved output/filter_sallenkey_response.png")


def test_sallenkey_character():
    """Test FILTER_SALLENKEY diode clipping character."""
    print("Test: Sallen-Key Diode Character")

    sr = 48000

    # High-amplitude sine through filter with high resonance
    # Should show asymmetric clipping from diode feedback
    duration = 0.1
    freq = 200.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.8

    host = CedarTestHost(sr)

    buf_in = 0
    buf_freq = host.set_param("cutoff", 500.0)  # Above input freq
    buf_res = host.set_param("res", 3.5)  # High resonance
    buf_mode = host.set_param("mode", 0.0)  # LP
    buf_out = 1

    state_id = cedar.hash("sallenkey_char") & 0xFFFF
    host.load_instruction(
        cedar.Instruction.make_quaternary(
            cedar.Opcode.FILTER_SALLENKEY, buf_out, buf_in,
            buf_freq, buf_res, buf_mode, state_id
        )
    )
    host.load_instruction(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out)
    )

    output = host.process(sine_input)

    # Save WAV for human evaluation of diode character
    wav_path = "output/filter_sallenkey_character.wav"
    scipy.io.wavfile.write(wav_path, sr, output)
    print(f"  Saved {wav_path} - Listen for asymmetric distortion character")

    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    fig.suptitle('Sallen-Key Diode Character Analysis')

    # Waveform
    axes[0, 0].plot(t[:1000] * 1000, sine_input[:1000], label='Input', alpha=0.7)
    axes[0, 0].plot(t[:1000] * 1000, output[:1000], label='Output')
    axes[0, 0].set_xlabel('Time (ms)')
    axes[0, 0].set_ylabel('Amplitude')
    axes[0, 0].set_title('Waveform (First 20ms)')
    axes[0, 0].legend()
    axes[0, 0].grid(True, alpha=0.3)

    # Transfer curve (input vs output)
    # Use steady-state
    steady_in = sine_input[2000:]
    steady_out = output[2000:]
    axes[0, 1].plot(steady_in, steady_out, 'b.', markersize=0.5, alpha=0.5)
    axes[0, 1].plot([-1, 1], [-1, 1], 'k--', alpha=0.3, label='Linear')
    axes[0, 1].set_xlabel('Input')
    axes[0, 1].set_ylabel('Output')
    axes[0, 1].set_title('Transfer Curve (shows diode asymmetry)')
    axes[0, 1].set_aspect('equal')
    axes[0, 1].grid(True, alpha=0.3)

    # Spectrum comparison
    fft_size = 4096
    freqs = np.fft.rfftfreq(fft_size, 1/sr)

    spec_in = 20 * np.log10(np.abs(np.fft.rfft(sine_input[:fft_size])) + 1e-10)
    spec_out = 20 * np.log10(np.abs(np.fft.rfft(output[:fft_size])) + 1e-10)

    axes[1, 0].plot(freqs[:500], spec_in[:500], label='Input', alpha=0.7)
    axes[1, 0].plot(freqs[:500], spec_out[:500], label='Output')
    axes[1, 0].set_xlabel('Frequency (Hz)')
    axes[1, 0].set_ylabel('Magnitude (dB)')
    axes[1, 0].set_title('Spectrum (Harmonic Content)')
    axes[1, 0].legend()
    axes[1, 0].grid(True, alpha=0.3)

    # Harmonic analysis
    fundamental_idx = int(freq / (sr / fft_size))
    harmonics = []
    for h in range(1, 8):
        h_idx = fundamental_idx * h
        if h_idx < len(spec_out):
            harmonics.append(spec_out[h_idx] - spec_out[fundamental_idx])

    axes[1, 1].bar(range(1, len(harmonics)+1), harmonics)
    axes[1, 1].set_xlabel('Harmonic Number')
    axes[1, 1].set_ylabel('Level (dB rel. fundamental)')
    axes[1, 1].set_title('Harmonic Distribution')
    axes[1, 1].grid(True, alpha=0.3, axis='y')

    plt.tight_layout()
    save_figure(fig, "output/filter_sallenkey_character.png")
    print("  Saved output/filter_sallenkey_character.png")


if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)

    # Original tests
    test_svf_comparison()
    test_moog_resonance()

    # New SquelchEngine filter tests
    print("\n=== FILTER_DIODE Tests ===")
    test_diode_frequency_response()
    test_diode_self_oscillation()
    test_diode_vs_moog()

    print("\n=== FILTER_FORMANT Tests ===")
    test_formant_vowels()
    test_formant_morph()

    print("\n=== FILTER_SALLENKEY Tests ===")
    test_sallenkey_modes()
    test_sallenkey_character()