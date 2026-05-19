"""
Test: REVERB_DATTORRO (Dattorro Reverb)
=======================================
Tests Dattorro reverb impulse response, decay characteristics, AND the
Phase 0+1 stereo-native conversion (prd-stereo-native-opcodes).

Expected behavior:
- Impulse response shows a smooth decay tail (per channel)
- Higher decay values produce longer reverb tails
- Predelay introduces a gap before the reverb onset
- Output is STEREO: writes to out_buffer (L) and out_buffer + 1 (R)
- L/R are decorrelated for non-trivial input (cross-correlation < 1.0)
- Cross-coupling: STEREO_OUTPUT | STEREO_INPUT feeds opposing tanks

If this test fails, check the implementation in cedar/include/cedar/opcodes/reverbs.hpp
"""

import os
import numpy as np
import matplotlib.pyplot as plt

import cedar_core as cedar
from cedar_testing import CedarTestHost, output_dir
from utils import gen_impulse, gen_noise_pulse, save_wav
from visualize import save_figure

OUT = output_dir("op_dattorro")


def test_reverb_decay():
    """Mono impulse → stereo-native Dattorro → stereo decay tail."""
    print("Test: Reverb Impulse Response (stereo-native)")

    sr = 48000
    host = CedarTestHost(sr)

    # Short impulse to trigger reverb
    impulse = gen_impulse(2.0, sr)

    # Dattorro Reverb Params
    buf_in = 0
    buf_decay = host.set_param("decay", 0.95)  # Long tail
    buf_predelay = host.set_param("predelay", 20.0)  # 20ms
    buf_indiff = host.set_param("input_diffusion", 0.75)
    buf_decdiff = host.set_param("decay_diffusion", 0.625)

    # Dattorro is stereo-native: writes out_buffer (L) and out_buffer+1 (R).
    # Allocate two contiguous output buffers (1 = L, 2 = R) and tag the
    # instruction with STEREO_OUTPUT. STEREO_INPUT is NOT set because the
    # primary input is mono (the impulse buffer in slot 0).
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.REVERB_DATTORRO, 1, buf_in, buf_decay, buf_predelay,
        buf_indiff, buf_decdiff, cedar.hash("verb") & 0xFFFF
    )
    inst.rate = (0 << 4) | 0  # No mod, no damping for clear tail
    inst.flags = cedar.STEREO_OUTPUT_FLAG

    host.load_instruction(inst)
    # OUTPUT: inputs[0] = L (buf 1), inputs[1] = R (buf 2)
    out_inst = cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 1, 2)
    host.load_instruction(out_inst)

    # Custom block loop that collects both L and R from the stereo output bus
    host.vm.load_program(host.program)
    n_samples = len(impulse)
    n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    padded_len = n_blocks * cedar.BLOCK_SIZE
    input_padded = np.zeros(padded_len, dtype=np.float32)
    input_padded[:n_samples] = impulse
    out_l_chunks, out_r_chunks = [], []
    for i in range(n_blocks):
        start = i * cedar.BLOCK_SIZE
        end = start + cedar.BLOCK_SIZE
        host.vm.set_buffer(0, input_padded[start:end])
        l, r = host.vm.process()
        out_l_chunks.append(l)
        out_r_chunks.append(r)
    output_l = np.concatenate(out_l_chunks)[:n_samples]
    output_r = np.concatenate(out_r_chunks)[:n_samples]

    # Save WAV for human evaluation
    wav_path = os.path.join(OUT, "dattorro_impulse.wav")
    stereo_arr = np.stack([output_l, output_r], axis=1)
    save_wav(wav_path, stereo_arr, sr)
    print(f"  Saved {wav_path} - Listen for stereo reverb tail with width")

    # L and R should be decorrelated for non-trivial decay tail
    # (mono input but tank cross-coupling produces L != R).
    nonzero = (np.abs(output_l) > 1e-6) | (np.abs(output_r) > 1e-6)
    if np.any(nonzero):
        l = output_l[nonzero]
        r = output_r[nonzero]
        # Pearson correlation
        if np.std(l) > 1e-9 and np.std(r) > 1e-9:
            corr = float(np.corrcoef(l, r)[0, 1])
        else:
            corr = 1.0
        print(f"  L/R Pearson correlation: {corr:.4f}")
        if corr < 0.999:
            print("  ✓ PASS: L and R are non-trivially decorrelated")
        else:
            print("  ✗ FAIL: L and R are virtually identical")

    # Plot log-magnitude envelope, L and R overlaid
    time = np.arange(len(output_l)) / sr
    env_l_db = 20 * np.log10(np.abs(output_l) + 1e-6)
    env_r_db = 20 * np.log10(np.abs(output_r) + 1e-6)

    fig, ax = plt.subplots(figsize=(12, 6))
    ax.plot(time, env_l_db, linewidth=0.5, color='purple', label='L')
    ax.plot(time, env_r_db, linewidth=0.5, color='orange', alpha=0.7, label='R')
    ax.set_title("Dattorro Reverb Impulse Response (Decay 0.95, stereo-native)")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Amplitude (dB)")
    ax.set_ylim(-100, 0)
    ax.legend()
    ax.grid(True, alpha=0.3)

    save_figure(fig, os.path.join(OUT, "reverb_ir.png"))
    print(f"  Saved {os.path.join(OUT, 'reverb_ir.png')}")


