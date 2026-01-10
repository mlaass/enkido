/**
 * Documentation state store using Svelte 5 runes
 */

import type { DocCategory, DocsState, LookupEntry, DocFile, NavItem } from '$lib/docs/types';

function createDocsStore() {
	let state = $state<DocsState>({
		activeCategory: 'builtins',
		activeSlug: null,
		activeAnchor: null,
		searchQuery: '',
		searchResults: [],
		previousEditorCode: null,
		isLoading: false
	});

	// Track if docs have been initialized
	let isInitialized = $state(false);

	// Lookup index for F1 help
	let lookupIndex = $state<Map<string, LookupEntry>>(new Map());

	// Loaded documents
	let documents = $state<Map<string, DocFile>>(new Map());

	// Navigation structure
	let navigation = $state<NavItem[]>([]);

	/**
	 * Set the active category
	 */
	function setCategory(category: DocCategory) {
		state.activeCategory = category;
		state.activeSlug = null;
		state.activeAnchor = null;
	}

	/**
	 * Navigate to a specific document
	 */
	function setDocument(slug: string, anchor?: string) {
		state.activeSlug = slug;
		state.activeAnchor = anchor || null;
	}

	/**
	 * Set the search query and filter results
	 */
	function setSearchQuery(query: string) {
		state.searchQuery = query;
		if (query.length < 2) {
			state.searchResults = [];
			return;
		}

		const lowerQuery = query.toLowerCase();
		const results: LookupEntry[] = [];

		for (const [keyword, entry] of lookupIndex) {
			if (keyword.includes(lowerQuery)) {
				results.push(entry);
			}
		}

		// Sort by relevance (exact matches first, then by keyword length)
		results.sort((a, b) => {
			const aExact = a.keyword === lowerQuery;
			const bExact = b.keyword === lowerQuery;
			if (aExact && !bExact) return -1;
			if (!aExact && bExact) return 1;
			return a.keyword.length - b.keyword.length;
		});

		// Deduplicate by slug+anchor
		const seen = new Set<string>();
		state.searchResults = results.filter(entry => {
			const key = `${entry.slug}#${entry.anchor || ''}`;
			if (seen.has(key)) return false;
			seen.add(key);
			return true;
		}).slice(0, 20); // Limit to 20 results
	}

	/**
	 * F1 lookup - find documentation for a keyword
	 * Returns true if found and navigates to it
	 */
	function f1Lookup(keyword: string): boolean {
		const lowerKeyword = keyword.toLowerCase();
		const entry = lookupIndex.get(lowerKeyword);

		if (entry) {
			state.activeCategory = entry.category;
			state.activeSlug = entry.slug;
			state.activeAnchor = entry.anchor || null;
			return true;
		}

		return false;
	}

	/**
	 * Save the current editor code before running an example
	 */
	function saveEditorCode(code: string) {
		state.previousEditorCode = code;
	}

	/**
	 * Restore the saved editor code
	 */
	function restoreEditorCode(): string | null {
		const code = state.previousEditorCode;
		state.previousEditorCode = null;
		return code;
	}

	/**
	 * Check if there's saved code to restore
	 */
	function hasSavedCode(): boolean {
		return state.previousEditorCode !== null;
	}

	/**
	 * Register a document in the store
	 */
	function registerDocument(doc: DocFile) {
		documents.set(doc.slug, doc);
	}

	/**
	 * Add entries to the lookup index
	 */
	function addLookupEntries(entries: LookupEntry[]) {
		for (const entry of entries) {
			// Don't overwrite existing entries (first one wins)
			if (!lookupIndex.has(entry.keyword)) {
				lookupIndex.set(entry.keyword, entry);
			}
		}
	}

	/**
	 * Set the navigation structure
	 */
	function setNavigation(nav: NavItem[]) {
		navigation = nav;
	}

	/**
	 * Get a document by slug
	 */
	function getDocument(slug: string): DocFile | undefined {
		return documents.get(slug);
	}

	/**
	 * Mark docs as initialized
	 */
	function setInitialized() {
		isInitialized = true;
	}

	return {
		// State getters
		get activeCategory() { return state.activeCategory; },
		get activeSlug() { return state.activeSlug; },
		get activeAnchor() { return state.activeAnchor; },
		get searchQuery() { return state.searchQuery; },
		get searchResults() { return state.searchResults; },
		get previousEditorCode() { return state.previousEditorCode; },
		get isLoading() { return state.isLoading; },
		get navigation() { return navigation; },
		get isInitialized() { return isInitialized; },

		// Methods
		setCategory,
		setDocument,
		setSearchQuery,
		f1Lookup,
		saveEditorCode,
		restoreEditorCode,
		hasSavedCode,
		registerDocument,
		addLookupEntries,
		setNavigation,
		getDocument,
		setInitialized
	};
}

export const docsStore = createDocsStore();
