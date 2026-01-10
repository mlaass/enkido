/**
 * F1 context-sensitive help lookup
 *
 * Provides keyword-to-documentation lookup for the F1 help feature.
 */

import { docsStore } from '$lib/stores/docs.svelte';

/**
 * Trigger F1 help for a keyword
 * @param keyword The word to look up
 * @returns true if documentation was found and navigated to
 */
export function triggerF1Help(keyword: string): boolean {
	if (!keyword || keyword.length < 1) {
		return false;
	}

	// Use the store's lookup function
	return docsStore.f1Lookup(keyword);
}

/**
 * Get word at cursor position from CodeMirror EditorView
 */
export function getWordAtCursor(view: any): string | null {
	const pos = view.state.selection.main.head;
	const line = view.state.doc.lineAt(pos);
	const text = line.text;

	// Find word boundaries
	let start = pos - line.from;
	let end = start;

	// Expand backwards to find word start
	while (start > 0 && /[\w|>%]/.test(text[start - 1])) {
		start--;
	}

	// Expand forwards to find word end
	while (end < text.length && /[\w|>%]/.test(text[end])) {
		end++;
	}

	const word = text.slice(start, end);

	// Handle special cases like |> and %
	if (word === '|>' || word === '%') {
		return word;
	}

	// Return just alphanumeric word
	return word.replace(/[^a-zA-Z0-9_]/g, '') || null;
}
