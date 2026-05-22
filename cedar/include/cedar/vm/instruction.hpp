#pragma once

#include <cstdint>

namespace cedar {

// Opcode categories organized for easy extension
// Each category has room for 10 opcodes
enum class Opcode : std::uint8_t {
    // Stack/Constants (0-9)
    NOP = 0,
    PUSH_CONST = 1,   // Fill buffer with constant value
    COPY = 2,         // Copy buffer to buffer

    // Arithmetic (10-19)
    ADD = 10,
    SUB = 11,
    MUL = 12,
    DIV = 13,
    POW = 14,
    NEG = 15,         // Negate

    // Oscillators (20-29)
    OSC_SIN = 20,
    OSC_TRI = 21,
    OSC_SAW = 22,
    OSC_SQR = 23,
    OSC_RAMP = 24,
    OSC_PHASOR = 25,
    OSC_SQR_MINBLEP = 26,      // Square wave with MinBLEP (perfect harmonic purity)
    OSC_SQR_PWM = 27,          // Square wave with PWM (PolyBLEP)
    OSC_SAW_PWM = 28,          // Variable-slope saw (morphs saw↔tri↔ramp)
    OSC_SQR_PWM_MINBLEP = 29,  // Square PWM with MinBLEP (highest quality)

    // Filters (30-39)
    // Note: Opcodes 30-32 removed (biquad filters deprecated in favor of SVF)
    FILTER_SVF_LP = 33,
    FILTER_SVF_HP = 34,
    FILTER_SVF_BP = 35,
    FILTER_MOOG = 36,       // 4-pole Moog-style ladder filter with resonance
    FILTER_DIODE = 37,      // ZDF diode ladder filter (TB-303 acid)
    FILTER_FORMANT = 38,    // 3-band vowel morphing filter
    FILTER_SALLENKEY = 39,  // MS-20 style filter with diode feedback

    // Math (40-49)
    ABS = 40,
    SQRT = 41,
    LOG = 42,
    EXP = 43,
    MIN = 44,
    MAX = 45,
    CLAMP = 46,
    WRAP = 47,
    FLOOR = 48,
    CEIL = 49,

    // Utility (50-59)
    OUTPUT = 50,      // Write to output buffer
    NOISE = 51,       // White noise
    MTOF = 52,        // MIDI to frequency
    DC = 53,          // DC offset
    SLEW = 54,        // Slew rate limiter
    EDGE_OP = 55,     // Edge primitives: rate=0 SAH, 1 gateup, 2 gatedown, 3 counter
    ENV_GET = 56,     // Read external parameter from EnvMap
    STATE_OP = 57,    // User state cell I/O: rate=0 init, 1 load, 2 store
    INPUT = 58,       // Live audio input: copies ctx.input_left/right to adjacent
                      // output buffer pair (out_buffer = L, out_buffer+1 = R).
                      // Writes silence when ctx.input_left/right are null.
    INTERP_TIME = 59, // Time-based interpolator with change detection:
                      // rate=0 linear, 1 ease_in, 2 ease_out, 3 cosine.
                      // in0=target (stereo-capable), in1=ramp time (seconds).

    // Envelopes (60-69) - reserved
    ENV_ADSR = 60,
    ENV_AR = 61,
    ENV_FOLLOWER = 62,

    // Samplers (63-69)
    SAMPLE_PLAY = 63,       // One-shot sample playback
    SAMPLE_PLAY_LOOP = 64,  // Looping sample playback
    SOUNDFONT_VOICE = 65,   // Polyphonic SoundFont playback
    SF_VOICE = 66,          // Single-voice SoundFont player (no voice management; for poly())

    // Delays & Reverbs (70-79)
    DELAY = 70,
    REVERB_FREEVERB = 71,    // Schroeder-Moorer algorithm
    REVERB_DATTORRO = 72,    // Plate reverb
    REVERB_FDN = 73,         // Feedback Delay Network
    DELAY_TAP = 74,          // Read from delay line (coordinated with DELAY_WRITE)
    DELAY_WRITE = 75,        // Write feedback to delay line (coordinated with DELAY_TAP)

    // Effects - Modulation (80-83)
    EFFECT_CHORUS = 80,      // Multi-voice chorus
    EFFECT_FLANGER = 81,     // Modulated delay with feedback
    EFFECT_PHASER = 82,      // Cascaded allpass filters
    EFFECT_COMB = 83,        // Feedback comb filter

