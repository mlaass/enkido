/**
 * Akkado language autocomplete for CodeMirror 6
 *
 * Provides completions for:
 * - Builtin functions (from compiler via WASM)
 * - Builtin aliases
 * - Keywords
 * - User-defined variables and functions
 */

import type { CompletionContext, CompletionResult, Completion } from '@codemirror/autocomplete';
import {
	audioEngine,
	type BuiltinsData,
	type BuiltinInfo,
	type OptionFieldSpec
} from '$stores/audio.svelte';
import {
	getShape,
	getPatternHoleShape,
	type Shape,
	type ShapeField
} from '$lib/editor/akkado-shape-index';

// Cache for builtins data
let builtinsCache: BuiltinsData | null = null;
let builtinsLoading = false;
let builtinsLoadPromise: Promise<BuiltinsData | null> | null = null;

/**
 * Load builtins data from the audio engine (WASM compiler)
 */
async function loadBuiltins(): Promise<BuiltinsData | null> {
	if (builtinsCache) return builtinsCache;

	if (builtinsLoading && builtinsLoadPromise) {
		return builtinsLoadPromise;
	}

	builtinsLoading = true;
	builtinsLoadPromise = audioEngine.getBuiltins();

	const data = await builtinsLoadPromise;
	if (data) {
		builtinsCache = data;
	}
	builtinsLoading = false;

	return data;
}

/**
 * Format a function signature for display
 */
function formatSignature(name: string, info: BuiltinInfo): string {
	const params = info.params.map((p) => {
		if (!p.required) {
			return p.default !== undefined ? `${p.name}=${p.default}` : `${p.name}?`;
		}
		return p.name;
	});
	return `${name}(${params.join(', ')})`;
}

/**
 * Create a completion item for a builtin function
 */
function createBuiltinCompletion(name: string, info: BuiltinInfo, boost: number = 0): Completion {
	const signature = formatSignature(name, info);
	return {
		label: name,
		type: 'function',
		detail: signature,
		info: info.description,
		boost,
		apply: (view, completion, from, to) => {
			// Insert function name with opening paren
			const insert = `${name}(`;
			view.dispatch({
				changes: { from, to, insert },
				selection: { anchor: from + insert.length }
			});
		}
	};
}

/**
 * Create a completion item for a keyword
 */
function createKeywordCompletion(keyword: string): Completion {
	return {
		label: keyword,
		type: 'keyword',
		boost: -1 // Lower priority than functions
	};
}

/**
 * Extract user-defined variables from code
 * Pattern: identifier = expression (at line start or after semicolon)
 */
function extractUserVariables(code: string): string[] {
	const vars: string[] = [];
	const pattern = /(?:^|[;\n])\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*=/gm;
	let match;
	while ((match = pattern.exec(code)) !== null) {
		const name = match[1];
		// Skip keywords
		if (!['fn', 'true', 'false', 'match'].includes(name)) {
			vars.push(name);
		}
	}
	return [...new Set(vars)]; // Remove duplicates
}

/**
 * Extract user-defined functions from code
 * Pattern: fn name(params) = ... or /// docstring\nfn name(params) = ...
 */
interface UserFunction {
	name: string;
	params: string[];
	docstring?: string;
}

function extractUserFunctions(code: string): UserFunction[] {
	const functions: UserFunction[] = [];
	// Match fn declarations with optional preceding docstring
	const pattern = /(?:\/\/\/\s*(.+?)\n)?fn\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\(([^)]*)\)/g;
	let match;
	while ((match = pattern.exec(code)) !== null) {
		const docstring = match[1]?.trim();
		const name = match[2];
		const paramsStr = match[3];
		const params = paramsStr
			.split(',')
			.map((p) => p.trim())
			.filter((p) => p.length > 0);
		functions.push({ name, params, docstring });
	}
	return functions;
}

/**
 * Create completion for a user-defined function
 */
function createUserFunctionCompletion(fn: UserFunction): Completion {
	const signature = `${fn.name}(${fn.params.join(', ')})`;
	return {
		label: fn.name,
		type: 'function',
		detail: signature,
		info: fn.docstring,
		boost: 2, // Higher priority for user-defined
		apply: (view, completion, from, to) => {
			const insert = `${fn.name}(`;
			view.dispatch({
				changes: { from, to, insert },
				selection: { anchor: from + insert.length }
			});
		}
	};
}

