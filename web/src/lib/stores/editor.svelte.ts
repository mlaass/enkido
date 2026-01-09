/**
 * Editor state store using Svelte 5 runes
 */

interface EditorState {
	code: string;
	hasUnsavedChanges: boolean;
	lastCompileError: string | null;
	lastCompileTime: number | null;
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
		lastCompileTime: null
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

	return {
		get code() { return state.code; },
		get hasUnsavedChanges() { return state.hasUnsavedChanges; },
		get lastCompileError() { return state.lastCompileError; },
		get lastCompileTime() { return state.lastCompileTime; },

		setCode,
		setCompileError,
		markCompiled,
		reset
	};
}

export const editorStore = createEditorStore();
