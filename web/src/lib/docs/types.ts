/**
 * Documentation system types for NKIDO/Akkado
 */

/** Documentation categories */
export type DocCategory = 'builtins' | 'language' | 'mini-notation' | 'concepts' | 'tutorials';

/** Builtin subcategories for grouping */
export type BuiltinCategory =
	| 'oscillators'
	| 'filters'
	| 'envelopes'
	| 'effects'
	| 'distortion'
	| 'dynamics'
	| 'math'
	| 'utility'
	| 'sequencing';

/** Frontmatter schema for all documentation files */
export interface DocFrontmatter {
	/** Display title */
	title: string;
	/** Main category */
	category: DocCategory;
	/** Keywords for search and F1 lookup */
	keywords: string[];
	/** Order within category (for tutorials) */
	order?: number;
	/** Links to related documentation */
	related?: string[];
	/** Subcategory for builtins */
	subcategory?: BuiltinCategory;
}

/** A parsed documentation file */
export interface DocFile {
	/** URL-safe slug derived from filename */
	slug: string;
	/** File path relative to docs/ */
	path: string;
	/** Parsed frontmatter */
	frontmatter: DocFrontmatter;
	/** Raw markdown content (without frontmatter) */
	content: string;
}

/** Entry in the keyword lookup index */
export interface LookupEntry {
	/** The keyword that triggered this match */
	keyword: string;
	/** Document slug */
	slug: string;
	/** Document category */
	category: DocCategory;
	/** Document title */
	title: string;
	/** Optional anchor within the document (e.g., #lp for a specific function) */
	anchor?: string;
}

/** Navigation item for the docs sidebar */
export interface NavItem {
	/** Display label */
	label: string;
	/** Document slug or category ID */
	slug: string;
	/** Whether this is a category header */
	isCategory?: boolean;
	/** Child items (for categories) */
	children?: NavItem[];
	/** Order for sorting */
	order?: number;
}

/** Documentation state for the store */
export interface DocsState {
	/** Currently active category */
	activeCategory: DocCategory;
	/** Currently active document slug */
	activeSlug: string | null;
	/** Anchor within the active document */
	activeAnchor: string | null;
	/** Search query */
	searchQuery: string;
	/** Search results */
	searchResults: LookupEntry[];
	/** Editor code saved before running an example */
	previousEditorCode: string | null;
	/** Whether the docs panel is loading */
	isLoading: boolean;
}
