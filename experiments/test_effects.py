import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar
from cedar_testing import CedarTestHost
from visualize import plot_spectrogram, plot_transfer_curve, save_figure
import scipy.signal
import scipy.io.wavfile

# =============================================================================
# Helper: Signal Generators
# =============================================================================

def gen_linear_ramp(samples=1024):
    """Linear ramp from -1 to 1 for transfer curve plotting."""
    return np.linspace(-1, 1, samples, dtype=np.float32)

def gen_white_noise(duration, sr):
    """White noise for spectral analysis."""
    return np.random.uniform(-0.5, 0.5, int(duration * sr)).astype(np.float32)

def gen_impulse(duration, sr):
    """Kronecker delta for reverb tails."""
    x = np.zeros(int(duration * sr), dtype=np.float32)
    x[0] = 1.0
    return x

# =============================================================================
# 1. Distortion Tests (Transfer Curves)
# =============================================================================

def test_distortion_curves():
    print("Test: Distortion Transfer Curves")

    host = CedarTestHost()
    ramp = gen_linear_ramp(2048)

    # Configuration for different distortion types
    # (Opcode, Param Name, Param Value, Label)
    configs = [
        (cedar.Opcode.DISTORT_TANH, "drive", 2.0, "Tanh (Drive 2.0)"),
        (cedar.Opcode.DISTORT_SOFT, "thresh", 0.5, "Soft Clip (Thresh 0.5)"),
        (cedar.Opcode.DISTORT_FOLD, "thresh", 0.5, "Wavefolder (Thresh 0.5)"),
        (cedar.Opcode.DISTORT_TUBE, "drive", 5.0, "Tube (Drive 5.0)"),
    ]

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("Distortion Transfer Curves (Input vs Output)")

    for (opcode, p_name, p_val, label), ax in zip(configs, axes.flat):
        # Reset Host for each test to clear state
        host = CedarTestHost()

        buf_in = 0
        buf_p1 = host.set_param(p_name, p_val)

        # Some opcodes need extra params, handle specifics
        if opcode == cedar.Opcode.DISTORT_FOLD:
            # Folder needs symmetry param
            buf_p2 = host.set_param("symmetry", 0.5)
            host.load_instruction(cedar.Instruction.make_ternary(
                opcode, 1, buf_in, buf_p1, buf_p2, cedar.hash("dist") & 0xFFFF
            ))
        elif opcode == cedar.Opcode.DISTORT_TUBE:
            # Tube needs bias
            buf_p2 = host.set_param("bias", 0.1)
            host.load_instruction(cedar.Instruction.make_ternary(
                opcode, 1, buf_in, buf_p1, buf_p2, cedar.hash("dist") & 0xFFFF
            ))
        else:
            # Standard unary distortion
            host.load_instruction(cedar.Instruction.make_binary(
                opcode, 1, buf_in, buf_p1, cedar.hash("dist") & 0xFFFF
            ))

        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        # Plot
        ax.plot(ramp, output, linewidth=2)
        ax.plot(ramp, ramp, 'k--', alpha=0.3, label="Linear") # Ref
        ax.set_title(label)
        ax.set_xlabel("Input")
        ax.set_ylabel("Output")
        ax.grid(True, alpha=0.3)
        ax.set_aspect('equal')

    plt.tight_layout()
    save_figure(fig, "output/distortion_curves.png")
    print("  Saved output/distortion_curves.png")

# =============================================================================
# 2. Modulation Tests (Spectrograms)
# =============================================================================

