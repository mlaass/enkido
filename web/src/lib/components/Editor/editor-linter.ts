/**
 * CodeMirror linter integration for compilation errors
 */

import { lintGutter, setDiagnostics, type Diagnostic } from '@codemirror/lint';
import type { EditorView } from '@codemirror/view';
import type { EditorDiagnostic } from '$lib/stores/editor.svelte';

/**
 * Convert our diagnostics to CodeMirror format and update the editor
 */
export function updateEditorDiagnostics(view: EditorView, diagnostics: EditorDiagnostic[]) {
	const cmDiagnostics: Diagnostic[] = [];

	for (const d of diagnostics) {
		// Validate line number is within document bounds
		if (d.line < 1 || d.line > view.state.doc.lines) {
			continue;
		}

		const line = view.state.doc.line(d.line);
		const from = Math.min(line.from + Math.max(0, d.column - 1), line.to);
		const to = line.to;

		cmDiagnostics.push({
			from,
			to,
			severity: d.severity === 2 ? 'error' : d.severity === 1 ? 'warning' : 'info',
			message: d.message
		});
	}

	view.dispatch(setDiagnostics(view.state, cmDiagnostics));
}

/**
 * Clear all diagnostics from the editor
 */
export function clearEditorDiagnostics(view: EditorView) {
	view.dispatch(setDiagnostics(view.state, []));
}

/**
 * Linter extensions to add to CodeMirror
 * Includes gutter for error indicators
 */
export const linterExtensions = [
	lintGutter()
];
