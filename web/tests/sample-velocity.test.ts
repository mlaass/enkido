/**
 * Sample velocity end-to-end test
 *
 * Loads the actual `web/static/wasm/nkido.wasm` (the same artifact the browser
 * fetches for the AudioWorklet), registers a synthesized impulse sample,
 * compiles a sample pattern with and without `{vel:V}`, runs `cedar_process_block`
 * end-to-end, and verifies the sampler output is scaled by velocity.
 *
 * Why this test exists: previously, sampler velocity was only verified via
 * `nkido render` (offline WAV with --bank file://). That confirms the
 * compiler emits the right bytecode but does NOT prove the WASM artifact loaded
 * by the AudioWorklet executes velocity-scaled output. If a future change
 * regresses any link in {codegen, opcode VM, WASM glue, sample bank wiring},
 * the user hears no attenuation despite a "fix" landing — exactly the symptom
 * that prompted this test.
 *
 * NOTE: these tests compile via the test-only `akkado_compile_bypass_master`
 * export, which suppresses the master bus (out() then has no DISTORT_SOFT
 * soft-clip / safety stage). Without it, the soft-clip saturates the loud
 * reference more than the quiet signal, so a true 0.25× velocity reads ~0.33×.
 * Bypassing the master makes the output linear in amplitude, so velocity
 * ratios can be read directly.
 */

import { describe, it, expect, beforeAll } from 'vitest';
import { loadNkido, getBytecode, type WrappedNkido } from './wasm-helper';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const BPM = 120;

interface NkidoExt extends WrappedNkido {
	cedar_load_sample: (
		name: string,
		audioData: Float32Array,
		channels: number,
		sampleRate: number
	) => number;
	cedar_clear_samples: () => void;
	// Test-only compile that suppresses the master bus (no DISTORT_SOFT
	// soft-clip / safety stage), so the measured peaks are linear in the
	// signal amplitude — required to read velocity ratios directly.
	compile_bypass_master: (source: string) => number;
	// Buffer-based apply path (compile-off-audio-thread, PRD §5). The old
	// in-WASM g_compile_result accessors (akkado_resolve_sample_ids,
	// akkado_patch_sample_ids_in_bytecode, cedar_apply_state_inits) were
	// replaced by these *_from_buffer / pack_* exports.
	pack_state_inits_buffer: () => number;
	get_state_inits_buffer_size: () => number;
	pack_block_table_buffer: () => number;
	get_block_table_buffer_size: () => number;
	get_main_instruction_count: () => number;
	cedar_set_block_table: (entriesPtr: number, count: number, mainInst: number) => void;
	patch_sample_ids_from_buffer: (
		bcPtr: number,
		bcLen: number,
		siPtr: number,
		siLen: number
	) => number;
	cedar_apply_state_inits_from_buffer: (siPtr: number, siLen: number) => number;
	resolve_sample_ids_from_buffer: (siPtr: number, siLen: number) => number;
	getOutputLeft: () => Float32Array;
}

