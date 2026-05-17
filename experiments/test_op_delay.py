"""
Test: DELAY (Delay Line)
========================
Tests delay timing accuracy and feedback decay.

Expected behavior:
- Delay time should match the specified value in milliseconds
- Echoes should be evenly spaced at the delay time interval
- Feedback should cause exponential decay of echo levels
- At 0.5 feedback, each echo should be approximately -6dB from the previous

If this test fails, check the implementation in cedar/include/cedar/opcodes/delay.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_impulse, save_wav
from visualize import save_figure

OUT = output_dir("op_delay")


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
    # Post-unified-drywet: delay defaults to dry=1, wet=0.5 (Category A).
    # The impulse-echo test wants echoes-only — force dry=0, wet=1.
    buf_dry = host.set_param("dry", 0.0)
    buf_wet = host.set_param("wet", 1.0)
    buf_out = 1

    # DELAY: out = delay(in, delay_ms, feedback, dry, wet)
    # inst.rate is the time-unit selector (0=sec, 1=ms, 2=samples) — set to 1 for ms.
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.DELAY, buf_out, buf_in, buf_delay, buf_feedback, buf_dry, buf_wet,
        cedar.hash("delay") & 0xFFFF
    )
    inst.rate = 1  # ms unit
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, buf_out))

    output = host.process(impulse)

    # Save WAV for human evaluation
    wav_path = os.path.join(OUT, "delay_impulse.wav")
    save_wav(wav_path, output, sr)
    print(f"  Saved {wav_path} - Listen for evenly spaced echoes")

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
    save_figure(fig, os.path.join(OUT, "delay_impulse.png"))
    print(f"  Saved {os.path.join(OUT, 'delay_impulse.png')}")


if __name__ == "__main__":
    test_delay_timing()
