"""Test MinBLEP square wave oscillator for perfect harmonic purity."""

import numpy as np
import matplotlib.pyplot as plt
import json
import cedar_core as cedar

def test_minblep_comparison():
    """Compare PolyBLEP vs MinBLEP square wave."""
    print("\n=== MinBLEP Square Wave Test ===")
    sr = 48000
    freq = 440.0
    duration = 10.0  # Long duration for high FFT resolution
    
    # Test both oscillator types
    results = {}
    
    for osc_name, opcode in [('PolyBLEP', cedar.Opcode.OSC_SQR), 
                              ('MinBLEP', cedar.Opcode.OSC_SQR_MINBLEP)]:
        print(f"\nTesting {osc_name}...")
        
        # Create VM and program
        vm = cedar.VM()
        vm.set_sample_rate(sr)
        vm.set_param('freq', freq)
        
        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash('freq')),
            cedar.Instruction.make_unary(opcode, 1, 10, cedar.hash('sqr')),
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 1)
        ]
        
        vm.load_program(program)
        
        # Generate signal
        signal = []
        num_blocks = int((duration * sr) / cedar.BLOCK_SIZE)
        for _ in range(num_blocks):
            left, right = vm.process()
            signal.append(left)
        signal = np.concatenate(signal)
        
        # FFT analysis
        n_samples = len(signal)
        fft_freqs = np.fft.rfftfreq(n_samples, 1/sr)
        fft_mag = np.abs(np.fft.rfft(signal))

        # Find fundamental magnitude for normalization
        fund_idx = np.argmin(np.abs(fft_freqs - freq))
        window = 10
        fund_mag = np.max(fft_mag[max(0, fund_idx-window):fund_idx+window+1])

        # Find even harmonic levels RELATIVE to fundamental
        even_harmonics = []
        for n in range(2, 31, 2):  # Even harmonics 2, 4, 6, ..., 30
            harmonic_freq = freq * n
            if harmonic_freq >= sr / 2:
                break

            idx = np.argmin(np.abs(fft_freqs - harmonic_freq))
            start_idx = max(0, idx - window)
            end_idx = min(len(fft_mag), idx + window + 1)
            peak_mag = np.max(fft_mag[start_idx:end_idx])

            # Convert to dB relative to fundamental
            peak_mag = 20 * np.log10(peak_mag / fund_mag + 1e-20)
            
            even_harmonics.append({
                'harmonic': n,
                'freq': harmonic_freq,
                'magnitude_db': float(peak_mag)
            })
        
        # Calculate DC offset
        dc_offset = np.mean(signal)
        
        results[osc_name] = {
            'dc_offset': float(dc_offset),
            'even_harmonics': even_harmonics,
            'avg_even_harmonic_db': float(np.mean([h['magnitude_db'] for h in even_harmonics])),
            'signal_mean': float(np.mean(signal)),
            'signal_min': float(np.min(signal)),
            'signal_max': float(np.max(signal))
        }
        
        print(f"  DC offset: {dc_offset:.10f}")
        print(f"  Average even harmonic level: {results[osc_name]['avg_even_harmonic_db']:.1f} dB")
        print(f"  First 5 even harmonics:")
        for h in even_harmonics[:5]:
            print(f"    H{h['harmonic']}: {h['magnitude_db']:.1f} dB @ {h['freq']:.0f} Hz")
    
    # Save results
    with open('output/minblep_comparison.json', 'w') as f:
        json.dump(results, f, indent=2)
    print("\n  Saved: output/minblep_comparison.json")
    
    # Print comparison
    print("\n=== Comparison ===")
    polyblep_avg = results['PolyBLEP']['avg_even_harmonic_db']
    minblep_avg = results['MinBLEP']['avg_even_harmonic_db']
    improvement = minblep_avg - polyblep_avg
    
    print(f"PolyBLEP avg even harmonic: {polyblep_avg:.1f} dB")
    print(f"MinBLEP avg even harmonic:  {minblep_avg:.1f} dB")
    print(f"Improvement: {improvement:.1f} dB")
    
    if improvement < -20:
        print("✓ MinBLEP provides significant improvement!")
    elif improvement < -10:
        print("✓ MinBLEP provides moderate improvement")
    else:
        print("⚠ MinBLEP improvement is marginal")

if __name__ == "__main__":
    import os
    os.makedirs('output', exist_ok=True)
    test_minblep_comparison()
