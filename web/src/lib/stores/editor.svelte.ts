/**
 * Editor state store using Svelte 5 runes
 */

import { compile } from '$lib/compiler/akkado';
import { audioEngine } from './audio.svelte';

interface EditorState {
	code: string;
	hasUnsavedChanges: boolean;
	lastCompileError: string | null;
	lastCompileTime: number | null;
	isEvaluating: boolean;
}

const DEFAULT_CODE = `// Welcome to Enkido!
// Press Ctrl+Enter to evaluate

bpm = 120

// Simple sine wave
sin(440) |> out(%, %)
`;

function createEditorStore() {
	let state = $state<EditorState>({
		code: DEFAULT_CODE,
		hasUnsavedChanges: false,
		lastCompileError: null,
		lastCompileTime: null,
		isEvaluating: false
	});

	function setCode(code: string) {
		state.code = code;
		state.hasUnsavedChanges = true;
	}

	function setCompileError(error: string | null) {
		state.lastCompileError = error;
	}

	function markCompiled() {
		state.lastCompileTime = Date.now();
		state.hasUnsavedChanges = false;
	}

	function reset() {
		state.code = DEFAULT_CODE;
		state.hasUnsavedChanges = false;
		state.lastCompileError = null;
		state.lastCompileTime = null;
	}

	/**
	 * Compile and run the current code
	 * Used by both Ctrl+Enter and Play button
	 */
	async function evaluate(): Promise<boolean> {
		if (state.isEvaluating) return false;
		state.isEvaluating = true;

		console.log('[Editor] evaluate() called');

		try {
			// Compile with akkado.wasm
			console.log('[Editor] Calling compile()...');
			const result = await compile(state.code);
			console.log('[Editor] Compile result:', result);

			if (result.success && result.bytecode) {
				// Ensure audio engine is initialized before loading program
				if (!audioEngine.isInitialized) {
					console.log('[Editor] Initializing audio engine first...');
					await audioEngine.play();
					// Give the worklet time to initialize
					await new Promise(resolve => setTimeout(resolve, 500));
				}
				// Send bytecode to Cedar VM
				audioEngine.loadProgram(result.bytecode);
				markCompiled();
				setCompileError(null);
				console.log('[Editor] Compiled and loaded bytecode:', result.bytecode.length, 'bytes');

				// Start playback if not already playing
				if (!audioEngine.isPlaying) {
					audioEngine.play();
				}

				return true;
			} else {
				// Show first error
				const firstError = result.diagnostics.find(d => d.severity === 2);
				const errorMsg = firstError
					? `${firstError.message} (line ${firstError.line})`
					: 'Compilation failed';
				setCompileError(errorMsg);
				console.error('[Editor] Compile error:', errorMsg);
				return false;
			}
		} catch (err) {
			const errorMsg = err instanceof Error ? err.message : String(err);
			setCompileError(errorMsg);
			console.error('[Editor] Compile exception:', err);
			return false;
		} finally {
			state.isEvaluating = false;
			console.log('[Editor] evaluate() finished');
		}
	}

	return {
		get code() { return state.code; },
		get hasUnsavedChanges() { return state.hasUnsavedChanges; },
		get lastCompileError() { return state.lastCompileError; },
		get lastCompileTime() { return state.lastCompileTime; },
		get isEvaluating() { return state.isEvaluating; },

		setCode,
		setCompileError,
		markCompiled,
		reset,
		evaluate
	};
}

export const editorStore = createEditorStore();
