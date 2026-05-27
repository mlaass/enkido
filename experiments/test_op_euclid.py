"""
Euclid Opcode Quality Tests (Cedar Engine)
============================================
Tests for EUCLID opcode: pattern accuracy, rotation, timing precision,
and cross-opcode alignment.
"""

import numpy as np
import matplotlib.pyplot as plt
import json
import os
import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import NumpyEncoder, save_wav
from visualize import save_figure

OUT = output_dir("op_euclid")


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

    def create_euclid_program(self, hits: int, steps: int, rotation: int = 0,
                              dur: float | None = None, state_id: int = 1):
        """Create EUCLID program.

        Args:
            hits: Number of hits in pattern
            steps: Total steps in pattern
            rotation: Pattern rotation
            dur: Pattern span in cycles. If None, uses make_ternary (slot 3
                 BUFFER_UNUSED → opcode falls back to its built-in default 4.0).
                 If set, uses make_quinary and wires the value into slot 3.
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

        if dur is None:
            self.program.append(
                cedar.Instruction.make_ternary(cedar.Opcode.EUCLID, 10, 1, 2, 3, state_id)
            )
        else:
            self.vm.set_param("dur", float(dur))
            self.program.append(
                cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 4, cedar.hash("dur"))
            )
            # slot 4 is unused — same buffer as dur is fine, opcode never reads it
            self.program.append(
                cedar.Instruction.make_quinary(cedar.Opcode.EUCLID, 10, 1, 2, 3, 4, 4, state_id)
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


# =============================================================================
# Test 1: EUCLID Pattern Accuracy
# =============================================================================

def test_euclid_patterns():
    """Test EUCLID pattern generation accuracy."""
    print("Test 1: EUCLID Pattern Accuracy")
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
    save_figure(fig, os.path.join(OUT, 'euclid.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'euclid.png')}")

    with open(os.path.join(OUT, 'euclid.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'euclid.json')}")

    return results


# =============================================================================
# Test 2: EUCLID Rotation
# =============================================================================

def test_euclid_rotation():
    """Test EUCLID pattern rotation."""
    print("\nTest 2: EUCLID Rotation")
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
    save_figure(fig, os.path.join(OUT, 'euclid_rotation.png'))
    print(f"\n  Saved: {os.path.join(OUT, 'euclid_rotation.png')}")

    with open(os.path.join(OUT, 'euclid_rotation.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"  Saved: {os.path.join(OUT, 'euclid_rotation.json')}")

    return results


# =============================================================================
# Test 3: EUCLID Step Timing Precision
# =============================================================================

def test_euclid_timing_precision():
    """Test EUCLID trigger timing at sample level."""
    print("\nTest 3: EUCLID Step Timing Precision")
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

    with open(os.path.join(OUT, 'euclid_timing.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: {os.path.join(OUT, 'euclid_timing.json')}")

    return results


# =============================================================================
# Test 4: EUCLID dur parameter (pattern span in cycles)
# =============================================================================

def test_euclid_dur_parameter():
    """
    Verify the `dur` parameter scales pattern span linearly.

    Expected behavior (per implementation):
    - dur=N stretches the pattern over N cycles (= N beats under cycle=beat).
    - Default (no slot wired) = 4 cycles, so E(3,8) defaults to 3 hits/bar at 4/4.
    - Trigger count over a long window scales as hits * total_cycles / (steps * dur)
      ... well, more simply: pattern repeats once every (dur * spb) samples, so total
      triggers ≈ hits * (duration_sec * bpm / 60) / dur.

    If this test fails, check op_euclid in cedar/include/cedar/opcodes/sequencing.hpp.
    """
    print("\nTest 4: EUCLID dur parameter (pattern span)")
    print("=" * 60)

    sr = 48000
    bpm = 120
    spb = sr * 60.0 / bpm  # samples per beat (= per cycle)
    # ≥300s simulated audio per CLAUDE.md DSP testing guidelines for time-based opcodes.
    duration = 320.0

    results = {'sample_rate': sr, 'bpm': bpm, 'duration_sec': duration, 'tests': []}

    cases = [
        # (hits, steps, dur, label)
        (3, 8, 1.0,  "E(3,8) dur=1 (1 beat span — legacy fast feel)"),
        (3, 8, None, "E(3,8) dur=default (slot unwired → 4)"),
        (3, 8, 4.0,  "E(3,8) dur=4 (1 bar at 4/4 — new default)"),
        (3, 8, 8.0,  "E(3,8) dur=8 (2 bars span)"),
    ]

    trigger_counts = {}
    for i, (hits, steps, dur, label) in enumerate(cases):
        host = SequencerTestHost(sr, bpm)
        host.create_euclid_program(hits, steps, rotation=0, dur=dur,
                                    state_id=1300 + i)
        output = host.run(duration)
        triggers = count_triggers(output, threshold=0.5)
        n_triggers = len(triggers)

        # dur=None should match dur=4.0 (default fallback)
        effective_dur = 4.0 if dur is None else dur
        cycles_in_window = duration * bpm / 60.0
        # Number of full pattern repetitions = cycles_in_window / dur
        expected_repetitions = cycles_in_window / effective_dur
        expected_triggers = hits * expected_repetitions

        tolerance = max(2, expected_triggers * 0.02)  # 2% or ≥2 triggers
        passed = abs(n_triggers - expected_triggers) <= tolerance

        # Save WAV for human listening
        wav_path = os.path.join(OUT, f"dur_{i}_{('default' if dur is None else dur)}.wav")
        save_wav(wav_path, output, sr)
        print(f"  {label}")
        print(f"    Triggers: {n_triggers} (expected ~{expected_triggers:.1f})")
        print(f"    Saved {wav_path} — listen for {hits} hits per {effective_dur:.0f} beats")
        status = "✓ PASS" if passed else "✗ FAIL"
        print(f"    {status}")
        results['tests'].append({
            'label': label,
            'hits': hits, 'steps': steps, 'dur': dur,
            'triggers': n_triggers,
            'expected': float(expected_triggers),
            'passed': bool(passed),
        })

        trigger_counts[i] = n_triggers

    # Cross-check: dur=1 should produce ~4x triggers compared to dur=4
    if 0 in trigger_counts and 2 in trigger_counts:
        ratio = trigger_counts[0] / max(1, trigger_counts[2])
        ratio_ok = abs(ratio - 4.0) < 0.1
        print(f"\n  Ratio dur=1 / dur=4 = {ratio:.3f} (expected 4.0) "
              f"{'✓ PASS' if ratio_ok else '✗ FAIL'}")
        results['ratio_dur1_over_dur4'] = ratio

    # Cross-check: default slot wiring matches dur=4 within ±1%
    if 1 in trigger_counts and 2 in trigger_counts:
        default_match = abs(trigger_counts[1] - trigger_counts[2]) <= max(1, trigger_counts[2] * 0.01)
        print(f"  Default (unwired) trigger count = {trigger_counts[1]}, "
              f"dur=4 = {trigger_counts[2]} "
              f"{'✓ PASS' if default_match else '✗ FAIL'}")
        results['default_matches_dur4'] = bool(default_match)

    with open(os.path.join(OUT, 'euclid_dur.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: {os.path.join(OUT, 'euclid_dur.json')}")

    return results


# =============================================================================
# Test 5: Cross-Opcode Timing Alignment
# =============================================================================

def test_cross_opcode_alignment():
    """Test timing alignment between different sequencer opcodes."""
    print("\nTest 4: Cross-Opcode Timing Alignment")
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

    with open(os.path.join(OUT, 'cross_opcode.json'), 'w') as f:
        json.dump(results, f, indent=2, cls=NumpyEncoder)
    print(f"\n  Saved: {os.path.join(OUT, 'cross_opcode.json')}")

    return results


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print("Cedar Euclid Opcode Tests")
    print("=" * 60)
    print()

    test_euclid_patterns()
    test_euclid_rotation()
    test_euclid_timing_precision()
    test_euclid_dur_parameter()
    test_cross_opcode_alignment()

    print()
    print("=" * 60)
    print("All euclid tests complete.")