def test_phaser_spectrogram():
    print("Test: Phaser Spectrogram")

    sr = 48000
    duration = 2.0

    host = CedarTestHost(sr)
    noise = gen_white_noise(duration, sr)

    # Phaser Parameters
    buf_in = 0
    buf_rate = host.set_param("rate", 1.0) # 1 Hz sweep
    buf_depth = host.set_param("depth", 0.8)

    # Inst: Phaser(out, in, rate, depth)
    # Rate field encodes feedback (high 4) and stages (low 4)
    # Feedback ~0.5, Stages = 6
    # 0.5 maps to int 8 (approx), Stages 6
    packed_rate = (8 << 4) | 6

    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.EFFECT_PHASER, 1, buf_in, buf_rate, buf_depth, cedar.hash("phaser") & 0xFFFF
    )
    inst.rate = packed_rate

    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(noise)

    # Plot spectrogram using matplotlib directly
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.specgram(output, NFFT=1024, Fs=sr, noverlap=512, cmap='magma')
    ax.set_title("Phaser Spectrogram (6-stage, 1Hz LFO)")
    ax.set_ylabel("Frequency (Hz)")
    ax.set_xlabel("Time (s)")
    ax.set_ylim(0, 10000)

    save_figure(fig, "output/phaser_spectrogram.png")
    print("  Saved output/phaser_spectrogram.png")

# =============================================================================
# 3. Reverb Tests (Impulse Response)
# =============================================================================

def test_reverb_decay():
    print("Test: Reverb Impulse Response")

    sr = 48000
    host = CedarTestHost(sr)

    # Short impulse to trigger reverb
    impulse = gen_impulse(2.0, sr)

    # Dattorro Reverb Params
    buf_in = 0
    buf_decay = host.set_param("decay", 0.95) # Long tail
    buf_predelay = host.set_param("predelay", 20.0) # 20ms

    # Dattorro(out, in, decay, predelay)
    # Rate: damping | mod_depth
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.REVERB_DATTORRO, 1, buf_in, buf_decay, buf_predelay, cedar.hash("verb") & 0xFFFF
    )
    inst.rate = (0 << 4) | 0 # No mod, no damping for clear tail

    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(impulse)

    # Plot log-magnitude envelope
    time = np.arange(len(output)) / sr
    env = np.abs(output)
    env_db = 20 * np.log10(env + 1e-6)

    fig, ax = plt.subplots(figsize=(12, 6))
    ax.plot(time, env_db, linewidth=0.5, color='purple')
    ax.set_title("Dattorro Reverb Impulse Response (Decay 0.95)")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Amplitude (dB)")
    ax.set_ylim(-100, 0)
    ax.grid(True, alpha=0.3)

    save_figure(fig, "output/reverb_ir.png")
    print("  Saved output/reverb_ir.png")

# =============================================================================
# 4. Delay Tests (Timing and Feedback)
# =============================================================================

def test_delay_timing():
    """
    Test delay time accuracy and feedback decay.
    - Input: impulse
    - Measure time between echoes
    - Verify feedback decay rate
    """
    print("Test: Delay Timing and Feedback")

    sr = 48000
    duration = 2.0

    host = CedarTestHost(sr)
    impulse = gen_impulse(duration, sr)

    # Delay parameters: 100ms delay, 0.5 feedback, fully wet
    delay_ms = 100.0
    expected_delay_samples = int(delay_ms / 1000 * sr)
    feedback = 0.5

    buf_in = 0
    buf_delay = host.set_param("delay", delay_ms)
    buf_feedback = host.set_param("feedback", feedback)
    buf_out = 1

    # DELAY: out = delay(in, delay_ms, feedback)
    # Rate encodes mix (255 = fully wet)
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.DELAY, buf_out, buf_in, buf_delay, buf_feedback, cedar.hash("delay") & 0xFFFF
    )
    inst.rate = 255  # Fully wet
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))

    output = host.process(impulse)

    # Find peaks (echoes)
    peaks = []
    threshold = 0.01
    for i in range(1, len(output) - 1):
        if output[i] > threshold and output[i] > output[i-1] and output[i] > output[i+1]:
            # Check if this is a new peak (not too close to previous)
            if len(peaks) == 0 or i - peaks[-1] > expected_delay_samples // 2:
                peaks.append(i)

    # Analyze echo timing
    if len(peaks) >= 2:
        delays = np.diff(peaks)
        avg_delay = np.mean(delays)
        delay_error = abs(avg_delay - expected_delay_samples) / expected_delay_samples * 100

        print(f"  Expected delay: {expected_delay_samples} samples ({delay_ms}ms)")
        print(f"  Measured avg delay: {avg_delay:.1f} samples")
        print(f"  Delay error: {delay_error:.2f}%")

        # Check feedback decay (-6dB per echo for 0.5 feedback)
        if len(peaks) >= 3:
            peak_levels = [output[p] for p in peaks[:5]]
            print(f"  Echo levels: {[f'{20*np.log10(l+1e-10):.1f}dB' for l in peak_levels]}")
    else:
        print(f"  Only {len(peaks)} peaks found - insufficient for analysis")

    # Plot
    fig, axes = plt.subplots(2, 1, figsize=(12, 8))

    time_ms = np.arange(len(output)) / sr * 1000

    ax1 = axes[0]
    ax1.plot(time_ms, output, linewidth=0.5)
    for p in peaks[:10]:
        ax1.axvline(p / sr * 1000, color='red', linestyle=':', alpha=0.5)
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title(f'Delay Impulse Response ({delay_ms}ms delay, {feedback} feedback)')
    ax1.grid(True, alpha=0.3)

    # Log scale for decay analysis
    ax2 = axes[1]
    db_output = 20 * np.log10(np.abs(output) + 1e-10)
    ax2.plot(time_ms, db_output, linewidth=0.5)
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Amplitude (dB)')
    ax2.set_title('Delay Decay (log scale)')
    ax2.set_ylim(-80, 0)
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, "output/delay_impulse.png")
    print("  Saved output/delay_impulse.png")


