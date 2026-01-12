"""Evaluate the MinBLEP table to see if values are reasonable."""

import numpy as np
import matplotlib.pyplot as plt
import cedar_core as cedar

# Get the MinBLEP table by generating a test signal
vm = cedar.VM()
vm.set_sample_rate(48000)
vm.set_param('freq', 440.0)

program = [
    cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
    cedar.Instruction.make_unary(cedar.Opcode.OSC_SQR_MINBLEP, 1, 10, cedar.hash('sqr')),
    cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
]

vm.load_program(program)

# Generate just a few samples to see the table in action
signal = []
for _ in range(10):
    left, right = vm.process()
    signal.append(left)
signal = np.concatenate(signal)

print("MinBLEP Square Wave - First 128 samples:")
print(f"Min: {np.min(signal):.6f}")
print(f"Max: {np.max(signal):.6f}")
print(f"Mean: {np.mean(signal):.6f}")
print(f"Std: {np.std(signal):.6f}")
print(f"\nFirst 20 values:")
for i in range(20):
    print(f"  [{i:2d}] = {signal[i]:+.6f}")

# Plot first few cycles
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 8))

# Time domain
time_ms = np.arange(len(signal)) / 48000 * 1000
ax1.plot(time_ms, signal, 'b-', linewidth=1, label='MinBLEP output')
ax1.axhline(1.0, color='r', linestyle='--', alpha=0.3, label='Expected levels')
ax1.axhline(-1.0, color='r', linestyle='--', alpha=0.3)
ax1.axhline(0.0, color='k', linestyle=':', alpha=0.3)
ax1.set_xlabel('Time (ms)')
ax1.set_ylabel('Amplitude')
ax1.set_title('MinBLEP Square Wave Output')
ax1.grid(True, alpha=0.3)
ax1.legend()
ax1.set_ylim(-1.5, 1.5)

# Zoom into first discontinuity
ax2.plot(time_ms[:64], signal[:64], 'b.-', linewidth=1, markersize=3)
ax2.axhline(1.0, color='r', linestyle='--', alpha=0.3)
ax2.axhline(-1.0, color='r', linestyle='--', alpha=0.3)
ax2.axhline(0.0, color='k', linestyle=':', alpha=0.3)
ax2.set_xlabel('Time (ms)')
ax2.set_ylabel('Amplitude')
ax2.set_title('First 64 Samples (zoomed)')
ax2.grid(True, alpha=0.3)
ax2.set_ylim(-1.5, 1.5)

plt.tight_layout()
plt.savefig('output/minblep_table_debug.png', dpi=150)
print("\nSaved: output/minblep_table_debug.png")

# Check for extreme values
if np.max(np.abs(signal)) > 2.0:
    print("\n⚠️  WARNING: Signal has extreme values (> 2.0)")
    print("   This indicates the MinBLEP residuals are too large")
    
if np.std(signal) > 1.0:
    print("\n⚠️  WARNING: Signal has high variance")
    print("   Expected std ~0.7 for square wave, got {:.3f}".format(np.std(signal)))
