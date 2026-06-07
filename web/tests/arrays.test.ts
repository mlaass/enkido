/**
 * Array Integration Tests
 *
 * Tests that array features compile correctly and produce expected bytecode.
 */

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import {
	loadNkido,
	getBytecode,
	getDisassembly,
	getOutputBuffers,
	type WrappedNkido
} from './wasm-helper';

describe('Array Compilation', () => {
	let nkido: WrappedNkido;

	beforeAll(async () => {
		nkido = await loadNkido();
		nkido.cedar_init();
		nkido.cedar_set_sample_rate(48000);
		nkido.cedar_set_bpm(120);
	});

	afterAll(() => {
		nkido.cedar_reset();
	});

	it('compiles array literal [1, 2, 3]', () => {
		const result = nkido.akkado_compile('[1, 2, 3]');
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);
		expect(disasm.instructions.length).toBeGreaterThan(0);

		// Should have PUSH_CONST instructions for each element
		const pushConsts = disasm.instructions.filter((i) => i.opcode === 'PUSH_CONST');
		expect(pushConsts.length).toBeGreaterThanOrEqual(3);
	});

	it('compiles array indexing with constant index xs[0]', () => {
		// `arr` is a reserved type-annotation keyword; use `xs` as the binding.
		const source = `
			xs = [100, 200, 300]
			xs[0] |> out(%, %)
		`;
		const result = nkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);

		// out(%, %) is stereo → one OUTPUT instruction per channel (L + R).
		const outputs = disasm.instructions.filter((i) => i.opcode === 'OUTPUT');
		expect(outputs.length).toBe(2);
	});

	it('compiles array indexing with dynamic index xs[lfo()]', () => {
		const source = `
			xs = [220, 330, 440, 550]
			xs[lfo(1) * 3.99] |> sine(%) |> out(%, %)
		`;
		const result = nkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);

		// Should have ARRAY_INDEX for dynamic indexing
		const arrayIndex = disasm.instructions.filter((i) => i.opcode === 'ARRAY_INDEX');
		expect(arrayIndex.length).toBe(1);

		// Should also have ARRAY_PACK to pack the multi-buffer array
		const arrayPack = disasm.instructions.filter((i) => i.opcode === 'ARRAY_PACK');
		expect(arrayPack.length).toBeGreaterThanOrEqual(1);
	});

	it('compiles array with UGen auto-expansion', () => {
		// Array through oscillator should expand to multiple oscillators
		// Note: osc() routes through stdlib match, so use sine directly
		const source = `
			[220, 330, 440] |> sine(%) |> sum(%) |> out(%, %)
		`;
		const result = nkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);

		// Should have 3 OSC_SIN instructions (one for each frequency)
		const oscs = disasm.instructions.filter((i) => i.opcode === 'OSC_SIN');
		expect(oscs.length).toBe(3);
	});

	it('compiles map() over array', () => {
		const source = `
			map([1, 2, 3], (x) -> x * 2) |> sum(%) |> out(%, %)
		`;
		const result = nkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);

		// Should have 3 MUL instructions (one for each element)
		const muls = disasm.instructions.filter((i) => i.opcode === 'MUL');
		expect(muls.length).toBe(3);
	});

	it('compiles sum() of array', () => {
		const source = `
			sum([1, 2, 3, 4]) |> out(%, %)
		`;
		const result = nkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);

		// Sum of 4 elements requires 3 ADD instructions
		const adds = disasm.instructions.filter((i) => i.opcode === 'ADD');
		expect(adds.length).toBe(3);
	});

	it('compiles broadcasting operations', () => {
		const source = `
			[1, 2, 3] * 2 |> sum(%) |> out(%, %)
		`;
		const result = nkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);

		// Should have 3 MUL instructions (one for each element)
		const muls = disasm.instructions.filter((i) => i.opcode === 'MUL');
		expect(muls.length).toBe(3);
	});

	it('compiles harmonics() for additive synthesis', () => {
		const source = `
			harmonics(110, 4) |> sine(%) |> sum(%) * 0.25 |> out(%, %)
		`;
		const result = nkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);

		// Should have 4 OSC_SIN instructions
		const oscs = disasm.instructions.filter((i) => i.opcode === 'OSC_SIN');
		expect(oscs.length).toBe(4);
	});

	it('compiles len() of array', () => {
		const source = `
			xs = [10, 20, 30, 40, 50]
			len(xs) |> out(%, %)
		`;
		const result = nkido.akkado_compile(source);
		expect(result).toBe(1);

		const disasm = getDisassembly(nkido);

		// len() should emit a PUSH_CONST with value 5
		const pushConsts = disasm.instructions.filter((i) => i.opcode === 'PUSH_CONST');
		// One of them should be the length constant
		expect(pushConsts.length).toBeGreaterThan(0);
	});
});