    // Effects - Distortion (84-89, 96-99)
    DISTORT_TANH = 84,       // Tanh saturation
    DISTORT_SOFT = 85,       // Polynomial soft clipping
    DISTORT_BITCRUSH = 86,   // Bit/sample rate reduction
    DISTORT_FOLD = 87,       // Wavefolder
    DISTORT_TUBE = 88,       // Asymmetric tube saturation (even harmonics)
    DISTORT_SMOOTH = 89,     // ADAA alias-free saturation

    // Sequencers & Timing (90-95)
    CLOCK = 90,       // Beat/bar phase output (rate field: 0=beat, 1=bar, 2=cycle)
    LFO = 91,         // Beat-synced LFO (reserved field: shape 0-6)
    MIDI_QUERY = 92,  // Runtime MIDI event source (formerly SEQ_STEP). Drains
                      // the live-event ring (and, post-Phase 5, advances the
                      // .mid playback head) into a MidiQueueState::output the
                      // downstream POLY block consumes via seq_state_id.
    EUCLID = 93,      // Euclidean rhythm trigger generator
    TRIGGER = 94,     // Beat-division impulse generator
    TIMELINE = 95,    // Breakpoint automation

    // Effects - Distortion continued (96-99)
    DISTORT_TAPE = 96,       // Tape-style saturation with warmth
    DISTORT_XFMR = 97,       // Transformer saturation (bass emphasis)
    DISTORT_EXCITE = 98,     // Harmonic exciter (HF enhancement)

    // Dynamics (100-109)
    DYNAMICS_COMP = 100,     // Feedforward compressor
    DYNAMICS_LIMITER = 101,  // Brick-wall limiter
    DYNAMICS_GATE = 102,     // Noise gate with hysteresis

    // Oversampled Oscillators (110-119) - for alias-free FM synthesis
    // Note: 2x variants (110, 112, 114, 116) removed — FM detection upgrades 1x directly to 4x
    OSC_SIN_4X = 111,        // 4x oversampled sine
    OSC_SAW_4X = 113,        // 4x oversampled saw with PolyBLEP
    OSC_SQR_4X = 115,        // 4x oversampled square with PolyBLEP
    OSC_TRI_4X = 117,        // 4x oversampled triangle
    OSC_SQR_PWM_4X = 118,    // 4x oversampled PWM square
    OSC_SAW_PWM_4X = 119,    // 4x oversampled variable-slope saw

    // Trigonometric Math (120-129) - pure mathematical functions
    MATH_SIN = 120,          // sin(x) - radians, NOT oscillator
    MATH_COS = 121,          // cos(x)
    MATH_TAN = 122,          // tan(x)
    MATH_ASIN = 123,         // asin(x)
    MATH_ACOS = 124,         // acos(x)
    MATH_ATAN = 125,         // atan(x)
    MATH_ATAN2 = 126,        // atan2(y, x) - binary

    // Hyperbolic Math (130-139)
    MATH_SINH = 130,         // sinh(x)
    MATH_COSH = 131,         // cosh(x)
    MATH_TANH = 132,         // tanh(x) - pure math, also useful as waveshaper

    // Logic & Conditionals (140-149)
    SELECT = 140,            // out = (cond > 0) ? a : b (signal mux)
    CMP_GT = 141,            // out = (a > b) ? 1.0 : 0.0
    CMP_LT = 142,            // out = (a < b) ? 1.0 : 0.0
    CMP_GTE = 143,           // out = (a >= b) ? 1.0 : 0.0
    CMP_LTE = 144,           // out = (a <= b) ? 1.0 : 0.0
    CMP_EQ = 145,            // out = (|a - b| < epsilon) ? 1.0 : 0.0
    CMP_NEQ = 146,           // out = (|a - b| >= epsilon) ? 1.0 : 0.0
    LOGIC_AND = 147,         // out = ((a > 0) && (b > 0)) ? 1.0 : 0.0
    LOGIC_OR = 148,          // out = ((a > 0) || (b > 0)) ? 1.0 : 0.0
    LOGIC_NOT = 149,         // out = (a > 0) ? 0.0 : 1.0