/**
 * Create completion for a user-defined variable
 */
function createUserVariableCompletion(name: string): Completion {
	return {
		label: name,
		type: 'variable',
		boost: 1 // Higher than keywords, lower than user functions
	};
}

/**
 * Forward-scan the document to detect whether the cursor sits inside a record
 * literal `{...}` whose parent paren is a call to a known builtin. Returns the
 * matching builtin's option-field schema for that argument slot, or null when:
 *   - the cursor is inside a string or line comment
 *   - the innermost open bracket isn't `{`
 *   - the parent of `{` isn't `(`
 *   - the function being called isn't a known builtin
 *   - the relevant parameter has no option-field schema
 *
 * Single forward pass, O(n) where n = cursor position. No regex backtracking.
 */
type RecordLiteralCtx = {
	builtinName: string;
	argIndex: number;
	optionFields: OptionFieldSpec[];
	acceptsSpread: boolean;
};

function detectRecordLiteralCtx(
	text: string,
	pos: number,
	builtins: BuiltinsData
): RecordLiteralCtx | null {
	type Frame = { kind: '(' | '[' | '{'; openPos: number; argIndex: number };
	const stack: Frame[] = [];
	let inString: '"' | "'" | null = null;

	for (let i = 0; i < pos; i++) {
		const ch = text[i];

		if (inString) {
			if (ch === '\\') {
				i++; // skip the escaped character
				continue;
			}
			if (ch === inString) inString = null;
			continue;
		}

		// Line comment runs to end of line
		if (ch === '/' && text[i + 1] === '/') {
			while (i < pos && text[i] !== '\n') i++;
			continue;
		}

		if (ch === '"' || ch === "'") {
			inString = ch;
			continue;
		}

		if (ch === '(' || ch === '[' || ch === '{') {
			stack.push({ kind: ch, openPos: i, argIndex: 0 });
			continue;
		}

		if (ch === ')' || ch === ']' || ch === '}') {
			stack.pop();
			continue;
		}

		if (ch === ',' && stack.length > 0) {
			stack[stack.length - 1].argIndex++;
		}
	}

	if (inString) return null;
	if (stack.length < 2) return null;

	const top = stack[stack.length - 1];
	if (top.kind !== '{') return null;

	const parent = stack[stack.length - 2];
	if (parent.kind !== '(') return null;

	// Find the function name immediately before `parent.openPos`. Skip whitespace,
	// then walk back over an identifier.
	let j = parent.openPos - 1;
	while (j >= 0 && /\s/.test(text[j])) j--;
	let nameEnd = j + 1;
	while (j >= 0 && /[a-zA-Z0-9_]/.test(text[j])) j--;
	const nameStart = j + 1;
	if (nameStart >= nameEnd) return null;

	const fnName = text.slice(nameStart, nameEnd);
	const canonical = builtins.aliases[fnName] ?? fnName;
	const fn = builtins.functions[canonical];
	if (!fn) return null;

	const param = fn.params[parent.argIndex];
	if (!param || !param.optionFields || param.optionFields.length === 0) return null;

	return {
		builtinName: canonical,
		argIndex: parent.argIndex,
		optionFields: param.optionFields,
		acceptsSpread: param.acceptsSpread ?? true
	};
}

function formatOptionDetail(f: OptionFieldSpec): string {
	const parts: string[] = [f.type];
	if (f.default !== undefined && f.default !== '') parts.push(`= ${f.default}`);
	if (f.type === 'enum' && f.values) parts.push(`(${f.values})`);
	return parts.join(' ');
}

/**
 * Detect a `.`-context immediately before the cursor.
 *
 * Walks backwards from `pos - 1`, skipping whitespace, and recognises:
 *   - `<identifier>.` → kind: 'identifier', name: <identifier>
 *   - `@.` or `%.`    → kind: 'hole' (`@` is canonical; `%` is a legacy alias)
 *
 * Reuses the same string/comment guard from `detectRecordLiteralCtx` so we
 * never trigger inside string literals (forward scan to confirm string
 * state at `pos`).
 */
