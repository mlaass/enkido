/**
 * Signature help tooltip for Akkado
 *
 * Shows function signature while typing inside parentheses.
 * Highlights the current parameter.
 */

import { EditorView, showTooltip, ViewPlugin } from '@codemirror/view';
import type { Tooltip, ViewUpdate } from '@codemirror/view';
import { StateField, StateEffect } from '@codemirror/state';
import { audioEngine, type BuiltinsData, type BuiltinInfo, type BuiltinParam } from '$stores/audio.svelte';

// Cache for builtins
let builtinsCache: BuiltinsData | null = null;

/**
 * Load builtins (uses shared cache)
 */
async function getBuiltins(): Promise<BuiltinsData | null> {
	if (builtinsCache) return builtinsCache;
	builtinsCache = await audioEngine.getBuiltins();
	return builtinsCache;
}

/**
 * User function info extracted from code
 */
interface UserFunctionInfo {
	name: string;
	params: string[];
	docstring?: string;
}

/**
 * Extract user functions from document text
 */
function extractUserFunctions(text: string): Map<string, UserFunctionInfo> {
	const functions = new Map<string, UserFunctionInfo>();
	const pattern = /(?:\/\/\/\s*(.+?)\n)?fn\s+([a-zA-Z_][a-zA-Z0-9_]*)\s*\(([^)]*)\)/g;
	let match;
	while ((match = pattern.exec(text)) !== null) {
		const docstring = match[1]?.trim();
		const name = match[2];
		const paramsStr = match[3];
		const params = paramsStr
			.split(',')
			.map((p) => p.trim())
			.filter((p) => p.length > 0);
		functions.set(name, { name, params, docstring });
	}
	return functions;
}

/**
 * Find function call context at cursor position
 * Returns the function name and current parameter index
 *
 * Handles two cases:
 * 1. Cursor on function name: trigge|r(1, 2) → look forward for "("
 * 2. Cursor inside parens: trigger(1|, 2) → look backward for "(" (original logic)
 */
function findFunctionContext(
	view: EditorView
): { funcName: string; paramIndex: number; openParenPos: number } | null {
	const pos = view.state.selection.main.head;
	const text = view.state.doc.toString();

	// First: check if cursor is inside an identifier followed by "("
	// This handles: trigge|r(1) or l|p(x, y)
	let idStart = pos,
		idEnd = pos;
	while (idStart > 0 && /[a-zA-Z0-9_]/.test(text[idStart - 1])) idStart--;
	while (idEnd < text.length && /[a-zA-Z0-9_]/.test(text[idEnd])) idEnd++;

	// Check if there's a "(" after the identifier (with optional whitespace)
	let lookAhead = idEnd;
	while (lookAhead < text.length && /\s/.test(text[lookAhead])) lookAhead++;

	if (text[lookAhead] === '(' && idEnd > idStart) {
		// Cursor is on function name of a call
		const funcName = text.slice(idStart, idEnd);
		return { funcName, paramIndex: 0, openParenPos: lookAhead };
	}

	// Second: existing logic - search backwards for "("
	// This handles: trigger(1|, 2) and trigger(1, 2|)
	let depth = 0;
	let openParenPos = -1;
	let paramIndex = 0;

	for (let i = pos - 1; i >= 0; i--) {
		const char = text[i];

		if (char === ')') {
			depth++;
		} else if (char === '(') {
			if (depth === 0) {
				openParenPos = i;
				break;
			}
			depth--;
		} else if (char === ',' && depth === 0) {
			paramIndex++;
		} else if (char === '\n') {
			// Don't search across multiple lines for the opening paren
			// But allow commas across lines
			if (openParenPos === -1) {
				// Continue searching
			}
		}
	}

	if (openParenPos === -1) return null;

	// Find function name before the opening paren
	let nameEnd = openParenPos;
	// Skip whitespace
	while (nameEnd > 0 && /\s/.test(text[nameEnd - 1])) {
		nameEnd--;
	}
	// Find start of identifier
	let nameStart = nameEnd;
	while (nameStart > 0 && /[a-zA-Z0-9_]/.test(text[nameStart - 1])) {
		nameStart--;
	}

	if (nameStart === nameEnd) return null;

	const funcName = text.slice(nameStart, nameEnd);

	return { funcName, paramIndex, openParenPos };
}

