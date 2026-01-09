/**
 * Akkado Compiler Wrapper
 *
 * Provides a TypeScript interface to the Akkado WASM compiler.
 */

import { loadEnkidoModule, allocString, type EnkidoModule } from '$audio/wasm-loader';

export enum DiagnosticSeverity {
	Info = 0,
	Warning = 1,
	Error = 2
}

export interface Diagnostic {
	severity: DiagnosticSeverity;
	message: string;
	line: number;
	column: number;
}

export interface CompileResult {
	success: boolean;
	bytecode: Uint8Array | null;
	diagnostics: Diagnostic[];
}

let module: EnkidoModule | null = null;

/**
 * Initialize the compiler (loads WASM if needed)
 */
export async function initCompiler(): Promise<void> {
	if (!module) {
		module = await loadEnkidoModule();
	}
}

/**
 * Compile Akkado source code to Cedar bytecode
 */
export async function compile(source: string): Promise<CompileResult> {
	await initCompiler();
	if (!module) throw new Error('Compiler not initialized');

	// Allocate source string in WASM memory
	const sourcePtr = allocString(module, source);

	try {
		// Compile
		const success = module._akkado_compile(sourcePtr, source.length) === 1;

		// Get diagnostics
		const diagnostics: Diagnostic[] = [];
		const diagCount = module._akkado_get_diagnostic_count();

		for (let i = 0; i < diagCount; i++) {
			const severity = module._akkado_get_diagnostic_severity(i) as DiagnosticSeverity;
			const messagePtr = module._akkado_get_diagnostic_message(i);
			const message = module.UTF8ToString(messagePtr);
			const line = module._akkado_get_diagnostic_line(i);
			const column = module._akkado_get_diagnostic_column(i);

			diagnostics.push({ severity, message, line, column });
		}

		// Get bytecode if successful
		let bytecode: Uint8Array | null = null;
		if (success) {
			const bytecodePtr = module._akkado_get_bytecode();
			const bytecodeSize = module._akkado_get_bytecode_size();

			if (bytecodeSize > 0) {
				// Copy bytecode from WASM memory
				bytecode = new Uint8Array(module.HEAPU8.buffer, bytecodePtr, bytecodeSize).slice();
			}
		}

		// Clear result to free memory
		module._akkado_clear_result();

		return { success, bytecode, diagnostics };
	} finally {
		// Free source string
		module._enkido_free(sourcePtr);
	}
}

/**
 * Check if compiler is ready
 */
export function isCompilerReady(): boolean {
	return module !== null;
}
