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
import { audioEngine, type BuiltinsData, type BuiltinInfo } from '$stores/audio.svelte';

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
	const options: Completion[] = [];

	// Load builtins (async, may use cache)
	const builtins = await loadBuiltins();

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
	const docText = context.state.doc.toString();

	// Add user functions
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
