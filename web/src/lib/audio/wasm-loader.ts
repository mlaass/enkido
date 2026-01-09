/**
 * WASM Module Loader
 *
 * Loads and wraps the Enkido WASM module for both main thread
 * and AudioWorklet contexts.
 */

// Type definitions for the WASM module
export interface EnkidoModule {
	// Memory access
	HEAPU8: Uint8Array;
	HEAPF32: Float32Array;

	// Cedar VM
	_cedar_init(): void;
	_cedar_destroy(): void;
	_cedar_set_sample_rate(rate: number): void;
	_cedar_set_bpm(bpm: number): void;
	_cedar_set_crossfade_blocks(blocks: number): void;
	_cedar_load_program(bytecode: number, byteCount: number): number;
	_cedar_process_block(): void;
	_cedar_get_output_left(): number;
	_cedar_get_output_right(): number;
	_cedar_reset(): void;
	_cedar_is_crossfading(): number;
	_cedar_crossfade_position(): number;
	_cedar_has_program(): number;
	_cedar_set_param(name: number, value: number): number;
	_cedar_set_param_slew(name: number, value: number, slewMs: number): number;

	// Akkado Compiler
	_akkado_compile(source: number, sourceLen: number): number;
	_akkado_get_bytecode(): number;
	_akkado_get_bytecode_size(): number;
	_akkado_get_diagnostic_count(): number;
	_akkado_get_diagnostic_severity(index: number): number;
	_akkado_get_diagnostic_message(index: number): number;
	_akkado_get_diagnostic_line(index: number): number;
	_akkado_get_diagnostic_column(index: number): number;
	_akkado_clear_result(): void;

	// Utility
	_enkido_get_block_size(): number;
	_enkido_malloc(size: number): number;
	_enkido_free(ptr: number): void;

	// Runtime helpers
	ccall: (name: string, returnType: string, argTypes: string[], args: unknown[]) => unknown;
	cwrap: (name: string, returnType: string, argTypes: string[]) => (...args: unknown[]) => unknown;
	UTF8ToString: (ptr: number) => string;
	stringToUTF8: (str: string, outPtr: number, maxBytes: number) => void;
	lengthBytesUTF8: (str: string) => number;
	getValue: (ptr: number, type: string) => number;
	setValue: (ptr: number, value: number, type: string) => void;
}

// Factory function type (provided by Emscripten)
export type EnkidoModuleFactory = () => Promise<EnkidoModule>;

let modulePromise: Promise<EnkidoModule> | null = null;
let module: EnkidoModule | null = null;

/**
 * Load the Enkido WASM module
 * Can be called multiple times - returns cached promise
 */
export async function loadEnkidoModule(): Promise<EnkidoModule> {
	if (module) return module;

	if (!modulePromise) {
		modulePromise = (async () => {
			// Dynamic import of the WASM module
			// @ts-expect-error - Module is loaded at runtime
			const createModule = (await import('/wasm/enkido.js')).default as EnkidoModuleFactory;
			const mod = await createModule();
			module = mod;
			return mod;
		})();
	}

	return modulePromise;
}

/**
 * Get the loaded module (throws if not loaded)
 */
export function getModule(): EnkidoModule {
	if (!module) {
		throw new Error('Enkido WASM module not loaded. Call loadEnkidoModule() first.');
	}
	return module;
}

/**
 * Check if module is loaded
 */
export function isModuleLoaded(): boolean {
	return module !== null;
}

/**
 * Helper to allocate a string in WASM memory
 * Returns pointer that must be freed with module._enkido_free()
 */
export function allocString(mod: EnkidoModule, str: string): number {
	const len = mod.lengthBytesUTF8(str) + 1;
	const ptr = mod._enkido_malloc(len);
	if (ptr === 0) throw new Error('Failed to allocate string in WASM memory');
	mod.stringToUTF8(str, ptr, len);
	return ptr;
}

/**
 * Helper to allocate a byte array in WASM memory
 * Returns pointer that must be freed with module._enkido_free()
 */
export function allocBytes(mod: EnkidoModule, bytes: Uint8Array): number {
	const ptr = mod._enkido_malloc(bytes.length);
	if (ptr === 0) throw new Error('Failed to allocate bytes in WASM memory');
	mod.HEAPU8.set(bytes, ptr);
	return ptr;
}