describe('Array Audio Processing', () => {
	let nkido: WrappedNkido;

	beforeAll(async () => {
		nkido = await loadNkido();
		nkido.cedar_init();
		nkido.cedar_set_sample_rate(48000);
		nkido.cedar_set_bpm(120);
	});

	afterAll(() => {
		nkido.cedar_reset();
	});

	it('produces audio from chord (array of frequencies)', () => {
		// Note: use sine directly and sum(%) for proper multi-buffer piping
		const source = `
			[220, 330, 440] |> sine(%) |> sum(%) * 0.3 |> out(%, %)
		`;
		const compileResult = nkido.akkado_compile(source);
		expect(compileResult).toBe(1);

		const bytecode = getBytecode(nkido);
		expect(bytecode.length).toBeGreaterThan(0);

		const loadResult = nkido.cedar_load_program(bytecode);
		expect(loadResult).toBe(0);

		// Process a few blocks
		for (let i = 0; i < 10; i++) {
			nkido.cedar_process_block();
		}

		const output = getOutputBuffers(nkido);

		// Should have non-zero audio output
		const maxSample = Math.max(...Array.from(output.left).map(Math.abs));
		expect(maxSample).toBeGreaterThan(0);
		expect(maxSample).toBeLessThan(1); // Should be scaled by 0.3
	});

	it('produces audio from wavetable-style indexing', () => {
		// Simple wavetable: phasor scans through array values
		const source = `
			wave = [0, 0.5, 1, 0.5, 0, -0.5, -1, -0.5]
			phasor(110) * 7.99 |> wave[%] * 0.5 |> out(%, %)
		`;
		const compileResult = nkido.akkado_compile(source);
		expect(compileResult).toBe(1);

		const bytecode = getBytecode(nkido);
		const loadResult = nkido.cedar_load_program(bytecode);
		expect(loadResult).toBe(0);

		// Process several blocks to get stable output
		for (let i = 0; i < 20; i++) {
			nkido.cedar_process_block();
		}

		const output = getOutputBuffers(nkido);

		// Should have audio output in range [-0.5, 0.5]
		const maxSample = Math.max(...Array.from(output.left).map(Math.abs));
		expect(maxSample).toBeGreaterThan(0);
		expect(maxSample).toBeLessThanOrEqual(0.5 + 0.01); // Small tolerance for float precision
	});
});

describe('Array Error Handling', () => {
	let nkido: WrappedNkido;

	beforeAll(async () => {
		nkido = await loadNkido();
		nkido.cedar_init();
	});

	afterAll(() => {
		nkido.cedar_reset();
	});

	it('reports error for undefined array variable', () => {
		const result = nkido.akkado_compile('undefined_arr[0]');
		expect(result).toBe(0);

		const diagCount = nkido.akkado_get_diagnostic_count();
		expect(diagCount).toBeGreaterThan(0);
	});

	it('compiles empty array without error', () => {
		// The empty-array literal itself must compile. (Piping it into out()
		// is a separate, legitimate type error — out() expects a Signal, not
		// an Array — so it is not part of what this test asserts.)
		const result = nkido.akkado_compile('xs = []\nlen(xs) |> out(%, %)');
		expect(result).toBe(1);
	});
});