function wrapExt(nkido: WrappedNkido): NkidoExt {
	const m = nkido.module;
	const ccall = m.ccall.bind(m);
	const cwrap = m.cwrap.bind(m);

	const cedar_clear_samples = cwrap('cedar_clear_samples', null, []) as () => void;
	const compile_bypass_master = (source: string): number => {
		const utf8ByteLen = m.lengthBytesUTF8(source);
		const ptr = m._nkido_malloc(utf8ByteLen + 1);
		m.stringToUTF8(source, ptr, utf8ByteLen + 1);
		const result = ccall(
			'akkado_compile_bypass_master',
			'number',
			['number', 'number'],
			[ptr, utf8ByteLen]
		) as number;
		m._nkido_free(ptr);
		return result;
	};
	const pack_state_inits_buffer = cwrap(
		'akkado_pack_state_inits_buffer',
		'number',
		[]
	) as () => number;
	const get_state_inits_buffer_size = cwrap(
		'akkado_get_state_inits_buffer_size',
		'number',
		[]
	) as () => number;
	const pack_block_table_buffer = cwrap(
		'akkado_pack_block_table_buffer',
		'number',
		[]
	) as () => number;
	const get_block_table_buffer_size = cwrap(
		'akkado_get_block_table_buffer_size',
		'number',
		[]
	) as () => number;
	const get_main_instruction_count = cwrap(
		'akkado_get_main_instruction_count',
		'number',
		[]
	) as () => number;
	const cedar_set_block_table = cwrap('cedar_set_block_table', null, [
		'number',
		'number',
		'number'
	]) as (entriesPtr: number, count: number, mainInst: number) => void;
	const patch_sample_ids_from_buffer = cwrap(
		'akkado_patch_sample_ids_in_bytecode_from_buffer',
		'number',
		['number', 'number', 'number', 'number']
	) as (bcPtr: number, bcLen: number, siPtr: number, siLen: number) => number;
	const cedar_apply_state_inits_from_buffer = cwrap(
		'cedar_apply_state_inits_from_buffer',
		'number',
		['number', 'number']
	) as (siPtr: number, siLen: number) => number;
	const resolve_sample_ids_from_buffer = cwrap(
		'akkado_resolve_sample_ids_from_buffer',
		'number',
		['number', 'number']
	) as (siPtr: number, siLen: number) => number;

	const cedar_load_sample = (
		name: string,
		audioData: Float32Array,
		channels: number,
		sampleRate: number
	): number => {
		// Allocate and copy name (null-terminated UTF-8).
		const nameLen = m.lengthBytesUTF8(name) + 1;
		const namePtr = m._nkido_malloc(nameLen);
		m.stringToUTF8(name, namePtr, nameLen);

		// Allocate and copy audio data (interleaved float32).
		const audioBytes = audioData.length * 4;
		const audioPtr = m._nkido_malloc(audioBytes);
		const heapF32 = new Float32Array(m.wasmMemory?.buffer ?? m.HEAPF32.buffer);
		heapF32.set(audioData, audioPtr / 4);

		const id = ccall(
			'cedar_load_sample',
			'number',
			['number', 'number', 'number', 'number', 'number'],
			[namePtr, audioPtr, audioData.length, channels, sampleRate]
		) as number;

		m._nkido_free(audioPtr);
		m._nkido_free(namePtr);
		return id;
	};

	const getOutputLeft = (): Float32Array => {
		const ptr = nkido.cedar_get_output_left();
		const out = new Float32Array(BLOCK_SIZE);
		const heap = new Float32Array(m.wasmMemory?.buffer ?? m.HEAPF32.buffer);
		const idx = ptr / 4;
		for (let i = 0; i < BLOCK_SIZE; i++) out[i] = heap[idx + i];
		return out;
	};

	return Object.assign({}, nkido, {
		cedar_load_sample,
		cedar_clear_samples,
		compile_bypass_master,
		pack_state_inits_buffer,
		get_state_inits_buffer_size,
		pack_block_table_buffer,
		get_block_table_buffer_size,
		get_main_instruction_count,
		cedar_set_block_table,
		patch_sample_ids_from_buffer,
		cedar_apply_state_inits_from_buffer,
		resolve_sample_ids_from_buffer,
		getOutputLeft
	}) as NkidoExt;
}

/** Copy `size` bytes from the WASM heap at `ptr` into a fresh Uint8Array. */
function copyHeap(m: NkidoExt['module'], ptr: number, size: number): Uint8Array {
	const heap = new Uint8Array(m.wasmMemory?.buffer ?? m.HEAPU8.buffer);
	return heap.slice(ptr, ptr + size);
}

/**
 * Replicate the AudioWorklet's load sequence exactly:
 *   1. compile akkado source
 *   2. extract bytecode
 *   3. pack the state-init + block-table buffers from the compile result
 *   4. stage the block table, copy bytecode + state-inits into WASM memory
 *   5. patch scalar sample IDs into the bytecode (from the state-init buffer)
 *   6. cedar_load_program
 *   7. cedar_apply_state_inits_from_buffer + resolve_sample_ids_from_buffer
 *
 * This mirrors the post-PRD AudioWorklet load path
 * (web/static/worklet/cedar-processor.js `runProgramLoad`): the in-WASM
 * g_compile_result accessors are gone, so every datum travels as a packed
 * buffer.
 */