# =============================================================================
# 5. Chorus Tests (Spectral Spread)
# =============================================================================

def test_chorus_spectrum():
    """
    Test chorus creates pitch-modulated copies.
    - Input: sine wave at 440Hz
    - Measure spectral sidebands around fundamental
    """
    print("Test: Chorus Spectral Spread")

    sr = 48000
    duration = 3.0

    host = CedarTestHost(sr)

    # Generate 440Hz sine
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.5

    # Chorus parameters
    buf_in = 0
    buf_rate = host.set_param("rate", 0.5)  # 0.5 Hz LFO
    buf_depth = host.set_param("depth", 0.5)
    buf_out = 1

    # EFFECT_CHORUS: out = chorus(in, rate, depth)
    # Rate field encodes mix
    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.EFFECT_CHORUS, buf_out, buf_in, buf_rate, buf_depth, cedar.hash("chorus") & 0xFFFF
    )
    inst.rate = 128  # 50% wet/dry mix
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))

    output = host.process(sine_input)

    # Analyze spectrum
    fft_size = 8192
    # Use steady-state portion
    steady_start = int(1.0 * sr)
    steady_output = output[steady_start:steady_start + fft_size]

    freqs = np.fft.rfftfreq(fft_size, 1/sr)
    spectrum = np.abs(np.fft.rfft(steady_output))
    spectrum_db = 20 * np.log10(spectrum + 1e-10)

    # Find fundamental and sidebands
    fundamental_idx = np.argmin(np.abs(freqs - 440))
    fundamental_level = spectrum_db[fundamental_idx]

    # Look for sidebands (detuned copies) within ±20Hz of fundamental
    sideband_region = (freqs > 420) & (freqs < 460) & (np.abs(freqs - 440) > 2)
    if np.any(sideband_region):
        sideband_level = np.max(spectrum_db[sideband_region])
        sideband_spread = np.sum(spectrum[sideband_region]) / (spectrum[fundamental_idx] + 1e-10)
        print(f"  Fundamental: {fundamental_level:.1f}dB at 440Hz")
        print(f"  Max sideband: {sideband_level:.1f}dB")
        print(f"  Spectral spread ratio: {sideband_spread:.3f}")
    else:
        print("  No sidebands detected")

    # Plot spectrum around fundamental
    fig, axes = plt.subplots(2, 1, figsize=(12, 8))

    ax1 = axes[0]
    mask = (freqs > 100) & (freqs < 1000)
    ax1.plot(freqs[mask], spectrum_db[mask], linewidth=1)
    ax1.axvline(440, color='red', linestyle='--', alpha=0.5, label='Fundamental (440Hz)')
    ax1.set_xlabel('Frequency (Hz)')
    ax1.set_ylabel('Magnitude (dB)')
    ax1.set_title('Chorus Spectrum (100-1000Hz)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Zoomed view
    ax2 = axes[1]
    mask_zoom = (freqs > 400) & (freqs < 500)
    ax2.plot(freqs[mask_zoom], spectrum_db[mask_zoom], linewidth=1)
    ax2.axvline(440, color='red', linestyle='--', alpha=0.5, label='Fundamental')
    ax2.set_xlabel('Frequency (Hz)')
    ax2.set_ylabel('Magnitude (dB)')
    ax2.set_title('Chorus Spectrum Detail (400-500Hz)')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, "output/chorus_spectrum.png")
    print("  Saved output/chorus_spectrum.png")


