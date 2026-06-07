/**
 * State-init wire-format cross-check.
 *
 * Loads the real `web/static/wasm/nkido.wasm` (the artifact the AudioWorklet
 * uses), compiles representative patches, asks the C++ packer for the exact
 * stateInitsBuf bytes it ships to the worklet, and walks them with the
 * GENERATED TypeScript decoder (`$lib/audio/state-init-codec`).
 *
 * The decoder steps over `cedar::Event[]` payloads using the generated
 * `EVENT_SIZE` and asserts the per-record walk lands exactly on each record's
 * declared `payload_size` and on `buf.byteLength`. So if the C++ wire format
 * (struct sizes, field order, record header) ever drifts from the generated
 * codec, this test fails — the build-time drift guard PRD §5.4 asked for,
 * achieved by decoding live C++ output rather than re-packing in TS.
 *
 * Regenerate the codec after any wire-format change:  bun run build:state-init-codec
 */

import { describe, it, expect, beforeAll } from 'vitest';
import { loadNkido, type WrappedNkido } from './wasm-helper';
import {
	decodeStateInits,
	MAGIC_STATE_INITS,
	WIRE_VERSION,
	RecordKind,
	StateInitType
} from '$lib/audio/state-init-codec';

interface CodecExt {
	nkido: WrappedNkido;
	akkado_clear_result: () => void;
	pack_state_inits_buffer: () => number;
	get_state_inits_buffer_size: () => number;
	get_state_init_count: () => number;
	get_state_init_type: (index: number) => number;
	/** copy `size` bytes from the WASM heap at `ptr` into a fresh Uint8Array */
	copyBuf: (ptr: number, size: number) => Uint8Array;
}

function wrap(nkido: WrappedNkido): CodecExt {
	const m = nkido.module;
	const cwrap = m.cwrap.bind(m);
	return {
		nkido,
		akkado_clear_result: cwrap('akkado_clear_result', null, []) as () => void,
		pack_state_inits_buffer: cwrap(
			'akkado_pack_state_inits_buffer',
			'number',
			[]
		) as () => number,
		get_state_inits_buffer_size: cwrap(
			'akkado_get_state_inits_buffer_size',
			'number',
			[]
		) as () => number,
		get_state_init_count: cwrap(
			'akkado_get_state_init_count',
			'number',
			[]
		) as () => number,
		get_state_init_type: cwrap('akkado_get_state_init_type', 'number', [
			'number'
		]) as (i: number) => number,
		copyBuf: (ptr: number, size: number) => {
			// Fetch a fresh heap view — the pack call may have grown WASM memory.
			const heap = new Uint8Array(m.wasmMemory?.buffer ?? m.HEAPU8.buffer);
			return heap.slice(ptr, ptr + size);
		}
	};
}

/** compile `src`, then pack + copy the stateInitsBuf and snapshot the per-index types. */
function packStateInits(ext: CodecExt, src: string) {
	ext.akkado_clear_result();
	const ok = ext.nkido.akkado_compile(src);
	if (!ok) {
		const n = ext.nkido.akkado_get_diagnostic_count();
		const msgs = [];
		for (let i = 0; i < n; i++) msgs.push(ext.nkido.akkado_get_diagnostic_message(i));
		throw new Error(`compile failed: ${msgs.join('; ')}`);
	}
	const count = ext.get_state_init_count();
	const types: number[] = [];
	for (let i = 0; i < count; i++) types.push(ext.get_state_init_type(i));

	const ptr = ext.pack_state_inits_buffer();
	const size = ext.get_state_inits_buffer_size();
	const buf = ext.copyBuf(ptr, size);
	ext.akkado_clear_result();
	return { buf, count, types };
}

describe('state-init codec cross-check (TS decoder vs C++ packer)', () => {
	let ext: CodecExt;

	beforeAll(async () => {
		ext = wrap(await loadNkido());
	});

	// Proven to compile against the committed WASM artifact and to exercise a
	// spread of record types (mirrors akkado/tests/test_state_init_buffer_codec.cpp).
	const POLY_CHORD = `fn pad({freq, gate, vel}) ->
    saw(freq) * adsr(gate, 0.01, 0.1, 0.7, 0.3) * vel
chord("Cmaj7 Am7") |> poly(@, pad) |> out(@)
`;
	const UNISON_PAD = `
bpm = 135
fn pad_voice(freq, gate, vel, ext) ->
    saw(freq, ext.phase) * adsr(gate, 0.5, 0.6, 0.7, 1.2) * vel
    |> lp(@, 2800)
fn fat({freq, gate, vel}) ->
    unison(freq, gate, vel, pad_voice, voices: 3, detune: 0.3, width: .7, phase: 0.2)
c"Cmaj9 Am11@2 Fmaj7 G9@2".slow(2).transpose(-12)
    |> poly(@, fat, 8, 3) * 0.2
    |> reverb(@, 0.75, 0.6, ..{wet: 0.1})
    |> out(@)
`;

	const SOURCES: Array<{ name: string; src: string; expectTypes: StateInitType[] }> = [
		{
			name: 'poly chord pattern',
			src: POLY_CHORD,
			expectTypes: [StateInitType.SequenceProgram, StateInitType.ForeachAlloc]
		},
		{
			name: 'unison pad (multi-record)',
			src: UNISON_PAD,
			// SequenceProgram + RateScale + EventTransform + ForeachAlloc + ExtendedParams
			expectTypes: [
				StateInitType.SequenceProgram,
				StateInitType.ForeachAlloc,
				StateInitType.ExtendedParams
			]
		}
	];

	for (const { name, src, expectTypes } of SOURCES) {
		it(`decodes C++-packed buffer for: ${name}`, () => {
			const { buf, count, types } = packStateInits(ext, src);
			expect(count).toBeGreaterThan(0);
			expect(buf.byteLength).toBeGreaterThan(8);

			const decoded = decodeStateInits(buf);

			// Header agrees with the generated constants.
			expect(decoded.magic).toBe(MAGIC_STATE_INITS);
			expect(decoded.version).toBe(WIRE_VERSION);

			// THE drift guard: the walk (which steps over Event[] via EVENT_SIZE)
			// must consume the buffer exactly. Wrong struct size / field order
			// would land off the end here.
			expect(decoded.bytesConsumed).toBe(buf.byteLength);
			expect(decoded.records.length).toBe(decoded.recordCount);

			// Every state-init the compiler reported appears, in order, as a
			// StateInit record whose type matches.
			expect(decoded.recordCount).toBeGreaterThanOrEqual(count);
			for (let i = 0; i < count; i++) {
				expect(decoded.records[i].kind).toBe(RecordKind.StateInit);
				expect(decoded.records[i].type).toBe(types[i]);
			}

			// The patch produced the record types we expect.
			for (const t of expectTypes) expect(types).toContain(t);
		});
	}

	it('every SequenceProgram record reports a self-consistent sequence list', () => {
		const { buf } = packStateInits(ext, UNISON_PAD);
		const decoded = decodeStateInits(buf);
		const seqRecords = decoded.records.filter(
			(r) => r.type === StateInitType.SequenceProgram
		);
		expect(seqRecords.length).toBeGreaterThan(0);
		for (const r of seqRecords) {
			expect(r.numSequences).toBe(r.sequences?.length);
			// the chord progression yields at least one sequence carrying events.
			expect(r.sequences?.some((s) => s.numEvents > 0)).toBe(true);
		}
	});
});
