"""
Oscillator Testing Examples (Cedar Engine)
===========================================
Tests C++ oscillator implementations via Pybind11 bindings.
"""

import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar  # The compiled C++ module
from visualize import create_dsp_report, plot_spectrum, save_figure

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

    for ax, osc_type in zip(axes.flat, types):
        host = CedarTestHost(sr)
        host.add_osc(osc_type, freq)
        signal = host.run(0.02)  # 20ms

        time_ms = np.arange(len(signal)) / sr * 1000
        ax.plot(time_ms, signal, linewidth=1)
        ax.set_title(osc_type.capitalize())
        ax.set_ylim(-1.2, 1.2)
        ax.grid(True, alpha=0.3)

    save_figure(fig, 'output/cedar_waveforms.png')
    print("  Saved: output/cedar_waveforms.png")

# =============================================================================
# Test 2: Aliasing / Spectrum Analysis
# =============================================================================

def test_aliasing():
    print("\nTest 2: Aliasing Analysis (Sawtooth)")
    sr = 48000
    freq = 5000.0 # High freq to provoke aliasing

    host = CedarTestHost(sr)
    host.add_osc('saw', freq)
    signal = host.run(1.0)

    # Analyze
    freqs = np.fft.rfftfreq(len(signal), 1/sr)
    mag = 20 * np.log10(np.abs(np.fft.rfft(signal)) + 1e-10)

    fig = plot_spectrum(freqs, mag, title=f"Cedar Sawtooth @ {freq}Hz")
    save_figure(fig, 'output/cedar_aliasing.png')
    print("  Saved: output/cedar_aliasing.png")

if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)
    test_waveform()
    test_aliasing()