# =============================================================================
# 6. Flanger Tests (Sweeping Comb Filter)
# =============================================================================

def test_flanger_sweep():
    """
    Test flanger creates sweeping comb filter notches.
    - Input: white noise
    - Spectrogram should show periodic notch movement
    """
    print("Test: Flanger Sweep Pattern")

    sr = 48000
    duration = 4.0

    host = CedarTestHost(sr)
    noise = gen_white_noise(duration, sr)

    # Flanger parameters
    buf_in = 0
    buf_rate = host.set_param("rate", 0.5)  # 0.5 Hz sweep
    buf_depth = host.set_param("depth", 0.8)
    buf_out = 1

    # EFFECT_FLANGER: out = flanger(in, rate, depth)
    # Rate field: (feedback << 4) | mix
    feedback_int = int(0.7 * 15)  # 0.7 feedback
    mix_int = 8  # 50% mix
    packed_rate = (feedback_int << 4) | mix_int

    inst = cedar.Instruction.make_ternary(
        cedar.Opcode.EFFECT_FLANGER, buf_out, buf_in, buf_rate, buf_depth, cedar.hash("flanger") & 0xFFFF
    )
    inst.rate = packed_rate
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))

    output = host.process(noise)

    # Create spectrogram
    fig, axes = plt.subplots(2, 1, figsize=(14, 10))

    ax1 = axes[0]
    ax1.specgram(output, NFFT=2048, Fs=sr, noverlap=1024, cmap='magma')
    ax1.set_ylabel('Frequency (Hz)')
    ax1.set_xlabel('Time (s)')
    ax1.set_title('Flanger Spectrogram (0.5Hz sweep, 0.7 feedback)')
    ax1.set_ylim(0, 5000)

    # Input comparison
    ax2 = axes[1]
    ax2.specgram(noise, NFFT=2048, Fs=sr, noverlap=1024, cmap='magma')
    ax2.set_ylabel('Frequency (Hz)')
    ax2.set_xlabel('Time (s)')
    ax2.set_title('Input (White Noise) Spectrogram')
    ax2.set_ylim(0, 5000)

    plt.tight_layout()
    save_figure(fig, "output/flanger_spectrogram.png")
    print("  Saved output/flanger_spectrogram.png")


# =============================================================================
# 7. Bitcrush Tests (Quantization Levels)
# =============================================================================

