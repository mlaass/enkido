/**
 * Documentation system entry point
 *
 * Loads and indexes all documentation files from /static/docs/
 */

import { docsStore } from '$lib/stores/docs.svelte';
import { parseMarkdown, buildLookupEntries } from './parser';
import type { DocFile } from './types';

// List of all documentation files to load
// These are copied from docs/akkado/ to web/static/docs/ by the copy-docs script
const DOC_FILES = [
	'reference/builtins/oscillators.md',
	'reference/builtins/filters.md',
	'reference/builtins/envelopes.md',
	'reference/builtins/utility.md',
	'reference/language/pipes.md',
	'tutorials/01-hello-sine.md',
	'DOCUMENTATION_GUIDE.md'
];

let initialized = false;

/**
 * Initialize the documentation system by loading all markdown files
 */
export async function initializeDocs(): Promise<void> {
	console.log('[Docs] initializeDocs called');
	if (initialized) return;

	try {
		// Load all documentation files in parallel
		const loadPromises = DOC_FILES.map(async (path) => {
			const response = await fetch(`/docs/${path}`);
			if (!response.ok) {
				console.warn(`[Docs] Failed to load ${path}: ${response.status}`);
				return null;
			}
			const raw = await response.text();
			return parseMarkdown(raw, path);
		});

		const docs = (await Promise.all(loadPromises)).filter(Boolean);
		console.log('[Docs] Loaded docs:', docs.length);

		// Register all documents and build lookup index
		for (const doc of docs) {
			if (doc) {
				console.log('[Docs] Registering:', doc.slug);
				docsStore.registerDocument(doc);
				const lookupEntries = buildLookupEntries(doc);
				docsStore.addLookupEntries(lookupEntries);
			}
		}

		initialized = true;
		docsStore.setInitialized();
		console.log('[Docs] setInitialized called');
		console.log(`[Docs] Initialized with ${docs.length} documents`);
	} catch (err) {
		console.error('[Docs] Failed to initialize:', err);
	}
}

/**
 * Get a documentation file by slug
 */
export function getDoc(slug: string): DocFile | undefined {
	return docsStore.getDocument(slug);
}

// Re-export types and components
export * from './types';
export { parseMarkdown, renderMarkdown, extractHeadings } from './parser';
