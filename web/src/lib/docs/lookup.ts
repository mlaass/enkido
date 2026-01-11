/**
 * F1 context-sensitive help lookup
 *
 * Uses pre-built lookup index for instant keyword-to-documentation lookup.
 */

import { docsStore } from '$lib/stores/docs.svelte';
import { lookup as lookupIndex } from './manifest';

/**
 * Trigger F1 help for a keyword
 * @param keyword The word to look up
 * @returns true if documentation was found and navigated to
 */
export function triggerF1Help(keyword: string): boolean {
	if (!keyword || keyword.length < 1) {
		return false;
	}

	const entry = lookupIndex[keyword.toLowerCase()];

	if (entry) {
		docsStore.setCategory(entry.category as any);
		docsStore.setDocument(entry.slug, entry.anchor);
		return true;
	}

	return false;
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