def estimate_rt60(signal, sr, threshold_db=-60):
    """Estimate RT60 from impulse response envelope."""
    env = np.abs(signal)
    window = int(0.01 * sr)
    if window > 0:
        env = np.convolve(env, np.ones(window) / window, mode='same')
    peak = np.max(env)
    if peak < 1e-10:
        return 0.0
    peak_idx = np.argmax(env)
    env_db = 20 * np.log10(env / peak + 1e-10)
    below = np.where(env_db[peak_idx:] < threshold_db)[0]
    if len(below) > 0:
        return below[0] / sr
    return (len(signal) - peak_idx) / sr


def test_tail_length():
    """
    Test reverb tail length with short noise pulse input at different decay values.

    Expected:
    - Noise pulse excites all tank modes for realistic RT60 measurement
    - RT60 increases monotonically with decay
    - Each setting produces audible reverb tail in WAV output
    """
    print("Test: REVERB_DATTORRO Tail Length (noise pulse)")

    sr = 48000
    duration = 5.0

    decay_values = [0.3, 0.6, 0.8, 0.95]
    rt60s = []

    fig, axes = plt.subplots(len(decay_values), 1, figsize=(12, 3 * len(decay_values)))
    fig.suptitle("Dattorro Tail Length — Noise Pulse Excitation")

    for idx, decay in enumerate(decay_values):
        noise_pulse = gen_noise_pulse(duration, sr, pulse_ms=10)

        host = CedarTestHost(sr)
        buf_in = 0
        buf_decay = host.set_param("decay", decay)
        buf_predelay = host.set_param("predelay", 10.0)
        buf_indiff = host.set_param("input_diffusion", 0.75)
        buf_decdiff = host.set_param("decay_diffusion", 0.625)

        inst = cedar.Instruction.make_quinary(
            cedar.Opcode.REVERB_DATTORRO, 1, buf_in, buf_decay, buf_predelay,
            buf_indiff, buf_decdiff, cedar.hash("dat_tail") & 0xFFFF
        )
        inst.rate = (0 << 4) | 0  # No mod, no damping
        inst.flags = cedar.STEREO_OUTPUT_FLAG
        host.load_instruction(inst)
        # OUTPUT both L (buf 1) and R (buf 2) so the bus contains the full mix
        host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 1, 2))

        # RT60 is measured on the average of L and R to approximate the
        # historic mono (L+R)*0.5 output. Inline block loop because
        # process() only returns L from the stereo bus.
        host.vm.load_program(host.program)
        n_samples = len(noise_pulse)
        n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
        padded_len = n_blocks * cedar.BLOCK_SIZE
        in_padded = np.zeros(padded_len, dtype=np.float32)
        in_padded[:n_samples] = noise_pulse
        l_chunks, r_chunks = [], []
        for k in range(n_blocks):
            s = k * cedar.BLOCK_SIZE
            e = s + cedar.BLOCK_SIZE
            host.vm.set_buffer(0, in_padded[s:e])
            l, r = host.vm.process()
            l_chunks.append(l)
            r_chunks.append(r)
        out_l = np.concatenate(l_chunks)[:n_samples]
        out_r = np.concatenate(r_chunks)[:n_samples]
        output = 0.5 * (out_l + out_r)
        rt60 = estimate_rt60(output, sr)
        rt60s.append(rt60)

        wav_path = os.path.join(OUT, f"dattorro_tail_decay{decay}.wav")
        save_wav(wav_path, np.stack([out_l, out_r], axis=1), sr)

        time = np.arange(len(output)) / sr
        env_db = 20 * np.log10(np.abs(output) + 1e-10)
        axes[idx].plot(time, env_db, linewidth=0.5)
        axes[idx].axhline(-60, color='red', linestyle='--', alpha=0.5)
        axes[idx].set_title(f'Decay={decay} (RT60≈{rt60:.2f}s)')
        axes[idx].set_ylim(-100, 0)
        axes[idx].set_ylabel('dB')
        axes[idx].grid(True, alpha=0.3)

        print(f"  Decay {decay}: RT60 ≈ {rt60:.2f}s")
        print(f"  Saved {wav_path} - Listen for reverb tail length")

    axes[-1].set_xlabel('Time (s)')
    plt.tight_layout()
    save_figure(fig, os.path.join(OUT, "dattorro_tail_length.png"))
    print(f"  Saved {os.path.join(OUT, 'dattorro_tail_length.png')}")

    is_monotonic = all(rt60s[i] <= rt60s[i + 1] for i in range(len(rt60s) - 1))
    if is_monotonic:
        print("  ✓ PASS: RT60 increases with decay (noise pulse)")
    else:
        print(f"  ✗ FAIL: RT60 not monotonic: {rt60s}")


