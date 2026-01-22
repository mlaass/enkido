"""
Envelope Opcode Quality Tests (Cedar Engine)
=============================================
Tests for ENV_ADSR, ENV_AR, and ENV_FOLLOWER opcodes.
Validates timing accuracy, gate behavior, and signal tracking.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
from scipy.io import wavfile
import cedar_core as cedar
from visualize import save_figure
from utils import rms, ms_to_samples, samples_to_ms


class NumpyEncoder(json.JSONEncoder):
    """JSON encoder that handles numpy types."""
    def default(self, obj):
        if isinstance(obj, np.integer):
            return int(obj)
        if isinstance(obj, np.floating):
            return float(obj)
        if isinstance(obj, np.ndarray):
            return obj.tolist()
        if isinstance(obj, np.bool_):
            return bool(obj)
        return super().default(obj)


def save_wav(filename: str, data: np.ndarray, sample_rate: int = 48000):
    """Save audio data to WAV file."""
    wav_dir = 'output/wav'
    os.makedirs(wav_dir, exist_ok=True)
    filepath = os.path.join(wav_dir, filename)
    data_clipped = np.clip(data, -1.0, 1.0)
    data_int16 = (data_clipped * 32767).astype(np.int16)
    wavfile.write(filepath, sample_rate, data_int16)
    print(f"    Saved: {filepath}")


class EnvelopeTestHost:
    """Helper to run Cedar VM envelope tests."""

    def __init__(self, sample_rate=48000):
        self.vm = cedar.VM()
        self.vm.set_sample_rate(sample_rate)
        self.sr = sample_rate
        self.program = []

    def create_adsr_program(self, attack: float, decay: float, sustain: float,
                            release: float, state_id: int = 1):
        """Create ENV_ADSR program.

        Args:
            attack: Attack time in seconds
            decay: Decay time in seconds
            sustain: Sustain level (0-1)
            release: Release time in seconds
            state_id: State ID for the envelope
        """
        self.program = []

        # Set parameters
        self.vm.set_param("attack", attack)
        self.vm.set_param("decay", decay)
        self.vm.set_param("sustain", sustain)

        # Get params into buffers
        # Buffer 0: gate (will be set externally)
        # Buffer 1: attack
        # Buffer 2: decay
        # Buffer 3: sustain
        # Buffer 10: output

        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("attack"))
        )
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("decay"))
        )
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 3, cedar.hash("sustain"))
        )

        # ENV_ADSR: gate, attack, decay, sustain -> output
        # Release time is encoded in rate field (0-255 -> 0.0-25.5s)
        release_rate = min(255, int(release * 10))  # 0.1s resolution

        inst = cedar.Instruction.make_quaternary(
            cedar.Opcode.ENV_ADSR, 10, 0, 1, 2, 3, state_id
        )
        inst.rate = release_rate
        self.program.append(inst)

        # Route to output
        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def create_ar_program(self, attack: float, release: float, state_id: int = 1):
        """Create ENV_AR program.

        Args:
            attack: Attack time in seconds
            release: Release time in seconds
            state_id: State ID for the envelope
        """
        self.program = []

        self.vm.set_param("attack", attack)
        self.vm.set_param("release", release)

        # Buffer 0: trigger (set externally)
        # Buffer 1: attack
        # Buffer 2: release
        # Buffer 10: output

        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("attack"))
        )
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("release"))
        )

        # ENV_AR: trigger, attack, release -> output
        self.program.append(
            cedar.Instruction.make_ternary(
                cedar.Opcode.ENV_AR, 10, 0, 1, 2, state_id
            )
        )

        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def create_follower_program(self, attack: float, release: float, state_id: int = 1):
        """Create ENV_FOLLOWER program.

        Args:
            attack: Attack time in seconds
            release: Release time in seconds
            state_id: State ID for the envelope
        """
        self.program = []

        self.vm.set_param("attack", attack)
        self.vm.set_param("release", release)

        # Buffer 0: input signal (set externally)
        # Buffer 1: attack
        # Buffer 2: release
        # Buffer 10: output

        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("attack"))
        )
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("release"))
        )

        # ENV_FOLLOWER: input, attack, release -> output
        self.program.append(
            cedar.Instruction.make_ternary(
                cedar.Opcode.ENV_FOLLOWER, 10, 0, 1, 2, state_id
            )
        )

        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def run_with_gate(self, gate_signal: np.ndarray) -> np.ndarray:
        """Run with a gate signal, returning envelope output."""
        num_blocks = (len(gate_signal) + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
        padded_len = num_blocks * cedar.BLOCK_SIZE

        gate_padded = np.zeros(padded_len, dtype=np.float32)
        gate_padded[:len(gate_signal)] = gate_signal

        output = []
        for i in range(num_blocks):
            start = i * cedar.BLOCK_SIZE
            end = start + cedar.BLOCK_SIZE
            self.vm.set_buffer(0, gate_padded[start:end])
            left, right = self.vm.process()
            output.append(left)

        return np.concatenate(output)[:len(gate_signal)]


def measure_envelope_time(signal: np.ndarray, threshold: float, sr: int,
                          start_idx: int = 0, direction: str = 'rising') -> dict:
    """Measure time to reach threshold.

    Args:
        signal: Envelope signal
        threshold: Target threshold (e.g., 0.99 for attack, 0.01 for release)
        sr: Sample rate
        start_idx: Index to start searching from
        direction: 'rising' or 'falling'

    Returns:
        Dict with timing information
    """
    if direction == 'rising':
        for i in range(start_idx, len(signal)):
            if signal[i] >= threshold:
                return {
                    'reached_threshold': True,
                    'sample_index': i,
                    'time_seconds': (i - start_idx) / sr,
                    'time_ms': (i - start_idx) / sr * 1000,
                    'final_value': float(signal[i])
                }
    else:  # falling
        for i in range(start_idx, len(signal)):
            if signal[i] <= threshold:
                return {
                    'reached_threshold': True,
                    'sample_index': i,
                    'time_seconds': (i - start_idx) / sr,
                    'time_ms': (i - start_idx) / sr * 1000,
                    'final_value': float(signal[i])
                }

    return {
        'reached_threshold': False,
        'sample_index': len(signal) - 1,
        'time_seconds': (len(signal) - 1 - start_idx) / sr,
        'time_ms': (len(signal) - 1 - start_idx) / sr * 1000,
        'final_value': float(signal[-1])
    }


def find_gate_edge(gate: np.ndarray, edge_type: str = 'rising', start_idx: int = 0) -> int:
    """Find gate edge in signal.

    Args:
        gate: Gate signal
        edge_type: 'rising' or 'falling'
        start_idx: Index to start searching

    Returns:
        Index of edge, or -1 if not found
    """
    for i in range(start_idx + 1, len(gate)):
        if edge_type == 'rising' and gate[i-1] <= 0 and gate[i] > 0:
            return i
        elif edge_type == 'falling' and gate[i-1] > 0 and gate[i] <= 0:
            return i
    return -1


# =============================================================================
# Test 1: ADSR Stage Timing Accuracy
# =============================================================================

def test_adsr_timing():
    """Test ADSR envelope timing accuracy."""
    print("Test 1: ADSR Stage Timing Accuracy")
    print("=" * 60)

    sr = 48000

    # Test various timing configurations
    test_configs = [
        {'attack': 0.01, 'decay': 0.05, 'sustain': 0.7, 'release': 0.1, 'name': 'Fast'},
        {'attack': 0.05, 'decay': 0.1, 'sustain': 0.5, 'release': 0.2, 'name': 'Medium'},
        {'attack': 0.1, 'decay': 0.2, 'sustain': 0.3, 'release': 0.5, 'name': 'Slow'},
        {'attack': 0.001, 'decay': 0.01, 'sustain': 0.8, 'release': 0.05, 'name': 'Snappy'},
    ]

    results = {
        'sample_rate': sr,
        'tests': []
    }

    fig, axes = plt.subplots(len(test_configs), 2, figsize=(16, 4 * len(test_configs)))

    for idx, config in enumerate(test_configs):
        attack = config['attack']
        decay = config['decay']
        sustain = config['sustain']
        release = config['release']
        name = config['name']

        # Create gate: on for 0.5s, then off
        gate_on_time = 0.5
        total_time = gate_on_time + release + 0.2  # Extra time for release
        num_samples = int(total_time * sr)

        gate = np.zeros(num_samples, dtype=np.float32)
        gate_on_samples = int(gate_on_time * sr)
        gate[:gate_on_samples] = 1.0

        host = EnvelopeTestHost(sr)
        host.create_adsr_program(attack, decay, sustain, release, state_id=idx+1)
        output = host.run_with_gate(gate)

        # Measure attack time (to 99% of peak)
        attack_result = measure_envelope_time(output, 0.99, sr, start_idx=0, direction='rising')

        # Measure decay time (from peak to sustain + 1% of range)
        decay_target = sustain + 0.01 * (1.0 - sustain)
        peak_idx = attack_result['sample_index']
        decay_result = measure_envelope_time(output, decay_target, sr, start_idx=peak_idx, direction='falling')

        # Measure release time (from sustain to 1% of sustain)
        release_target = 0.01 * sustain
        release_start_idx = gate_on_samples
        release_result = measure_envelope_time(output, release_target, sr, start_idx=release_start_idx, direction='falling')

        # Calculate errors (using factor 4.6 for exponential ~99% target)
        attack_error_pct = (attack_result['time_seconds'] - attack) / attack * 100 if attack > 0 else 0
        decay_error_pct = (decay_result['time_seconds'] - decay) / decay * 100 if decay > 0 else 0
        release_error_pct = (release_result['time_seconds'] - release) / release * 100 if release > 0 else 0

        test_result = {
            'name': name,
            'config': config,
            'attack_measured_ms': attack_result['time_ms'],
            'attack_expected_ms': attack * 1000,
            'attack_error_pct': attack_error_pct,
            'decay_measured_ms': decay_result['time_ms'],
            'decay_expected_ms': decay * 1000,
            'decay_error_pct': decay_error_pct,
            'release_measured_ms': release_result['time_ms'],
            'release_expected_ms': release * 1000,
            'release_error_pct': release_error_pct,
            'peak_value': float(output[peak_idx]) if peak_idx < len(output) else 0,
            'sustain_value': float(output[release_start_idx-1]) if release_start_idx > 0 else 0
        }
        results['tests'].append(test_result)

        # Print results
        attack_status = "PASS" if abs(attack_error_pct) < 10 else "FAIL"
        decay_status = "PASS" if abs(decay_error_pct) < 15 else "FAIL"
        release_status = "PASS" if abs(release_error_pct) < 10 else "FAIL"

        print(f"\n  {name} ADSR (A={attack*1000:.0f}ms, D={decay*1000:.0f}ms, S={sustain:.1f}, R={release*1000:.0f}ms):")
        print(f"    Attack:  expected={attack*1000:.1f}ms, measured={attack_result['time_ms']:.1f}ms, error={attack_error_pct:.1f}% [{attack_status}]")
        print(f"    Decay:   expected={decay*1000:.1f}ms, measured={decay_result['time_ms']:.1f}ms, error={decay_error_pct:.1f}% [{decay_status}]")
        print(f"    Release: expected={release*1000:.1f}ms, measured={release_result['time_ms']:.1f}ms, error={release_error_pct:.1f}% [{release_status}]")
        print(f"    Peak: {test_result['peak_value']:.4f}, Sustain: {test_result['sustain_value']:.4f}")

        # Plot envelope
        ax1 = axes[idx, 0]
        time_ms = np.arange(len(output)) / sr * 1000
        ax1.plot(time_ms, output, 'b-', linewidth=1, label='Envelope')
        ax1.plot(time_ms, gate * 0.5, 'g--', linewidth=0.5, alpha=0.5, label='Gate (scaled)')

        # Mark stages
        ax1.axvline(attack_result['time_ms'], color='red', linestyle=':', alpha=0.7, label=f'Attack end')
        ax1.axhline(sustain, color='orange', linestyle='--', alpha=0.5, label=f'Sustain={sustain}')
        ax1.axvline(gate_on_time * 1000, color='purple', linestyle=':', alpha=0.7, label='Gate off')

        ax1.set_xlabel('Time (ms)')
        ax1.set_ylabel('Level')
        ax1.set_title(f'{name} ADSR Envelope')
        ax1.legend(fontsize=8)
        ax1.grid(True, alpha=0.3)
        ax1.set_ylim(-0.1, 1.1)

        # Plot zoomed attack
        ax2 = axes[idx, 1]
        attack_zoom = int((attack + decay + 0.05) * sr)
        ax2.plot(time_ms[:attack_zoom], output[:attack_zoom], 'b-', linewidth=1)
        ax2.axvline(attack * 1000, color='red', linestyle='--', alpha=0.7, label=f'Expected attack={attack*1000:.1f}ms')
        ax2.axvline(attack_result['time_ms'], color='green', linestyle=':', alpha=0.7, label=f'Measured={attack_result["time_ms"]:.1f}ms')
        ax2.axhline(sustain, color='orange', linestyle='--', alpha=0.5)
        ax2.set_xlabel('Time (ms)')
        ax2.set_ylabel('Level')
        ax2.set_title(f'{name} Attack+Decay Detail')
        ax2.legend(fontsize=8)
        ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/env_adsr_timing.png')
    print(f"\n  Saved: output/env_adsr_timing.png")

    with open('output/env_adsr_timing.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/env_adsr_timing.json")

    return results


# =============================================================================
# Test 2: ADSR Gate Edge Detection
# =============================================================================

def test_adsr_gate_edges():
    """Test ADSR gate edge detection and response."""
    print("\nTest 2: ADSR Gate Edge Detection")
    print("=" * 60)

    sr = 48000
    attack = 0.02
    decay = 0.05
    sustain = 0.6
    release = 0.1

    # Test various gate patterns
    results = {'sample_rate': sr, 'tests': []}

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Test 1: Multiple short gates (retriggering)
    gate1 = np.zeros(int(1.0 * sr), dtype=np.float32)
    for i in range(4):
        start = int((0.1 + i * 0.2) * sr)
        end = int((0.2 + i * 0.2) * sr)
        gate1[start:end] = 1.0

    host = EnvelopeTestHost(sr)
    host.create_adsr_program(attack, decay, sustain, release, state_id=10)
    output1 = host.run_with_gate(gate1)

    # Count peaks
    peaks = []
    for i in range(1, len(output1) - 1):
        if output1[i] > output1[i-1] and output1[i] > output1[i+1] and output1[i] > 0.9:
            peaks.append(i)

    results['tests'].append({
        'name': 'Retrigger',
        'num_gates': 4,
        'peaks_detected': len(peaks),
        'correct': bool(len(peaks) >= 4)
    })

    print(f"  Retrigger test: 4 gates -> {len(peaks)} peaks detected")

    ax1 = axes[0, 0]
    time_ms = np.arange(len(output1)) / sr * 1000
    ax1.plot(time_ms, output1, 'b-', linewidth=1, label='Envelope')
    ax1.plot(time_ms, gate1 * 0.3, 'g--', linewidth=0.5, alpha=0.5, label='Gate')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Level')
    ax1.set_title('Retrigger Behavior (4 short gates)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Test 2: Gate off during attack (release pending)
    gate2 = np.zeros(int(0.5 * sr), dtype=np.float32)
    gate2[:int(0.005 * sr)] = 1.0  # Very short gate (5ms, shorter than attack)

    host2 = EnvelopeTestHost(sr)
    host2.create_adsr_program(attack, decay, sustain, release, state_id=20)
    output2 = host2.run_with_gate(gate2)

    # Find peak and check if it completes attack before release
    peak_idx = np.argmax(output2)
    peak_value = output2[peak_idx]

    results['tests'].append({
        'name': 'Short gate (release pending)',
        'gate_duration_ms': 5,
        'attack_time_ms': attack * 1000,
        'peak_value': float(peak_value),
        'peak_reached_1.0': bool(peak_value > 0.95)
    })

    print(f"  Short gate test: gate=5ms, attack={attack*1000:.0f}ms -> peak={peak_value:.3f}")

    ax2 = axes[0, 1]
    time_ms2 = np.arange(len(output2)) / sr * 1000
    ax2.plot(time_ms2, output2, 'b-', linewidth=1, label='Envelope')
    ax2.plot(time_ms2, gate2 * 0.3, 'g--', linewidth=0.5, alpha=0.5, label='Gate')
    ax2.axvline(5, color='red', linestyle=':', alpha=0.7, label='Gate off')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Level')
    ax2.set_title('Release Pending (gate shorter than attack)')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    # Test 3: Retrigger during release
    gate3 = np.zeros(int(0.6 * sr), dtype=np.float32)
    gate3[:int(0.1 * sr)] = 1.0  # First gate
    gate3[int(0.15 * sr):int(0.25 * sr)] = 1.0  # Second gate during release

    host3 = EnvelopeTestHost(sr)
    host3.create_adsr_program(attack, decay, sustain, release, state_id=30)
    output3 = host3.run_with_gate(gate3)

    # Check that second attack starts from current level (not 0)
    retrigger_idx = int(0.15 * sr)
    level_at_retrigger = output3[retrigger_idx]

    results['tests'].append({
        'name': 'Retrigger during release',
        'level_at_retrigger': float(level_at_retrigger),
        'starts_from_current': bool(level_at_retrigger > 0.1)
    })

    print(f"  Retrigger during release: level at retrigger={level_at_retrigger:.3f}")

    ax3 = axes[1, 0]
    time_ms3 = np.arange(len(output3)) / sr * 1000
    ax3.plot(time_ms3, output3, 'b-', linewidth=1, label='Envelope')
    ax3.plot(time_ms3, gate3 * 0.3, 'g--', linewidth=0.5, alpha=0.5, label='Gate')
    ax3.axvline(150, color='red', linestyle=':', alpha=0.7, label='Retrigger')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Level')
    ax3.set_title('Retrigger During Release')
    ax3.legend()
    ax3.grid(True, alpha=0.3)

    # Test 4: Sustain level accuracy
    gate4 = np.zeros(int(1.0 * sr), dtype=np.float32)
    gate4[:int(0.8 * sr)] = 1.0  # Long gate to reach sustain

    host4 = EnvelopeTestHost(sr)
    host4.create_adsr_program(attack, decay, sustain, release, state_id=40)
    output4 = host4.run_with_gate(gate4)

    # Measure sustain level (average in sustain region)
    sustain_start = int((attack + decay + 0.1) * sr)
    sustain_end = int(0.75 * sr)
    sustain_measured = np.mean(output4[sustain_start:sustain_end])
    sustain_error = abs(sustain_measured - sustain) / sustain * 100

    results['tests'].append({
        'name': 'Sustain accuracy',
        'expected_sustain': sustain,
        'measured_sustain': float(sustain_measured),
        'error_pct': sustain_error
    })

    print(f"  Sustain accuracy: expected={sustain:.2f}, measured={sustain_measured:.4f}, error={sustain_error:.2f}%")

    ax4 = axes[1, 1]
    time_ms4 = np.arange(len(output4)) / sr * 1000
    ax4.plot(time_ms4, output4, 'b-', linewidth=1, label='Envelope')
    ax4.plot(time_ms4, gate4 * 0.3, 'g--', linewidth=0.5, alpha=0.5, label='Gate')
    ax4.axhline(sustain, color='orange', linestyle='--', alpha=0.7, label=f'Target sustain={sustain}')
    ax4.axhline(sustain_measured, color='red', linestyle=':', alpha=0.7, label=f'Measured={sustain_measured:.3f}')
    ax4.set_xlabel('Time (ms)')
    ax4.set_ylabel('Level')
    ax4.set_title('Sustain Level Accuracy')
    ax4.legend()
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/env_adsr_gates.png')
    print(f"\n  Saved: output/env_adsr_gates.png")

    with open('output/env_adsr_gates.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/env_adsr_gates.json")

    return results


# =============================================================================
# Test 3: AR Envelope (One-Shot)
# =============================================================================

def test_ar_envelope():
    """Test AR envelope one-shot behavior."""
    print("\nTest 3: AR Envelope (One-Shot)")
    print("=" * 60)

    sr = 48000

    test_configs = [
        {'attack': 0.005, 'release': 0.05, 'name': 'Percussive'},
        {'attack': 0.02, 'release': 0.1, 'name': 'Snare-like'},
        {'attack': 0.05, 'release': 0.3, 'name': 'Pluck'},
        {'attack': 0.1, 'release': 0.5, 'name': 'Slow'},
    ]

    results = {'sample_rate': sr, 'tests': []}

    fig, axes = plt.subplots(len(test_configs), 2, figsize=(16, 4 * len(test_configs)))

    for idx, config in enumerate(test_configs):
        attack = config['attack']
        release = config['release']
        name = config['name']

        total_time = attack + release + 0.2
        num_samples = int(total_time * sr)

        # Single trigger pulse
        trigger = np.zeros(num_samples, dtype=np.float32)
        trigger[100] = 1.0  # Single sample trigger

        host = EnvelopeTestHost(sr)
        host.create_ar_program(attack, release, state_id=idx+100)
        output = host.run_with_gate(trigger)

        # Measure timing
        attack_result = measure_envelope_time(output, 0.99, sr, start_idx=100, direction='rising')
        peak_idx = 100 + attack_result['sample_index'] - 100
        release_result = measure_envelope_time(output, 0.01, sr, start_idx=peak_idx, direction='falling')

        # Calculate errors
        attack_error = (attack_result['time_seconds'] - attack) / attack * 100 if attack > 0 else 0
        release_error = (release_result['time_seconds'] - release) / release * 100 if release > 0 else 0

        test_result = {
            'name': name,
            'config': config,
            'attack_measured_ms': attack_result['time_ms'],
            'attack_error_pct': attack_error,
            'release_measured_ms': release_result['time_ms'],
            'release_error_pct': release_error,
            'peak_value': float(np.max(output))
        }
        results['tests'].append(test_result)

        # Check if it's one-shot (returns to zero)
        final_value = output[-1]
        is_oneshot = final_value < 0.01

        print(f"\n  {name} AR (A={attack*1000:.0f}ms, R={release*1000:.0f}ms):")
        print(f"    Attack:  expected={attack*1000:.1f}ms, measured={attack_result['time_ms']:.1f}ms, error={attack_error:.1f}%")
        print(f"    Release: expected={release*1000:.1f}ms, measured={release_result['time_ms']:.1f}ms, error={release_error:.1f}%")
        print(f"    Peak: {test_result['peak_value']:.4f}, Final: {final_value:.6f}, One-shot: {is_oneshot}")

        # Plot full envelope
        ax1 = axes[idx, 0]
        time_ms = np.arange(len(output)) / sr * 1000
        ax1.plot(time_ms, output, 'b-', linewidth=1, label='Envelope')
        ax1.axvline(100/sr*1000, color='green', linestyle='--', alpha=0.5, label='Trigger')
        ax1.set_xlabel('Time (ms)')
        ax1.set_ylabel('Level')
        ax1.set_title(f'{name} AR Envelope')
        ax1.legend()
        ax1.grid(True, alpha=0.3)

        # Plot zoomed attack
        ax2 = axes[idx, 1]
        zoom_end = int((attack + 0.02) * sr) + 100
        ax2.plot(time_ms[:zoom_end], output[:zoom_end], 'b-', linewidth=1)
        ax2.axvline((100/sr + attack) * 1000, color='red', linestyle='--', alpha=0.7, label=f'Expected peak')
        ax2.set_xlabel('Time (ms)')
        ax2.set_ylabel('Level')
        ax2.set_title(f'{name} Attack Detail')
        ax2.legend()
        ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/env_ar.png')
    print(f"\n  Saved: output/env_ar.png")

    with open('output/env_ar.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/env_ar.json")

    # Test retrigger during release
    print("\n  Retrigger test:")
    host = EnvelopeTestHost(sr)
    host.create_ar_program(0.02, 0.2, state_id=200)

    trigger = np.zeros(int(0.5 * sr), dtype=np.float32)
    trigger[100] = 1.0  # First trigger
    trigger[int(0.1 * sr)] = 1.0  # Second trigger during release

    output = host.run_with_gate(trigger)

    # Find peaks
    peaks = []
    for i in range(1, len(output) - 1):
        if output[i] > output[i-1] and output[i] > output[i+1] and output[i] > 0.8:
            peaks.append(i)

    print(f"    2 triggers -> {len(peaks)} peaks detected")

    return results


# =============================================================================
# Test 4: Envelope Follower
# =============================================================================

def test_envelope_follower():
    """Test envelope follower tracking accuracy."""
    print("\nTest 4: Envelope Follower")
    print("=" * 60)

    sr = 48000

    results = {'sample_rate': sr, 'tests': []}

    fig, axes = plt.subplots(3, 2, figsize=(16, 12))

    # Test 1: DC tracking
    print("\n  DC Tracking:")
    dc_signal = np.ones(int(0.5 * sr), dtype=np.float32) * 0.8
    dc_signal[:int(0.1 * sr)] = 0  # Ramp up test

    host = EnvelopeTestHost(sr)
    host.create_follower_program(attack=0.01, release=0.1, state_id=300)
    dc_output = host.run_with_gate(dc_signal)

    # Measure tracking accuracy
    steady_start = int(0.3 * sr)
    steady_value = np.mean(dc_output[steady_start:])
    dc_error = abs(steady_value - 0.8) / 0.8 * 100

    results['tests'].append({
        'name': 'DC tracking',
        'expected': 0.8,
        'measured': float(steady_value),
        'error_pct': dc_error
    })

    print(f"    Input=0.8 DC, Output={steady_value:.4f}, Error={dc_error:.2f}%")

    ax1 = axes[0, 0]
    time_ms = np.arange(len(dc_output)) / sr * 1000
    ax1.plot(time_ms, dc_signal, 'g--', linewidth=0.5, alpha=0.5, label='Input')
    ax1.plot(time_ms, dc_output, 'b-', linewidth=1, label='Follower')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Level')
    ax1.set_title('DC Tracking')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Test 2: Sine wave tracking (rectified)
    print("\n  Sine Wave Tracking:")
    duration = 0.5
    freq = 100  # 100 Hz sine
    t = np.arange(int(duration * sr)) / sr
    sine_signal = np.sin(2 * np.pi * freq * t).astype(np.float32) * 0.9

    host2 = EnvelopeTestHost(sr)
    host2.create_follower_program(attack=0.005, release=0.02, state_id=301)
    sine_output = host2.run_with_gate(sine_signal)

    # The follower should track the rectified signal's peaks
    steady_region = sine_output[int(0.2 * sr):]
    avg_level = np.mean(steady_region)
    ripple = np.std(steady_region)

    results['tests'].append({
        'name': 'Sine tracking',
        'input_amplitude': 0.9,
        'avg_output': float(avg_level),
        'ripple': float(ripple)
    })

    print(f"    Input amplitude=0.9, Avg output={avg_level:.4f}, Ripple={ripple:.4f}")

    ax2 = axes[0, 1]
    time_ms2 = np.arange(len(sine_output)) / sr * 1000
    ax2.plot(time_ms2, np.abs(sine_signal), 'g--', linewidth=0.3, alpha=0.5, label='|Input|')
    ax2.plot(time_ms2, sine_output, 'b-', linewidth=1, label='Follower')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Level')
    ax2.set_title('Sine Wave Tracking')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    # Test 3: Asymmetric attack/release
    print("\n  Asymmetric Attack/Release:")
    # Fast attack, slow release
    step_signal = np.zeros(int(0.8 * sr), dtype=np.float32)
    step_signal[int(0.1 * sr):int(0.4 * sr)] = 0.9

    host3 = EnvelopeTestHost(sr)
    host3.create_follower_program(attack=0.001, release=0.1, state_id=302)
    step_output = host3.run_with_gate(step_signal)

    # Measure attack and release times
    attack_result = measure_envelope_time(step_output, 0.8, sr, start_idx=int(0.1 * sr), direction='rising')
    release_start = int(0.4 * sr)
    release_result = measure_envelope_time(step_output, 0.1, sr, start_idx=release_start, direction='falling')

    results['tests'].append({
        'name': 'Asymmetric response',
        'attack_ms': attack_result['time_ms'],
        'release_ms': release_result['time_ms'],
        'ratio': release_result['time_seconds'] / attack_result['time_seconds'] if attack_result['time_seconds'] > 0 else 0
    })

    print(f"    Attack time: {attack_result['time_ms']:.2f}ms")
    print(f"    Release time: {release_result['time_ms']:.2f}ms")

    ax3 = axes[1, 0]
    time_ms3 = np.arange(len(step_output)) / sr * 1000
    ax3.plot(time_ms3, step_signal, 'g--', linewidth=0.5, alpha=0.5, label='Input')
    ax3.plot(time_ms3, step_output, 'b-', linewidth=1, label='Follower')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Level')
    ax3.set_title('Asymmetric Attack/Release (fast attack, slow release)')
    ax3.legend()
    ax3.grid(True, alpha=0.3)

    # Test 4: Burst tracking
    print("\n  Burst Tracking:")
    burst_signal = np.zeros(int(1.0 * sr), dtype=np.float32)
    # Create bursts of sine waves
    for i in range(4):
        start = int((0.1 + i * 0.2) * sr)
        end = int((0.15 + i * 0.2) * sr)
        t_burst = np.arange(end - start) / sr
        burst_signal[start:end] = np.sin(2 * np.pi * 440 * t_burst) * 0.8

    host4 = EnvelopeTestHost(sr)
    host4.create_follower_program(attack=0.001, release=0.02, state_id=303)
    burst_output = host4.run_with_gate(burst_signal)

    # Count envelope peaks
    env_peaks = []
    for i in range(1, len(burst_output) - 1):
        if burst_output[i] > burst_output[i-1] and burst_output[i] > burst_output[i+1] and burst_output[i] > 0.5:
            if len(env_peaks) == 0 or i - env_peaks[-1] > sr * 0.05:  # Minimum 50ms between peaks
                env_peaks.append(i)

    results['tests'].append({
        'name': 'Burst tracking',
        'num_bursts': 4,
        'peaks_detected': len(env_peaks)
    })

    print(f"    4 bursts -> {len(env_peaks)} envelope peaks detected")

    ax4 = axes[1, 1]
    time_ms4 = np.arange(len(burst_output)) / sr * 1000
    ax4.plot(time_ms4, np.abs(burst_signal), 'g--', linewidth=0.3, alpha=0.3, label='|Input|')
    ax4.plot(time_ms4, burst_output, 'b-', linewidth=1, label='Follower')
    ax4.set_xlabel('Time (ms)')
    ax4.set_ylabel('Level')
    ax4.set_title('Burst Tracking')
    ax4.legend()
    ax4.grid(True, alpha=0.3)

    # Test 5: Different attack/release times comparison
    print("\n  Attack/Release Time Comparison:")
    test_signal = np.zeros(int(0.5 * sr), dtype=np.float32)
    test_signal[int(0.05 * sr):int(0.25 * sr)] = 0.8

    configs = [
        {'attack': 0.001, 'release': 0.01, 'color': 'blue', 'label': 'Fast (1ms/10ms)'},
        {'attack': 0.01, 'release': 0.05, 'color': 'green', 'label': 'Medium (10ms/50ms)'},
        {'attack': 0.05, 'release': 0.2, 'color': 'red', 'label': 'Slow (50ms/200ms)'},
    ]

    ax5 = axes[2, 0]
    ax5.plot(np.arange(len(test_signal)) / sr * 1000, test_signal, 'k--', linewidth=0.5, alpha=0.5, label='Input')

    for i, cfg in enumerate(configs):
        host_cfg = EnvelopeTestHost(sr)
        host_cfg.create_follower_program(cfg['attack'], cfg['release'], state_id=310+i)
        out = host_cfg.run_with_gate(test_signal)
        ax5.plot(np.arange(len(out)) / sr * 1000, out, color=cfg['color'], linewidth=1, label=cfg['label'])

    ax5.set_xlabel('Time (ms)')
    ax5.set_ylabel('Level')
    ax5.set_title('Attack/Release Time Comparison')
    ax5.legend()
    ax5.grid(True, alpha=0.3)

    # Test 6: Audio rate tracking (amplitude modulated signal)
    print("\n  AM Signal Tracking:")
    duration = 0.3
    t = np.arange(int(duration * sr)) / sr
    carrier = np.sin(2 * np.pi * 440 * t)
    modulator = 0.5 + 0.5 * np.sin(2 * np.pi * 5 * t)  # 5 Hz modulation
    am_signal = (carrier * modulator * 0.9).astype(np.float32)

    host6 = EnvelopeTestHost(sr)
    host6.create_follower_program(attack=0.002, release=0.02, state_id=320)
    am_output = host6.run_with_gate(am_signal)

    ax6 = axes[2, 1]
    time_ms6 = np.arange(len(am_output)) / sr * 1000
    ax6.plot(time_ms6, np.abs(am_signal), 'g-', linewidth=0.2, alpha=0.3, label='|AM Signal|')
    ax6.plot(time_ms6, modulator * 0.9, 'r--', linewidth=1, alpha=0.7, label='Modulator envelope')
    ax6.plot(time_ms6, am_output, 'b-', linewidth=1, label='Follower')
    ax6.set_xlabel('Time (ms)')
    ax6.set_ylabel('Level')
    ax6.set_title('AM Signal Envelope Tracking (5 Hz modulation)')
    ax6.legend()
    ax6.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/env_follower.png')
    print(f"\n  Saved: output/env_follower.png")

    with open('output/env_follower.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/env_follower.json")

    return results


# =============================================================================
# Test 5: Envelope Curve Shape
# =============================================================================

def test_envelope_curves():
    """Test exponential curve shape of envelopes."""
    print("\nTest 5: Envelope Curve Shape")
    print("=" * 60)

    sr = 48000
    attack = 0.1
    release = 0.2

    # Generate ADSR with long gate
    gate = np.ones(int(0.8 * sr), dtype=np.float32)
    gate[int(0.5 * sr):] = 0  # Release after 0.5s

    host = EnvelopeTestHost(sr)
    host.create_adsr_program(attack, 0.05, 0.7, release, state_id=400)
    output = host.run_with_gate(gate)

    results = {'sample_rate': sr, 'tests': []}

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Analyze attack curve
    ax1 = axes[0, 0]
    attack_end = int(attack * sr * 1.5)
    attack_region = output[:attack_end]
    time_ms = np.arange(len(attack_region)) / sr * 1000

    # Fit exponential: y = 1 - exp(-t/tau)
    # At t=tau, y = 0.632 (63.2%)
    idx_63 = np.argmax(attack_region >= 0.632)
    tau_attack = idx_63 / sr

    # Generate ideal exponential for comparison
    ideal_attack = 1 - np.exp(-np.arange(len(attack_region)) / sr / (attack / 4.6))

    results['tests'].append({
        'name': 'Attack curve',
        'time_constant_ms': tau_attack * 1000,
        'expected_tau_ms': (attack / 4.6) * 1000,
        'reaches_63pct_at_ms': tau_attack * 1000
    })

    print(f"  Attack curve:")
    print(f"    Reaches 63.2%% at {tau_attack*1000:.2f}ms")
    print(f"    Expected time constant: {(attack/4.6)*1000:.2f}ms")

    ax1.plot(time_ms, attack_region, 'b-', linewidth=2, label='Actual')
    ax1.plot(time_ms, ideal_attack, 'r--', linewidth=1, alpha=0.7, label='Ideal exp')
    ax1.axhline(0.632, color='green', linestyle=':', alpha=0.5, label='63.2%')
    ax1.axvline(tau_attack * 1000, color='green', linestyle=':', alpha=0.5)
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Level')
    ax1.set_title('Attack Curve (exponential)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Analyze release curve
    ax2 = axes[0, 1]
    release_start = int(0.5 * sr)
    release_region = output[release_start:release_start + int(release * sr * 2)]
    time_ms_rel = np.arange(len(release_region)) / sr * 1000

    # Find where it reaches 36.8% of starting value (1 time constant)
    start_level = release_region[0]
    target_level = start_level * 0.368
    idx_37 = np.argmax(release_region <= target_level) if np.any(release_region <= target_level) else len(release_region) - 1
    tau_release = idx_37 / sr

    # Generate ideal exponential decay
    ideal_release = start_level * np.exp(-np.arange(len(release_region)) / sr / (release / 4.6))

    results['tests'].append({
        'name': 'Release curve',
        'start_level': float(start_level),
        'time_constant_ms': tau_release * 1000,
        'expected_tau_ms': (release / 4.6) * 1000
    })

    print(f"  Release curve:")
    print(f"    Starts at {start_level:.3f}")
    print(f"    Reaches 36.8%% of start at {tau_release*1000:.2f}ms")

    ax2.plot(time_ms_rel, release_region, 'b-', linewidth=2, label='Actual')
    ax2.plot(time_ms_rel, ideal_release, 'r--', linewidth=1, alpha=0.7, label='Ideal exp')
    ax2.axhline(target_level, color='green', linestyle=':', alpha=0.5, label='36.8% of start')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Level')
    ax2.set_title('Release Curve (exponential)')
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    # Plot log scale to verify exponential
    ax3 = axes[1, 0]
    # Avoid log(0)
    safe_release = np.maximum(release_region, 1e-6)
    ax3.semilogy(time_ms_rel, safe_release, 'b-', linewidth=2, label='Actual')
    ax3.semilogy(time_ms_rel, np.maximum(ideal_release, 1e-6), 'r--', linewidth=1, alpha=0.7, label='Ideal exp')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('Level (log scale)')
    ax3.set_title('Release Curve (log scale - should be linear)')
    ax3.legend()
    ax3.grid(True, alpha=0.3)

    # Full ADSR visualization
    ax4 = axes[1, 1]
    full_time = np.arange(len(output)) / sr * 1000
    ax4.plot(full_time, output, 'b-', linewidth=2, label='ADSR')
    ax4.fill_between(full_time, 0, output, alpha=0.3)
    ax4.plot(full_time, gate * 0.5, 'g--', linewidth=0.5, alpha=0.5, label='Gate')

    # Annotate stages
    ax4.annotate('Attack', xy=(attack*500, 0.5), fontsize=10)
    ax4.annotate('Decay', xy=((attack+0.025)*1000, 0.85), fontsize=10)
    ax4.annotate('Sustain', xy=(250, 0.65), fontsize=10)
    ax4.annotate('Release', xy=(550, 0.35), fontsize=10)

    ax4.set_xlabel('Time (ms)')
    ax4.set_ylabel('Level')
    ax4.set_title('Full ADSR Envelope')
    ax4.legend()
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/env_curves.png')
    print(f"\n  Saved: output/env_curves.png")

    with open('output/env_curves.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/env_curves.json")

    # Save audio example
    print("\n  Saving audio examples:")

    # ADSR applied to oscillator
    osc_freq = 440
    t = np.arange(int(1.0 * sr)) / sr
    osc = np.sin(2 * np.pi * osc_freq * t).astype(np.float32)

    gate_audio = np.zeros(int(1.0 * sr), dtype=np.float32)
    gate_audio[:int(0.6 * sr)] = 1.0

    host = EnvelopeTestHost(sr)
    host.create_adsr_program(0.01, 0.1, 0.6, 0.3, state_id=500)
    env = host.run_with_gate(gate_audio)

    audio = osc * env
    save_wav('env_adsr_sine.wav', audio, sr)

    return results


# =============================================================================
# Test 6: Sample-Accurate Envelope Timing
# =============================================================================

def test_sample_accurate_timing():
    """Test envelope timing accuracy at sample level."""
    print("\nTest 6: Sample-Accurate Envelope Timing")
    print("=" * 60)

    sr = 48000
    results = {'sample_rate': sr, 'tests': []}

    # Test 1: ADSR attack time - measure exact sample where peak is reached
    print("\n  ADSR Attack Sample Accuracy:")

    attack_times_ms = [1, 5, 10, 20, 50, 100]

    for attack_ms in attack_times_ms:
        attack_sec = attack_ms / 1000.0
        decay = 0.05
        sustain = 0.7
        release = 0.1

        # Expected samples to reach 99% (using coefficient -4.6)
        expected_samples = int(attack_sec * sr)

        gate = np.ones(int(0.3 * sr), dtype=np.float32)

        host = EnvelopeTestHost(sr)
        host.create_adsr_program(attack_sec, decay, sustain, release, state_id=600 + attack_ms)
        output = host.run_with_gate(gate)

        # Find exact sample where we first reach 0.99
        peak_sample = -1
        for i, v in enumerate(output):
            if v >= 0.99:
                peak_sample = i
                break

        error_samples = peak_sample - expected_samples if peak_sample >= 0 else float('nan')
        error_percent = (error_samples / expected_samples * 100) if expected_samples > 0 else 0

        # Pass criteria: within 10% or 5 samples, whichever is larger
        tolerance_samples = max(5, int(expected_samples * 0.1))
        passed = abs(error_samples) <= tolerance_samples if peak_sample >= 0 else False
        status = "PASS" if passed else "FAIL"

        test_result = {
            'attack_ms': attack_ms,
            'expected_samples': expected_samples,
            'measured_samples': peak_sample,
            'error_samples': int(error_samples) if peak_sample >= 0 else None,
            'error_percent': error_percent,
            'tolerance_samples': tolerance_samples,
            'passed': passed
        }
        results['tests'].append(test_result)

        print(f"    A={attack_ms:3d}ms: expected={expected_samples:5d}, measured={peak_sample:5d}, "
              f"error={error_samples:+5.0f} samples ({error_percent:+.1f}%) [{status}]")

    # Test 2: AR envelope peak timing
    print("\n  AR Envelope Peak Sample Accuracy:")

    ar_attack_times_ms = [1, 5, 10, 20]

    for attack_ms in ar_attack_times_ms:
        attack_sec = attack_ms / 1000.0
        release = 0.1

        expected_peak_sample = int(attack_sec * sr)

        # Single trigger at sample 100
        trigger_offset = 100
        trigger = np.zeros(int(0.3 * sr), dtype=np.float32)
        trigger[trigger_offset] = 1.0

        host = EnvelopeTestHost(sr)
        host.create_ar_program(attack_sec, release, state_id=700 + attack_ms)
        output = host.run_with_gate(trigger)

        # Find peak
        peak_sample = np.argmax(output)
        measured_attack_samples = peak_sample - trigger_offset

        error_samples = measured_attack_samples - expected_peak_sample
        tolerance_samples = max(5, int(expected_peak_sample * 0.1))
        passed = abs(error_samples) <= tolerance_samples
        status = "PASS" if passed else "FAIL"

        test_result = {
            'attack_ms': attack_ms,
            'expected_peak_sample': expected_peak_sample + trigger_offset,
            'measured_peak_sample': int(peak_sample),
            'error_samples': int(error_samples),
            'passed': passed
        }
        results['tests'].append(test_result)

        print(f"    A={attack_ms:3d}ms: expected peak @ {expected_peak_sample + trigger_offset}, "
              f"measured @ {peak_sample}, error={error_samples:+d} samples [{status}]")

    # Test 3: Gate edge detection precision
    print("\n  Gate Edge Detection Precision:")

    gate_on_samples = [128, 256, 512, 1000, 2000]

    for gate_on_sample in gate_on_samples:
        attack = 0.01
        decay = 0.05
        sustain = 0.7
        release = 0.1

        gate = np.zeros(int(0.5 * sr), dtype=np.float32)
        gate[gate_on_sample:gate_on_sample + int(0.3 * sr)] = 1.0

        host = EnvelopeTestHost(sr)
        host.create_adsr_program(attack, decay, sustain, release, state_id=800 + gate_on_sample)
        output = host.run_with_gate(gate)

        # Find first sample where output rises above threshold
        threshold = 0.01
        first_rise_sample = -1
        for i in range(gate_on_sample, len(output)):
            if output[i] > threshold:
                first_rise_sample = i
                break

        # Should be within 1 block (128 samples) of gate on
        delay_samples = first_rise_sample - gate_on_sample if first_rise_sample >= 0 else float('nan')
        passed = delay_samples <= cedar.BLOCK_SIZE if first_rise_sample >= 0 else False
        status = "PASS" if passed else "FAIL"

        test_result = {
            'gate_on_sample': gate_on_sample,
            'first_rise_sample': first_rise_sample,
            'delay_samples': int(delay_samples) if first_rise_sample >= 0 else None,
            'passed': passed
        }
        results['tests'].append(test_result)

        print(f"    Gate @ {gate_on_sample:5d}: first rise @ {first_rise_sample}, "
              f"delay={delay_samples:.0f} samples [{status}]")

    # Summary
    all_passed = all(t.get('passed', False) for t in results['tests'])
    summary_status = "ALL TESTS PASSED" if all_passed else "SOME TESTS FAILED"
    print(f"\n  Summary: {summary_status}")

    with open('output/env_sample_accurate.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/env_sample_accurate.json")

    return results


# =============================================================================
# Test 7: Exponential Curve Verification
# =============================================================================

def test_exponential_accuracy():
    """Verify exponential envelope curves match expected mathematical form."""
    print("\nTest 7: Exponential Curve Verification")
    print("=" * 60)

    sr = 48000
    results = {'sample_rate': sr, 'tests': []}

    # The envelope uses coefficient: 1 - exp(-4.6 / samples)
    # This means at t = attack_time, level should be 1 - exp(-4.6) ≈ 0.99

    attack = 0.1  # 100ms
    attack_samples = int(attack * sr)

    gate = np.ones(int(0.5 * sr), dtype=np.float32)

    host = EnvelopeTestHost(sr)
    host.create_adsr_program(attack, 0.05, 0.7, 0.1, state_id=900)
    output = host.run_with_gate(gate)

    # Generate ideal exponential curve for comparison
    t = np.arange(len(output))
    tau = attack_samples / 4.6  # time constant
    ideal_attack = 1 - np.exp(-t / tau)

    # Compare attack phase
    attack_region = output[:attack_samples]
    ideal_attack_region = ideal_attack[:attack_samples]

    # Calculate error statistics
    errors = attack_region - ideal_attack_region
    max_error = float(np.max(np.abs(errors)))
    rms_error = float(np.sqrt(np.mean(errors ** 2)))
    mean_error = float(np.mean(errors))

    # Check at specific time points
    checkpoints = [0.25, 0.5, 0.75, 1.0]  # fractions of attack time
    checkpoint_results = []

    print("\n  Attack curve checkpoints:")
    for frac in checkpoints:
        sample_idx = int(frac * attack_samples)
        if sample_idx < len(output):
            expected = 1 - np.exp(-4.6 * frac)
            measured = output[sample_idx]
            error = abs(measured - expected)
            checkpoint_results.append({
                'fraction': frac,
                'sample': sample_idx,
                'expected': float(expected),
                'measured': float(measured),
                'error': float(error)
            })
            status = "PASS" if error < 0.05 else "FAIL"
            print(f"    t={frac:.2f}*attack: expected={expected:.4f}, measured={measured:.4f}, "
                  f"error={error:.4f} [{status}]")

    results['tests'].append({
        'name': 'Attack curve accuracy',
        'max_error': max_error,
        'rms_error': rms_error,
        'mean_error': mean_error,
        'checkpoints': checkpoint_results
    })

    print(f"\n  Overall: max_error={max_error:.4f}, rms_error={rms_error:.4f}")

    # Plot comparison
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))

    # Attack phase
    ax1 = axes[0, 0]
    time_ms = np.arange(attack_samples) / sr * 1000
    ax1.plot(time_ms, attack_region, 'b-', linewidth=1.5, label='Measured')
    ax1.plot(time_ms, ideal_attack_region, 'r--', linewidth=1, alpha=0.7, label='Ideal exp')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Level')
    ax1.set_title('Attack Phase Comparison')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Error plot
    ax2 = axes[0, 1]
    ax2.plot(time_ms, errors, 'b-', linewidth=0.8)
    ax2.axhline(0, color='gray', linewidth=0.5)
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Error')
    ax2.set_title(f'Error (max={max_error:.4f})')
    ax2.grid(True, alpha=0.3)

    # Log scale comparison
    ax3 = axes[1, 0]
    # Plot 1-level to show exponential approach on log scale
    safe_attack = np.maximum(1.0 - attack_region, 1e-6)
    safe_ideal = np.maximum(1.0 - ideal_attack_region, 1e-6)
    ax3.semilogy(time_ms, safe_attack, 'b-', linewidth=1.5, label='Measured')
    ax3.semilogy(time_ms, safe_ideal, 'r--', linewidth=1, alpha=0.7, label='Ideal')
    ax3.set_xlabel('Time (ms)')
    ax3.set_ylabel('1 - Level (log scale)')
    ax3.set_title('Exponential Verification (should be linear on log scale)')
    ax3.legend()
    ax3.grid(True, alpha=0.3)

    # Histogram of errors
    ax4 = axes[1, 1]
    ax4.hist(errors, bins=50, edgecolor='black', alpha=0.7)
    ax4.axvline(0, color='red', linestyle='--')
    ax4.set_xlabel('Error')
    ax4.set_ylabel('Count')
    ax4.set_title('Error Distribution')
    ax4.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/env_exponential_accuracy.png')
    print(f"  Saved: output/env_exponential_accuracy.png")

    with open('output/env_exponential_accuracy.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/env_exponential_accuracy.json")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    os.makedirs('output', exist_ok=True)

    print("Cedar Envelope Quality Tests")
    print("=" * 60)
    print()

    test_adsr_timing()
    test_adsr_gate_edges()
    test_ar_envelope()
    test_envelope_follower()
    test_envelope_curves()
    test_sample_accurate_timing()
    test_exponential_accuracy()

    print()
    print("=" * 60)
    print("All envelope tests complete. Results saved to output/")
