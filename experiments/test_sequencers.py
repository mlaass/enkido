"""
Sequencer Opcode Quality Tests (Cedar Engine)
==============================================
Tests for CLOCK, LFO, TRIGGER, EUCLID, and TIMELINE opcodes.
Validates timing accuracy, pattern generation, and beat synchronization.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
from scipy.io import wavfile
import cedar_core as cedar
from visualize import save_figure
from utils import ms_to_samples, samples_to_ms


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


class SequencerTestHost:
    """Helper to run Cedar VM sequencer tests."""

    def __init__(self, sample_rate=48000, bpm=120.0):
        self.vm = cedar.VM()
        self.vm.set_sample_rate(sample_rate)
        self.vm.set_bpm(bpm)
        self.sr = sample_rate
        self.bpm = bpm
        self.program = []
        self.samples_per_beat = sample_rate * 60.0 / bpm
        self.samples_per_bar = self.samples_per_beat * 4

    def create_clock_program(self, phase_type: int = 0, state_id: int = 1):
        """Create CLOCK program.

        Args:
            phase_type: 0=beat_phase, 1=bar_phase, 2=cycle_offset
            state_id: State ID
        """
        self.program = []

        # CLOCK outputs to buffer 10
        inst = cedar.Instruction.make_nullary(cedar.Opcode.CLOCK, 10, state_id)
        inst.rate = phase_type
        self.program.append(inst)

        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def create_lfo_program(self, freq_mult: float, shape: int, duty: float = 0.5, state_id: int = 1):
        """Create LFO program.

        Args:
            freq_mult: Cycles per beat
            shape: 0=SIN, 1=TRI, 2=SAW, 3=RAMP, 4=SQR, 5=PWM, 6=SAH
            duty: Duty cycle for PWM (0-1)
            state_id: State ID
        """
        self.program = []

        self.vm.set_param("freq_mult", freq_mult)
        self.vm.set_param("duty", duty)

        # Get freq_mult into buffer 1
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("freq_mult"))
        )
        # Get duty into buffer 2
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("duty"))
        )

        # LFO: freq_mult (buf 1), duty (buf 2) -> output (buf 10)
        inst = cedar.Instruction.make_binary(cedar.Opcode.LFO, 10, 1, 2, state_id)
        inst.rate = shape
        self.program.append(inst)

        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def create_trigger_program(self, division: float, state_id: int = 1):
        """Create TRIGGER program.

        Args:
            division: Triggers per beat (1=quarter, 2=eighth, 4=16th)
            state_id: State ID
        """
        self.program = []

        self.vm.set_param("division", division)

        # Get division into buffer 1
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("division"))
        )

        # TRIGGER: division (buf 1) -> output (buf 10)
        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.TRIGGER, 10, 1, state_id)
        )

        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def create_euclid_program(self, hits: int, steps: int, rotation: int = 0, state_id: int = 1):
        """Create EUCLID program.

        Args:
            hits: Number of hits in pattern
            steps: Total steps in pattern
            rotation: Pattern rotation
            state_id: State ID
        """
        self.program = []

        self.vm.set_param("hits", float(hits))
        self.vm.set_param("steps", float(steps))
        self.vm.set_param("rotation", float(rotation))

        # Get parameters into buffers
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("hits"))
        )
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("steps"))
        )
        self.program.append(
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 3, cedar.hash("rotation"))
        )

        # EUCLID: hits (buf 1), steps (buf 2), rotation (buf 3) -> output (buf 10)
        self.program.append(
            cedar.Instruction.make_ternary(cedar.Opcode.EUCLID, 10, 1, 2, 3, state_id)
        )

        self.program.append(
            cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
        )

        self.vm.load_program(self.program)

    def run(self, duration_sec: float) -> np.ndarray:
        """Run program and return output."""
        num_blocks = int((duration_sec * self.sr) / cedar.BLOCK_SIZE)
        output = []

        for _ in range(num_blocks):
            left, right = self.vm.process()
            output.append(left)

        return np.concatenate(output)


def count_triggers(signal: np.ndarray, threshold: float = 0.5, include_initial: bool = True) -> list:
    """Count rising edges (triggers) and return their positions.

    Args:
        signal: Input signal
        threshold: Trigger threshold
        include_initial: If True, include a trigger at sample 0 if signal starts above threshold

    Returns:
        List of sample indices where triggers occur
    """
    triggers = []
    # Check sample 0 as a special case (trigger at start)
    if include_initial and len(signal) > 0 and signal[0] >= threshold:
        triggers.append(0)
    # Check for rising edges from sample 1 onwards
    for i in range(1, len(signal)):
        if signal[i-1] < threshold and signal[i] >= threshold:
            triggers.append(i)
    return triggers


def analyze_trigger_timing(triggers: list, expected_interval: float, sr: int) -> dict:
    """Analyze trigger timing accuracy.

    Args:
        triggers: List of trigger sample indices
        expected_interval: Expected samples between triggers
        sr: Sample rate

    Returns:
        Dict with timing analysis
    """
    if len(triggers) < 2:
        return {'error': 'Not enough triggers'}

    intervals = np.diff(triggers)
    avg_interval = np.mean(intervals)
    interval_error = (avg_interval - expected_interval) / expected_interval * 100
    jitter = np.std(intervals)

    return {
        'num_triggers': len(triggers),
        'avg_interval_samples': float(avg_interval),
        'expected_interval_samples': expected_interval,
        'interval_error_pct': float(interval_error),
        'jitter_samples': float(jitter),
        'jitter_ms': float(jitter / sr * 1000),
        'intervals': intervals.tolist()[:20]  # First 20 intervals
    }


# =============================================================================
# Test 1: CLOCK Phase Accuracy
# =============================================================================

def test_clock_phase():
    """Test CLOCK beat and bar phase accuracy."""
    print("Test 1: CLOCK Phase Accuracy")
    print("=" * 60)

    results = {'tests': []}

    # Test at various BPMs
    bpms = [60, 120, 180, 90]
    sr = 48000

    fig, axes = plt.subplots(len(bpms), 2, figsize=(16, 4 * len(bpms)))

    for idx, bpm in enumerate(bpms):
        samples_per_beat = sr * 60.0 / bpm
        samples_per_bar = samples_per_beat * 4

        # Test 4 bars
        duration = 4 * 4 * 60.0 / bpm

        # Test beat phase
        host = SequencerTestHost(sr, bpm)
        host.create_clock_program(phase_type=0, state_id=idx*10+1)  # beat_phase
        beat_output = host.run(duration)

        # Test bar phase
        host2 = SequencerTestHost(sr, bpm)
        host2.create_clock_program(phase_type=1, state_id=idx*10+2)  # bar_phase
        bar_output = host2.run(duration)

        # Analyze beat phase: should wrap at every beat
        beat_wraps = []
        for i in range(1, len(beat_output)):
            if beat_output[i] < beat_output[i-1] - 0.5:  # Phase wrapped
                beat_wraps.append(i)

        # Calculate beat timing accuracy
        expected_beats = int(duration * bpm / 60)
        actual_beats = len(beat_wraps)

        if len(beat_wraps) >= 2:
            beat_intervals = np.diff(beat_wraps)
            avg_beat_interval = np.mean(beat_intervals)
            beat_error = (avg_beat_interval - samples_per_beat) / samples_per_beat * 100
        else:
            avg_beat_interval = 0
            beat_error = 0

        # Analyze bar phase
        bar_wraps = []
        for i in range(1, len(bar_output)):
            if bar_output[i] < bar_output[i-1] - 0.5:
                bar_wraps.append(i)

        if len(bar_wraps) >= 2:
            bar_intervals = np.diff(bar_wraps)
            avg_bar_interval = np.mean(bar_intervals)
            bar_error = (avg_bar_interval - samples_per_bar) / samples_per_bar * 100
        else:
            avg_bar_interval = 0
            bar_error = 0

        test_result = {
            'bpm': bpm,
            'expected_samples_per_beat': samples_per_beat,
            'measured_samples_per_beat': float(avg_beat_interval),
            'beat_error_pct': float(beat_error),
            'expected_samples_per_bar': samples_per_bar,
            'measured_samples_per_bar': float(avg_bar_interval),
            'bar_error_pct': float(bar_error),
            'beats_detected': actual_beats,
            'expected_beats': expected_beats
        }
        results['tests'].append(test_result)

        beat_status = "PASS" if abs(beat_error) < 0.1 else "FAIL"
        bar_status = "PASS" if abs(bar_error) < 0.1 else "FAIL"

        print(f"\n  BPM={bpm}:")
        print(f"    Beat phase: expected={samples_per_beat:.1f} samples/beat, measured={avg_beat_interval:.1f}, error={beat_error:.3f}% [{beat_status}]")
        print(f"    Bar phase:  expected={samples_per_bar:.1f} samples/bar, measured={avg_bar_interval:.1f}, error={bar_error:.3f}% [{bar_status}]")
        print(f"    Beats: expected={expected_beats}, detected={actual_beats}")

        # Plot beat phase
        ax1 = axes[idx, 0]
        time_ms = np.arange(min(len(beat_output), int(2 * samples_per_bar))) / sr * 1000
        plot_samples = len(time_ms)
        ax1.plot(time_ms, beat_output[:plot_samples], 'b-', linewidth=0.8, label='Beat phase')

        # Mark beat boundaries
        for wrap in beat_wraps:
            if wrap < plot_samples:
                ax1.axvline(wrap / sr * 1000, color='red', linestyle=':', alpha=0.3)

        ax1.set_xlabel('Time (ms)')
        ax1.set_ylabel('Phase (0-1)')
        ax1.set_title(f'Beat Phase @ {bpm} BPM')
        ax1.legend()
        ax1.grid(True, alpha=0.3)
        ax1.set_ylim(-0.1, 1.1)

        # Plot bar phase
        ax2 = axes[idx, 1]
        ax2.plot(time_ms, bar_output[:plot_samples], 'g-', linewidth=0.8, label='Bar phase')

        # Mark bar boundaries
        for wrap in bar_wraps:
            if wrap < plot_samples:
                ax2.axvline(wrap / sr * 1000, color='red', linestyle=':', alpha=0.5)

        ax2.set_xlabel('Time (ms)')
        ax2.set_ylabel('Phase (0-1)')
        ax2.set_title(f'Bar Phase @ {bpm} BPM')
        ax2.legend()
        ax2.grid(True, alpha=0.3)
        ax2.set_ylim(-0.1, 1.1)

    plt.tight_layout()
    save_figure(fig, 'output/seq_clock.png')
    print(f"\n  Saved: output/seq_clock.png")

    with open('output/seq_clock.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/seq_clock.json")

    return results


# =============================================================================
# Test 2: LFO Shape Accuracy
# =============================================================================

def test_lfo_shapes():
    """Test LFO waveform shapes."""
    print("\nTest 2: LFO Shape Accuracy")
    print("=" * 60)

    sr = 48000
    bpm = 120
    freq_mult = 1.0  # 1 cycle per beat

    shapes = [
        {'id': 0, 'name': 'SIN', 'expected_range': (-1, 1)},
        {'id': 1, 'name': 'TRI', 'expected_range': (-1, 1)},
        {'id': 2, 'name': 'SAW', 'expected_range': (-1, 1)},
        {'id': 3, 'name': 'RAMP', 'expected_range': (-1, 1)},
        {'id': 4, 'name': 'SQR', 'expected_range': (-1, 1)},
        {'id': 5, 'name': 'PWM', 'expected_range': (-1, 1)},
        {'id': 6, 'name': 'SAH', 'expected_range': (-1, 1)},
    ]

    results = {'sample_rate': sr, 'bpm': bpm, 'freq_mult': freq_mult, 'tests': []}

    fig, axes = plt.subplots(4, 2, figsize=(14, 14))
    axes = axes.flatten()

    for idx, shape in enumerate(shapes):
        host = SequencerTestHost(sr, bpm)
        host.create_lfo_program(freq_mult, shape['id'], duty=0.5, state_id=idx+100)

        # 2 beats
        duration = 2 * 60.0 / bpm
        output = host.run(duration)

        # Analyze waveform
        min_val = float(np.min(output))
        max_val = float(np.max(output))
        mean_val = float(np.mean(output))

        # Count zero crossings per cycle
        zero_crossings = sum(1 for i in range(1, len(output))
                           if (output[i-1] < 0 and output[i] >= 0) or
                              (output[i-1] >= 0 and output[i] < 0))

        # Measure frequency via zero crossings (each cycle has 2 crossings for bipolar)
        expected_crossings = 2 * 2  # 2 cycles, 2 crossings each

        test_result = {
            'name': shape['name'],
            'min': min_val,
            'max': max_val,
            'mean': mean_val,
            'zero_crossings': zero_crossings,
            'expected_crossings': expected_crossings,
            'bounded': bool(min_val >= -1.01 and max_val <= 1.01)
        }
        results['tests'].append(test_result)

        # Check bounds
        bounded_status = "PASS" if test_result['bounded'] else "FAIL"
        print(f"  {shape['name']:4s}: min={min_val:.3f}, max={max_val:.3f}, mean={mean_val:.3f}, crossings={zero_crossings} [{bounded_status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)
        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Value')
        ax.set_title(f'{shape["name"]} LFO (1 cycle/beat)')
        ax.set_xlim(0, 2)
        ax.set_ylim(-1.2, 1.2)
        ax.axhline(0, color='gray', linewidth=0.5)
        ax.grid(True, alpha=0.3)

        # Mark beat boundaries
        ax.axvline(1, color='red', linestyle='--', alpha=0.3)

    # Hide unused subplot
    if len(shapes) < len(axes):
        axes[-1].set_visible(False)

    plt.tight_layout()
    save_figure(fig, 'output/seq_lfo_shapes.png')
    print(f"\n  Saved: output/seq_lfo_shapes.png")

    with open('output/seq_lfo_shapes.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/seq_lfo_shapes.json")

    return results


# =============================================================================
# Test 3: LFO Frequency Sync
# =============================================================================

def test_lfo_freq_sync():
    """Test LFO frequency synchronization."""
    print("\nTest 3: LFO Frequency Sync")
    print("=" * 60)

    sr = 48000
    bpm = 120

    freq_mults = [0.25, 0.5, 1.0, 2.0, 4.0]

    results = {'sample_rate': sr, 'bpm': bpm, 'tests': []}

    fig, axes = plt.subplots(len(freq_mults), 1, figsize=(14, 3 * len(freq_mults)))

    for idx, freq_mult in enumerate(freq_mults):
        host = SequencerTestHost(sr, bpm)
        host.create_lfo_program(freq_mult, shape=2, state_id=idx+200)  # SAW for clear cycle detection

        # 4 beats
        duration = 4 * 60.0 / bpm
        output = host.run(duration)

        # Count phase wraps (cycle completions for SAW: drops from ~1 to ~-1)
        cycle_wraps = []
        for i in range(1, len(output)):
            if output[i-1] > 0.5 and output[i] < -0.5:  # Large drop = cycle wrap
                cycle_wraps.append(i)

        expected_cycles = int(4 * freq_mult)  # 4 beats * freq_mult cycles per beat
        actual_cycles = len(cycle_wraps)

        # Analyze cycle timing
        if len(cycle_wraps) >= 2:
            intervals = np.diff(cycle_wraps)
            avg_interval = np.mean(intervals)
            expected_interval = host.samples_per_beat / freq_mult
            interval_error = (avg_interval - expected_interval) / expected_interval * 100
        else:
            avg_interval = 0
            expected_interval = host.samples_per_beat / freq_mult
            interval_error = 0

        test_result = {
            'freq_mult': freq_mult,
            'expected_cycles': expected_cycles,
            'actual_cycles': actual_cycles,
            'expected_interval_samples': expected_interval,
            'measured_interval_samples': float(avg_interval),
            'interval_error_pct': float(interval_error)
        }
        results['tests'].append(test_result)

        cycle_status = "PASS" if abs(actual_cycles - expected_cycles) <= 1 else "FAIL"
        print(f"  freq_mult={freq_mult}: expected={expected_cycles} cycles, actual={actual_cycles}, error={interval_error:.2f}% [{cycle_status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)

        # Mark cycle boundaries
        for wrap in cycle_wraps:
            ax.axvline(wrap / host.samples_per_beat, color='red', linestyle=':', alpha=0.5)

        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Value')
        ax.set_title(f'LFO SAW @ {freq_mult} cycles/beat ({actual_cycles} cycles in 4 beats)')
        ax.set_xlim(0, 4)
        ax.set_ylim(-1.2, 1.2)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/seq_lfo_sync.png')
    print(f"\n  Saved: output/seq_lfo_sync.png")

    with open('output/seq_lfo_sync.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/seq_lfo_sync.json")

    return results


# =============================================================================
# Test 4: LFO PWM Duty Cycle
# =============================================================================

def test_lfo_pwm():
    """Test LFO PWM duty cycle accuracy."""
    print("\nTest 4: LFO PWM Duty Cycle")
    print("=" * 60)

    sr = 48000
    bpm = 120

    duty_cycles = [0.1, 0.25, 0.5, 0.75, 0.9]

    results = {'sample_rate': sr, 'bpm': bpm, 'tests': []}

    fig, axes = plt.subplots(len(duty_cycles), 1, figsize=(14, 2 * len(duty_cycles)))

    for idx, duty in enumerate(duty_cycles):
        host = SequencerTestHost(sr, bpm)
        host.create_lfo_program(1.0, shape=5, duty=duty, state_id=idx+300)  # PWM

        # 2 beats
        duration = 2 * 60.0 / bpm
        output = host.run(duration)

        # Measure actual duty cycle (fraction of time at +1)
        high_samples = np.sum(output > 0)
        total_samples = len(output)
        measured_duty = high_samples / total_samples

        duty_error = (measured_duty - duty) / duty * 100 if duty > 0 else 0

        test_result = {
            'expected_duty': duty,
            'measured_duty': float(measured_duty),
            'error_pct': float(duty_error)
        }
        results['tests'].append(test_result)

        status = "PASS" if abs(duty_error) < 5 else "FAIL"
        print(f"  duty={duty:.2f}: measured={measured_duty:.3f}, error={duty_error:.1f}% [{status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)
        ax.fill_between(time_beats, 0, output, where=output > 0, alpha=0.3)
        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Value')
        ax.set_title(f'PWM duty={duty:.0%} (measured={measured_duty:.1%})')
        ax.set_xlim(0, 2)
        ax.set_ylim(-1.2, 1.2)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/seq_lfo_pwm.png')
    print(f"\n  Saved: output/seq_lfo_pwm.png")

    with open('output/seq_lfo_pwm.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/seq_lfo_pwm.json")

    return results


# =============================================================================
# Test 5: TRIGGER Division Accuracy
# =============================================================================

def test_trigger_division():
    """Test TRIGGER beat division accuracy."""
    print("\nTest 5: TRIGGER Division Accuracy")
    print("=" * 60)

    sr = 48000
    bpm = 120

    divisions = [1, 2, 4, 8, 16]

    results = {'sample_rate': sr, 'bpm': bpm, 'tests': []}

    fig, axes = plt.subplots(len(divisions), 1, figsize=(14, 2 * len(divisions)))

    for idx, division in enumerate(divisions):
        host = SequencerTestHost(sr, bpm)
        host.create_trigger_program(float(division), state_id=idx+400)

        # 4 beats
        duration = 4 * 60.0 / bpm
        output = host.run(duration)

        # Find triggers
        triggers = count_triggers(output, threshold=0.5)

        expected_triggers = 4 * division  # 4 beats * division per beat
        expected_interval = host.samples_per_beat / division

        timing = analyze_trigger_timing(triggers, expected_interval, sr)

        test_result = {
            'division': division,
            'expected_triggers': expected_triggers,
            'actual_triggers': len(triggers),
            **timing
        }
        results['tests'].append(test_result)

        trigger_status = "PASS" if abs(len(triggers) - expected_triggers) <= 2 else "FAIL"
        timing_status = "PASS" if abs(timing.get('interval_error_pct', 100)) < 1 else "FAIL"

        print(f"  div={division:2d}: expected={expected_triggers:3d} triggers, actual={len(triggers):3d} [{trigger_status}], "
              f"interval_error={timing.get('interval_error_pct', 0):.2f}% [{timing_status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)

        # Mark triggers
        for trig in triggers[:50]:  # First 50
            ax.axvline(trig / host.samples_per_beat, color='red', linestyle=':', alpha=0.3)

        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Trigger')
        ax.set_title(f'TRIGGER div={division} ({len(triggers)} triggers in 4 beats)')
        ax.set_xlim(0, 4)
        ax.set_ylim(-0.1, 1.2)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/seq_trigger.png')
    print(f"\n  Saved: output/seq_trigger.png")

    with open('output/seq_trigger.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/seq_trigger.json")

    return results


# =============================================================================
# Test 6: EUCLID Pattern Accuracy
# =============================================================================

def test_euclid_patterns():
    """Test EUCLID pattern generation accuracy."""
    print("\nTest 6: EUCLID Pattern Accuracy")
    print("=" * 60)

    sr = 48000
    bpm = 120

    # Known Euclidean patterns
    # Format: (hits, steps, expected_pattern_binary)
    patterns = [
        (3, 8, "10010010"),   # E(3,8) - Cuban tresillo
        (5, 8, "10110110"),   # E(5,8) - Cuban cinquillo
        (3, 4, "1011"),       # E(3,4)
        (5, 16, "1001001001001000"),  # E(5,16)
        (7, 16, "1010101010101010"),  # E(7,16) - close to every other
        (4, 12, "100100100100"),  # E(4,12)
    ]

    results = {'sample_rate': sr, 'bpm': bpm, 'tests': []}

    fig, axes = plt.subplots(len(patterns), 1, figsize=(14, 2 * len(patterns)))

    for idx, (hits, steps, expected_pattern) in enumerate(patterns):
        host = SequencerTestHost(sr, bpm)
        host.create_euclid_program(hits, steps, rotation=0, state_id=idx+500)

        # Run for 2 bars (8 beats) - pattern repeats over 1 bar at this speed
        # Since steps are spread across 1 bar, we need enough time to see the full pattern
        duration = 8 * 60.0 / bpm
        output = host.run(duration)

        # Find triggers
        triggers = count_triggers(output, threshold=0.5)

        # Reconstruct pattern from trigger positions
        # Each step occupies samples_per_bar / steps samples
        samples_per_step = host.samples_per_bar / steps

        # Build pattern from first bar's triggers
        first_bar_triggers = [t for t in triggers if t < host.samples_per_bar]
        measured_pattern = ['0'] * steps
        for trig in first_bar_triggers:
            step_idx = int(trig / samples_per_step) % steps
            measured_pattern[step_idx] = '1'
        measured_pattern_str = ''.join(measured_pattern)

        # Count hits
        measured_hits = measured_pattern_str.count('1')

        pattern_match = measured_pattern_str == expected_pattern
        hits_match = measured_hits == hits

        test_result = {
            'hits': hits,
            'steps': steps,
            'expected_pattern': expected_pattern,
            'measured_pattern': measured_pattern_str,
            'pattern_match': pattern_match,
            'expected_hits': hits,
            'measured_hits': measured_hits,
            'hits_match': hits_match
        }
        results['tests'].append(test_result)

        status = "PASS" if hits_match else "FAIL"
        print(f"  E({hits},{steps}): expected='{expected_pattern}', measured='{measured_pattern_str}', hits={measured_hits} [{status}]")

        # Plot
        ax = axes[idx]
        # Show 2 bars
        plot_samples = int(2 * host.samples_per_bar)
        time_beats = np.arange(min(len(output), plot_samples)) / host.samples_per_beat
        ax.plot(time_beats, output[:len(time_beats)], linewidth=0.8)

        # Mark step boundaries
        for s in range(steps * 2):
            step_time = s * (4 / steps)  # 4 beats per bar
            ax.axvline(step_time, color='gray', linestyle=':', alpha=0.2)

        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Trigger')
        ax.set_title(f'E({hits},{steps}): {measured_pattern_str}')
        ax.set_xlim(0, 8)
        ax.set_ylim(-0.1, 1.2)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/seq_euclid.png')
    print(f"\n  Saved: output/seq_euclid.png")

    with open('output/seq_euclid.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/seq_euclid.json")

    return results


# =============================================================================
# Test 7: EUCLID Rotation
# =============================================================================

def test_euclid_rotation():
    """Test EUCLID pattern rotation."""
    print("\nTest 7: EUCLID Rotation")
    print("=" * 60)

    sr = 48000
    bpm = 120

    hits = 3
    steps = 8
    rotations = [0, 1, 2, 3, 4]

    results = {'sample_rate': sr, 'bpm': bpm, 'hits': hits, 'steps': steps, 'tests': []}

    fig, axes = plt.subplots(len(rotations), 1, figsize=(14, 2 * len(rotations)))

    base_pattern = None

    for idx, rotation in enumerate(rotations):
        host = SequencerTestHost(sr, bpm)
        host.create_euclid_program(hits, steps, rotation=rotation, state_id=idx+600)

        # 1 bar
        duration = 4 * 60.0 / bpm
        output = host.run(duration)

        # Find triggers
        triggers = count_triggers(output, threshold=0.5)

        # Build pattern
        samples_per_step = host.samples_per_bar / steps
        first_bar_triggers = [t for t in triggers if t < host.samples_per_bar]
        measured_pattern = ['0'] * steps
        for trig in first_bar_triggers:
            step_idx = int(trig / samples_per_step) % steps
            measured_pattern[step_idx] = '1'
        measured_pattern_str = ''.join(measured_pattern)

        if rotation == 0:
            base_pattern = measured_pattern_str

        # Calculate expected rotated pattern
        if base_pattern:
            expected_rotated = base_pattern[-rotation:] + base_pattern[:-rotation] if rotation > 0 else base_pattern
        else:
            expected_rotated = "unknown"

        rotation_correct = measured_pattern_str == expected_rotated

        test_result = {
            'rotation': rotation,
            'measured_pattern': measured_pattern_str,
            'expected_pattern': expected_rotated,
            'correct': rotation_correct
        }
        results['tests'].append(test_result)

        status = "PASS" if rotation_correct else "FAIL"
        print(f"  rot={rotation}: expected='{expected_rotated}', measured='{measured_pattern_str}' [{status}]")

        # Plot
        ax = axes[idx]
        time_beats = np.arange(len(output)) / host.samples_per_beat
        ax.plot(time_beats, output, linewidth=0.8)

        # Mark step boundaries
        for s in range(steps):
            ax.axvline(s * (4 / steps), color='gray', linestyle=':', alpha=0.3)

        ax.set_xlabel('Time (beats)')
        ax.set_ylabel('Trigger')
        ax.set_title(f'E({hits},{steps}) rot={rotation}: {measured_pattern_str}')
        ax.set_xlim(0, 4)
        ax.set_ylim(-0.1, 1.2)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/seq_euclid_rotation.png')
    print(f"\n  Saved: output/seq_euclid_rotation.png")

    with open('output/seq_euclid_rotation.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/seq_euclid_rotation.json")

    return results


# =============================================================================
# Test 8: Trigger to Audio (Integration)
# =============================================================================

def test_trigger_audio():
    """Test trigger-to-audio integration using envelopes."""
    print("\nTest 8: Trigger to Audio Integration")
    print("=" * 60)

    sr = 48000
    bpm = 120

    # Create a simple kick drum patch using TRIGGER -> ENV_AR -> OSC

    # First, generate triggers
    host = SequencerTestHost(sr, bpm)

    # Program:
    # 1. TRIGGER div=1 -> buf 5 (quarter notes)
    # 2. ENV_AR (trigger=buf5, attack=0.001, release=0.05) -> buf 6
    # 3. OSC_SIN (freq=60) -> buf 7
    # 4. MUL buf6 * buf7 -> buf 10
    # 5. OUTPUT buf 10

    host.program = []

    # Set parameters
    host.vm.set_param("division", 1.0)
    host.vm.set_param("attack", 0.001)
    host.vm.set_param("release", 0.1)
    host.vm.set_param("freq", 60.0)

    # Get division -> buf 1
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 1, cedar.hash("division"))
    )
    # Get attack -> buf 2
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 2, cedar.hash("attack"))
    )
    # Get release -> buf 3
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 3, cedar.hash("release"))
    )
    # Get freq -> buf 4
    host.program.append(
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 4, cedar.hash("freq"))
    )

    # TRIGGER -> buf 5
    host.program.append(
        cedar.Instruction.make_unary(cedar.Opcode.TRIGGER, 5, 1, cedar.hash("kick_trig"))
    )

    # ENV_AR -> buf 6
    host.program.append(
        cedar.Instruction.make_ternary(cedar.Opcode.ENV_AR, 6, 5, 2, 3, cedar.hash("kick_env"))
    )

    # OSC_SIN -> buf 7
    host.program.append(
        cedar.Instruction.make_unary(cedar.Opcode.OSC_SIN, 7, 4, cedar.hash("kick_osc"))
    )

    # MUL buf6 * buf7 -> buf 10
    host.program.append(
        cedar.Instruction.make_binary(cedar.Opcode.MUL, 10, 6, 7, 0)
    )

    # OUTPUT
    host.program.append(
        cedar.Instruction.make_unary(cedar.Opcode.OUTPUT, 0, 10)
    )

    host.vm.load_program(host.program)

    # Run for 4 bars
    duration = 16 * 60.0 / bpm
    output = host.run(duration)

    # Analyze output
    peak_level = np.max(np.abs(output))
    rms_level = np.sqrt(np.mean(output**2))

    # Find peaks (kick hits)
    peaks = []
    threshold = peak_level * 0.5
    in_peak = False
    for i, val in enumerate(output):
        if abs(val) > threshold and not in_peak:
            peaks.append(i)
            in_peak = True
        elif abs(val) < threshold * 0.5:
            in_peak = False

    print(f"  Generated kick drum pattern:")
    print(f"    Peak level: {peak_level:.3f}")
    print(f"    RMS level: {rms_level:.4f}")
    print(f"    Kicks detected: {len(peaks)} (expected 16)")

    # Save results
    results = {
        'sample_rate': sr,
        'bpm': bpm,
        'duration_sec': duration,
        'peak_level': float(peak_level),
        'rms_level': float(rms_level),
        'kicks_detected': len(peaks),
        'expected_kicks': 16
    }

    with open('output/seq_trigger_audio.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: output/seq_trigger_audio.json")

    # Plot
    fig, axes = plt.subplots(2, 1, figsize=(14, 8))

    # Full waveform
    ax1 = axes[0]
    time_beats = np.arange(len(output)) / host.samples_per_beat
    ax1.plot(time_beats, output, linewidth=0.5)
    ax1.set_xlabel('Time (beats)')
    ax1.set_ylabel('Amplitude')
    ax1.set_title(f'Kick Drum Pattern (quarter notes @ {bpm} BPM)')
    ax1.grid(True, alpha=0.3)

    # Zoomed view (first 4 beats)
    ax2 = axes[1]
    zoom_samples = int(4 * host.samples_per_beat)
    time_ms = np.arange(zoom_samples) / sr * 1000
    ax2.plot(time_ms, output[:zoom_samples], linewidth=0.8)
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Amplitude')
    ax2.set_title('First 4 beats (zoomed)')
    ax2.grid(True, alpha=0.3)

    # Mark beat boundaries
    for beat in range(5):
        ax2.axvline(beat * 60000 / bpm, color='red', linestyle='--', alpha=0.3)

    plt.tight_layout()
    save_figure(fig, 'output/seq_trigger_audio.png')
    print(f"  Saved: output/seq_trigger_audio.png")

    # Save audio
    save_wav('seq_kick_pattern.wav', output, sr)

    return results


# =============================================================================
# Test 9: CLOCK Phase Sample Accuracy
# =============================================================================

def test_clock_phase_sample_accuracy():
    """Test CLOCK phase accuracy at exact sample positions."""
    print("\nTest 9: CLOCK Phase Sample Accuracy")
    print("=" * 60)

    sr = 48000
    bpm = 120
    samples_per_beat = sr * 60.0 / bpm  # 24000 samples

    results = {'sample_rate': sr, 'bpm': bpm, 'samples_per_beat': samples_per_beat, 'tests': []}

    # Run for 100 beats
    num_beats = 100
    duration = num_beats * 60.0 / bpm

    host = SequencerTestHost(sr, bpm)
    host.create_clock_program(phase_type=0, state_id=1000)  # beat_phase
    output = host.run(duration)

    print(f"\n  Testing phase at exact sample positions over {num_beats} beats...")

    # Test phase values at specific samples
    test_samples = [
        0,                                    # Start
        int(samples_per_beat * 0.25),        # Quarter beat
        int(samples_per_beat * 0.5),         # Half beat
        int(samples_per_beat * 0.75),        # 3/4 beat
        int(samples_per_beat) - 1,           # Just before beat boundary
        int(samples_per_beat),               # Beat boundary
        int(samples_per_beat * 10),          # 10 beats in
        int(samples_per_beat * 50),          # 50 beats in
        int(samples_per_beat * 99),          # 99 beats in
    ]

    max_error = 0.0
    all_errors = []

    for sample_idx in test_samples:
        if sample_idx < len(output):
            # Expected phase: (sample_idx % samples_per_beat) / samples_per_beat
            expected_phase = (sample_idx % samples_per_beat) / samples_per_beat
            measured_phase = output[sample_idx]
            error = abs(measured_phase - expected_phase)
            max_error = max(max_error, error)
            all_errors.append(error)

            # Phase error as fraction of a sample
            phase_increment = 1.0 / samples_per_beat
            error_in_samples = error / phase_increment

            passed = error_in_samples < 2.0  # Within 2 sample equivalents
            status = "PASS" if passed else "FAIL"

            results['tests'].append({
                'sample_idx': sample_idx,
                'expected_phase': float(expected_phase),
                'measured_phase': float(measured_phase),
                'error': float(error),
                'error_in_samples': float(error_in_samples),
                'passed': passed
            })

            print(f"    Sample {sample_idx:8d}: expected={expected_phase:.6f}, "
                  f"measured={measured_phase:.6f}, error={error_in_samples:+.3f} samples [{status}]")

    # Long-term drift test: measure phase at every beat boundary
    print(f"\n  Long-term drift test over {num_beats} beats...")
    beat_boundary_errors = []

    for beat in range(num_beats):
        boundary_sample = int(beat * samples_per_beat)
        if boundary_sample < len(output):
            # At beat boundary, phase should wrap to near 0 (or 1.0 just before)
            measured = output[boundary_sample]
            # Phase should be very small (just wrapped) or very close to 0
            error = min(measured, 1.0 - measured)  # Distance to 0 or 1
            beat_boundary_errors.append(float(error))

    if beat_boundary_errors:
        max_boundary_error = max(beat_boundary_errors)
        avg_boundary_error = sum(beat_boundary_errors) / len(beat_boundary_errors)
        cumulative_drift = beat_boundary_errors[-1] if beat_boundary_errors else 0

        # Convert to samples
        phase_increment = 1.0 / samples_per_beat
        max_error_samples = max_boundary_error / phase_increment
        avg_error_samples = avg_boundary_error / phase_increment

        passed = max_error_samples < 2.0
        status = "PASS" if passed else "FAIL"

        results['long_term'] = {
            'num_beats': num_beats,
            'max_boundary_error': max_boundary_error,
            'max_error_samples': max_error_samples,
            'avg_error_samples': avg_error_samples,
            'passed': passed
        }

        print(f"    Max error at beat boundaries: {max_error_samples:.3f} samples [{status}]")
        print(f"    Average error: {avg_error_samples:.3f} samples")

    with open('output/seq_clock_sample_accuracy.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: output/seq_clock_sample_accuracy.json")

    return results


# =============================================================================
# Test 10: TRIGGER Long-Term Precision (1000 beats)
# =============================================================================

def test_trigger_long_term_precision():
    """Test TRIGGER timing precision over 1000 beats."""
    print("\nTest 10: TRIGGER Long-Term Precision")
    print("=" * 60)

    sr = 48000
    bpm = 120
    num_beats = 1000

    results = {'sample_rate': sr, 'bpm': bpm, 'num_beats': num_beats, 'tests': []}

    divisions = [1, 2, 4, 8]

    for division in divisions:
        samples_per_trigger = sr * 60.0 / bpm / division
        expected_triggers = num_beats * division
        duration = num_beats * 60.0 / bpm

        print(f"\n  Division={division} ({expected_triggers} triggers expected over {num_beats} beats)...")

        host = SequencerTestHost(sr, bpm)
        host.create_trigger_program(float(division), state_id=1100 + division)
        output = host.run(duration)

        # Find ALL trigger positions
        triggers = count_triggers(output, threshold=0.5)

        # Calculate timing error for EVERY trigger
        errors_samples = []
        for i, trigger_sample in enumerate(triggers):
            expected_sample = i * samples_per_trigger
            error = trigger_sample - expected_sample
            errors_samples.append(error)

        if errors_samples:
            max_error = max(abs(e) for e in errors_samples)
            final_drift = errors_samples[-1] if errors_samples else 0
            mean_error = sum(errors_samples) / len(errors_samples)

            # Pass criteria: ≤1 sample error at any point
            passed = max_error <= 1.5
            status = "PASS" if passed else "FAIL"

            test_result = {
                'division': division,
                'expected_triggers': expected_triggers,
                'actual_triggers': len(triggers),
                'max_error_samples': float(max_error),
                'final_drift_samples': float(final_drift),
                'mean_error_samples': float(mean_error),
                'passed': passed,
                # Store first and last 10 errors for inspection
                'first_10_errors': errors_samples[:10],
                'last_10_errors': errors_samples[-10:] if len(errors_samples) >= 10 else errors_samples
            }
            results['tests'].append(test_result)

            trigger_count_status = "PASS" if abs(len(triggers) - expected_triggers) <= 2 else "FAIL"
            print(f"    Triggers: expected={expected_triggers}, actual={len(triggers)} [{trigger_count_status}]")
            print(f"    Max timing error: {max_error:.2f} samples [{status}]")
            print(f"    Final drift: {final_drift:.2f} samples")
            print(f"    Mean error: {mean_error:.2f} samples")
        else:
            print(f"    ERROR: No triggers detected!")
            results['tests'].append({
                'division': division,
                'expected_triggers': expected_triggers,
                'actual_triggers': 0,
                'error': 'No triggers detected',
                'passed': False
            })

    # Summary
    all_passed = all(t.get('passed', False) for t in results['tests'])
    summary = "ALL PASSED" if all_passed else "SOME FAILED"
    print(f"\n  Overall: {summary}")

    with open('output/seq_trigger_long_term.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: output/seq_trigger_long_term.json")

    return results


# =============================================================================
# Test 11: EUCLID Step Timing Precision
# =============================================================================

def test_euclid_timing_precision():
    """Test EUCLID trigger timing at sample level."""
    print("\nTest 11: EUCLID Step Timing Precision")
    print("=" * 60)

    sr = 48000
    bpm = 120
    samples_per_bar = sr * 60.0 / bpm * 4  # 96000 samples per bar

    results = {'sample_rate': sr, 'bpm': bpm, 'samples_per_bar': samples_per_bar, 'tests': []}

    # Test E(3,8): 3 hits over 8 steps, pattern spread across 1 bar
    patterns = [
        (3, 8, "E(3,8) - tresillo"),
        (5, 8, "E(5,8) - cinquillo"),
        (7, 16, "E(7,16)"),
    ]

    for hits, steps, name in patterns:
        samples_per_step = samples_per_bar / steps

        print(f"\n  {name}: {samples_per_step:.1f} samples/step")

        # Run for 4 bars
        duration = 4 * 4 * 60.0 / bpm
        host = SequencerTestHost(sr, bpm)
        host.create_euclid_program(hits, steps, rotation=0, state_id=1200 + hits * 100 + steps)
        output = host.run(duration)

        # Find all triggers
        triggers = count_triggers(output, threshold=0.5)

        # Compute expected pattern using same algorithm as C++
        pattern_mask = 0
        bucket = 0.0
        increment = hits / steps
        for i in range(steps):
            bucket += increment
            if bucket >= 1.0:
                pattern_mask |= (1 << i)
                bucket -= 1.0

        # Calculate expected trigger positions for first bar
        expected_triggers_first_bar = []
        for step in range(steps):
            if (pattern_mask >> step) & 1:
                expected_sample = int(step * samples_per_step)
                expected_triggers_first_bar.append(expected_sample)

        print(f"    Pattern mask: {bin(pattern_mask)}")
        print(f"    Expected triggers per bar: {len(expected_triggers_first_bar)}")

        # Compare actual triggers in first bar with expected
        first_bar_triggers = [t for t in triggers if t < samples_per_bar]

        timing_errors = []
        for i, expected in enumerate(expected_triggers_first_bar):
            if i < len(first_bar_triggers):
                actual = first_bar_triggers[i]
                error = actual - expected
                timing_errors.append(error)

        if timing_errors:
            max_error = max(abs(e) for e in timing_errors)
            passed = max_error <= 2.0  # Within 2 samples
            status = "PASS" if passed else "FAIL"

            test_result = {
                'pattern': name,
                'hits': hits,
                'steps': steps,
                'samples_per_step': samples_per_step,
                'expected_triggers_per_bar': len(expected_triggers_first_bar),
                'actual_triggers_first_bar': len(first_bar_triggers),
                'timing_errors': timing_errors,
                'max_error_samples': float(max_error),
                'passed': passed
            }
            results['tests'].append(test_result)

            print(f"    Timing errors: {[f'{e:+.0f}' for e in timing_errors]}")
            print(f"    Max error: {max_error:.1f} samples [{status}]")
        else:
            print(f"    ERROR: Could not compare triggers")
            results['tests'].append({
                'pattern': name,
                'error': 'No matching triggers',
                'passed': False
            })

    with open('output/seq_euclid_timing.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: output/seq_euclid_timing.json")

    return results


# =============================================================================
# Test 12: LFO Zero-Crossing Precision
# =============================================================================

def test_lfo_zero_crossing_precision():
    """Test LFO zero-crossing timing at sample level."""
    print("\nTest 12: LFO Zero-Crossing Precision")
    print("=" * 60)

    sr = 48000
    bpm = 120
    samples_per_beat = sr * 60.0 / bpm

    results = {'sample_rate': sr, 'bpm': bpm, 'samples_per_beat': samples_per_beat, 'tests': []}

    # Test SAW LFO at 1 cycle per beat
    # SAW goes from -1 to +1, so crosses zero at phase 0.5 (mid-beat)
    # and wraps at phase 1.0 (beat boundary, going from +1 to -1)

    num_beats = 100
    duration = num_beats * 60.0 / bpm

    host = SequencerTestHost(sr, bpm)
    host.create_lfo_program(1.0, shape=2, state_id=1300)  # SAW shape
    output = host.run(duration)

    # Find positive-going zero crossings (from negative to positive)
    # For SAW: this happens at phase 0.5, i.e., mid-beat
    zero_crossings_pos = []
    for i in range(1, len(output)):
        if output[i-1] < 0 and output[i] >= 0:
            # Interpolate exact position
            t = -output[i-1] / (output[i] - output[i-1])
            zero_crossings_pos.append(i - 1 + t)

    # Expected positions: at mid-beat (0.5 * samples_per_beat from each beat start)
    expected_crossings = [(beat + 0.5) * samples_per_beat for beat in range(num_beats)]

    print(f"  SAW LFO @ 1 cycle/beat, {num_beats} beats:")
    print(f"    Positive zero crossings: {len(zero_crossings_pos)} (expected {num_beats})")

    # Calculate timing errors
    timing_errors = []
    for i, expected in enumerate(expected_crossings):
        if i < len(zero_crossings_pos):
            actual = zero_crossings_pos[i]
            error = actual - expected
            timing_errors.append(error)

    if timing_errors:
        max_error = max(abs(e) for e in timing_errors)
        mean_error = sum(timing_errors) / len(timing_errors)
        drift = timing_errors[-1] if timing_errors else 0

        passed = max_error <= 1.5  # Within 1.5 samples
        status = "PASS" if passed else "FAIL"

        results['tests'].append({
            'shape': 'SAW',
            'freq_mult': 1.0,
            'num_beats': num_beats,
            'zero_crossings_detected': len(zero_crossings_pos),
            'max_error_samples': float(max_error),
            'mean_error_samples': float(mean_error),
            'final_drift_samples': float(drift),
            'first_10_errors': timing_errors[:10],
            'last_10_errors': timing_errors[-10:] if len(timing_errors) >= 10 else timing_errors,
            'passed': passed
        })

        print(f"    Max error: {max_error:.3f} samples [{status}]")
        print(f"    Mean error: {mean_error:.3f} samples")
        print(f"    Final drift: {drift:.3f} samples")
        print(f"    First 5 errors: {[f'{e:.2f}' for e in timing_errors[:5]]}")
    else:
        print(f"    ERROR: No zero crossings detected!")
        results['tests'].append({
            'shape': 'SAW',
            'error': 'No zero crossings detected',
            'passed': False
        })

    # Also test phase wrapping (negative-going crossings at beat boundaries)
    print(f"\n  Testing phase wrap (beat boundaries)...")
    negative_crossings = []
    for i in range(1, len(output)):
        if output[i-1] > 0.5 and output[i] < -0.5:  # Large drop = phase wrap
            # This happens at beat boundaries
            negative_crossings.append(i)

    expected_wraps = [int(beat * samples_per_beat) for beat in range(1, num_beats)]
    wrap_errors = []
    for i, expected in enumerate(expected_wraps):
        if i < len(negative_crossings):
            actual = negative_crossings[i]
            error = actual - expected
            wrap_errors.append(error)

    if wrap_errors:
        max_wrap_error = max(abs(e) for e in wrap_errors)
        passed = max_wrap_error <= 1.5
        status = "PASS" if passed else "FAIL"

        results['tests'].append({
            'measurement': 'phase_wrap',
            'num_wraps': len(negative_crossings),
            'max_error_samples': float(max_wrap_error),
            'passed': passed
        })

        print(f"    Phase wraps detected: {len(negative_crossings)} (expected {num_beats - 1})")
        print(f"    Max wrap timing error: {max_wrap_error:.2f} samples [{status}]")

    with open('output/seq_lfo_zero_crossing.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: output/seq_lfo_zero_crossing.json")

    return results


# =============================================================================
# Test 13: Cross-Opcode Timing Alignment
# =============================================================================

def test_cross_opcode_alignment():
    """Test timing alignment between different sequencer opcodes."""
    print("\nTest 13: Cross-Opcode Timing Alignment")
    print("=" * 60)

    sr = 48000
    bpm = 120
    samples_per_beat = sr * 60.0 / bpm

    results = {'sample_rate': sr, 'bpm': bpm, 'tests': []}

    # Run CLOCK, TRIGGER, and LFO together and verify they align
    duration = 10 * 60.0 / bpm  # 10 beats

    # CLOCK - beat phase
    host_clock = SequencerTestHost(sr, bpm)
    host_clock.create_clock_program(phase_type=0, state_id=1400)
    clock_output = host_clock.run(duration)

    # TRIGGER - division=1 (quarter notes)
    host_trigger = SequencerTestHost(sr, bpm)
    host_trigger.create_trigger_program(1.0, state_id=1401)
    trigger_output = host_trigger.run(duration)

    # LFO - SAW at 1 cycle/beat
    host_lfo = SequencerTestHost(sr, bpm)
    host_lfo.create_lfo_program(1.0, shape=2, state_id=1402)
    lfo_output = host_lfo.run(duration)

    print("\n  Comparing beat boundaries across opcodes...")

    # Find beat boundaries from each source
    # CLOCK: phase wraps (goes from ~1 to ~0)
    clock_wraps = []
    for i in range(1, len(clock_output)):
        if clock_output[i-1] > 0.9 and clock_output[i] < 0.1:
            clock_wraps.append(i)

    # TRIGGER: rising edges (exclude initial trigger for consistency with wrap detection)
    trigger_edges = count_triggers(trigger_output, threshold=0.5, include_initial=False)

    # LFO SAW: phase wraps (large negative jump)
    lfo_wraps = []
    for i in range(1, len(lfo_output)):
        if lfo_output[i-1] > 0.5 and lfo_output[i] < -0.5:
            lfo_wraps.append(i)

    print(f"    CLOCK wraps detected: {len(clock_wraps)}")
    print(f"    TRIGGER edges detected: {len(trigger_edges)}")
    print(f"    LFO wraps detected: {len(lfo_wraps)}")

    # Compare timing of beat boundaries
    min_beats = min(len(clock_wraps), len(trigger_edges), len(lfo_wraps))

    alignment_errors = []
    for i in range(min_beats):
        clock_pos = clock_wraps[i]
        trigger_pos = trigger_edges[i]
        lfo_pos = lfo_wraps[i]

        # Calculate max difference between any two
        diff_ct = abs(clock_pos - trigger_pos)
        diff_cl = abs(clock_pos - lfo_pos)
        diff_tl = abs(trigger_pos - lfo_pos)
        max_diff = max(diff_ct, diff_cl, diff_tl)

        alignment_errors.append({
            'beat': i + 1,
            'clock': clock_pos,
            'trigger': trigger_pos,
            'lfo': lfo_pos,
            'max_diff': max_diff
        })

    if alignment_errors:
        max_misalignment = max(e['max_diff'] for e in alignment_errors)
        passed = max_misalignment <= 2  # Within 2 samples
        status = "PASS" if passed else "FAIL"

        results['alignment'] = {
            'beats_compared': min_beats,
            'max_misalignment_samples': max_misalignment,
            'passed': passed,
            'first_5_beats': alignment_errors[:5]
        }

        print(f"\n    Max misalignment: {max_misalignment} samples [{status}]")
        print(f"\n    First 5 beat boundaries:")
        for e in alignment_errors[:5]:
            print(f"      Beat {e['beat']}: CLOCK={e['clock']}, TRIGGER={e['trigger']}, "
                  f"LFO={e['lfo']}, diff={e['max_diff']}")

    with open('output/seq_cross_opcode.json', 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: output/seq_cross_opcode.json")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    os.makedirs('output', exist_ok=True)

    print("Cedar Sequencer Quality Tests")
    print("=" * 60)
    print()

    test_clock_phase()
    test_lfo_shapes()
    test_lfo_freq_sync()
    test_lfo_pwm()
    test_trigger_division()
    test_euclid_patterns()
    test_euclid_rotation()
    test_trigger_audio()

    # Sample-accurate precision tests
    test_clock_phase_sample_accuracy()
    test_trigger_long_term_precision()
    test_euclid_timing_precision()
    test_lfo_zero_crossing_precision()
    test_cross_opcode_alignment()

    print()
    print("=" * 60)
    print("All sequencer tests complete. Results saved to output/")