function loadProgramWithSamples(nkido: NkidoExt, source: string): void {
	const m = nkido.module;
	const writeBytes = (ptr: number, bytes: Uint8Array): void => {
		const heap = new Uint8Array(m.wasmMemory?.buffer ?? m.HEAPU8.buffer);
		heap.set(bytes, ptr);
	};

	// Compile with the master bus bypassed so the output is linear in the
	// signal amplitude (no DISTORT_SOFT soft-clip on out()); otherwise the
	// loud reference gets saturated more than the quiet one and velocity
	// ratios read high (e.g. tanh-like 0.25 -> ~0.33).
	const compileOk = nkido.compile_bypass_master(source);
	expect(compileOk, `compile failed for: ${source}`).toBe(1);

	// Pack the wire buffers from the compile result, copying each out with a
	// fresh heap view (pack/get_bytecode may grow WASM memory).
	const stateInits = copyHeap(
		m,
		nkido.pack_state_inits_buffer(),
		nkido.get_state_inits_buffer_size()
	);
	const blockTable = copyHeap(
		m,
		nkido.pack_block_table_buffer(),
		nkido.get_block_table_buffer_size()
	);
	const mainInst = nkido.get_main_instruction_count();
	const bytecode = getBytecode(nkido);
	expect(bytecode.length).toBeGreaterThan(0);

	// Stage the block table before loading (matches the worklet ordering).
	const blockEntryCount = blockTable.byteLength > 0 ? blockTable.byteLength / 12 : 0;
	let btPtr = 0;
	if (blockEntryCount > 0) {
		btPtr = m._nkido_malloc(blockTable.byteLength);
		writeBytes(btPtr, blockTable);
	}
	nkido.cedar_set_block_table(btPtr, blockEntryCount, mainInst);
	if (btPtr) m._nkido_free(btPtr);

	// Copy bytecode + state-inits into WASM memory.
	const bcPtr = m._nkido_malloc(bytecode.length);
	writeBytes(bcPtr, bytecode);
	let siPtr = 0;
	if (stateInits.byteLength > 0) {
		siPtr = m._nkido_malloc(stateInits.byteLength);
		writeBytes(siPtr, stateInits);
	}

	// Patch scalar sample() IDs into the bytecode from the state-init buffer,
	// then load, then apply state-inits and resolve pattern-event sample IDs.
	if (siPtr) {
		nkido.patch_sample_ids_from_buffer(bcPtr, bytecode.length, siPtr, stateInits.byteLength);
	}
	const loadResult = m.ccall(
		'cedar_load_program',
		'number',
		['number', 'number'],
		[bcPtr, bytecode.length]
	) as number;
	expect(loadResult).toBe(0);

	if (siPtr) {
		nkido.cedar_apply_state_inits_from_buffer(siPtr, stateInits.byteLength);
		nkido.resolve_sample_ids_from_buffer(siPtr, stateInits.byteLength);
	}

	m._nkido_free(bcPtr);
	if (siPtr) m._nkido_free(siPtr);
}

/**
 * Run blocks until the sampler has fired and finished, returning the peak |sample|
 * observed across all blocks on the left output channel.
 */
function runAndPeak(nkido: NkidoExt, numBlocks: number): number {
	let peak = 0;
	for (let i = 0; i < numBlocks; i++) {
		nkido.cedar_process_block();
		const out = nkido.getOutputLeft();
		for (let s = 0; s < out.length; s++) {
			const v = Math.abs(out[s]);
			if (v > peak) peak = v;
		}
	}
	return peak;
}