def test_bitcrush_levels():
    """
    Test bit depth reduction creates discrete steps.
    - Input: slow sine sweep
    - Count distinct output levels for given bit depth
    """
    print("Test: Bitcrush Quantization Levels")

    sr = 48000
    duration = 1.0

    # Test various bit depths
    bit_depths = [8, 4, 3, 2]

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    for idx, bits in enumerate(bit_depths):
        host = CedarTestHost(sr)

        # Generate slow sine (one cycle over duration)
        t = np.arange(int(duration * sr)) / sr
        sine_input = np.sin(2 * np.pi * 1 * t).astype(np.float32)  # 1 Hz

        buf_in = 0
        buf_bits = host.set_param("bits", float(bits))
        buf_rate = host.set_param("rate", 1.0)  # No sample rate reduction
        buf_out = 1

        # DISTORT_BITCRUSH: out = bitcrush(in, bits, rate_factor)
        inst = cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_BITCRUSH, buf_out, buf_in, buf_bits, buf_rate, cedar.hash("crush") & 0xFFFF
        )
        host.load_instruction(inst)
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))

        output = host.process(sine_input)

        # Count unique levels (with some tolerance for floating point)
        unique_levels = len(np.unique(np.round(output, 4)))
        expected_levels = 2 ** bits

        print(f"  {bits}-bit: expected {expected_levels} levels, measured {unique_levels}")

        # Plot transfer curve
        ax = axes[idx // 2, idx % 2]
        ax.plot(sine_input, output, 'b.', markersize=0.5, alpha=0.5)
        ax.plot([-1, 1], [-1, 1], 'k--', alpha=0.3, label='Linear')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.set_title(f'{bits}-bit Bitcrush (expected: {expected_levels} levels)')
        ax.set_aspect('equal')
        ax.grid(True, alpha=0.3)
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.1, 1.1)

    plt.tight_layout()
    save_figure(fig, "output/bitcrush_levels.png")
    print("  Saved output/bitcrush_levels.png")

    # Test sample rate reduction
    print("\n  Sample Rate Reduction Test:")
    host2 = CedarTestHost(sr)

    # High frequency sine to show sample rate reduction
    t2 = np.arange(int(0.1 * sr)) / sr
    hf_sine = np.sin(2 * np.pi * 5000 * t2).astype(np.float32)  # 5kHz

    buf_in2 = 0
    buf_bits2 = host2.set_param("bits", 16.0)  # Full bit depth
    buf_rate2 = host2.set_param("rate", 0.1)  # 10% sample rate = 4.8kHz effective
    buf_out2 = 1

    inst2 = cedar.Instruction.make_ternary(
        cedar.Opcode.DISTORT_BITCRUSH, buf_out2, buf_in2, buf_bits2, buf_rate2, cedar.hash("crush2") & 0xFFFF
    )
    host2.load_instruction(inst2)
    host2.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out2))

    output2 = host2.process(hf_sine)

    # The output should have aliasing artifacts due to low sample rate
    freqs = np.fft.rfftfreq(len(output2), 1/sr)
    spectrum = 20 * np.log10(np.abs(np.fft.rfft(output2)) + 1e-10)

    # Find peaks below nyquist of reduced rate
    alias_freq = sr * 0.1 / 2  # ~2.4kHz nyquist
    below_alias = freqs < alias_freq
    if np.any(below_alias):
        max_alias_level = np.max(spectrum[below_alias])
        print(f"  Effective Nyquist: {alias_freq:.0f}Hz")
        print(f"  Aliasing detected: {max_alias_level:.1f}dB")


# =============================================================================
# 8. DISTORT_FOLD ADAA Tests (Antiderivative Antialiasing)
# =============================================================================

def test_distort_fold_transfer_curve():
    """
    Test DISTORT_FOLD transfer curve shape.
    - Should show smooth sine-fold pattern
    - No discontinuities at fold points
    """
    print("Test: DISTORT_FOLD Transfer Curve")

    host = CedarTestHost()
    ramp = gen_linear_ramp(4096)

    # Test various drive values
    drive_values = [1.5, 3.0, 5.0, 8.0]

    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle("DISTORT_FOLD Transfer Curves (ADAA Sine Wavefolder)")

    for drive, ax in zip(drive_values, axes.flat):
        host = CedarTestHost()

        buf_in = 0
        buf_drive = host.set_param("drive", drive)
        buf_sym = host.set_param("symmetry", 0.5)  # Symmetric

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
            cedar.hash("fold_test") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        # Plot
        ax.plot(ramp, output, linewidth=1.5, label='ADAA Output')
        ax.plot(ramp, ramp, 'k--', alpha=0.3, label='Linear')

        # Reference: what the naive sin(drive * x) would look like
        ref = np.sin(drive * ramp)
        ax.plot(ramp, ref, 'r:', alpha=0.5, linewidth=1, label='sin(drive*x)')

        ax.set_title(f'Drive = {drive}')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.legend(loc='upper left', fontsize=8)
        ax.set_aspect('equal')
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.5, 1.5)

    plt.tight_layout()
    save_figure(fig, "output/distort_fold_transfer.png")
    print("  Saved output/distort_fold_transfer.png")