    // Polyphony (150-151)
    POLY_BEGIN = 150,     // Start poly block: rate=body_length, out=mix_buf,
                          // in0=freq, in1=gate, in2=vel, in3=trig, in4=voice_out
    POLY_END = 151,       // End marker for poly block (no data, just terminator)

    // Lazy Queryable Patterns (152-159)
    SEQPAT_QUERY = 152,      // Query sequence system at block boundaries
    SEQPAT_STEP = 153,       // Step through sequence results: out=value, in[0]=velocity, in[1]=trigger
    SEQPAT_TYPE = 154,       // Type ID signal for routing: out=type_id, in[0]=voice_idx
    SEQPAT_GATE = 155,       // Gate signal (1 during event, 0 otherwise): out=gate, in[0]=voice_idx
    SEQPAT_TRANSPORT = 156,  // Trigger-driven clock: out=beat_pos, in[0]=trig, in[1]=step, in[2]=reset, cycle_length packed in in[3]+in[4]
    SEQPAT_PROP = 157,       // Custom-property signal: out=value, rate=slot (0..3), in[0]=voice_idx, in[1]=clock_override
    SEQPAT_FIELD = 158,      // Built-in event field: out=value, rate=field (0=dur, 1=chance, 2=time, 3=note, 4=sample_id), in[0]=voice_idx, in[1]=clock_override
    SEQPAT_PHASE = 159,      // Event-scoped 0..1 phasor (linear ramp across active event's duration): out=phase, in[0]=voice_idx, in[1]=clock_override

    // Array Operations (160-169)
    // Arrays reuse existing BufferPool buffers - elements stored at indices 0..length-1
    // Max 128 elements (matches BLOCK_SIZE). Akkado tracks lengths at compile time.
    ARRAY_PACK = 160,        // Pack up to 5 scalars into array (rate=count, in0-4=values)
    ARRAY_INDEX = 161,       // Per-sample indexing: out[i]=arr[idx[i]] (rate: 0=wrap, 1=clamp)
    ARRAY_UNPACK = 162,      // Extract element: out=fill(arr[rate]) where rate=element index
    ARRAY_LEN = 163,         // Fill with length: out=fill(rate) where rate=array length
    ARRAY_SLICE = 164,       // out=arr[start:end] (in0=arr, in1=start, in2=end)
    ARRAY_CONCAT = 165,      // out=concat(a,b) (in0=a, in1=b, rate=len_a, inputs[2]=len_b)
    ARRAY_PUSH = 166,        // out=push(arr,elem) (in0=arr, in1=elem, rate=arr_len)
    ARRAY_SUM = 167,         // out=fill(sum(arr)) (in0=arr, rate=length)
    ARRAY_REVERSE = 168,     // out=reverse(arr) (in0=arr, rate=length)
    ARRAY_FILL = 169,        // out=fill(value,length) (in0=value, rate=length)

    // Stereo Operations (170-179)
    // True stereo effects requiring cross-channel processing
    PAN = 170,               // Mono to stereo panning: out=L, out+1=R, in0=mono, in1=pan (-1..1)
    WIDTH = 171,             // Stereo width: out=L', out+1=R', in0=L, in1=R, in2=width
    MS_ENCODE = 172,         // Mid/side encode: out=M, out+1=S, in0=L, in1=R
    MS_DECODE = 173,         // Mid/side decode: out=L, out+1=R, in0=M, in1=S
    DELAY_PINGPONG = 174,    // Ping-pong delay: out=L', out+1=R', in0=L, in1=R, in2=time, in3=fb, in4=pan_width
    MONO_DOWNMIX = 175,      // Stereo-to-mono sum: out=mono, in0=L, in1=R, out[i] = (L[i] + R[i]) * 0.5
    PAN_STEREO = 176,        // Stereo balance (equal-power): out=L', out+1=R', in0=L, in1=R, in2=pos (-1..1)

    // Visualization/Debug (180-189)
    PROBE = 180,             // Capture signal to ring buffer for visualization: out=passthrough, in0=signal
    FFT_PROBE = 181,         // Forward FFT: accumulate samples, compute FFT, store magnitudes
                             // out=passthrough, in0=signal, rate=fft_size_log2 (8=256..11=2048)
    IFFT = 182,              // Inverse FFT (reserved for Revision 2+)

