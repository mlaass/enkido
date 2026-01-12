"""
Oscillator Testing Examples (Cedar Engine)
===========================================
Tests C++ oscillator implementations via Pybind11 bindings.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import cedar_core as cedar  # The compiled C++ module
from visualize import create_dsp_report, plot_spectrum, save_figure

def analyze_phase_cycles(signal, sample_rate, expected_freq, num_cycles=10):
    """
    Analyze the actual cycle lengths by detecting zero crossings or peaks.
    
    Returns:
        dict with cycle information including measured periods and frequencies
    """
    cycles = []
    
    # Find zero crossings (positive-going)
    zero_crossings = []
    for i in range(len(signal) - 1):
        if signal[i] <= 0 and signal[i + 1] > 0:
            # Interpolate exact zero crossing position
            t = -signal[i] / (signal[i + 1] - signal[i])
            zero_crossings.append(i + t)
    
    # Calculate cycle lengths from zero crossings
    for i in range(min(num_cycles, len(zero_crossings) - 1)):
        cycle_samples = zero_crossings[i + 1] - zero_crossings[i]
        cycle_freq = sample_rate / cycle_samples
        cycles.append({
            'cycle_number': i + 1,
            'start_sample': float(zero_crossings[i]),
            'length_samples': float(cycle_samples),
            'measured_freq': float(cycle_freq),
            'freq_error_hz': float(cycle_freq - expected_freq),
            'freq_error_percent': float(((cycle_freq - expected_freq) / expected_freq) * 100)
        })
    
    return {
        'expected_freq': expected_freq,
        'num_zero_crossings': len(zero_crossings),
        'cycles': cycles,
        'avg_freq': sum(c['measured_freq'] for c in cycles) / len(cycles) if cycles else 0,
        'avg_error_percent': sum(c['freq_error_percent'] for c in cycles) / len(cycles) if cycles else 0
    }

def get_theoretical_harmonics(waveform_type, fundamental_freq, sample_rate, num_harmonics=20):
    """
    Calculate theoretical harmonic amplitudes for ideal waveforms.
    
    Args:
        waveform_type: 'sine', 'saw', 'sqr', 'tri'
        fundamental_freq: Fundamental frequency in Hz
        sample_rate: Sample rate in Hz
        num_harmonics: Number of harmonics to calculate
    
    Returns:
        freqs: Array of harmonic frequencies
        amps: Array of harmonic amplitudes (linear scale)
    """
    nyquist = sample_rate / 2
    freqs = []
    amps = []
    
    if waveform_type == 'sine':
        # Pure sine: only fundamental
        freqs = [fundamental_freq]
        amps = [1.0]
    
    elif waveform_type == 'saw':
        # Sawtooth: all harmonics, amplitude = 1/n
        for n in range(1, num_harmonics + 1):
            freq = fundamental_freq * n
            if freq < nyquist:
                freqs.append(freq)
                amps.append(1.0 / n)
    
    elif waveform_type == 'sqr':
        # Square: odd harmonics only, amplitude = 1/n
        for n in range(1, num_harmonics * 2, 2):  # 1, 3, 5, 7...
            freq = fundamental_freq * n
            if freq < nyquist:
                freqs.append(freq)
                amps.append(1.0 / n)
    
    elif waveform_type == 'tri':
        # Triangle: odd harmonics only, amplitude = 1/n^2, alternating phase
        for n in range(1, num_harmonics * 2, 2):  # 1, 3, 5, 7...
            freq = fundamental_freq * n
            if freq < nyquist:
                freqs.append(freq)
                amps.append(1.0 / (n * n))
    
    return np.array(freqs), np.array(amps)

class CedarTestHost:
    """Helper to run Cedar VM tests."""
    def __init__(self, sample_rate=48000):
        self.vm = cedar.VM()
        self.vm.set_sample_rate(sample_rate)
        self.sr = sample_rate
        self.program = []

    def add_osc(self, osc_type, freq, out_idx=0):
        """Add oscillator instruction."""
        # 1. Set frequency parameter
        param_name = f"freq_{len(self.program)}"
        self.vm.set_param(param_name, freq)

        # 2. Get frequency into buffer 0 (temp)
        # Hash the name to get state_id
        freq_hash = cedar.hash(param_name)
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, freq_hash)
        )

        # 3. Run Oscillator (Freq in buf 10 -> Out buf out_idx)
        # Using specific state ID to ensure persistence
        osc_state = cedar.hash(f"osc_{len(self.program)}")

        op_map = {
            'sine': cedar.Opcode.OSC_SIN,
            'tri': cedar.Opcode.OSC_TRI,
            'saw': cedar.Opcode.OSC_SAW,
            'sqr': cedar.Opcode.OSC_SQR
        }

        self.program.append(
            cedar.Instruction.make_unary(op_map[osc_type], out_idx, 10, osc_state)
        )

        # 4. Route to Main Output
        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, out_idx)
        )

    def run(self, duration_sec):
        """Compile and run the program, returning audio."""
        self.vm.load_program(self.program)

        num_blocks = int((duration_sec * self.sr) / cedar.BLOCK_SIZE)
        output = []

        for _ in range(num_blocks):
            left, right = self.vm.process()
            output.append(left)

        return np.concatenate(output)

# =============================================================================
# Test 1: Basic Waveform Visualization
# =============================================================================

def test_waveform():
    print("Test 1: Waveform Visualization")
    sr = 48000
    freq = 440.0

    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    fig.suptitle('Cedar Oscillator Waveforms @ 440 Hz')

    types = ['sine', 'saw', 'sqr', 'tri']
    
    # Collect data for JSON output
    json_data = {
        'sample_rate': sr,
        'frequency': freq,
        'oscillators': {}
    }

    for ax, osc_type in zip(axes.flat, types):
        host = CedarTestHost(sr)
        host.add_osc(osc_type, freq)
        signal = host.run(0.02)  # 20ms

        time_ms = np.arange(len(signal)) / sr * 1000
        ax.plot(time_ms, signal, linewidth=1)
        ax.set_title(osc_type.capitalize())
        ax.set_ylim(-1.2, 1.2)
        ax.grid(True, alpha=0.3)
        
        # Analyze phase cycles
        phase_analysis = analyze_phase_cycles(signal, sr, freq, num_cycles=10)
        
        # Save first 100 samples to JSON for inspection
        json_data['oscillators'][osc_type] = {
            'first_100_samples': signal[:100].tolist(),
            'first_value': float(signal[0]),
            'min_value': float(np.min(signal)),
            'max_value': float(np.max(signal)),
            'mean_value': float(np.mean(signal)),
            'phase_analysis': phase_analysis
        }

    save_figure(fig, 'output/cedar_waveforms.png')
    print("  Saved: output/cedar_waveforms.png")
    
    # Print phase analysis summary
    print("\n  Phase Analysis (First 10 Cycles):")
    print("  " + "="*70)
    for osc_type in types:
        analysis = json_data['oscillators'][osc_type]['phase_analysis']
        print(f"\n  {osc_type.upper()}:")
        print(f"    Expected frequency: {analysis['expected_freq']} Hz")
        print(f"    Average measured:   {analysis['avg_freq']:.2f} Hz")
        print(f"    Average error:      {analysis['avg_error_percent']:.3f}%")
        print(f"    Cycle details:")
        for cycle in analysis['cycles'][:5]:  # Show first 5 cycles
            print(f"      Cycle {cycle['cycle_number']}: {cycle['length_samples']:.2f} samples "
                  f"({cycle['measured_freq']:.2f} Hz, error: {cycle['freq_error_percent']:.2f}%)")
    
    # Save JSON data
    with open('output/oscillator_data.json', 'w') as f:
        json.dump(json_data, f, indent=2)
    print("\n  Saved: output/oscillator_data.json")

# =============================================================================
# Test 2: Comprehensive Frequency Analysis with Theoretical Harmonics
# =============================================================================

def test_frequency_analysis():
    print("\nTest 2: Frequency Analysis with Theoretical Harmonics")
    sr = 48000
    freq = 440.0  # A4
    duration = 1.0
    
    types = ['sine', 'saw', 'sqr', 'tri']
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle(f'Frequency Analysis: Theoretical vs Actual @ {freq} Hz', fontsize=14, fontweight='bold')
    
    for ax, osc_type in zip(axes.flat, types):
        # Generate signal
        host = CedarTestHost(sr)
        host.add_osc(osc_type, freq)
        signal = host.run(duration)
        
        # FFT analysis
        fft_freqs = np.fft.rfftfreq(len(signal), 1/sr)
        fft_mag = np.abs(np.fft.rfft(signal))
        fft_mag_db = 20 * np.log10(fft_mag + 1e-10)
        
        # Get theoretical harmonics
        theo_freqs, theo_amps = get_theoretical_harmonics(osc_type, freq, sr, num_harmonics=30)
        theo_amps_db = 20 * np.log10(theo_amps)
        
        # Plot actual spectrum
        ax.plot(fft_freqs, fft_mag_db, color='steelblue', alpha=0.7, linewidth=0.8, label='Actual')
        
        # Plot theoretical harmonics as stems
        if len(theo_freqs) > 0:
            # Find peak value for normalization reference
            peak_db = np.max(fft_mag_db[fft_freqs < sr/2])
            theo_amps_db_normalized = theo_amps_db + (peak_db - theo_amps_db[0])
            
            markerline, stemlines, baseline = ax.stem(theo_freqs, theo_amps_db_normalized, 
                   linefmt='red', markerfmt='ro', basefmt=' ',
                   label='Theoretical')
            markerline.set_alpha(0.6)
            stemlines.set_alpha(0.6)
        
        ax.set_xlim(0, 15000)
        ax.set_ylim(-120, 10)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.set_title(f'{osc_type.capitalize()} Wave', fontweight='bold')
        ax.grid(True, alpha=0.3, which='both')
        ax.legend(loc='upper right')
        
        # Add vertical lines at harmonic positions for reference
        for i, f in enumerate(theo_freqs[:10]):
            if f < 15000:
                ax.axvline(f, color='gray', alpha=0.2, linewidth=0.5, linestyle='--')
    
    plt.tight_layout()
    save_figure(fig, 'output/cedar_frequency_analysis.png')
    print("  Saved: output/cedar_frequency_analysis.png")

# =============================================================================
# Test 3: Aliasing Analysis at High Frequencies
# =============================================================================

def test_aliasing():
    print("\nTest 3: Aliasing Analysis at High Frequencies")
    sr = 48000
    freq = 8000.0  # High freq to show aliasing effects
    duration = 1.0
    
    types = ['saw', 'sqr', 'tri']
    fig, axes = plt.subplots(1, 3, figsize=(18, 5))
    fig.suptitle(f'Aliasing Analysis @ {freq} Hz (Nyquist = {sr/2} Hz)', fontsize=14, fontweight='bold')
    
    for ax, osc_type in zip(axes.flat, types):
        # Generate signal
        host = CedarTestHost(sr)
        host.add_osc(osc_type, freq)
        signal = host.run(duration)
        
        # FFT analysis
        fft_freqs = np.fft.rfftfreq(len(signal), 1/sr)
        fft_mag = np.abs(np.fft.rfft(signal))
        fft_mag_db = 20 * np.log10(fft_mag + 1e-10)
        
        # Get theoretical harmonics
        theo_freqs, theo_amps = get_theoretical_harmonics(osc_type, freq, sr, num_harmonics=30)
        theo_amps_db = 20 * np.log10(theo_amps)
        
        # Plot actual spectrum
        ax.plot(fft_freqs, fft_mag_db, color='steelblue', alpha=0.7, linewidth=0.8, label='Actual')
        
        # Plot theoretical harmonics
        if len(theo_freqs) > 0:
            peak_db = np.max(fft_mag_db[fft_freqs < sr/2])
            theo_amps_db_normalized = theo_amps_db + (peak_db - theo_amps_db[0])
            markerline, stemlines, baseline = ax.stem(theo_freqs, theo_amps_db_normalized,
                   linefmt='red', markerfmt='ro', basefmt=' ',
                   label='Theoretical (band-limited)')
            markerline.set_alpha(0.6)
            stemlines.set_alpha(0.6)
        
        # Mark Nyquist frequency
        ax.axvline(sr/2, color='orange', linewidth=2, linestyle='--', label='Nyquist', alpha=0.8)
        
        ax.set_xlim(0, sr/2)
        ax.set_ylim(-120, 10)
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Magnitude (dB)')
        ax.set_title(f'{osc_type.capitalize()} Wave', fontweight='bold')
        ax.grid(True, alpha=0.3, which='both')
        ax.legend(loc='upper right')
    
    plt.tight_layout()
    save_figure(fig, 'output/cedar_aliasing.png')
    print("  Saved: output/cedar_aliasing.png")

if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)
    test_waveform()
    test_frequency_analysis()
    test_aliasing()