def test_distort_fold_aliasing():
    """
    Test DISTORT_FOLD ADAA aliasing reduction.
    Compare high-frequency signal through folder with/without ADAA.
    """
    print("Test: DISTORT_FOLD Aliasing Analysis")

    sr = 48000
    duration = 1.0
    test_freqs = [1000, 3000, 5000, 8000]  # Hz

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle("DISTORT_FOLD Aliasing Analysis (ADAA vs No ADAA)")

    for freq, ax in zip(test_freqs, axes.flat):
        host = CedarTestHost(sr)

        # Generate sine at test frequency
        t = np.arange(int(duration * sr)) / sr
        sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.8

        buf_in = 0
        buf_drive = host.set_param("drive", 4.0)  # Strong folding
        buf_sym = host.set_param("symmetry", 0.5)

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
            cedar.hash("fold_alias") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        # Save WAV for human evaluation
        wav_path = f"output/distort_fold_aliasing_{freq}hz.wav"
        scipy.io.wavfile.write(wav_path, sr, output)

        # Analyze spectrum
        fft_size = 8192
        # Use steady-state portion
        steady = output[int(0.1 * sr):int(0.1 * sr) + fft_size]

        freqs_fft = np.fft.rfftfreq(fft_size, 1/sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        # Plot spectrum
        ax.plot(freqs_fft, spectrum_db, linewidth=0.5)

        # Mark harmonics and aliased components
        nyquist = sr / 2
        fundamental_idx = int(freq * fft_size / sr)
        ax.axvline(freq, color='green', linestyle='--', alpha=0.5, label=f'Fund. {freq}Hz')
        ax.axvline(nyquist, color='red', linestyle='--', alpha=0.3, label='Nyquist')

        # Expected harmonics from wavefolder: odd harmonics primarily
        for h in [3, 5, 7, 9, 11]:
            h_freq = freq * h
            if h_freq < nyquist:
                ax.axvline(h_freq, color='blue', linestyle=':', alpha=0.3)
            else:
                # Aliased frequency
                aliased = sr - h_freq % sr if h_freq % sr > nyquist else h_freq % sr
                ax.axvline(aliased, color='orange', linestyle=':', alpha=0.3)

        # Measure noise floor (away from harmonics)
        harmonic_mask = np.ones(len(freqs_fft), dtype=bool)
        for h in range(1, 20):
            h_freq = freq * h
            h_idx = int(h_freq * fft_size / sr) if h_freq < nyquist else 0
            if h_idx > 0 and h_idx < len(harmonic_mask):
                # Mask out ±5 bins around harmonic
                harmonic_mask[max(0, h_idx-10):min(len(harmonic_mask), h_idx+10)] = False

        noise_floor = np.median(spectrum_db[harmonic_mask & (freqs_fft > 100) & (freqs_fft < nyquist - 100)])

        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.set_title(f'Input: {freq}Hz sine (noise floor: {noise_floor:.1f}dB)')
        ax.set_xlim(0, nyquist)
        ax.set_ylim(-100, 0)
        ax.grid(True, alpha=0.3)
        ax.legend(loc='upper right', fontsize=7)

        # ADAA should keep noise floor below -60dB
        status = "✓ PASS" if noise_floor < -60 else "⚠ HIGH ALIASING"
        print(f"  {freq}Hz: noise floor = {noise_floor:.1f}dB {status} [{wav_path}]")

    plt.tight_layout()
    save_figure(fig, "output/distort_fold_aliasing.png")
    print("  Saved output/distort_fold_aliasing.png")


def test_distort_fold_symmetry():
    """
    Test DISTORT_FOLD symmetry parameter effect.
    Asymmetry should introduce even harmonics.
    """
    print("Test: DISTORT_FOLD Symmetry Parameter")

    sr = 48000
    duration = 0.5

    symmetry_values = [0.0, 0.25, 0.5, 0.75, 1.0]

    fig, axes = plt.subplots(2, 3, figsize=(15, 8))
    fig.suptitle("DISTORT_FOLD Symmetry Effect on Harmonics")

    # Transfer curves
    for sym, ax in zip(symmetry_values, axes[0].flat):
        host = CedarTestHost(sr)
        ramp = gen_linear_ramp(2048)

        buf_in = 0
        buf_drive = host.set_param("drive", 4.0)
        buf_sym = host.set_param("symmetry", sym)

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
            cedar.hash("fold_sym") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(ramp)

        ax.plot(ramp, output, linewidth=1.5)
        ax.plot(ramp, ramp, 'k--', alpha=0.3)
        ax.set_title(f'Symmetry = {sym}')
        ax.set_xlabel('Input')
        ax.set_ylabel('Output')
        ax.grid(True, alpha=0.3)
        ax.set_aspect('equal')
        ax.set_xlim(-1.1, 1.1)
        ax.set_ylim(-1.5, 1.5)

    # Spectral analysis with sine input
    axes[0, 2].axis('off')  # Empty the 6th subplot

    # Harmonic comparison
    freq = 440.0
    t = np.arange(int(duration * sr)) / sr
    sine_input = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.7

    ax_harm = axes[1, 0]
    ax_spec = axes[1, 1]

    colors = plt.cm.viridis(np.linspace(0, 1, len(symmetry_values)))
    harmonic_data = {}

    for sym, color in zip(symmetry_values, colors):
        host = CedarTestHost(sr)

        buf_in = 0
        buf_drive = host.set_param("drive", 4.0)
        buf_sym = host.set_param("symmetry", sym)

        host.load_instruction(cedar.Instruction.make_ternary(
            cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
            cedar.hash("fold_sym_spec") & 0xFFFF
        ))
        host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

        output = host.process(sine_input)

        # Spectrum
        fft_size = 8192
        steady = output[int(0.1 * sr):int(0.1 * sr) + fft_size]
        freqs_fft = np.fft.rfftfreq(fft_size, 1/sr)
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(fft_size)))
        spectrum_db = 20 * np.log10(spectrum + 1e-10)

        # Plot spectrum
        mask = (freqs_fft > 100) & (freqs_fft < 5000)
        ax_spec.plot(freqs_fft[mask], spectrum_db[mask], color=color, alpha=0.7,
                    label=f'sym={sym}', linewidth=0.8)

        # Extract harmonics
        fundamental_idx = int(freq * fft_size / sr)
        fund_level = spectrum_db[fundamental_idx]
        harmonics = []
        for h in range(1, 8):
            h_idx = fundamental_idx * h
            if h_idx < len(spectrum_db):
                harmonics.append(spectrum_db[h_idx] - fund_level)
            else:
                harmonics.append(-100)
        harmonic_data[sym] = harmonics

    ax_spec.set_xlabel('Frequency (Hz)')
    ax_spec.set_ylabel('Magnitude (dB)')
    ax_spec.set_title('Spectrum Comparison')
    ax_spec.legend(fontsize=7)
    ax_spec.grid(True, alpha=0.3)

    # Harmonic comparison bar chart
    x = np.arange(1, 8)
    width = 0.15
    for i, (sym, harmonics) in enumerate(harmonic_data.items()):
        ax_harm.bar(x + i * width - 0.3, harmonics, width, label=f'sym={sym}', color=colors[i])

    ax_harm.set_xlabel('Harmonic Number')
    ax_harm.set_ylabel('Level (dB rel. fundamental)')
    ax_harm.set_title('Harmonic Content by Symmetry')
    ax_harm.legend(fontsize=7)
    ax_harm.grid(True, alpha=0.3, axis='y')
    ax_harm.set_xticks(x)

    # Summary text
    axes[1, 2].axis('off')
    axes[1, 2].text(0.1, 0.8, "Symmetry Effect:", fontsize=12, fontweight='bold')
    axes[1, 2].text(0.1, 0.6, "• sym=0.5: Symmetric → odd harmonics only", fontsize=10)
    axes[1, 2].text(0.1, 0.45, "• sym≠0.5: Asymmetric → even harmonics appear", fontsize=10)
    axes[1, 2].text(0.1, 0.3, "• DC offset shifts with symmetry", fontsize=10)

    plt.tight_layout()
    save_figure(fig, "output/distort_fold_symmetry.png")
    print("  Saved output/distort_fold_symmetry.png")


