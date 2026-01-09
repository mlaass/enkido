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
	console.log('[Akkado] compile() called');
	await initCompiler();
	console.log('[Akkado] Compiler initialized');
	if (!module) throw new Error('Compiler not initialized');

	// Allocate source string in WASM memory
	console.log('[Akkado] Allocating source string...');
	const sourcePtr = allocString(module, source);
	console.log('[Akkado] Source allocated at ptr:', sourcePtr);

	try {
		// Compile
		console.log('[Akkado] Calling _akkado_compile...');
		const compileResult = module._akkado_compile(sourcePtr, source.length);
		console.log('[Akkado] _akkado_compile returned:', compileResult);
		const success = compileResult === 1;

		// Get diagnostics
		console.log('[Akkado] Getting diagnostics...');
		const diagnostics: Diagnostic[] = [];
		const diagCount = module._akkado_get_diagnostic_count();
		console.log('[Akkado] Diagnostic count:', diagCount);

		for (let i = 0; i < diagCount; i++) {
			const severity = module._akkado_get_diagnostic_severity(i) as DiagnosticSeverity;
			const messagePtr = module._akkado_get_diagnostic_message(i);
			const message = module.UTF8ToString(messagePtr);
			const line = module._akkado_get_diagnostic_line(i);
			const column = module._akkado_get_diagnostic_column(i);

			diagnostics.push({ severity, message, line, column });
			console.log('[Akkado] Diagnostic', i, ':', severity, message, 'at', line, ':', column);
		}

		// Get bytecode if successful
		let bytecode: Uint8Array | null = null;
		if (success) {
			console.log('[Akkado] Getting bytecode...');
			const bytecodePtr = module._akkado_get_bytecode();
			const bytecodeSize = module._akkado_get_bytecode_size();
			console.log('[Akkado] Bytecode ptr:', bytecodePtr, 'size:', bytecodeSize);

			if (bytecodeSize > 0) {
				// Copy bytecode from WASM memory
				console.log('[Akkado] Copying bytecode from WASM memory...');
				try {
					bytecode = new Uint8Array(bytecodeSize);
					// Try different methods to access memory
					if (module.HEAPU8) {
						console.log('[Akkado] Using HEAPU8');
						for (let i = 0; i < bytecodeSize; i++) {
							bytecode[i] = module.HEAPU8[bytecodePtr + i];
						}
					} else if (module.getValue) {
						console.log('[Akkado] Using getValue');
						for (let i = 0; i < bytecodeSize; i++) {
							bytecode[i] = module.getValue(bytecodePtr + i, 'i8') & 0xFF;
						}
					} else {
						console.error('[Akkado] No way to access WASM memory!');
					}
					console.log('[Akkado] Bytecode copied, first bytes:', Array.from(bytecode.slice(0, 16)));
				} catch (copyErr) {
					console.error('[Akkado] Error copying bytecode:', copyErr);
				}
			}
		}

		// Clear result to free memory
		module._akkado_clear_result();

		console.log('[Akkado] Returning compile result - success:', success, 'bytecode length:', bytecode?.length);
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
