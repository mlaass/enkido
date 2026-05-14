"""
Test: STEREO_INPUT dispatch flag
=================================
Verifies that setting Instruction.flags = STEREO_INPUT_FLAG on a stereo-native
DSP op makes the opcode read inputs[0] for L and inputs[0]+1 for R, writing
out_buffer (M) and out_buffer+1 (M+1) with independent per-channel state in a
single dispatch.

Expected behavior (per prd-stereo-native-opcodes.md — auto-lift retired in
Phase 5; STEREO_INPUT is now only ever paired with STEREO_OUTPUT):
- When STEREO_INPUT is set, the opcode reads two input buffers (N and N+1)
  and writes two output buffers (M and M+1) in one stereo-native dispatch.
- Left and right have distinct filter memory: if L gets a sine and R gets
  silence, the right output remains silent (filter state doesn't leak
  across channels).
- A mono primary input (flag clear) reads inputs[0] for both internal lanes;
  its L lane matches the stereo run's L lane sample-for-sample.

If this test fails, check the stereo-native FILTER_SVF_LP body in
cedar/include/cedar/opcodes/filters.hpp.
"""

import os
import numpy as np

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import save_wav

OUT = output_dir("op_stereo_input_flag")


def test_independent_channel_state():
    print("Test: STEREO_INPUT → independent per-channel state (single dispatch)")
    sr = 48000

    # Build inputs: left gets sine, right gets silence. Adjacent buffers (4, 5).
    # FILTER_SVF_LP needs 3 inputs (signal, cutoff, q). We put:
    #   buf 4 = left input sine
    #   buf 5 = right input silence
    #   buf 6 = cutoff (500 Hz)
    #   buf 7 = q (0.707)
    # out at buf 8 (and VM will write buf 9 for right pass)
    host = CedarTestHost(sr)
    host.vm.set_param("cutoff", 500.0)
    host.vm.set_param("q", 0.707)

    # Build a program that fills buf 6 and buf 7 then runs the lifted filter.
    state_id = cedar.hash("stereo_test/L")
    lp_inst = cedar.Instruction.make_ternary(
        cedar.Opcode.FILTER_SVF_LP, 8, 4, 10, 11, state_id
    )
    lp_inst.flags = cedar.STEREO_INPUT_FLAG

    program = [
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash("cutoff")),
        cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash("q")),
        lp_inst,
        # OUTPUT: route L=buf 8, R=buf 9 to host output
        cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 8, 9),
    ]
    host.vm.load_program(program)

    # Run 4 blocks (enough for filter to settle) with L=sine, R=silence
    t = np.arange(cedar.BLOCK_SIZE) / sr
    sine_block = np.sin(2 * np.pi * 440 * t).astype(np.float32)
    silence = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)

    all_l = []
    all_r = []
    for _ in range(8):
        host.vm.set_buffer(4, sine_block)
        host.vm.set_buffer(5, silence)
        l, r = host.vm.process()
        all_l.append(l.copy())
        all_r.append(r.copy())

    left_out = np.concatenate(all_l)
    right_out = np.concatenate(all_r)

    left_peak = float(np.max(np.abs(left_out)))
    right_peak = float(np.max(np.abs(right_out)))

    print(f"  Left peak:  {left_peak:.4f}")
    print(f"  Right peak: {right_peak:.4e}")

    if left_peak > 0.5:
        print(f"  ✓ PASS: left channel produces filtered sine")
    else:
        print(f"  ✗ FAIL: left channel silent ({left_peak:.4e})")

    if right_peak < 1e-4:
        print(f"  ✓ PASS: right channel silent (no state leak from L to R)")
    else:
        print(f"  ✗ FAIL: right channel non-silent ({right_peak:.4e}) — state leak!")

    # Sanity: save WAV
    stereo = np.column_stack([left_out, right_out])
    wav_path = os.path.join(OUT, "stereo_input_flag.wav")
    save_wav(wav_path, stereo, sr)
    print(f"  Saved {wav_path}")


def test_plain_vs_lifted_left_match():
    """L lane of a STEREO_INPUT run matches a mono-primary-input run sample-for-sample."""
    print("Test: L lane of STEREO_INPUT matches mono-primary-input run")
    sr = 48000

    def run(stereo_flag: bool) -> np.ndarray:
        host = CedarTestHost(sr)
        host.vm.set_param("cutoff", 500.0)
        host.vm.set_param("q", 0.707)
        state_id = cedar.hash("stereo_test/L")
        lp_inst = cedar.Instruction.make_ternary(
            cedar.Opcode.FILTER_SVF_LP, 8, 4, 10, 11, state_id
        )
        if stereo_flag:
            lp_inst.flags = cedar.STEREO_INPUT_FLAG
        program = [
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 10, cedar.hash("cutoff")),
            cedar.Instruction.make_nullary(cedar.Opcode.ENV_GET, 11, cedar.hash("q")),
            lp_inst,
            cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 8, 8),
        ]
        host.vm.load_program(program)

        t = np.arange(cedar.BLOCK_SIZE) / sr
        sine_block = np.sin(2 * np.pi * 440 * t).astype(np.float32)
        silence = np.zeros(cedar.BLOCK_SIZE, dtype=np.float32)

        out = []
        for _ in range(8):
            host.vm.set_buffer(4, sine_block)
            host.vm.set_buffer(5, silence)
            l, _ = host.vm.process()
            out.append(l.copy())
        return np.concatenate(out)

    plain = run(False)
    lifted = run(True)

    max_diff = float(np.max(np.abs(plain - lifted)))
    if max_diff < 1e-6:
        print(f"  ✓ PASS: L channel bit-identical to plain mono run (max diff {max_diff:.2e})")
    else:
        print(f"  ✗ FAIL: L channel differs from plain mono (max diff {max_diff:.2e})")


if __name__ == "__main__":
    print("=" * 60)
    print("STEREO_INPUT FLAG TESTS")
    print("=" * 60)
    print()
    test_independent_channel_state()
    print()
    test_plain_vs_lifted_left_match()
    print()
    print("=" * 60)
    print("STEREO_INPUT FLAG TESTS COMPLETE")
    print("=" * 60)