def test_stereo_input_cross_coupling():
    """
    Stereo input → cross-coupled figure-8 tanks. Inject an impulse on L only
    (R = silence); the R output should pick up energy through the
    tank_feedback[0] → right_in feedback path within a few blocks.
    """
    print("Test: Stereo Input Cross-Coupling")

    sr = 48000
    host = CedarTestHost(sr)

    # L impulse, R silence
    n_samples = 2 * sr
    left = np.zeros(n_samples, dtype=np.float32)
    left[0] = 1.0
    right = np.zeros(n_samples, dtype=np.float32)

    # Dattorro reads L from buf 0 and R from buf 1 (adjacency convention).
    # set_param returns buffer indices ≥ 10 so this is safe.
    buf_in = 0
    buf_decay = host.set_param("decay", 0.85)
    buf_predelay = host.set_param("predelay", 10.0)
    buf_indiff = host.set_param("input_diffusion", 0.75)
    buf_decdiff = host.set_param("decay_diffusion", 0.625)

    # Output to buf 2 (L) and buf 3 (R) — keep clear of the stereo INPUT pair.
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.REVERB_DATTORRO, 2, buf_in, buf_decay, buf_predelay,
        buf_indiff, buf_decdiff, cedar.hash("dat_xc") & 0xFFFF
    )
    inst.rate = 0
    inst.flags = cedar.STEREO_OUTPUT_FLAG | cedar.STEREO_INPUT_FLAG
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 2, 3))

    # Run block-by-block feeding both inputs
    host.vm.load_program(host.program)
    n_blocks = (n_samples + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    padded_len = n_blocks * cedar.BLOCK_SIZE
    l_in = np.zeros(padded_len, dtype=np.float32); l_in[:n_samples] = left
    r_in = np.zeros(padded_len, dtype=np.float32); r_in[:n_samples] = right
    l_chunks, r_chunks = [], []
    for i in range(n_blocks):
        s = i * cedar.BLOCK_SIZE
        e = s + cedar.BLOCK_SIZE
        host.vm.set_buffer(0, l_in[s:e])
        host.vm.set_buffer(1, r_in[s:e])
        l, r = host.vm.process()
        l_chunks.append(l)
        r_chunks.append(r)
    out_l = np.concatenate(l_chunks)[:n_samples]
    out_r = np.concatenate(r_chunks)[:n_samples]

    # Save WAV
    wav_path = os.path.join(OUT, "dattorro_cross_coupling.wav")
    save_wav(wav_path, np.stack([out_l, out_r], axis=1), sr)
    print(f"  Saved {wav_path} - Listen for energy in BOTH L and R from a left-only impulse")

    # Energy in R should be substantial despite R input being silent —
    # cross-coupling via tank_feedback[0] → right_in feeds it.
    rms_l = float(np.sqrt(np.mean(out_l ** 2)))
    rms_r = float(np.sqrt(np.mean(out_r ** 2)))
    if rms_r > 0.01 * rms_l:
        print(f"  ✓ PASS: cross-coupling visible — rms_l={rms_l:.5f}, rms_r={rms_r:.5f}")
    else:
        print(f"  ✗ FAIL: R channel near silent — rms_l={rms_l:.5f}, rms_r={rms_r:.5f}")


# =============================================================================
# ExtendedParams migration tests (prd-extended-params-migration §4.4)
# =============================================================================

def _build_dattorro(host, decay=0.9, predelay=20.0, damping=0.0, mod_depth=0.0,
                     lfo_rate=0.5, init_ext=True, state_name="verb"):
    """Configure host with a stereo-native REVERB_DATTORRO instruction.

    damping/mod_depth moved from inst.rate bit-packing, and lfo_rate from a
    hardcoded constant, to ExtendedParams<5> ([damping, mod_depth, lfo_rate,
    dry, wet]) in prd-extended-params-migration §4.4. When init_ext is False
    the ExtendedParams state is left uninitialized to exercise the opcode's
    nullptr fallback (damping=0, mod_depth=0, lfo_rate=0.5).
    buf_in=0, output L=1 / R=2.
    """
    buf_decay = host.set_param("d_decay", decay)
    buf_predelay = host.set_param("d_predelay", predelay)
    buf_indiff = host.set_param("d_indiff", 0.75)
    buf_decdiff = host.set_param("d_decdiff", 0.625)
    state_id = cedar.hash(state_name) & 0xFFFF
    inst = cedar.Instruction.make_quinary(
        cedar.Opcode.REVERB_DATTORRO, 1, 0, buf_decay, buf_predelay,
        buf_indiff, buf_decdiff, state_id
    )
    inst.rate = 0  # rate field no longer carries damping/mod_depth
    inst.flags = cedar.STEREO_OUTPUT_FLAG
    host.load_instruction(inst)
    host.load_instruction(cedar.Instruction.make_binary(cedar.Opcode.OUTPUT, 0, 1, 2))
    host.vm.load_program(host.program)
    if init_ext:
        host.vm.init_extended_params(
            cedar.ext_params_state_id(state_id),
            np.array([damping, mod_depth, lfo_rate, 1.0, 0.5], dtype=np.float32),
            np.array([0xFFFF] * 5, dtype=np.uint16),
            5,
        )
    return state_id


def _render_stereo_loaded(host, signal):
    """Drive the block loop (no re-load) and collect both L and R."""
    n = len(signal)
    n_blocks = (n + cedar.BLOCK_SIZE - 1) // cedar.BLOCK_SIZE
    padded = np.zeros(n_blocks * cedar.BLOCK_SIZE, dtype=np.float32)
    padded[:n] = signal
    l_chunks, r_chunks = [], []
    for k in range(n_blocks):
        s = k * cedar.BLOCK_SIZE
        host.vm.set_buffer(0, padded[s:s + cedar.BLOCK_SIZE])
        l, r = host.vm.process()
        l_chunks.append(l)
        r_chunks.append(r)
    return (np.concatenate(l_chunks)[:n], np.concatenate(r_chunks)[:n])


def _hf_energy_db(signal, sr, cutoff=4000.0):
    """Total spectral energy above `cutoff` Hz, in dB."""
    spec = np.abs(np.fft.rfft(signal))
    freqs = np.fft.rfftfreq(len(signal), 1.0 / sr)
    hf = spec[freqs >= cutoff]
    return 10.0 * np.log10(np.sum(hf ** 2) + 1e-12)


def test_default_fallback_equivalence():
    """
    Verify the nullptr-fallback (no ExtendedParams init) is identical to an
    explicit-default init (damping=0, mod_depth=0, lfo_rate=0.5, dry=1, wet=0.5).

    Pre-migration, dattorro always ran at inst.rate=0 → damping=0, mod_depth=0
    and lfo_rate hardcoded at 0.5. The post-migration opcode must reproduce
    that exactly when ExtendedParams is absent.

    Acceptance: peak sample error < -60 dBFS between the two renders (L+R).
    """
    print("Test: dattorro default-fallback equivalence")
    sr = 48000
    impulse = gen_impulse(2.0, sr)

    host_fb = CedarTestHost(sr)
    _build_dattorro(host_fb, decay=0.9, init_ext=False, state_name="verb_fb")
    l_fb, r_fb = _render_stereo_loaded(host_fb, impulse)

    host_def = CedarTestHost(sr)
    _build_dattorro(host_def, decay=0.9, damping=0.0, mod_depth=0.0,
                    lfo_rate=0.5, state_name="verb_def")
    l_def, r_def = _render_stereo_loaded(host_def, impulse)

    peak_err = max(float(np.max(np.abs(l_fb - l_def))),
                   float(np.max(np.abs(r_fb - r_def))))
    peak_err_db = 20.0 * np.log10(peak_err + 1e-12)
    print(f"  Peak error fallback vs default-init: {peak_err_db:.1f} dBFS")

    if peak_err_db < -60.0:
        print("  ✓ PASS: nullptr fallback matches explicit defaults")
        return True
    print("  ✗ FAIL: nullptr fallback DIVERGES from explicit defaults")
    return False


def test_damping_tunable():
    """
    Verify dattorro `damping` is now tunable via ExtendedParams.

    Damping is a one-pole lowpass in the tank feedback path: higher damping
    rolls off high frequencies in the reverb tail. A bright noise burst
    through the reverb should have measurably less high-frequency energy in
    the tail at high damping than at low damping.

    Acceptance: HF energy (>4kHz) of the tail drops by ≥ 6 dB from
    damping=0.1 to damping=0.85.
    """
    print("Test: dattorro damping tunability")
    sr = 48000
    burst = gen_noise_pulse(3.0, sr, pulse_ms=50)

    host_lo = CedarTestHost(sr)
    _build_dattorro(host_lo, decay=0.9, damping=0.1, state_name="verb_dlo")
    l_lo, r_lo = _render_stereo_loaded(host_lo, burst)

    host_hi = CedarTestHost(sr)
    _build_dattorro(host_hi, decay=0.9, damping=0.85, state_name="verb_dhi")
    l_hi, r_hi = _render_stereo_loaded(host_hi, burst)

    save_wav(os.path.join(OUT, "damping_low.wav"),
             np.stack([l_lo, r_lo], axis=1), sr)
    save_wav(os.path.join(OUT, "damping_high.wav"),
             np.stack([l_hi, r_hi], axis=1), sr)
    print("  Saved damping_low.wav / damping_high.wav - listen for tail brightness")

    # Measure HF energy of the tail (skip the first 100ms = direct burst).
    tail = slice(int(0.1 * sr), None)
    hf_lo = _hf_energy_db(l_lo[tail] + r_lo[tail], sr)
    hf_hi = _hf_energy_db(l_hi[tail] + r_hi[tail], sr)
    drop_db = hf_lo - hf_hi
    print(f"  HF energy: damping=0.1 → {hf_lo:.1f} dB, damping=0.85 → {hf_hi:.1f} dB")
    print(f"  HF rolloff from damping: {drop_db:.2f} dB")

    if drop_db >= 6.0:
        print("  ✓ PASS: damping param is tunable")
        return True
    print("  ✗ FAIL: damping param has NO measurable effect")
    return False


def test_modulation_tunable():
    """
    Verify dattorro `mod_depth` (and `lfo_rate`) are now tunable via
    ExtendedParams.

    Modulation continuously varies the tank delay lengths. With mod_depth=0
    the tail is static; with mod_depth>0 it is chorused/smeared, so the two
    renders of the same input diverge.

    Acceptance: RMS difference between mod_depth=0 and mod_depth=0.6 tails is
    > -40 dBFS (i.e. clearly non-identical).
    """
    print("Test: dattorro modulation tunability")
    sr = 48000
    burst = gen_noise_pulse(3.0, sr, pulse_ms=50)

    host_off = CedarTestHost(sr)
    _build_dattorro(host_off, decay=0.92, mod_depth=0.0, state_name="verb_moff")
    l_off, r_off = _render_stereo_loaded(host_off, burst)

    host_on = CedarTestHost(sr)
    _build_dattorro(host_on, decay=0.92, mod_depth=0.6, lfo_rate=1.2,
                    state_name="verb_mon")
    l_on, r_on = _render_stereo_loaded(host_on, burst)

    tail = slice(int(0.1 * sr), None)
    diff = np.concatenate([l_off[tail] - l_on[tail], r_off[tail] - r_on[tail]])
    diff_rms = np.sqrt(np.mean(diff ** 2))
    diff_db = 20.0 * np.log10(diff_rms + 1e-12)
    print(f"  RMS difference (mod off vs on): {diff_db:.1f} dBFS")

    if diff_db > -40.0:
        print("  ✓ PASS: mod_depth/lfo_rate are tunable")
        return True
    print("  ✗ FAIL: mod_depth has NO measurable effect")
    return False


if __name__ == "__main__":
    test_reverb_decay()
    test_tail_length()
    test_stereo_input_cross_coupling()
    test_default_fallback_equivalence()
    test_damping_tunable()
    test_modulation_tunable()
