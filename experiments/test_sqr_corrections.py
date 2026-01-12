"""Test square wave PolyBLEP corrections to find asymmetry source."""

import numpy as np

def poly_blep(t, dt):
    """Python implementation of PolyBLEP function."""
    dt = abs(dt)
    if dt < 1e-8:
        return 0.0

    if t < dt:
        t /= dt
        return t + t - t * t - 1.0
    elif t > 1.0 - dt:
        t = (t - 1.0) / dt
        return t * t + t + t + 1.0
    return 0.0

# Simulate square wave generation
sr = 48000
freq = 440.0
dt = freq / sr
num_samples = int(sr / freq * 2)  # 2 cycles

phase = 0.0
samples = []
corrections_rising = []
corrections_falling = []

for i in range(num_samples):
    # Naive square
    value = 1.0 if phase < 0.5 else -1.0

    # Rising edge correction
    corr_rising = poly_blep(phase, dt)

    # Falling edge correction - using the current implementation
    if phase < 0.5:
        t = phase + 0.5
    else:
        t = phase - 0.5
    corr_falling = poly_blep(t, dt)

    value += corr_rising
    value -= corr_falling

    samples.append(value)
    corrections_rising.append(corr_rising)
    corrections_falling.append(corr_falling)

    # Advance phase
    phase += dt
    if phase >= 1.0:
        phase -= 1.0

samples = np.array(samples)
corrections_rising = np.array(corrections_rising)
corrections_falling = np.array(corrections_falling)

print(f"Mean: {np.mean(samples):.10f}")
print(f"Min: {np.min(samples):.6f}, Max: {np.max(samples):.6f}")
print(f"\nSum of rising edge corrections: {np.sum(corrections_rising):.10f}")
print(f"Sum of falling edge corrections: {np.sum(corrections_falling):.10f}")
print(f"Net correction: {np.sum(corrections_rising) - np.sum(corrections_falling):.10f}")
print(f"\nThis net correction should be zero for perfect symmetry")

# Count non-zero corrections
rising_nonzero = np.sum(np.abs(corrections_rising) > 1e-10)
falling_nonzero = np.sum(np.abs(corrections_falling) > 1e-10)
print(f"\nNon-zero rising corrections: {rising_nonzero}")
print(f"Non-zero falling corrections: {falling_nonzero}")