    // Wavetable Oscillators (200-209)
    // Mip-mapped wavetable oscillator (Smooch). Reads from active bank in
    // VM's WavetableBankRegistry, cached on ctx.wavetable_bank at block start.
    // Inputs: in0=freq, in1=phaseOffset, in2=tablePos. State: SmoochState.
    OSC_WAVETABLE = 200,

    // Forward Control Flow (210-219) — PRD prd-runtime-functions-control-flow L1.
    // Pure control-flow opcodes: handled directly in the VM dispatch loop, NOT
    // through the execute() opcode switch (same as POLY_BEGIN). Forward-only —
    // they advance ip, never rewind it.
    SKIP_IF_ZERO = 210,      // if inputs[0][0] == 0.0f, ip += rate + 1; else ip += 1
    SKIP_IF_NONZERO = 211,   // if inputs[0][0] != 0.0f, ip += rate + 1; else ip += 1
    LOOP_STATIC = 212,       // header: rate=body_len, out_buffer=iteration count;
                             // re-runs the next body_len instructions `count` times

    // Callable blocks (213-214, 216) — PRD prd-runtime-functions-control-flow L2.
    // BLOCK_CALL is a real runtime-dispatch opcode (as shipped in L2): it is a
    // dispatch-loop opcode (like POLY_BEGIN/FOREACH_EVENT) that invokes a shared
    // user-`fn` body resident in the ProgramSlot subprogram table. `rate` carries
    // the block_id; `inputs[0..4]` carry the caller's first 5 argument buffers;
    // `out_buffer` receives the result; `state_id` is the per-call-site
    // disambiguator driving state isolation. The VM dispatch loop handles it
    // directly — it must NOT reach the execute() opcode switch.
    BLOCK_CALL = 213,        // dispatch: call site of a shared fn body
    RET = 214,               // reserved no-op: body length lives in BlockEntry

    // Event iteration (215) — PRD prd-runtime-functions-control-flow L3.
    // Dispatch-loop opcode (like POLY_BEGIN): invokes a subprogram body once
    // per event/voice. The body lives in the ProgramSlot subprogram table,
    // NOT inline after this opcode. block_id + allocator config travel in the
    // matching StateInitData (ForeachAlloc), keyed by state_id. Generalizes
    // POLY_BEGIN/POLY_END; see cedar/opcodes/foreach_event.hpp.
    FOREACH_EVENT = 215,

    // BLOCK_BIND (216) — PRD prd-runtime-functions-control-flow §4.2.
    // Extra-argument carrier for a BLOCK_CALL whose target shared `fn` has more
    // than 5 parameters (BLOCK_CALL's inputs[0..4] cover only the first 5). One
    // BLOCK_BIND binds one logical parameter slot >= 5: `rate` = slot index
    // (5..MAX_SHARED_FN_PARAMS-1), `inputs[0]` = source buffer. A contiguous run
    // of BLOCK_BINDs precedes its BLOCK_CALL; the VM dispatch loop scans the run
    // and hands it to execute_block_call. Dispatch-loop opcode — never reaches
    // execute().
    BLOCK_BIND = 216,        // dispatch: bind logical param slot >=5 for BLOCK_CALL

    // Pattern Event Arrays (217) — PRD prd-pattern-event-arrays.
    // Packs the active sequence event's chord notes (evt.values[]) into a
    // single buffer for the DynArray data path: samples 0..num_values-1 hold
    // the notes, the rest are 0. rate: 0 = raw Hz (freqs(e)), 1 = MIDI via
    // freq→note conversion (notes(e)). inputs[1] = external clock (0xFFFF =
    // internal; matches SEQPAT_FIELD's clock slot). state_id must match the
    // SEQPAT_QUERY that filled the state.
    SEQPAT_VALUES = 217,

