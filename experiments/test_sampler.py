"""
Sample Player Accuracy Tests (Cedar Engine)
============================================
Tests sample playback accuracy including pitch shifting,
interpolation quality, sample rate conversion, and loop accuracy.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
from scipy.io import wavfile
import cedar_core as cedar
from visualize import save_figure
from utils import linear_to_db, midi_to_freq, freq_to_midi


def save_wav(filename: str, data: np.ndarray, sample_rate: int = 48000):
    """Save audio data to WAV file.

    Args:
        filename: Output filename (will be placed in output/wav/ directory)
        data: Audio data (float32, -1.0 to 1.0)
        sample_rate: Sample rate in Hz
    """
    wav_dir = 'output/wav'
    os.makedirs(wav_dir, exist_ok=True)
    filepath = os.path.join(wav_dir, filename)

    # Convert to 16-bit PCM
    data_clipped = np.clip(data, -1.0, 1.0)
    data_int16 = (data_clipped * 32767).astype(np.int16)

    wavfile.write(filepath, sample_rate, data_int16)
    print(f"    Saved: {filepath}")


class SamplerTestHost:
    """Helper to run Cedar VM sample playback tests."""

    def __init__(self, sample_rate=48000):
        self.vm = cedar.VM()
        self.vm.set_sample_rate(sample_rate)
        self.sr = sample_rate
        self.program = []
        self.sample_ids = {}

    def load_sine_sample(self, name: str, freq: float, duration_sec: float,
                         sample_rate: float = None) -> int:
        """Generate and load a sine wave sample.

        Args:
            name: Sample name for identification
            freq: Frequency of the sine wave in Hz
            duration_sec: Duration of the sample in seconds
            sample_rate: Sample rate (defaults to host sample rate)

        Returns:
            Sample ID
        """
        if sample_rate is None:
            sample_rate = self.sr

        num_samples = int(duration_sec * sample_rate)
        t = np.arange(num_samples) / sample_rate
        # Use exact integer cycles to ensure clean looping
        cycles = int(freq * duration_sec)
        actual_freq = cycles / duration_sec
        data = np.sin(2 * np.pi * actual_freq * t).astype(np.float32)

        sample_id = self.vm.load_sample(name, data, channels=1, sample_rate=sample_rate)
        self.sample_ids[name] = sample_id
        return sample_id

    def load_custom_sample(self, name: str, data: np.ndarray, channels: int = 1,
                           sample_rate: float = None) -> int:
        """Load a custom sample.

        Args:
            name: Sample name
            data: Audio data (interleaved if stereo)
            channels: Number of channels
            sample_rate: Sample rate

        Returns:
            Sample ID
        """
        if sample_rate is None:
            sample_rate = self.sr

        data = data.astype(np.float32)
        sample_id = self.vm.load_sample(name, data, channels=channels, sample_rate=sample_rate)
        self.sample_ids[name] = sample_id
        return sample_id

    def create_sampler_program(self, sample_id: int, pitch: float = 1.0,
                               loop: bool = False, trigger_every_n_blocks: int = 0):
        """Create a program to play back a sample.

        Args:
            sample_id: Sample ID to play
            pitch: Pitch multiplier (1.0 = original, 2.0 = octave up)
            loop: If True, use looping playback
            trigger_every_n_blocks: If >0, retrigger every N blocks
        """
        self.program = []
        self.trigger_every_n_blocks = trigger_every_n_blocks
        self.current_block = 0

        # Set up parameters
        self.vm.set_param("pitch", pitch)
        self.vm.set_param("sample_id", float(sample_id))

        # Buffer layout:
        # 0: trigger/gate
        # 1: pitch
        # 2: sample_id
        # 10: output

        # Get pitch parameter -> buf 1
        pitch_hash = cedar.hash("pitch")
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, pitch_hash)
        )

        # Get sample_id parameter -> buf 2
        sid_hash = cedar.hash("sample_id")
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, sid_hash)
        )

        # Sampler state
        sampler_state = cedar.hash("sampler_test")

        if loop:
            # SAMPLE_PLAY_LOOP: gate, pitch, sample_id
            self.program.append(
                cedar.Instruction.make_ternary(
                    cedar.Opcode.SAMPLE_PLAY_LOOP, 10, 0, 1, 2, sampler_state
                )
            )
        else:
            # SAMPLE_PLAY: trigger, pitch, sample_id
            self.program.append(
                cedar.Instruction.make_ternary(
                    cedar.Opcode.SAMPLE_PLAY, 10, 0, 1, 2, sampler_state
                )
            )

        # Route to output
        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def run(self, duration_sec: float, trigger_at_start: bool = True) -> np.ndarray:
        """Run the program and return audio.

        Args:
            duration_sec: Duration to render in seconds
            trigger_at_start: Send trigger on first block

        Returns:
            Output audio as numpy array
        """
        num_blocks = int((duration_sec * self.sr) / cedar.BLOCK_SIZE)
        output = []

        for block_idx in range(num_blocks):
            # Create trigger/gate signal
            trigger = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)

            if trigger_at_start and block_idx == 0:
                trigger[0] = 1.0  # Rising edge trigger
            elif self.trigger_every_n_blocks > 0 and block_idx % self.trigger_every_n_blocks == 0:
                trigger[0] = 1.0

            # Set trigger in buffer 0
            self.vm.set_buffer(0, trigger)

            left, right = self.vm.process()
            output.append(left)

        return np.concatenate(output)

    def run_looped(self, duration_sec: float) -> np.ndarray:
        """Run looped playback with gate held high.

        Args:
            duration_sec: Duration to render

        Returns:
            Output audio
        """
        num_blocks = int((duration_sec * self.sr) / cedar.BLOCK_SIZE)
        output = []

        for block_idx in range(num_blocks):
            # Gate stays high for looping
            gate = np.ones(cedar.BLOCK_SIZE, dtype=np.float32)
            self.vm.set_buffer(0, gate)

            left, right = self.vm.process()
            output.append(left)

        return np.concatenate(output)


def measure_frequency(signal: np.ndarray, sample_rate: int, expected_freq: float = None) -> dict:
    """Measure the fundamental frequency of a signal using FFT.

    Args:
        signal: Input signal
        sample_rate: Sample rate in Hz
        expected_freq: Expected frequency for error calculation

    Returns:
        Dict with frequency measurement results
    """
    # Zero-pad for better frequency resolution
    n_fft = 2 ** int(np.ceil(np.log2(len(signal) * 4)))

    fft_freqs = np.fft.rfftfreq(n_fft, 1/sample_rate)
    fft_mag = np.abs(np.fft.rfft(signal, n=n_fft))

    # Find peak (ignore DC and very low frequencies)
    min_idx = int(20 * n_fft / sample_rate)  # Start at 20 Hz
    peak_idx = min_idx + np.argmax(fft_mag[min_idx:])

    # Quadratic interpolation for sub-bin accuracy
    if peak_idx > 0 and peak_idx < len(fft_mag) - 1:
        alpha = fft_mag[peak_idx - 1]
        beta = fft_mag[peak_idx]
        gamma = fft_mag[peak_idx + 1]

        if beta > 0:
            p = 0.5 * (alpha - gamma) / (alpha - 2*beta + gamma)
            measured_freq = fft_freqs[peak_idx] + p * (fft_freqs[1] - fft_freqs[0])
        else:
            measured_freq = fft_freqs[peak_idx]
    else:
        measured_freq = fft_freqs[peak_idx]

    result = {
        'measured_freq_hz': float(measured_freq),
        'peak_magnitude_db': float(20 * np.log10(fft_mag[peak_idx] + 1e-10)),
        'freq_resolution_hz': float(sample_rate / n_fft),
    }

    if expected_freq is not None:
        result['expected_freq_hz'] = float(expected_freq)
        result['error_hz'] = float(measured_freq - expected_freq)
        result['error_cents'] = float(1200 * np.log2(measured_freq / expected_freq)) if expected_freq > 0 else 0
        result['error_percent'] = float((measured_freq - expected_freq) / expected_freq * 100) if expected_freq > 0 else 0

    return result


def analyze_interpolation_quality(signal: np.ndarray, sample_rate: int,
                                   fundamental_freq: float) -> dict:
    """Analyze interpolation artifacts in pitch-shifted signal.

    Args:
        signal: Output signal
        sample_rate: Sample rate
        fundamental_freq: Expected fundamental frequency

    Returns:
        Dict with interpolation quality metrics
    """
    n_fft = 2 ** int(np.ceil(np.log2(len(signal) * 4)))
    fft_freqs = np.fft.rfftfreq(n_fft, 1/sample_rate)
    fft_mag = np.abs(np.fft.rfft(signal, n=n_fft))
    fft_mag_db = 20 * np.log10(fft_mag + 1e-10)

    # Find harmonics
    nyquist = sample_rate / 2
    harmonics = []
    n = 1
    while fundamental_freq * n < nyquist:
        freq = fundamental_freq * n
        idx = np.argmin(np.abs(fft_freqs - freq))

        # Search in window for peak
        window = 10
        start = max(0, idx - window)
        end = min(len(fft_mag_db), idx + window)
        local_peak_idx = start + np.argmax(fft_mag_db[start:end])

        harmonics.append({
            'harmonic': n,
            'expected_freq': float(freq),
            'magnitude_db': float(fft_mag_db[local_peak_idx])
        })
        n += 1

    # Calculate noise floor (excluding harmonics)
    noise_mask = np.ones(len(fft_freqs), dtype=bool)
    for h in harmonics:
        noise_mask &= (np.abs(fft_freqs - h['expected_freq']) > 100)

    noise_region = fft_mag_db[noise_mask & (fft_freqs > 100) & (fft_freqs < nyquist - 1000)]
    noise_floor_db = float(np.median(noise_region)) if len(noise_region) > 0 else -120

    # Look for intermodulation products (non-harmonic peaks)
    artifacts = []
    threshold_db = noise_floor_db + 20

    for i in range(1, len(fft_mag_db) - 1):
        if fft_mag_db[i] > threshold_db:
            if fft_mag_db[i] > fft_mag_db[i-1] and fft_mag_db[i] > fft_mag_db[i+1]:
                freq = fft_freqs[i]
                # Check if it's near a harmonic
                is_harmonic = any(abs(freq - h['expected_freq']) < 50 for h in harmonics)
                if not is_harmonic and freq > 50:
                    artifacts.append({
                        'frequency_hz': float(freq),
                        'magnitude_db': float(fft_mag_db[i])
                    })

    # Sort artifacts by magnitude
    artifacts.sort(key=lambda x: x['magnitude_db'], reverse=True)

    return {
        'harmonics': harmonics[:10],  # First 10
        'noise_floor_db': noise_floor_db,
        'artifact_count': len(artifacts),
        'top_artifacts': artifacts[:5],
        'snr_db': float(harmonics[0]['magnitude_db'] - noise_floor_db) if harmonics else 0
    }


def analyze_loop_continuity(signal: np.ndarray, loop_length_samples: int,
                            num_crossings: int = 10) -> dict:
    """Analyze loop point continuity by measuring discontinuities.

    Args:
        signal: Looped output signal
        loop_length_samples: Expected loop length in samples
        num_crossings: Number of loop crossings to analyze

    Returns:
        Dict with loop continuity metrics
    """
    crossings = []

    for i in range(1, num_crossings + 1):
        cross_idx = int(i * loop_length_samples)
        if cross_idx >= len(signal) - 1:
            break

        # Measure discontinuity at crossing
        before = signal[cross_idx - 1]
        after = signal[cross_idx]

        # Also check surrounding samples for smoothness
        window = min(10, cross_idx, len(signal) - cross_idx - 1)
        before_slope = signal[cross_idx] - signal[cross_idx - window]
        after_slope = signal[cross_idx + window] - signal[cross_idx]

        crossings.append({
            'crossing_number': i,
            'sample_index': cross_idx,
            'discontinuity': float(abs(after - before)),
            'slope_change': float(abs(after_slope - before_slope))
        })

    avg_discontinuity = np.mean([c['discontinuity'] for c in crossings]) if crossings else 0
    max_discontinuity = max([c['discontinuity'] for c in crossings]) if crossings else 0

    return {
        'crossings': crossings,
        'average_discontinuity': float(avg_discontinuity),
        'max_discontinuity': float(max_discontinuity),
        'loop_length_samples': loop_length_samples
    }


# =============================================================================
# Test 1: Pitch Accuracy at Various Ratios
# =============================================================================

def test_pitch_accuracy():
    """Test pitch accuracy at various pitch ratios."""
    print("Test 1: Pitch Accuracy at Various Ratios")
    print("=" * 60)

    sr = 48000
    base_freq = 440.0  # A4
    sample_duration = 0.5  # 0.5 second sample
    test_duration = 0.5  # 0.5 seconds playback

    # Pitch ratios to test: octaves and musical intervals
    pitch_ratios = [
        (0.5, "Octave Down"),
        (0.7937, "Minor Third Down"),  # 2^(-3/12)
        (1.0, "Unison"),
        (1.1225, "Whole Step Up"),  # 2^(2/12)
        (1.2599, "Major Third Up"),  # 2^(4/12)
        (1.4983, "Perfect Fifth Up"),  # 2^(7/12)
        (2.0, "Octave Up"),
        (3.0, "Octave + Fifth Up"),
        (4.0, "Two Octaves Up"),
    ]

    results = {
        'sample_rate': sr,
        'base_frequency': base_freq,
        'tests': []
    }

    for pitch_ratio, name in pitch_ratios:
        expected_freq = base_freq * pitch_ratio

        # Skip if above Nyquist
        if expected_freq >= sr / 2:
            print(f"  Skipping {name} (pitch={pitch_ratio:.4f}): {expected_freq:.1f} Hz >= Nyquist")
            continue

        host = SamplerTestHost(sr)
        host.load_sine_sample("test_sine", base_freq, sample_duration)
        sample_id = host.sample_ids["test_sine"]

        host.create_sampler_program(sample_id, pitch=pitch_ratio, loop=False)
        output = host.run(test_duration)

        # Measure frequency
        freq_result = measure_frequency(output, sr, expected_freq)

        test_result = {
            'name': name,
            'pitch_ratio': pitch_ratio,
            'expected_freq_hz': expected_freq,
            **freq_result
        }
        results['tests'].append(test_result)

        # Print results
        status = "PASS" if abs(freq_result['error_cents']) < 5 else "FAIL"
        print(f"  {name:20s} pitch={pitch_ratio:.4f}: "
              f"expected={expected_freq:.2f}Hz, measured={freq_result['measured_freq_hz']:.2f}Hz, "
              f"error={freq_result['error_cents']:.2f} cents [{status}]")

    # Save results
    with open('output/sampler_pitch_accuracy.json', 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\n  Saved: output/sampler_pitch_accuracy.json")

    # Create visualization
    fig, axes = plt.subplots(2, 1, figsize=(14, 10))

    test_data = results['tests']
    ratios = [t['pitch_ratio'] for t in test_data]
    errors_cents = [t['error_cents'] for t in test_data]
    errors_percent = [t['error_percent'] for t in test_data]

    # Error in cents
    ax1 = axes[0]
    bars1 = ax1.bar(range(len(ratios)), errors_cents, color='steelblue')
    ax1.axhline(0, color='black', linewidth=0.5)
    ax1.axhline(5, color='red', linestyle='--', alpha=0.5, label='+5 cents')
    ax1.axhline(-5, color='red', linestyle='--', alpha=0.5, label='-5 cents')
    ax1.set_xticks(range(len(ratios)))
    ax1.set_xticklabels([f"{r:.3f}" for r in ratios], rotation=45)
    ax1.set_xlabel('Pitch Ratio')
    ax1.set_ylabel('Error (cents)')
    ax1.set_title('Pitch Accuracy Error')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Error in percent
    ax2 = axes[1]
    bars2 = ax2.bar(range(len(ratios)), errors_percent, color='coral')
    ax2.axhline(0, color='black', linewidth=0.5)
    ax2.set_xticks(range(len(ratios)))
    ax2.set_xticklabels([t['name'] for t in test_data], rotation=45)
    ax2.set_xlabel('Pitch Ratio')
    ax2.set_ylabel('Error (%)')
    ax2.set_title('Pitch Accuracy Error (Percent)')
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sampler_pitch_accuracy.png')
    print(f"  Saved: output/sampler_pitch_accuracy.png")

    return results


# =============================================================================
# Test 2: Interpolation Quality
# =============================================================================

def test_interpolation_quality():
    """Test interpolation quality at various pitch ratios."""
    print("\nTest 2: Interpolation Quality")
    print("=" * 60)

    sr = 48000
    base_freq = 440.0
    sample_duration = 0.5
    test_duration = 0.5

    # Test at pitch ratios that stress linear interpolation
    pitch_ratios = [0.5, 0.75, 1.0, 1.5, 2.0, 3.0]

    results = {
        'sample_rate': sr,
        'base_frequency': base_freq,
        'tests': []
    }

    fig, axes = plt.subplots(len(pitch_ratios), 2, figsize=(16, 4 * len(pitch_ratios)))

    for idx, pitch_ratio in enumerate(pitch_ratios):
        expected_freq = base_freq * pitch_ratio

        if expected_freq >= sr / 2 - 1000:
            print(f"  Skipping pitch={pitch_ratio}: {expected_freq:.1f} Hz near Nyquist")
            continue

        host = SamplerTestHost(sr)
        host.load_sine_sample("test_sine", base_freq, sample_duration)
        sample_id = host.sample_ids["test_sine"]

        host.create_sampler_program(sample_id, pitch=pitch_ratio, loop=False)
        output = host.run(test_duration)

        # Analyze interpolation quality
        quality = analyze_interpolation_quality(output, sr, expected_freq)

        test_result = {
            'pitch_ratio': pitch_ratio,
            'expected_freq_hz': expected_freq,
            **quality
        }
        results['tests'].append(test_result)

        print(f"  Pitch {pitch_ratio:.2f}x: SNR={quality['snr_db']:.1f}dB, "
              f"noise_floor={quality['noise_floor_db']:.1f}dB, "
              f"artifacts={quality['artifact_count']}")

        # Plot waveform (first few cycles)
        ax_wave = axes[idx, 0]
        cycles_to_show = 5
        samples_per_cycle = sr / expected_freq
        samples_to_show = int(cycles_to_show * samples_per_cycle)

        time_ms = np.arange(samples_to_show) / sr * 1000
        ax_wave.plot(time_ms, output[:samples_to_show], linewidth=0.8)
        ax_wave.set_xlabel('Time (ms)')
        ax_wave.set_ylabel('Amplitude')
        ax_wave.set_title(f'Waveform @ pitch={pitch_ratio}x ({expected_freq:.1f} Hz)')
        ax_wave.grid(True, alpha=0.3)

        # Plot spectrum
        ax_spec = axes[idx, 1]
        n_fft = 2 ** int(np.ceil(np.log2(len(output) * 2)))
        fft_freqs = np.fft.rfftfreq(n_fft, 1/sr)
        fft_mag = np.abs(np.fft.rfft(output, n=n_fft))
        fft_mag_db = 20 * np.log10(fft_mag + 1e-10)

        ax_spec.plot(fft_freqs, fft_mag_db, linewidth=0.5, alpha=0.7)

        # Mark expected harmonics
        n = 1
        while expected_freq * n < sr / 2:
            ax_spec.axvline(expected_freq * n, color='red', alpha=0.3, linewidth=0.5)
            n += 1

        ax_spec.set_xlim(0, min(expected_freq * 10, sr / 2))
        ax_spec.set_ylim(quality['noise_floor_db'] - 10, fft_mag_db.max() + 5)
        ax_spec.set_xlabel('Frequency (Hz)')
        ax_spec.set_ylabel('Magnitude (dB)')
        ax_spec.set_title(f'Spectrum @ pitch={pitch_ratio}x (SNR={quality["snr_db"]:.1f}dB)')
        ax_spec.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sampler_interpolation_quality.png')
    print(f"\n  Saved: output/sampler_interpolation_quality.png")

    with open('output/sampler_interpolation_quality.json', 'w') as f:
        json.dump(results, f, indent=2)
    print(f"  Saved: output/sampler_interpolation_quality.json")

    return results


# =============================================================================
# Test 3: Sample Rate Conversion
# =============================================================================

def test_sample_rate_conversion():
    """Test sample playback with different source sample rates."""
    print("\nTest 3: Sample Rate Conversion")
    print("=" * 60)

    playback_sr = 48000
    base_freq = 440.0
    sample_duration = 0.5
    test_duration = 0.5

    # Test samples at various sample rates
    source_sample_rates = [22050, 44100, 48000, 96000]

    results = {
        'playback_sample_rate': playback_sr,
        'base_frequency': base_freq,
        'tests': []
    }

    fig, axes = plt.subplots(len(source_sample_rates), 2, figsize=(16, 4 * len(source_sample_rates)))

    for idx, source_sr in enumerate(source_sample_rates):
        host = SamplerTestHost(playback_sr)

        # Load sample at different sample rate
        host.load_sine_sample("test_sine", base_freq, sample_duration, sample_rate=source_sr)
        sample_id = host.sample_ids["test_sine"]

        host.create_sampler_program(sample_id, pitch=1.0, loop=False)
        output = host.run(test_duration)

        # Measure frequency - should still be 440 Hz regardless of source sample rate
        freq_result = measure_frequency(output, playback_sr, base_freq)

        test_result = {
            'source_sample_rate': source_sr,
            'expected_freq_hz': base_freq,
            **freq_result
        }
        results['tests'].append(test_result)

        status = "PASS" if abs(freq_result['error_cents']) < 5 else "FAIL"
        print(f"  Source SR {source_sr:5d} Hz: "
              f"measured={freq_result['measured_freq_hz']:.2f} Hz, "
              f"error={freq_result['error_cents']:.2f} cents [{status}]")

        # Plot waveform
        ax_wave = axes[idx, 0]
        samples_to_show = int(5 * playback_sr / base_freq)
        time_ms = np.arange(samples_to_show) / playback_sr * 1000
        ax_wave.plot(time_ms, output[:samples_to_show], linewidth=0.8)
        ax_wave.set_xlabel('Time (ms)')
        ax_wave.set_ylabel('Amplitude')
        ax_wave.set_title(f'Sample @ {source_sr}Hz -> Playback @ {playback_sr}Hz')
        ax_wave.grid(True, alpha=0.3)

        # Plot spectrum
        ax_spec = axes[idx, 1]
        n_fft = len(output)
        fft_freqs = np.fft.rfftfreq(n_fft, 1/playback_sr)
        fft_mag = np.abs(np.fft.rfft(output))
        fft_mag_db = 20 * np.log10(fft_mag + 1e-10)

        ax_spec.plot(fft_freqs, fft_mag_db, linewidth=0.5)
        ax_spec.axvline(base_freq, color='red', linestyle='--', alpha=0.5, label=f'Expected {base_freq}Hz')
        ax_spec.set_xlim(0, 2000)
        ax_spec.set_xlabel('Frequency (Hz)')
        ax_spec.set_ylabel('Magnitude (dB)')
        ax_spec.set_title(f'Spectrum (error={freq_result["error_cents"]:.2f} cents)')
        ax_spec.legend()
        ax_spec.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sampler_sample_rate_conversion.png')
    print(f"\n  Saved: output/sampler_sample_rate_conversion.png")

    with open('output/sampler_sample_rate_conversion.json', 'w') as f:
        json.dump(results, f, indent=2)
    print(f"  Saved: output/sampler_sample_rate_conversion.json")

    return results


# =============================================================================
# Test 4: Loop Accuracy
# =============================================================================

def test_loop_accuracy():
    """Test looped sample playback accuracy and continuity."""
    print("\nTest 4: Loop Accuracy")
    print("=" * 60)

    sr = 48000
    base_freq = 440.0
    sample_duration = 0.1  # Short sample for tight looping
    test_duration = 0.5  # Multiple loop iterations

    results = {
        'sample_rate': sr,
        'base_frequency': base_freq,
        'tests': []
    }

    pitch_ratios = [0.5, 1.0, 1.5, 2.0]

    fig, axes = plt.subplots(len(pitch_ratios), 2, figsize=(16, 4 * len(pitch_ratios)))

    for idx, pitch_ratio in enumerate(pitch_ratios):
        expected_freq = base_freq * pitch_ratio

        if expected_freq >= sr / 2:
            print(f"  Skipping pitch={pitch_ratio}: {expected_freq:.1f} Hz >= Nyquist")
            continue

        host = SamplerTestHost(sr)

        # Create sample with exact integer cycles for clean looping
        num_cycles = 44  # Multiple of common divisors
        sample_length = int(num_cycles * sr / base_freq)
        actual_freq = num_cycles / (sample_length / sr)

        t = np.arange(sample_length) / sr
        sample_data = np.sin(2 * np.pi * actual_freq * t).astype(np.float32)
        host.load_custom_sample("loop_test", sample_data, sample_rate=sr)
        sample_id = host.sample_ids["loop_test"]

        host.create_sampler_program(sample_id, pitch=pitch_ratio, loop=True)
        output = host.run_looped(test_duration)

        # Calculate expected loop length at this pitch
        loop_length = sample_length / pitch_ratio

        # Analyze loop continuity
        loop_result = analyze_loop_continuity(output, int(loop_length), num_crossings=10)

        # Measure frequency
        freq_result = measure_frequency(output, sr, expected_freq)

        test_result = {
            'pitch_ratio': pitch_ratio,
            'expected_freq_hz': expected_freq,
            'sample_length': sample_length,
            'expected_loop_length': loop_length,
            **freq_result,
            **loop_result
        }
        results['tests'].append(test_result)

        print(f"  Pitch {pitch_ratio:.2f}x: "
              f"freq_error={freq_result['error_cents']:.2f} cents, "
              f"avg_discontinuity={loop_result['average_discontinuity']:.6f}, "
              f"max_discontinuity={loop_result['max_discontinuity']:.6f}")

        # Plot multiple loops
        ax_wave = axes[idx, 0]
        loops_to_show = 3
        samples_to_show = min(int(loops_to_show * loop_length), len(output))
        time_ms = np.arange(samples_to_show) / sr * 1000
        ax_wave.plot(time_ms, output[:samples_to_show], linewidth=0.8)

        # Mark loop boundaries
        for i in range(loops_to_show + 1):
            ax_wave.axvline(i * loop_length / sr * 1000, color='red', linestyle='--', alpha=0.5)

        ax_wave.set_xlabel('Time (ms)')
        ax_wave.set_ylabel('Amplitude')
        ax_wave.set_title(f'Looped Playback @ pitch={pitch_ratio}x')
        ax_wave.grid(True, alpha=0.3)

        # Plot discontinuity analysis
        ax_disc = axes[idx, 1]
        crossings = loop_result['crossings']
        crossing_nums = [c['crossing_number'] for c in crossings]
        discontinuities = [c['discontinuity'] for c in crossings]

        ax_disc.bar(crossing_nums, discontinuities, color='coral')
        ax_disc.axhline(0.01, color='red', linestyle='--', alpha=0.5, label='0.01 threshold')
        ax_disc.set_xlabel('Loop Crossing #')
        ax_disc.set_ylabel('Discontinuity')
        ax_disc.set_title(f'Loop Discontinuity (avg={loop_result["average_discontinuity"]:.6f})')
        ax_disc.legend()
        ax_disc.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sampler_loop_accuracy.png')
    print(f"\n  Saved: output/sampler_loop_accuracy.png")

    with open('output/sampler_loop_accuracy.json', 'w') as f:
        json.dump(results, f, indent=2)
    print(f"  Saved: output/sampler_loop_accuracy.json")

    # Save WAV examples for each pitch ratio
    print("\n  Saving WAV examples:")
    for pitch_ratio in pitch_ratios:
        host = SamplerTestHost(sr)
        num_cycles = 44
        sample_length = int(num_cycles * sr / base_freq)
        t = np.arange(sample_length) / sr
        sample_data = np.sin(2 * np.pi * base_freq * t).astype(np.float32)
        host.load_custom_sample("loop_wav", sample_data, sample_rate=sr)
        sample_id = host.sample_ids["loop_wav"]
        host.create_sampler_program(sample_id, pitch=pitch_ratio, loop=True)
        output = host.run_looped(2.0)  # 2 seconds of looped audio
        save_wav(f"loop_pitch_{pitch_ratio:.1f}x.wav", output, sr)

    return results


# =============================================================================
# Test 5: High Frequency Aliasing
# =============================================================================

def test_high_frequency_aliasing():
    """Test aliasing artifacts when pitch-shifting high-frequency content."""
    print("\nTest 5: High Frequency Aliasing")
    print("=" * 60)

    sr = 48000
    nyquist = sr / 2
    test_duration = 0.5

    # Test with samples at various frequencies
    test_freqs = [2000, 4000, 8000, 12000]
    pitch_ratios = [1.0, 1.5, 2.0]

    results = {
        'sample_rate': sr,
        'nyquist': nyquist,
        'tests': []
    }

    fig, axes = plt.subplots(len(test_freqs), len(pitch_ratios), figsize=(6 * len(pitch_ratios), 4 * len(test_freqs)))

    for row_idx, base_freq in enumerate(test_freqs):
        for col_idx, pitch_ratio in enumerate(pitch_ratios):
            target_freq = base_freq * pitch_ratio

            host = SamplerTestHost(sr)
            host.load_sine_sample("test_sine", base_freq, 1.0)
            sample_id = host.sample_ids["test_sine"]

            host.create_sampler_program(sample_id, pitch=pitch_ratio, loop=False)
            output = host.run(test_duration)

            # Analyze spectrum
            n_fft = len(output)
            fft_freqs = np.fft.rfftfreq(n_fft, 1/sr)
            fft_mag = np.abs(np.fft.rfft(output))
            fft_mag_db = 20 * np.log10(fft_mag + 1e-10)

            # Find aliased components
            if target_freq >= nyquist:
                aliased_freq = target_freq
                while aliased_freq >= nyquist:
                    aliased_freq = 2 * nyquist - aliased_freq
                    if aliased_freq < 0:
                        aliased_freq = abs(aliased_freq)
            else:
                aliased_freq = target_freq

            # Measure actual peak
            peak_idx = np.argmax(fft_mag_db[10:]) + 10
            measured_freq = fft_freqs[peak_idx]

            test_result = {
                'base_freq_hz': base_freq,
                'pitch_ratio': pitch_ratio,
                'target_freq_hz': target_freq,
                'expected_aliased_freq_hz': aliased_freq,
                'measured_freq_hz': float(measured_freq),
                'above_nyquist': target_freq >= nyquist
            }
            results['tests'].append(test_result)

            status = "ALIASED" if target_freq >= nyquist else "OK"
            print(f"  {base_freq}Hz x {pitch_ratio}: target={target_freq:.0f}Hz, "
                  f"measured={measured_freq:.1f}Hz [{status}]")

            # Plot
            ax = axes[row_idx, col_idx]
            ax.plot(fft_freqs, fft_mag_db, linewidth=0.5)
            ax.axvline(target_freq, color='green', linestyle='--', alpha=0.5,
                      label=f'Target {target_freq:.0f}Hz')
            ax.axvline(nyquist, color='orange', linewidth=2, label=f'Nyquist {nyquist:.0f}Hz')

            if target_freq >= nyquist:
                ax.axvline(aliased_freq, color='red', linestyle=':',
                          label=f'Aliased {aliased_freq:.0f}Hz')

            ax.set_xlim(0, nyquist)
            ax.set_ylim(-100, fft_mag_db.max() + 10)
            ax.set_xlabel('Frequency (Hz)')
            ax.set_ylabel('Magnitude (dB)')
            ax.set_title(f'{base_freq}Hz x {pitch_ratio} = {target_freq}Hz')
            ax.legend(fontsize=8)
            ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sampler_high_freq_aliasing.png')
    print(f"\n  Saved: output/sampler_high_freq_aliasing.png")

    with open('output/sampler_high_freq_aliasing.json', 'w') as f:
        json.dump(results, f, indent=2)
    print(f"  Saved: output/sampler_high_freq_aliasing.json")

    return results


# =============================================================================
# Test 6: Sample-Perfect Loop Analysis
# =============================================================================

def test_sample_perfect_loop():
    """Detailed analysis of loop point discontinuities."""
    print("\nTest 6: Sample-Perfect Loop Analysis")
    print("=" * 60)

    sr = 48000

    # Create a perfectly loopable sample: exact integer cycles
    # Use a frequency that divides evenly into the sample length
    sample_frames = 4800  # 0.1 seconds at 48kHz
    cycles = 44  # 44 cycles in 4800 samples = exactly 440 Hz
    freq = cycles * sr / sample_frames  # Should be exactly 440 Hz

    t = np.arange(sample_frames) / sr
    # This should loop perfectly because sample[0] == sample[N] for a complete cycle
    sample_data = np.sin(2 * np.pi * freq * t).astype(np.float32)

    print(f"  Sample: {sample_frames} frames, {cycles} cycles, {freq:.2f} Hz")
    print(f"  First sample: {sample_data[0]:.10f}")
    print(f"  Last sample:  {sample_data[-1]:.10f}")
    print(f"  Expected next (wrap to 0): {sample_data[0]:.10f}")

    # The discontinuity at the loop point
    # When position wraps from frame N-1 + frac to 0 + frac
    # The interpolation should smoothly transition

    # Calculate what the interpolation SHOULD give at various sub-sample positions
    # near the loop boundary
    positions = np.linspace(sample_frames - 2, sample_frames + 2, 100)
    expected_values = []
    for pos in positions:
        wrapped_pos = pos % sample_frames
        # Perfect interpolation would give:
        frame0 = int(wrapped_pos)
        frame1 = (frame0 + 1) % sample_frames  # Wrap to 0!
        frac = wrapped_pos - frame0
        expected = sample_data[frame0] * (1 - frac) + sample_data[frame1] * frac
        expected_values.append(expected)

    # Now run through the sampler and see what we actually get
    host = SamplerTestHost(sr)
    host.load_custom_sample("loop_test", sample_data, sample_rate=sr)
    sample_id = host.sample_ids["loop_test"]

    # Test at pitch 1.0 (no interpolation between samples needed for integer positions)
    host.create_sampler_program(sample_id, pitch=1.0, loop=True)
    output = host.run_looped(0.5)  # 5 loops

    # Find loop boundaries and measure discontinuity
    loop_length = sample_frames
    num_loops = len(output) // loop_length

    discontinuities = []
    for i in range(1, num_loops):
        boundary_idx = i * loop_length
        if boundary_idx < len(output):
            # Sample before boundary
            before = output[boundary_idx - 1]
            # Sample at boundary (should be smooth continuation)
            at_boundary = output[boundary_idx]
            # Expected value (continuing the sine wave)
            expected = sample_data[0]  # First sample of loop

            disc = abs(at_boundary - expected)
            discontinuities.append({
                'loop': i,
                'boundary_idx': boundary_idx,
                'before': float(before),
                'at_boundary': float(at_boundary),
                'expected': float(expected),
                'discontinuity': float(disc)
            })

    avg_disc = np.mean([d['discontinuity'] for d in discontinuities])
    max_disc = max([d['discontinuity'] for d in discontinuities])

    print(f"\n  Loop boundary analysis (pitch=1.0):")
    print(f"    Average discontinuity: {avg_disc:.10f}")
    print(f"    Max discontinuity: {max_disc:.10f}")

    if avg_disc < 1e-6:
        print(f"    Status: PASS (sample-perfect)")
    else:
        print(f"    Status: FAIL (discontinuity detected)")

    # Now test with fractional pitch (requires interpolation)
    test_pitches = [0.5, 0.99, 1.01, 1.5, 2.0]
    results = {'pitch_1.0': {
        'avg_discontinuity': avg_disc,
        'max_discontinuity': max_disc,
        'discontinuities': discontinuities[:5]
    }}

    for pitch in test_pitches:
        host = SamplerTestHost(sr)
        host.load_custom_sample("loop_test", sample_data, sample_rate=sr)
        sample_id = host.sample_ids["loop_test"]
        host.create_sampler_program(sample_id, pitch=pitch, loop=True)
        output = host.run_looped(0.5)

        # For fractional pitch, loop length in output samples changes
        output_loop_length = sample_frames / pitch

        # Measure discontinuity at theoretical loop points
        disc_list = []
        for i in range(1, 5):
            boundary = i * output_loop_length
            idx = int(boundary)
            if idx < len(output) - 1:
                # Check for discontinuity in the signal derivative
                # A click would show as a sudden jump
                window = 10
                if idx > window and idx < len(output) - window:
                    before_slope = np.diff(output[idx-window:idx]).mean()
                    after_slope = np.diff(output[idx:idx+window]).mean()
                    slope_disc = abs(after_slope - before_slope)
                    disc_list.append(slope_disc)

        avg_slope_disc = np.mean(disc_list) if disc_list else 0

        results[f'pitch_{pitch}'] = {
            'avg_slope_discontinuity': float(avg_slope_disc),
            'output_loop_length': output_loop_length
        }

        print(f"    pitch={pitch}: avg_slope_discontinuity={avg_slope_disc:.10f}")

    # Save results
    with open('output/sampler_loop_perfect.json', 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\n  Saved: output/sampler_loop_perfect.json")

    # Visualization
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Plot 1: Waveform around loop boundary (pitch=1.0)
    ax1 = axes[0, 0]
    host = SamplerTestHost(sr)
    host.load_custom_sample("loop_test", sample_data, sample_rate=sr)
    sample_id = host.sample_ids["loop_test"]
    host.create_sampler_program(sample_id, pitch=1.0, loop=True)
    output = host.run_looped(0.3)

    # Show 200 samples around the first loop boundary
    boundary = sample_frames
    start = max(0, boundary - 100)
    end = min(len(output), boundary + 100)
    ax1.plot(range(start, end), output[start:end], 'b-', linewidth=0.8, label='Output')
    ax1.axvline(boundary, color='red', linestyle='--', label='Loop boundary')
    ax1.set_xlabel('Sample')
    ax1.set_ylabel('Amplitude')
    ax1.set_title('Loop Boundary (pitch=1.0)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Plot 2: Discontinuity zoomed in
    ax2 = axes[0, 1]
    zoom_start = boundary - 5
    zoom_end = boundary + 5
    ax2.plot(range(zoom_start, zoom_end), output[zoom_start:zoom_end], 'b-o', linewidth=0.8)
    ax2.axvline(boundary, color='red', linestyle='--')
    ax2.set_xlabel('Sample')
    ax2.set_ylabel('Amplitude')
    ax2.set_title('Zoomed Loop Boundary')
    ax2.grid(True, alpha=0.3)

    # Plot 3: pitch=1.5 around boundary
    ax3 = axes[1, 0]
    host = SamplerTestHost(sr)
    host.load_custom_sample("loop_test", sample_data, sample_rate=sr)
    sample_id = host.sample_ids["loop_test"]
    host.create_sampler_program(sample_id, pitch=1.5, loop=True)
    output15 = host.run_looped(0.3)

    boundary15 = int(sample_frames / 1.5)
    start = max(0, boundary15 - 100)
    end = min(len(output15), boundary15 + 100)
    ax3.plot(range(start, end), output15[start:end], 'b-', linewidth=0.8)
    ax3.axvline(boundary15, color='red', linestyle='--', label='Loop boundary')
    ax3.set_xlabel('Sample')
    ax3.set_ylabel('Amplitude')
    ax3.set_title('Loop Boundary (pitch=1.5)')
    ax3.legend()
    ax3.grid(True, alpha=0.3)

    # Plot 4: FFT showing any artifacts from looping
    ax4 = axes[1, 1]
    n_fft = len(output)
    fft_freqs = np.fft.rfftfreq(n_fft, 1/sr)
    fft_mag = np.abs(np.fft.rfft(output))
    fft_mag_db = 20 * np.log10(fft_mag + 1e-10)

    ax4.plot(fft_freqs, fft_mag_db, linewidth=0.5)
    ax4.axvline(freq, color='red', linestyle='--', alpha=0.5, label=f'Fundamental {freq}Hz')
    ax4.set_xlim(0, 5000)
    ax4.set_xlabel('Frequency (Hz)')
    ax4.set_ylabel('Magnitude (dB)')
    ax4.set_title('Spectrum (looking for loop artifacts)')
    ax4.legend()
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sampler_loop_perfect.png')
    print(f"  Saved: output/sampler_loop_perfect.png")

    # Save WAV examples comparing different pitches
    print("\n  Saving WAV examples:")
    for pitch in [1.0, 1.5, 2.0]:
        host = SamplerTestHost(sr)
        host.load_custom_sample("perfect_loop", sample_data, sample_rate=sr)
        sample_id = host.sample_ids["perfect_loop"]
        host.create_sampler_program(sample_id, pitch=pitch, loop=True)
        output = host.run_looped(2.0)  # 2 seconds
        save_wav(f"perfect_loop_pitch_{pitch:.1f}x.wav", output, sr)

    return results


# =============================================================================
# Test 7: Sequencer-Triggered Sample Timing
# =============================================================================

def test_sequencer_timing():
    """Test timing accuracy when samples are triggered by sequencer."""
    print("\nTest 7: Sequencer-Triggered Sample Timing")
    print("=" * 60)

    sr = 48000
    bpm = 120.0
    samples_per_beat = sr * 60.0 / bpm  # 24000 samples per beat

    print(f"  Sample rate: {sr} Hz")
    print(f"  BPM: {bpm}")
    print(f"  Samples per beat: {samples_per_beat}")

    # Create a click sample that's long enough to survive the 5-sample attack envelope
    # The sampler has a 5-sample fade-in for anti-click, so our pulse must be >5 samples
    click_length = 100
    click = np.zeros(click_length, dtype=np.float32)
    click[0:20] = 1.0  # 20-sample pulse (survives 5-sample fade, gives clear peak)

    # Test: trigger sample every beat and measure timing
    # We'll manually generate triggers at exact beat boundaries

    host = SamplerTestHost(sr)
    host.vm.set_bpm(bpm)
    host.load_custom_sample("click", click, sample_rate=sr)
    sample_id = host.sample_ids["click"]

    # Build a program that triggers on beat boundaries
    # For this test, we manually create triggers at expected positions
    host.program = []

    # Set up parameters
    host.vm.set_param("pitch", 1.0)
    host.vm.set_param("sample_id", float(sample_id))

    # Get pitch -> buf 1
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("pitch"))
    )
    # Get sample_id -> buf 2
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("sample_id"))
    )
    # Sampler: trigger (buf 0), pitch (buf 1), sample_id (buf 2) -> buf 10
    host.program.append(
        cedar.Instruction.make_ternary(
            cedar.Opcode.SAMPLE_PLAY, 10, 0, 1, 2, cedar.hash("sampler_timing")
        )
    )
    # Output
    host.program.append(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
    )

    host.vm.load_program(host.program)

    # Run for 8 beats (4 loops of quarter notes)
    duration = 8 * samples_per_beat / sr
    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)

    output = []
    trigger_positions = []
    expected_positions = [int(i * samples_per_beat) for i in range(8)]

    global_sample = 0
    for block_idx in range(num_blocks):
        trigger = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)

        # Place triggers at beat boundaries
        for i in range(cedar.BLOCK_SIZE):
            sample_pos = global_sample + i
            if sample_pos in expected_positions:
                trigger[i] = 1.0
                trigger_positions.append(sample_pos)

        host.vm.set_buffer(0, trigger)
        left, right = host.vm.process()
        output.append(left)
        global_sample += cedar.BLOCK_SIZE

    output = np.concatenate(output)

    # Find actual click positions in output (where amplitude spikes)
    detected_positions = []
    threshold = 0.5
    in_click = False
    for i, val in enumerate(output):
        if val > threshold and not in_click:
            detected_positions.append(i)
            in_click = True
        elif val < threshold:
            in_click = False

    # Calculate timing errors
    timing_errors = []
    for expected, detected in zip(expected_positions[:len(detected_positions)], detected_positions):
        error_samples = detected - expected
        error_ms = error_samples / sr * 1000
        timing_errors.append({
            'expected_sample': expected,
            'detected_sample': detected,
            'error_samples': error_samples,
            'error_ms': float(error_ms)
        })

    avg_error_ms = np.mean([abs(e['error_ms']) for e in timing_errors]) if timing_errors else 0
    max_error_ms = max([abs(e['error_ms']) for e in timing_errors]) if timing_errors else 0

    print(f"\n  Timing analysis:")
    print(f"    Expected triggers: {len(expected_positions)}")
    print(f"    Detected triggers: {len(detected_positions)}")
    print(f"    Average error: {avg_error_ms:.4f} ms")
    print(f"    Max error: {max_error_ms:.4f} ms")

    if avg_error_ms < 0.1:
        print(f"    Status: PASS (sample-accurate)")
    else:
        print(f"    Status: FAIL (timing drift detected)")

    for i, err in enumerate(timing_errors[:5]):
        print(f"    Beat {i}: expected={err['expected_sample']}, "
              f"detected={err['detected_sample']}, error={err['error_ms']:.4f}ms")

    results = {
        'sample_rate': sr,
        'bpm': bpm,
        'samples_per_beat': samples_per_beat,
        'expected_triggers': len(expected_positions),
        'detected_triggers': len(detected_positions),
        'avg_error_ms': float(avg_error_ms),
        'max_error_ms': float(max_error_ms),
        'timing_errors': timing_errors
    }

    with open('output/sampler_sequencer_timing.json', 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\n  Saved: output/sampler_sequencer_timing.json")

    # Visualization
    fig, axes = plt.subplots(2, 1, figsize=(14, 8))

    # Plot 1: Full output with trigger markers
    ax1 = axes[0]
    time_ms = np.arange(len(output)) / sr * 1000
    ax1.plot(time_ms, output, linewidth=0.5)
    for pos in expected_positions[:len(detected_positions)]:
        ax1.axvline(pos / sr * 1000, color='red', alpha=0.3, linewidth=0.5)
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title('Sequencer-Triggered Sample Playback')
    ax1.grid(True, alpha=0.3)

    # Plot 2: Timing errors
    ax2 = axes[1]
    beat_nums = list(range(len(timing_errors)))
    errors_ms = [e['error_ms'] for e in timing_errors]
    ax2.bar(beat_nums, errors_ms, color='coral')
    ax2.axhline(0, color='black', linewidth=0.5)
    ax2.set_xlabel('Beat Number')
    ax2.set_ylabel('Timing Error (ms)')
    ax2.set_title(f'Timing Error per Beat (avg={avg_error_ms:.4f}ms)')
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sampler_sequencer_timing.png')
    print(f"  Saved: output/sampler_sequencer_timing.png")

    # Save WAV example
    print("\n  Saving WAV example:")
    save_wav("sequencer_timing_8beats.wav", output, sr)

    return results


# =============================================================================
# Test 8: Long-Term Timing Drift
# =============================================================================

def test_timing_drift():
    """Test for cumulative timing drift over many loops."""
    print("\nTest 8: Long-Term Timing Drift")
    print("=" * 60)

    sr = 48000
    bpm = 120.0
    samples_per_beat = sr * 60.0 / bpm

    # Run for many loops to detect drift
    num_beats = 64  # 16 bars
    duration = num_beats * samples_per_beat / sr

    print(f"  Testing {num_beats} beats ({num_beats/4} bars)")

    # Create click sample (must survive 5-sample attack envelope)
    click_length = 100
    click = np.zeros(click_length, dtype=np.float32)
    click[0:20] = 1.0  # 20-sample pulse

    host = SamplerTestHost(sr)
    host.vm.set_bpm(bpm)
    host.load_custom_sample("click", click, sample_rate=sr)
    sample_id = host.sample_ids["click"]

    # Build program
    host.program = []
    host.vm.set_param("pitch", 1.0)
    host.vm.set_param("sample_id", float(sample_id))

    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("pitch"))
    )
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("sample_id"))
    )
    host.program.append(
        cedar.Instruction.make_ternary(
            cedar.Opcode.SAMPLE_PLAY, 10, 0, 1, 2, cedar.hash("drift_test")
        )
    )
    host.program.append(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
    )

    host.vm.load_program(host.program)

    num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
    output = []
    expected_positions = [int(i * samples_per_beat) for i in range(num_beats)]

    global_sample = 0
    for block_idx in range(num_blocks):
        trigger = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)

        for i in range(cedar.BLOCK_SIZE):
            sample_pos = global_sample + i
            if sample_pos in expected_positions:
                trigger[i] = 1.0

        host.vm.set_buffer(0, trigger)
        left, right = host.vm.process()
        output.append(left)
        global_sample += cedar.BLOCK_SIZE

    output = np.concatenate(output)

    # Detect clicks
    detected_positions = []
    threshold = 0.5
    in_click = False
    for i, val in enumerate(output):
        if val > threshold and not in_click:
            detected_positions.append(i)
            in_click = True
        elif val < threshold:
            in_click = False

    # Calculate cumulative drift
    timing_errors = []
    for beat_num, (expected, detected) in enumerate(zip(expected_positions[:len(detected_positions)], detected_positions)):
        error_samples = detected - expected
        error_ms = error_samples / sr * 1000
        timing_errors.append({
            'beat': beat_num,
            'expected': expected,
            'detected': detected,
            'error_samples': error_samples,
            'error_ms': float(error_ms)
        })

    # Check for drift trend
    if len(timing_errors) > 10:
        early_errors = [e['error_ms'] for e in timing_errors[:10]]
        late_errors = [e['error_ms'] for e in timing_errors[-10:]]
        drift_trend = np.mean(late_errors) - np.mean(early_errors)
    else:
        drift_trend = 0

    max_error = max([abs(e['error_ms']) for e in timing_errors]) if timing_errors else 0
    avg_error = np.mean([abs(e['error_ms']) for e in timing_errors]) if timing_errors else 0

    print(f"\n  Drift analysis over {len(timing_errors)} beats:")
    print(f"    Average error: {avg_error:.4f} ms")
    print(f"    Max error: {max_error:.4f} ms")
    print(f"    Drift trend (late - early): {drift_trend:.4f} ms")

    if abs(drift_trend) < 0.1:
        print(f"    Status: PASS (no significant drift)")
    else:
        print(f"    Status: WARN (drift detected: {drift_trend:.4f}ms over {num_beats} beats)")

    results = {
        'num_beats': num_beats,
        'avg_error_ms': float(avg_error),
        'max_error_ms': float(max_error),
        'drift_trend_ms': float(drift_trend),
        'errors': timing_errors
    }

    with open('output/sampler_timing_drift.json', 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\n  Saved: output/sampler_timing_drift.json")

    # Visualization
    fig, axes = plt.subplots(2, 1, figsize=(14, 8))

    # Plot 1: Timing error over time
    ax1 = axes[0]
    beats = [e['beat'] for e in timing_errors]
    errors = [e['error_ms'] for e in timing_errors]
    ax1.plot(beats, errors, 'b-', linewidth=0.8)
    ax1.axhline(0, color='black', linewidth=0.5)

    # Add trend line
    if len(beats) > 2:
        z = np.polyfit(beats, errors, 1)
        p = np.poly1d(z)
        ax1.plot(beats, p(beats), 'r--', label=f'Trend: {z[0]*1000:.4f} ms/beat')
        ax1.legend()

    ax1.set_xlabel('Beat Number')
    ax1.set_ylabel('Timing Error (ms)')
    ax1.set_title('Timing Error Over Time (Looking for Drift)')
    ax1.grid(True, alpha=0.3)

    # Plot 2: Histogram of errors
    ax2 = axes[1]
    ax2.hist(errors, bins=30, color='steelblue', edgecolor='navy')
    ax2.axvline(0, color='red', linewidth=2)
    ax2.set_xlabel('Timing Error (ms)')
    ax2.set_ylabel('Count')
    ax2.set_title(f'Distribution of Timing Errors (mean={avg_error:.4f}ms)')
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/sampler_timing_drift.png')
    print(f"  Saved: output/sampler_timing_drift.png")

    # Save WAV example
    print("\n  Saving WAV example:")
    save_wav("timing_drift_64beats.wav", output, sr)

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    # Change to script directory so all paths work correctly
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    os.makedirs('output', exist_ok=True)

    print("Cedar Sample Player Accuracy Tests")
    print("=" * 60)
    print()

    test_pitch_accuracy()
    test_interpolation_quality()
    test_sample_rate_conversion()
    test_loop_accuracy()
    # test_high_frequency_aliasing()  # Skip - takes too long
    test_sample_perfect_loop()
    test_sequencer_timing()
    test_timing_drift()

    print()
    print("=" * 60)
    print("All tests complete. Results saved to output/ and output/wav/")