/**
 * Format parameter with highlighting
 */
function formatParam(param: BuiltinParam, isActive: boolean): HTMLElement {
	const span = document.createElement('span');
	span.textContent = param.required
		? param.name
		: param.default !== undefined
			? `${param.name}=${param.default}`
			: `${param.name}?`;

	if (isActive) {
		span.style.fontWeight = 'bold';
		span.style.textDecoration = 'underline';
		span.style.color = 'var(--accent-primary)';
	}

	return span;
}

/**
 * Format user function parameter with highlighting
 */
function formatUserParam(param: string, isActive: boolean): HTMLElement {
	const span = document.createElement('span');
	span.textContent = param;

	if (isActive) {
		span.style.fontWeight = 'bold';
		span.style.textDecoration = 'underline';
		span.style.color = 'var(--accent-primary)';
	}

	return span;
}

/**
 * Create tooltip content for builtin function
 */
function createBuiltinTooltipContent(
	funcName: string,
	info: BuiltinInfo,
	paramIndex: number
): HTMLElement {
	const container = document.createElement('div');
	container.className = 'cm-signature-help';
	container.style.cssText = `
		padding: 6px 10px;
		font-family: var(--font-mono);
		font-size: 13px;
		background: var(--bg-secondary);
		border: 1px solid var(--border-muted);
		border-radius: 4px;
		box-shadow: 0 2px 8px rgba(0,0,0,0.3);
		max-width: 400px;
	`;

	// Signature line
	const sigLine = document.createElement('div');
	sigLine.style.marginBottom = '4px';

	const funcNameSpan = document.createElement('span');
	funcNameSpan.textContent = funcName;
	funcNameSpan.style.color = 'var(--accent-secondary)';
	sigLine.appendChild(funcNameSpan);

	sigLine.appendChild(document.createTextNode('('));

	info.params.forEach((param, i) => {
		if (i > 0) {
			sigLine.appendChild(document.createTextNode(', '));
		}
		sigLine.appendChild(formatParam(param, i === paramIndex));
	});

	sigLine.appendChild(document.createTextNode(')'));
	container.appendChild(sigLine);

	// Description
	if (info.description) {
		const descLine = document.createElement('div');
		descLine.style.cssText = `
			color: var(--text-muted);
			font-size: 12px;
			margin-top: 4px;
		`;
		descLine.textContent = info.description;
		container.appendChild(descLine);
	}

	return container;
}

/**
 * Create tooltip content for user function
 */
function createUserFunctionTooltipContent(
	funcInfo: UserFunctionInfo,
	paramIndex: number
): HTMLElement {
	const container = document.createElement('div');
	container.className = 'cm-signature-help';
	container.style.cssText = `
		padding: 6px 10px;
		font-family: var(--font-mono);
		font-size: 13px;
		background: var(--bg-secondary);
		border: 1px solid var(--border-muted);
		border-radius: 4px;
		box-shadow: 0 2px 8px rgba(0,0,0,0.3);
		max-width: 400px;
	`;

	// Signature line
	const sigLine = document.createElement('div');
	sigLine.style.marginBottom = '4px';

	const funcNameSpan = document.createElement('span');
	funcNameSpan.textContent = funcInfo.name;
	funcNameSpan.style.color = 'var(--accent-secondary)';
	sigLine.appendChild(funcNameSpan);

	sigLine.appendChild(document.createTextNode('('));

	funcInfo.params.forEach((param, i) => {
		if (i > 0) {
			sigLine.appendChild(document.createTextNode(', '));
		}
		sigLine.appendChild(formatUserParam(param, i === paramIndex));
	});

	sigLine.appendChild(document.createTextNode(')'));
	container.appendChild(sigLine);

	// Docstring
	if (funcInfo.docstring) {
		const descLine = document.createElement('div');
		descLine.style.cssText = `
			color: var(--text-muted);
			font-size: 12px;
			margin-top: 4px;
		`;
		descLine.textContent = funcInfo.docstring;
		container.appendChild(descLine);
	}

	return container;
}

