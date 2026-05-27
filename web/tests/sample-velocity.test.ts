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
	akkado_resolve_sample_ids: () => void;
	akkado_patch_sample_ids_in_bytecode: (ptr: number, byteCount: number) => number;
	cedar_apply_state_inits: () => number;
	getOutputLeft: () => Float32Array;
}

function wrapExt(nkido: WrappedNkido): NkidoExt {
	const m = nkido.module;
	const ccall = m.ccall.bind(m);
	const cwrap = m.cwrap.bind(m);

	const cedar_clear_samples = cwrap('cedar_clear_samples', null, []) as () => void;
	const akkado_resolve_sample_ids = cwrap(
		'akkado_resolve_sample_ids',
		null,
		[]
	) as () => void;
	const cedar_apply_state_inits = cwrap('cedar_apply_state_inits', 'number', []) as () => number;

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

	const akkado_patch_sample_ids_in_bytecode = (ptr: number, byteCount: number): number => {
		return ccall(
			'akkado_patch_sample_ids_in_bytecode',
			'number',
			['number', 'number'],
			[ptr, byteCount]
		) as number;
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
		akkado_resolve_sample_ids,
		akkado_patch_sample_ids_in_bytecode,
		cedar_apply_state_inits,
		getOutputLeft
	}) as NkidoExt;
}

/**
 * Replicate the AudioWorklet's load sequence exactly:
 *   1. compile akkado source
 *   2. extract bytecode
 *   3. allocate WASM memory, copy bytecode in
 *   4. patch sample IDs into PUSH_CONST placeholders (scalar sample() calls)
 *   5. cedar_load_program
 *   6. akkado_resolve_sample_ids (for SequenceProgram event values)
 *   7. cedar_apply_state_inits
 */
function loadProgramWithSamples(nkido: NkidoExt, source: string): void {
	const compileOk = nkido.akkado_compile(source);
	expect(compileOk, `compile failed for: ${source}`).toBe(1);
	const bytecode = getBytecode(nkido);
	expect(bytecode.length).toBeGreaterThan(0);

	const m = nkido.module;
	const ptr = m._nkido_malloc(bytecode.length);
	const heap = new Uint8Array(m.wasmMemory?.buffer ?? m.HEAPU8.buffer);
	heap.set(bytecode, ptr);

	nkido.akkado_patch_sample_ids_in_bytecode(ptr, bytecode.length);
	const loadResult = m.ccall(
		'cedar_load_program',
		'number',
		['number', 'number'],
		[ptr, bytecode.length]
	) as number;
	expect(loadResult).toBe(0);

	nkido.akkado_resolve_sample_ids();
	nkido.cedar_apply_state_inits();

	m._nkido_free(ptr);
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

	it('polyrhythm [hh,bd{vel:0.25}] attenuates the merged-event amplitude (Bug A)', () => {
		// Re-register both samples so the polyrhythm has two distinct sample IDs.
		nkido.cedar_clear_samples();
		const impulse = new Float32Array(64);
		for (let i = 0; i < impulse.length; i++) impulse[i] = 1.0;
		const bdId = nkido.cedar_load_sample('bd', impulse, 1, SAMPLE_RATE);
		const hhId = nkido.cedar_load_sample('hh', impulse, 1, SAMPLE_RATE);
		expect(bdId).toBeGreaterThan(0);
		expect(hhId).toBeGreaterThan(0);

		// Without velocity attenuation: stack of 2 unit-amplitude impulses fires
		// simultaneously. With voice clamping in op_sample_play (±2.0) plus output
		// summing, the loud peak is bounded but well above the quiet peak.
		nkido.cedar_reset();
		// Halve the output to keep the loud peak below clipping for a fair ratio.
		loadProgramWithSamples(nkido, 's"[hh,bd] ~ ~ ~" |> % * 0.4 |> out(%, %)');
		const loudPeak = runAndPeak(nkido, 200);
		expect(loudPeak, 'unattenuated polyrhythm should produce audio').toBeGreaterThan(0.1);

		nkido.cedar_reset();
		loadProgramWithSamples(
			nkido,
			's"[hh,bd{vel:0.25}] ~ ~ ~" |> % * 0.4 |> out(%, %)'
		);
		const quietPeak = runAndPeak(nkido, 200);

		const ratio = quietPeak / loudPeak;
		expect(
			ratio,
			`[hh,bd{vel:0.25}] should attenuate the merged event to ~0.25× ` +
				`(loud=${loudPeak.toFixed(4)}, quiet=${quietPeak.toFixed(4)})`
		).toBeGreaterThan(0.2);
		expect(ratio).toBeLessThan(0.3);
	});
});