type DotCtx =
	| { kind: 'identifier'; name: string; from: number }
	| { kind: 'hole'; from: number };

function isInString(text: string, pos: number): boolean {
	let inString: '"' | "'" | null = null;
	for (let i = 0; i < pos; i++) {
		const ch = text[i];
		if (inString) {
			if (ch === '\\') { i++; continue; }
			if (ch === inString) inString = null;
			continue;
		}
		if (ch === '/' && text[i + 1] === '/') {
			while (i < pos && text[i] !== '\n') i++;
			continue;
		}
		if (ch === '"' || ch === "'") inString = ch;
	}
	return inString !== null;
}

function detectDotContext(text: string, pos: number): DotCtx | null {
	if (isInString(text, pos)) return null;

	// Walk backwards from pos to find an identifier-or-`%` then `.`.
	let i = pos - 1;
	// Skip identifier characters (the partial word the user is typing).
	while (i >= 0 && /[a-zA-Z0-9_]/.test(text[i])) i--;
	if (i < 0 || text[i] !== '.') return null;

	const insertFrom = i + 1; // first character of the (possibly partial) field name
	i--; // step before the dot
	while (i >= 0 && /\s/.test(text[i])) i--;
	if (i < 0) return null;

	if (text[i] === '@' || text[i] === '%') {
		return { kind: 'hole', from: insertFrom };
	}

	// Walk back over the identifier preceding the dot.
	let nameEnd = i + 1;
	while (i >= 0 && /[a-zA-Z0-9_]/.test(text[i])) i--;
	const nameStart = i + 1;
	if (nameStart >= nameEnd) return null;

	// The identifier could be a numeric index (e.g. `voices.0.x`); reject
	// for v1 — Array-of-Record completion via `arr.0.` is deferred.
	const name = text.slice(nameStart, nameEnd);
	if (/^\d+$/.test(name)) return null;

	return { kind: 'identifier', name, from: insertFrom };
}

/**
 * Detect the dotless hole-field shorthand: `@<partial>` or `%<partial>`
 * adjacent to the cursor (no dot, no whitespace between hole and identifier).
 * Returns a `hole` context so pattern-field suggestions surface the same
 * way they do for the dotted form. Per PRD prd-hole-field-shorthand §3.4,
 * whitespace defeats the shorthand — we honour that here so `@ as e`
 * remains an `as`-binding intent, not a field-name autocompletion.
 */
function detectHoleDotlessContext(text: string, pos: number): DotCtx | null {
	if (isInString(text, pos)) return null;

	// Walk backwards skipping the (possibly partial) field name.
	let i = pos - 1;
	while (i >= 0 && /[a-zA-Z0-9_]/.test(text[i])) i--;

	// The character immediately before the identifier must be `@` or `%`
	// (no whitespace) — that is the adjacency rule from the parser.
	if (i < 0) return null;
	if (text[i] !== '@' && text[i] !== '%') return null;

	// `from` is the first char of the (possibly empty) partial field name.
	return { kind: 'hole', from: i + 1 };
}

function fieldDetailLabel(f: ShapeField): string {
	if (f.aliasOf) return `${f.type} (alias of ${f.aliasOf})`;
	if (f.source === 'set') return `${f.type} (custom)`;
	return f.type;
}

function shapeFieldToCompletion(f: ShapeField): Completion {
	return {
		label: f.name,
		type: 'property',
		detail: fieldDetailLabel(f),
		// Aliases rank below their canonical sibling so the popup shows
		// canonical-first.
		boost: f.aliasOf ? -0.25 : 0
	};
}

function completionsFromShape(shape: Shape, from: number): CompletionResult {
	return {
		from,
		options: shape.fields.map(shapeFieldToCompletion),
		validFor: /^[a-zA-Z_][a-zA-Z0-9_]*$/
	};
}

