"""Debug square wave to find even harmonic source."""

import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar

# Generate square wave
sr = 48000
freq = 440.0
duration = 0.1

host = cedar.VM()
host.set_sample_rate(sr)
host.set_param('freq', freq)

program = [
    cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
    cedar.Instruction.make_unary(cedar.Opcode.OSC_SQR, 1, 10, cedar.hash('sqr')),
    cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
]

host.load_program(program)
signal = []
num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
for _ in range(num_blocks):
    left, right = host.process()
    signal.append(left)
signal = np.concatenate(signal)

# Check DC offset
print(f"Mean (DC offset): {np.mean(signal):.10f}")
print(f"Min: {np.min(signal):.6f}, Max: {np.max(signal):.6f}")
print(f"Samples: {len(signal)}")

# Find transitions
positive_samples = np.sum(signal > 0)
negative_samples = np.sum(signal < 0)
print(f"\nPositive samples: {positive_samples}")
print(f"Negative samples: {negative_samples}")
print(f"Duty cycle: {positive_samples / len(signal) * 100:.2f}%")
print(f"Expected: 50%")

# Calculate average positive and negative values
avg_positive = np.mean(signal[signal > 0])
avg_negative = np.mean(signal[signal < 0])
print(f"\nAverage positive value: {avg_positive:.6f}")
print(f"Average negative value: {avg_negative:.6f}")
print(f"Asymmetry: {avg_positive + avg_negative:.6f}")
