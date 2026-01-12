"""Test PolyBLEP function symmetry."""

import numpy as np
import matplotlib.pyplot as plt

def poly_blep(t, dt):
    """Python implementation of PolyBLEP function."""
    dt = abs(dt)
    if dt < 1e-8:
        return 0.0
    
    if t < dt:
        # Just after discontinuity
        t /= dt
        return t + t - t * t - 1.0
    elif t > 1.0 - dt:
        # Just before discontinuity
        t = (t - 1.0) / dt
        return t * t + t + t + 1.0
    return 0.0

# Test symmetry
dt = 0.01
phases = np.linspace(0, 1, 1000)
corrections = [poly_blep(p, dt) for p in phases]

plt.figure(figsize=(12, 6))
plt.plot(phases, corrections)
plt.axhline(0, color='k', linestyle='--', alpha=0.3)
plt.axvline(dt, color='r', linestyle='--', alpha=0.3, label=f'dt={dt}')
plt.axvline(1-dt, color='r', linestyle='--', alpha=0.3)
plt.xlabel('Phase')
plt.ylabel('PolyBLEP Correction')
plt.title('PolyBLEP Function Symmetry Test')
plt.grid(True, alpha=0.3)
plt.legend()
plt.savefig('output/polyblep_symmetry.png')

# Check if function is symmetric
before_disc = [poly_blep(p, dt) for p in np.linspace(1-dt, 1.0, 100)]
after_disc = [poly_blep(p, dt) for p in np.linspace(0, dt, 100)]

print(f"Sum of corrections before discontinuity: {sum(before_disc):.10f}")
print(f"Sum of corrections after discontinuity: {sum(after_disc):.10f}")
print(f"Difference: {sum(before_disc) + sum(after_disc):.10f}")

# The integral of PolyBLEP over the discontinuity should be zero for symmetry
integral_before = np.trapz(before_disc, np.linspace(1-dt, 1.0, 100))
integral_after = np.trapz(after_disc, np.linspace(0, dt, 100))
print(f"\nIntegral before: {integral_before:.10f}")
print(f"Integral after: {integral_after:.10f}")
print(f"Total integral: {integral_before + integral_after:.10f}")
