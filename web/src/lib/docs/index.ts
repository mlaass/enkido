/**
 * Documentation system entry point
 *
 * Docs are loaded on-demand when navigating. The manifest provides
 * slug-to-path mapping, navigation structure, and F1 lookup index.
 */

import { docsStore } from '$lib/stores/docs.svelte';

/**
 * Initialize the documentation system
 * Just marks docs as ready - actual content is loaded on-demand
 */
export function initializeDocs(): void {
	docsStore.setInitialized();
}

// Re-export types and utilities
export * from './types';
export { renderMarkdown, extractHeadings } from './parser';
export { slugToPath, navigation, lookup } from './manifest';
