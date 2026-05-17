/**
 * Editor state store using Svelte 5 runes.
 *
 * Phase 1 (shareable patches): `code`/`setCode` delegate to the drafts store
 * when persistence is enabled (main app). When persistence is disabled
 * (embed mode), the editor reverts to an in-memory-only buffer that does
 * not touch the drafts list — preserves the embed iframe semantics.
 */

import { audioEngine } from './audio.svelte';
import { draftsStore, FALLBACK_CODE } from './drafts.svelte';

export interface EditorDiagnostic {
	severity: number;  // 0=Info, 1=Warning, 2=Error
	message: string;
	line: number;      // 1-based
	column: number;    // 1-based
}

interface EditorState {
	/** Local mirror used when persistence is disabled. With persistence on, the
	 *  source of truth is `draftsStore.activeCode`. */
	embedCode: string;
	hasUnsavedChanges: boolean;
	lastCompileError: string | null;
	lastCompileTime: number | null;
	isEvaluating: boolean;
	diagnostics: EditorDiagnostic[];
}

function createEditorStore() {
	let state = $state<EditorState>({
		embedCode: FALLBACK_CODE,
		hasUnsavedChanges: false,
		lastCompileError: null,
		lastCompileTime: null,
		isEvaluating: false,
		diagnostics: []
	});

	// When false, setCode is in-memory-only (no drafts-store writes).
	// The embed route flips this off so it doesn't clobber the main app's drafts.
	let persistenceEnabled = true;

	// Save immediately on page unload
	if (typeof window !== 'undefined') {
		window.addEventListener('beforeunload', () => {
			if (persistenceEnabled) draftsStore.flushPendingSave();
		});
	}

	function getCode(): string {
		return persistenceEnabled ? draftsStore.activeCode : state.embedCode;
	}

	function setCode(code: string) {
		state.hasUnsavedChanges = true;
		if (persistenceEnabled) {
			draftsStore.updateActiveCode(code);
		} else {
			state.embedCode = code;
		}
	}

	function setPersistenceEnabled(enabled: boolean) {
		if (persistenceEnabled === enabled) return;
		if (persistenceEnabled && !enabled) {
			// switching off: flush pending save, then snapshot active code into embedCode
			draftsStore.flushPendingSave();
			state.embedCode = draftsStore.activeCode;
		}
		persistenceEnabled = enabled;
	}

	function reloadFromPersistence() {
		// When persistence is on, draftsStore is already the source of truth.
		// When persistence is off (embed), reset the embed buffer to the
		// current active draft's code — matches legacy reloadFromPersistence
		// semantics (the main app's saved code is restored after embed unmount).
		state.embedCode = draftsStore.activeCode;
		state.hasUnsavedChanges = false;
		state.diagnostics = [];
		state.lastCompileError = null;
	}

	function setCompileError(error: string | null) {
		state.lastCompileError = error;
	}

	function setDiagnostics(diagnostics: EditorDiagnostic[]) {
		state.diagnostics = diagnostics;
		if (diagnostics.length === 0) {
			state.lastCompileError = null;
		}
	}

	function markCompiled() {
		state.lastCompileTime = Date.now();
		state.hasUnsavedChanges = false;
	}

	function reset() {
		if (persistenceEnabled) {
			draftsStore.updateActiveCode(FALLBACK_CODE);
		} else {
			state.embedCode = FALLBACK_CODE;
		}
		state.hasUnsavedChanges = false;
		state.lastCompileError = null;
		state.lastCompileTime = null;
	}

	async function evaluate(): Promise<boolean> {
		if (state.isEvaluating) return false;
		state.isEvaluating = true;

		console.log('[Editor] evaluate() called');

		try {
			if (!audioEngine.isInitialized) {
				console.log('[Editor] Initializing audio engine first...');
				await audioEngine.play();
			}

			const code = getCode();
			console.log('[Editor] Sending source to worklet for compilation...');
			const result = await audioEngine.compile(code);
			console.log('[Editor] Compile result:', result);

			if (result.success) {
				markCompiled();
				setCompileError(null);
				setDiagnostics([]);
				console.log('[Editor] Compiled and loaded, bytecode size:', result.bytecodeSize);

				if (!audioEngine.isPlaying) {
					audioEngine.play();
				}

				return true;
			} else {
				const diagnostics = result.diagnostics || [];
				setDiagnostics(diagnostics);

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
		get code() { return getCode(); },
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
		evaluate,
		setPersistenceEnabled,
		reloadFromPersistence
	};
}

export const editorStore = createEditorStore();