    INVALID = 255
};

// Instruction flag bits (16-bit field, room for future per-instruction attributes).
//
// Two bits cooperate to describe stereo behaviour (see
// prd-stereo-native-opcodes.md). As of Phase 5 the legacy auto-lift mechanism
// is retired — every audio-signal opcode is stereo-native, so STEREO_INPUT is
// only ever set together with STEREO_OUTPUT:
//
//   bit 0 = STEREO_INPUT, bit 1 = STEREO_OUTPUT
//
//   00: classic mono dispatch — opcode runs once on inputs[i], writes out_buffer.
//       Used by generators (osc/noise/...) and control/non-audio opcodes.
//   01: unused. STEREO_INPUT without STEREO_OUTPUT no longer occurs (auto-lift
//       retired in Phase 5 of prd-stereo-native-opcodes).
//   10: stereo-native opcode (STEREO_OUTPUT only). The opcode handles both
//       channels in one dispatch. Primary input is mono — the opcode reads
//       inputs[0] once and uses it for both internal L and R lanes. Output is
//       written to out_buffer (L) and out_buffer+1 (R) in a single call;
//       per-channel state lives inside one state struct.
//   11: stereo-native opcode with stereo primary input. The opcode reads
//       inputs[0] for L and inputs[0]+1 for R (adjacent-buffer convention).
//       Output as in case 10. The opcode body owns per-channel processing.
namespace InstructionFlag {
    constexpr std::uint16_t STEREO_INPUT  = 1u << 0;  // see truth table above
    constexpr std::uint16_t STEREO_OUTPUT = 1u << 1;  // see truth table above
    // BUS_WRITE (prd-bus-routing Phase 1): on an OUTPUT instruction, route
    // the accumulation into the scratch buffer pair (out_buffer, out_buffer+1)
    // — a numbered bus — instead of the device sinks. Without it OUTPUT
    // accumulates into ctx.output_left/right as before.
    constexpr std::uint16_t BUS_WRITE     = 1u << 2;
}

// XOR mask applied to state_id when storing/reading the ExtendedParams<N>
// companion state of an opcode. The opcode's own DSP state (e.g.
// ChorusState) lives at slot[state_id]; its ExtendedParams<N> lives at
// slot[state_id ^ EXT_PARAMS_STATE_XOR]. Apply via the
// `ext_params_state_id()` helper below so both codegen and opcode bodies
// stay in sync. PRD prd-extended-params §5.
constexpr std::uint32_t EXT_PARAMS_STATE_XOR = 0xB9D2A1C7u;

[[nodiscard]] constexpr std::uint32_t ext_params_state_id(std::uint32_t state_id) noexcept {
    return state_id ^ EXT_PARAMS_STATE_XOR;
}

// 160-bit (20 byte) fixed-width instruction for fast decoding
// Layout: [opcode:8][rate:8][out:16][in0:16][in1:16][in2:16][in3:16][in4:16][flags:16][state_id:32]
// The 2-byte `flags` field occupies what was previously padding inserted by
// the compiler to align `state_id` to 4 bytes — struct size is unchanged.
// Note: rate field also used for extra packed parameters (e.g., LFO shape)
// State ID uses full 32-bit FNV-1a hash to avoid collisions (birthday paradox at 256 states with 16-bit)
struct alignas(4) Instruction {
    Opcode opcode;              // Operation to perform
    std::uint8_t rate;          // 0=audio-rate, 1=control-rate, or packed params
    std::uint16_t out_buffer;   // Output buffer index
    std::uint16_t inputs[5];    // Input buffer indices (0xFFFF = unused)
    std::uint16_t flags;        // Per-instruction attribute flags (see InstructionFlag)
    std::uint32_t state_id;     // Semantic hash for state lookup (full 32-bit FNV-1a)

    // Convenience constructors
    static Instruction make_nullary(Opcode op, std::uint16_t out, std::uint32_t state = 0) {
        return {op, 0, out, {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, 0, state};
    }

    static Instruction make_unary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint32_t state = 0) {
        return {op, 0, out, {in0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, 0, state};
    }

    static Instruction make_binary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, 0xFFFF, 0xFFFF, 0xFFFF}, 0, state};
    }

    static Instruction make_ternary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint16_t in2, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, in2, 0xFFFF, 0xFFFF}, 0, state};
    }

    static Instruction make_quaternary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint16_t in2, std::uint16_t in3, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, in2, in3, 0xFFFF}, 0, state};
    }

    static Instruction make_quinary(Opcode op, std::uint16_t out, std::uint16_t in0, std::uint16_t in1, std::uint16_t in2, std::uint16_t in3, std::uint16_t in4, std::uint32_t state = 0) {
        return {op, 0, out, {in0, in1, in2, in3, in4}, 0, state};
    }
};

static_assert(sizeof(Instruction) == 20, "Instruction must be 20 bytes (160-bit)");

}  // namespace cedar
