/**
 * Analyzer-driven shape index for editor autocomplete.
 *
 * Phase 2 of `docs/prd-records-system-unification.md`. The editor pulls
 * `akkado_get_shape_index` from the WASM module on idle (300 ms after the
 * last keystroke or selection change), caches the result keyed by
 * `(sourceHash, cursorOffset)`, and the completion source consumes it for
 * `r.` / `%.` / `arr.0.` field-name suggestions.
 */

/** Fetcher abstraction so this module doesn't directly import the audio
 *  store (which uses Svelte 5 runes and isn't loadable by Vitest's plain
 *  TS resolver). The editor wires the real fetcher on startup via
 *  `setShapeIndexFetcher`. */
type ShapeIndexFetcher = (
	source: string,
	cursorOffset: number
) => Promise<ShapeIndex | null>;

let fetcher: ShapeIndexFetcher | null = null;

/** Configure how `scheduleShapeIndex` reaches WASM. Called once at editor
 *  startup; subsequent calls overwrite (used by tests). */
export function setShapeIndexFetcher(fn: ShapeIndexFetcher): void {
	fetcher = fn;
}

export type ShapeField = {
	name: string;
	type: string;
	fixed?: boolean;
	aliasOf?: string;
	source?: string;
};

export type RecordShape = { kind: 'record'; fields: ShapeField[] };
export type PatternShape = { kind: 'pattern'; fields: ShapeField[] };
export type ArrayShape = {
	kind: 'array';
	elementKind?: 'record';
	fields: ShapeField[];
};
export type UnknownShape = { kind: 'unknown'; fields: ShapeField[] };
export type Shape = RecordShape | PatternShape | ArrayShape | UnknownShape;

export type ShapeIndex = {
	version: number;
	sourceHash: number;
	bindings: Record<string, Shape>;
	patternHole?: Shape;
};

/** Sentinel passed when the editor wants top-level bindings only and has
 *  no meaningful cursor (e.g. when the caret is inside a string literal). */
export const SHAPE_INDEX_NO_CURSOR = 0xffffffff;

let cached: ShapeIndex | null = null;
let lastKey = '';
let pending: Promise<ShapeIndex | null> | null = null;
let timer: ReturnType<typeof setTimeout> | null = null;

const DEBOUNCE_MS = 300;

/**
 * Schedule a shape-index refresh. Coalesces by `(fnv1a(source), cursorOffset)`
 * — repeated calls within the debounce window collapse into a single WASM
 * call. Safe to call on every keystroke / selection change.
 */
export function scheduleShapeIndex(source: string, cursorOffset: number): void {
	if (timer !== null) clearTimeout(timer);
	timer = setTimeout(() => {
		void refreshShapeIndex(source, cursorOffset);
	}, DEBOUNCE_MS);
}

async function refreshShapeIndex(source: string, cursorOffset: number): Promise<void> {
	if (!fetcher) return; // editor hasn't wired the fetcher yet
	const key = `${fnv1a32(source)}:${cursorOffset >>> 0}`;
	if (key === lastKey) return;
	if (pending) return; // existing call will write back; the next debounced
	                     // tick will catch any newer source.
	const next = fetcher(source, cursorOffset);
	pending = next;
	try {
		const data = await next;
		cached = data ?? null;
		lastKey = key;
	} finally {
		pending = null;
	}
}

/** Read the cached shape for a binding name. Returns undefined when no
 *  shape index is available yet (initial load) or the binding doesn't
 *  expose a shape. */
export function getShape(name: string): Shape | undefined {
	return cached?.bindings[name];
}

/** Read the cached pattern-hole shape (corresponds to `@` in the enclosing
 *  pipe). Undefined when no pipe / no Pattern LHS / cursor outside any pipe. */
export function getPatternHoleShape(): Shape | undefined {
	return cached?.patternHole;
}

/** Drop the cached shape index — used on session reset / file switch. */
export function clearShapeIndex(): void {
	cached = null;
	lastKey = '';
}

/**
 * Convert a CodeMirror character offset to a UTF-8 byte offset. Akkado
 * source is mostly ASCII but string literals can contain unicode.
 */
export function utf8ByteOffset(text: string, charPos: number): number {
	if (charPos <= 0) return 0;
	if (charPos >= text.length) return new TextEncoder().encode(text).length;
	return new TextEncoder().encode(text.slice(0, charPos)).length;
}

/** FNV-1a 32-bit over UTF-8 — identical to the C++ side
 *  (`shape_index.cpp::hash_source`). */
export function fnv1a32(s: string): number {
	let h = 2166136261;
	const bytes = new TextEncoder().encode(s);
	for (let i = 0; i < bytes.length; i++) {
		h ^= bytes[i];
		// FNV prime 16777619 — multiply with Math.imul to keep result in 32-bit.
		h = Math.imul(h, 16777619);
	}
	return h >>> 0;
}