/**
 * Signature help state effect
 */
const setSignatureHelp = StateEffect.define<Tooltip | null>();

/**
 * State field for signature help tooltip
 */
const signatureHelpField = StateField.define<Tooltip | null>({
	create() {
		return null;
	},
	update(value, tr) {
		for (const effect of tr.effects) {
			if (effect.is(setSignatureHelp)) {
				return effect.value;
			}
		}
		return value;
	},
	provide: (f) => showTooltip.from(f)
});

/**
 * View plugin that updates signature help on cursor movement
 */
const signatureHelpPlugin = ViewPlugin.fromClass(
	class {
		// Track last context to detect changes (including paramIndex)
		private lastContextKey: string | null = null;

		constructor(private view: EditorView) {
			this.updateSignatureHelp();
		}

		update(update: ViewUpdate) {
			if (update.selectionSet || update.docChanged) {
				this.updateSignatureHelp();
			}
		}

		async updateSignatureHelp() {
			const pos = this.view.state.selection.main.head;
			const context = findFunctionContext(this.view);

			if (!context) {
				// No function context, hide tooltip
				if (this.lastContextKey !== null) {
					this.lastContextKey = null;
					queueMicrotask(() => {
						this.view.dispatch({
							effects: setSignatureHelp.of(null)
						});
					});
				}
				return;
			}

			// Create a key that includes paramIndex to detect parameter changes
			const contextKey = `${context.funcName}:${context.openParenPos}:${context.paramIndex}`;

			// Check if it's a user function first
			const docText = this.view.state.doc.toString();
			const userFunctions = extractUserFunctions(docText);
			const userFunc = userFunctions.get(context.funcName);

			if (userFunc) {
				// Only dispatch if context changed (including paramIndex)
				if (this.lastContextKey !== contextKey) {
					this.lastContextKey = contextKey;
					const tooltip: Tooltip = {
						pos: context.openParenPos,
						above: true,
						strictSide: false,
						create: () => {
							const dom = createUserFunctionTooltipContent(userFunc, context.paramIndex);
							return { dom };
						}
					};

					queueMicrotask(() => {
						this.view.dispatch({
							effects: setSignatureHelp.of(tooltip)
						});
					});
				}
				return;
			}

			// Check builtins (async)
			const builtins = await getBuiltins();

			// Abort if cursor moved during async operation
			if (this.view.state.selection.main.head !== pos) return;

			if (!builtins) {
				if (this.lastContextKey !== null) {
					this.lastContextKey = null;
					queueMicrotask(() => {
						this.view.dispatch({
							effects: setSignatureHelp.of(null)
						});
					});
				}
				return;
			}

			// Resolve alias to canonical name
			let funcName = context.funcName;
			if (builtins.aliases[funcName]) {
				funcName = builtins.aliases[funcName];
			}

			const info = builtins.functions[funcName];
			if (!info) {
				// Unknown function
				if (this.lastContextKey !== null) {
					this.lastContextKey = null;
					queueMicrotask(() => {
						this.view.dispatch({
							effects: setSignatureHelp.of(null)
						});
					});
				}
				return;
			}

			// Only dispatch if context changed (including paramIndex)
			if (this.lastContextKey !== contextKey) {
				this.lastContextKey = contextKey;
				const tooltip: Tooltip = {
					pos: context.openParenPos,
					above: true,
					strictSide: false,
					create: () => {
						const dom = createBuiltinTooltipContent(context.funcName, info, context.paramIndex);
						return { dom };
					}
				};

				queueMicrotask(() => {
					this.view.dispatch({
						effects: setSignatureHelp.of(tooltip)
					});
				});
			}
		}
	}
);

/**
 * CodeMirror extension for signature help
 */
export function signatureHelp() {
	return [signatureHelpField, signatureHelpPlugin];
}