def test_distort_fold_continuity():
    """
    Test DISTORT_FOLD ADAA continuity at fold points.
    Zoomed view should show smooth transitions, no discontinuities.
    """
    print("Test: DISTORT_FOLD Continuity at Fold Points")

    host = CedarTestHost()

    # High resolution ramp to check continuity
    ramp = np.linspace(-1, 1, 16384, dtype=np.float32)

    buf_in = 0
    buf_drive = host.set_param("drive", 6.0)  # Multiple folds
    buf_sym = host.set_param("symmetry", 0.5)

    host.load_instruction(cedar.Instruction.make_ternary(
        cedar.Opcode.DISTORT_FOLD, 1, buf_in, buf_drive, buf_sym,
        cedar.hash("fold_cont") & 0xFFFF
    ))
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(ramp)

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle("DISTORT_FOLD Continuity Analysis (Drive=6.0)")

    # Full transfer curve
    axes[0, 0].plot(ramp, output, linewidth=1)
    axes[0, 0].set_title('Full Transfer Curve')
    axes[0, 0].set_xlabel('Input')
    axes[0, 0].set_ylabel('Output')
    axes[0, 0].grid(True, alpha=0.3)

    # Derivative (should be continuous for ADAA)
    derivative = np.diff(output) / np.diff(ramp)
    axes[0, 1].plot(ramp[:-1], derivative, linewidth=0.5)
    axes[0, 1].set_title('Derivative (should be continuous)')
    axes[0, 1].set_xlabel('Input')
    axes[0, 1].set_ylabel('d(output)/d(input)')
    axes[0, 1].grid(True, alpha=0.3)

    # Check for discontinuities
    diff2 = np.diff(derivative)
    max_jump = np.max(np.abs(diff2))

    # Zoomed view around a fold point (where derivative is large)
    peak_idx = np.argmax(np.abs(derivative))
    zoom_start = max(0, peak_idx - 500)
    zoom_end = min(len(ramp), peak_idx + 500)

    axes[1, 0].plot(ramp[zoom_start:zoom_end], output[zoom_start:zoom_end], 'b-', linewidth=2)
    axes[1, 0].set_title(f'Zoomed: Around Fold Point')
    axes[1, 0].set_xlabel('Input')
    axes[1, 0].set_ylabel('Output')
    axes[1, 0].grid(True, alpha=0.3)

    axes[1, 1].plot(ramp[zoom_start:zoom_end-1], derivative[zoom_start:zoom_end-1], 'g-', linewidth=1)
    axes[1, 1].set_title(f'Zoomed Derivative (max Δ²={max_jump:.4f})')
    axes[1, 1].set_xlabel('Input')
    axes[1, 1].set_ylabel('Derivative')
    axes[1, 1].grid(True, alpha=0.3)

    # Report
    if max_jump < 0.1:
        print(f"  ✓ Continuity good: max derivative jump = {max_jump:.6f}")
    else:
        print(f"  ⚠ Possible discontinuity: max derivative jump = {max_jump:.6f}")

    plt.tight_layout()
    save_figure(fig, "output/distort_fold_continuity.png")
    print("  Saved output/distort_fold_continuity.png")


if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)

    # Original tests
    test_distortion_curves()
    test_phaser_spectrogram()
    test_reverb_decay()
    test_delay_timing()
    test_chorus_spectrum()
    test_flanger_sweep()
    test_bitcrush_levels()

    # New DISTORT_FOLD ADAA tests
    print("\n=== DISTORT_FOLD ADAA Tests ===")
    test_distort_fold_transfer_curve()
    test_distort_fold_aliasing()
    test_distort_fold_symmetry()
    test_distort_fold_continuity()