import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar
from cedar_testing import CedarTestHost
from visualize import plot_spectrogram, plot_transfer_curve, save_figure
import scipy.signal

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
                opcode, 1, buf_in, buf_p1, buf_p2, cedar.hash("dist")
            ))
        elif opcode == cedar.Opcode.DISTORT_TUBE:
            # Tube needs bias
            buf_p2 = host.set_param("bias", 0.1)
            host.load_instruction(cedar.Instruction.make_ternary(
                opcode, 1, buf_in, buf_p1, buf_p2, cedar.hash("dist")
            ))
        else:
            # Standard unary distortion
            host.load_instruction(cedar.Instruction.make_binary(
                opcode, 1, buf_in, buf_p1, cedar.hash("dist")
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

    inst = cedar.Instruction.make_binary(
        cedar.Opcode.EFFECT_PHASER, 1, buf_in, buf_rate, cedar.hash("phaser")
    )
    inst.inputs[2] = buf_depth # Set 3rd input manually for binary constructor limit
    inst.rate = packed_rate

    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1))

    output = host.process(noise)

    # Visualize
    fig = plot_spectrogram(
        np.linspace(0, duration, len(output)),
        np.fft.rfftfreq(1024, 1/sr), # Just dummy for now, helper handles it usually
        # Actually call helper logic here:
        None, title="Phaser Spectral Movement (White Noise Input)"
    )
    # Re-using visualize.py logic manually because helper signature varies
    plt.close(fig)

    # Let's use matplotlib directly for cleaner control here
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
    inst = cedar.Instruction.make_binary(
        cedar.Opcode.REVERB_DATTORRO, 1, buf_in, buf_decay, cedar.hash("verb")
    )
    inst.inputs[2] = buf_predelay
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

if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)
    test_distortion_curves()
    test_phaser_spectrogram()
    test_reverb_decay()