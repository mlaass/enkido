/**
 * Editor state store using Svelte 5 runes
 */

import { audioEngine } from './audio.svelte';

export interface EditorDiagnostic {
	severity: number;  // 0=Info, 1=Warning, 2=Error
	message: string;
	line: number;      // 1-based
	column: number;    // 1-based
}

interface EditorState {
	code: string;
	hasUnsavedChanges: boolean;
	lastCompileError: string | null;
	lastCompileTime: number | null;
	isEvaluating: boolean;
	diagnostics: EditorDiagnostic[];
}

const STORAGE_KEY = 'nkido-editor-code';

const DEFAULT_CODE = `// Welcome to NKIDO!
// Press Ctrl+Enter to evaluate

bpm = 120

// Simple sine wave
sin(440) |> out(%, %)
`;

function loadCode(): string {
	if (typeof localStorage === 'undefined') return DEFAULT_CODE;
	try {
		const stored = localStorage.getItem(STORAGE_KEY);
		return stored || DEFAULT_CODE;
	} catch {
		return DEFAULT_CODE;
	}
}

function saveCode(code: string) {
	if (typeof localStorage === 'undefined') return;
	try {
		localStorage.setItem(STORAGE_KEY, code);
	} catch (e) {
		console.warn('Failed to save code:', e);
	}
}

let saveTimeout: ReturnType<typeof setTimeout> | null = null;

function debouncedSaveCode(code: string) {
	if (saveTimeout) clearTimeout(saveTimeout);
	saveTimeout = setTimeout(() => saveCode(code), 500);
}

function createEditorStore() {
	let state = $state<EditorState>({
		code: loadCode(),
		hasUnsavedChanges: false,
		lastCompileError: null,
		lastCompileTime: null,
		isEvaluating: false,
		diagnostics: []
	});

	// Save immediately on page unload
	if (typeof window !== 'undefined') {
		window.addEventListener('beforeunload', () => {
			if (saveTimeout) clearTimeout(saveTimeout);
			saveCode(state.code);
		});
	}

	function setCode(code: string) {
		state.code = code;
		state.hasUnsavedChanges = true;
		debouncedSaveCode(code);
	}

	function setCompileError(error: string | null) {
		state.lastCompileError = error;
	}

	function setDiagnostics(diagnostics: EditorDiagnostic[]) {
		state.diagnostics = diagnostics;
		// Clear compile error when diagnostics are cleared
		if (diagnostics.length === 0) {
			state.lastCompileError = null;
		}
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
	 * Compilation happens in the AudioWorklet for atomic loading
	 */
	async function evaluate(): Promise<boolean> {
		if (state.isEvaluating) return false;
		state.isEvaluating = true;

		console.log('[Editor] evaluate() called');

		try {
			// Ensure audio engine is initialized first
			if (!audioEngine.isInitialized) {
				console.log('[Editor] Initializing audio engine first...');
				await audioEngine.play();
			}

			// Compile in the worklet - this is atomic with loading
			console.log('[Editor] Sending source to worklet for compilation...');
			const result = await audioEngine.compile(state.code);
			console.log('[Editor] Compile result:', result);

			if (result.success) {
				markCompiled();
				setCompileError(null);
				setDiagnostics([]);  // Clear diagnostics on success
				console.log('[Editor] Compiled and loaded, bytecode size:', result.bytecodeSize);

				// Start playback if not already playing
				if (!audioEngine.isPlaying) {
					audioEngine.play();
				}

				return true;
			} else {
				// Store all diagnostics for inline display
				const diagnostics = result.diagnostics || [];
				setDiagnostics(diagnostics);

				// Show first error in status bar
				const firstError = diagnostics.find(d => d.severity === 2);
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
		get diagnostics() { return state.diagnostics; },

		setCode,
		setCompileError,
		setDiagnostics,
		markCompiled,
		reset,
		evaluate
	};
}

export const editorStore = createEditorStore();