function createOptionFieldCompletion(f: OptionFieldSpec): Completion {
	return {
		label: f.name,
		type: 'property',
		detail: formatOptionDetail(f),
		info: f.description ?? '',
		boost: 3, // Above user functions — record-literal context is precise
		apply: (view, _completion, from, to) => {
			const insert = `${f.name}: `;
			view.dispatch({
				changes: { from, to, insert },
				selection: { anchor: from + insert.length }
			});
		}
	};
}

/**
 * CodeMirror completion source for Akkado
 */
export async function akkadoCompletions(context: CompletionContext): Promise<CompletionResult | null> {
	// Get word before cursor
	const word = context.matchBefore(/[a-zA-Z_][a-zA-Z0-9_]*/);

	// Don't show completions in the middle of a word (unless explicit)
	if (!word && !context.explicit) return null;

	// Don't show completions for very short words unless explicit
	if (word && word.text.length < 2 && !context.explicit) return null;

	const from = word ? word.from : context.pos;

	// Load builtins (async, may use cache)
	const builtins = await loadBuiltins();
	const docText = context.state.doc.toString();

	// Record-literal context: cursor is inside `{...}` whose parent is a builtin
	// call. Suppress all other completions and surface only the option fields
	// declared in the builtin's schema (PRD prd-records-system-unification §5.1).
	if (builtins) {
		const recordCtx = detectRecordLiteralCtx(docText, context.pos, builtins);
		if (recordCtx) {
			return {
				from,
				options: recordCtx.optionFields.map(createOptionFieldCompletion),
				validFor: /^[a-zA-Z_][a-zA-Z0-9_]*$/
			};
		}
	}

	// Dot context: `r.` or `@.`/`%.` — surface analyzer-driven shape fields when
	// the editor has a cached shape index for the target binding / pattern
	// hole. Phase 2 of records-system-unification PRD §5.2.
	const dotCtx = detectDotContext(docText, context.pos);
	if (dotCtx) {
		const shape =
			dotCtx.kind === 'identifier' ? getShape(dotCtx.name) : getPatternHoleShape();
		if (shape && shape.fields.length > 0) {
			return completionsFromShape(shape, dotCtx.from);
		}
		// Fall through if no shape — let the general completions try
		// (they're harmless after a dot since they validate against the
		// identifier regex).
	}

	// Dotless hole-field shorthand: `@field` / `%field`. Per
	// prd-hole-field-shorthand, the parser accepts a field name immediately
	// adjacent to the hole; surface the same pattern-hole shape suggestions
	// we do for the dotted form.
	const holeCtx = detectHoleDotlessContext(docText, context.pos);
	if (holeCtx) {
		const shape = getPatternHoleShape();
		if (shape && shape.fields.length > 0) {
			return completionsFromShape(shape, holeCtx.from);
		}
	}

	const options: Completion[] = [];

	if (builtins) {
		// Add builtin functions
		for (const [name, info] of Object.entries(builtins.functions)) {
			options.push(createBuiltinCompletion(name, info));
		}

		// Add aliases (with reference to canonical name)
		for (const [alias, canonical] of Object.entries(builtins.aliases)) {
			const info = builtins.functions[canonical];
			if (info) {
				options.push({
					label: alias,
					type: 'function',
					detail: `\u2192 ${canonical}`,
					info: info.description,
					boost: -0.5, // Slightly lower than canonical names
					apply: (view, completion, from, to) => {
						const insert = `${alias}(`;
						view.dispatch({
							changes: { from, to, insert },
							selection: { anchor: from + insert.length }
						});
					}
				});
			}
		}

		// Add keywords
		for (const keyword of builtins.keywords) {
			options.push(createKeywordCompletion(keyword));
		}
	}

	// Extract user-defined symbols from current document
	const userFunctions = extractUserFunctions(docText);
	for (const fn of userFunctions) {
		options.push(createUserFunctionCompletion(fn));
	}

	// Add user variables
	const userVars = extractUserVariables(docText);
	for (const varName of userVars) {
		// Don't add if it's also a function name
		if (!userFunctions.some((f) => f.name === varName)) {
			options.push(createUserVariableCompletion(varName));
		}
	}

	return {
		from,
		options,
		validFor: /^[a-zA-Z_][a-zA-Z0-9_]*$/
	};
}