describe('Sample velocity end-to-end (WASM)', () => {
	let nkido: NkidoExt;

	beforeAll(async () => {
		const base = await loadNkido();
		nkido = wrapExt(base);
		nkido.cedar_init();
		nkido.cedar_set_sample_rate(SAMPLE_RATE);
		nkido.cedar_set_bpm(BPM);
	});

	it('registers a synthesized impulse sample as "bd"', () => {
		nkido.cedar_clear_samples();
		// 64 frames of full-amplitude impulse so peak detection is unambiguous
		// regardless of where in a block the trigger lands.
		const audio = new Float32Array(64);
		for (let i = 0; i < audio.length; i++) audio[i] = 1.0;
		const id = nkido.cedar_load_sample('bd', audio, 1, SAMPLE_RATE);
		expect(id).toBeGreaterThan(0);
	});

	it('s"bd" with vel:0.25 plays at ~0.25× the unattenuated sample (Bug B)', () => {
		// Run unattenuated reference.
		nkido.cedar_reset();
		loadProgramWithSamples(nkido, 's"bd ~ ~ ~".out()');
		// Each cycle = num_elements beats = 4 beats at 120 bpm = 2 s = ~750 blocks.
		// 200 blocks (~0.53 s) is enough to span the trigger + tail of the 64-frame
		// impulse, while staying well inside the first quarter of the cycle.
		const loudPeak = runAndPeak(nkido, 200);
		expect(loudPeak, 'unattenuated bd should produce audio').toBeGreaterThan(0.5);

		// Run with {vel:0.25}.
		nkido.cedar_reset();
		loadProgramWithSamples(nkido, 's"bd{vel:0.25} ~ ~ ~".out()');
		const quietPeak = runAndPeak(nkido, 200);

		const ratio = quietPeak / loudPeak;
		expect(
			ratio,
			`vel:0.25 should attenuate sample to ~0.25× (loud=${loudPeak.toFixed(4)}, quiet=${quietPeak.toFixed(4)})`
		).toBeGreaterThan(0.2);
		expect(ratio).toBeLessThan(0.3);
	});

	it('polyrhythm [hh,bd{vel:0.25}] attenuates only the bd voice (per-voice velocity) (Bug A)', () => {
		// Re-register both samples so the polyrhythm has two distinct sample IDs.
		nkido.cedar_clear_samples();
		const impulse = new Float32Array(64);
		for (let i = 0; i < impulse.length; i++) impulse[i] = 1.0;
		const bdId = nkido.cedar_load_sample('bd', impulse, 1, SAMPLE_RATE);
		const hhId = nkido.cedar_load_sample('hh', impulse, 1, SAMPLE_RATE);
		expect(bdId).toBeGreaterThan(0);
		expect(hhId).toBeGreaterThan(0);

		// Per-voice velocity (samplers.hpp): each element of [hh, bd{vel}] keeps
		// its own velocity — hh stays at 1.0, only bd is attenuated. With the
		// master bus bypassed (linear), two stacked unit impulses scaled by 0.4:
		//   loud  [hh, bd]            = (1.0 + 1.0)  * 0.4 = 0.8
		//   quiet [hh, bd{vel:0.25}]  = (1.0 + 0.25) * 0.4 = 0.5
		//   ratio = 0.5 / 0.8 = 0.625
		// This ratio is the unique signature of *per-voice* attenuation:
		//   ~1.0  would mean bd was NOT attenuated at all,
		//   ~0.25 would mean the whole merged event was (wrongly) attenuated.
		nkido.cedar_reset();
		loadProgramWithSamples(nkido, 's"[hh,bd] ~ ~ ~" |> @ * 0.4 |> out(@, @)');
		const loudPeak = runAndPeak(nkido, 200);
		expect(loudPeak, 'unattenuated polyrhythm should produce audio').toBeGreaterThan(0.1);

		nkido.cedar_reset();
		loadProgramWithSamples(
			nkido,
			's"[hh,bd{vel:0.25}] ~ ~ ~" |> @ * 0.4 |> out(@, @)'
		);
		const quietPeak = runAndPeak(nkido, 200);

		const ratio = quietPeak / loudPeak;
		const msg =
			`[hh,bd{vel:0.25}] should attenuate only bd → merged ratio ~0.625 ` +
			`(loud=${loudPeak.toFixed(4)}, quiet=${quietPeak.toFixed(4)})`;
		// bd was attenuated (ratio well below 1.0) but hh was not (well above the
		// 0.25 whole-event-attenuation value).
		expect(ratio, msg).toBeGreaterThan(0.55);
		expect(ratio, msg).toBeLessThan(0.7);
	});
});
