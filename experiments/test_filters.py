import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar
from cedar_testing import CedarTestHost
from visualize import plot_frequency_response, save_figure
import scipy.signal

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
    state_id = cedar.hash(f"{filter_type_name}_state")
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

if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)
    test_svf_comparison()
    test_moog_resonance()