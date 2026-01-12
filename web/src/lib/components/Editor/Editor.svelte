<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { EditorView, keymap, lineNumbers, highlightActiveLineGutter, highlightSpecialChars, drawSelection, dropCursor, rectangularSelection, crosshairCursor, highlightActiveLine } from '@codemirror/view';
	import { EditorState } from '@codemirror/state';
	import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands';
	import { syntaxHighlighting, defaultHighlightStyle, bracketMatching, foldGutter, foldKeymap } from '@codemirror/language';
	import { autocompletion, completionKeymap, closeBrackets, closeBracketsKeymap } from '@codemirror/autocomplete';
	import { editorStore } from '$stores/editor.svelte';
	import { audioEngine } from '$stores/audio.svelte';
	import { triggerF1Help, getWordAtCursor } from '$lib/docs/lookup';
	import { linterExtensions, updateEditorDiagnostics } from './editor-linter';

	let editorContainer: HTMLDivElement;
	let view: EditorView | null = null;

	// Map superscript Unicode to ASCII ^N (fixes macOS text substitution)
	const superscriptMap: Record<string, string> = {
		'⁰': '^0', '¹': '^1', '²': '^2', '³': '^3', '⁴': '^4',
		'⁵': '^5', '⁶': '^6', '⁷': '^7', '⁸': '^8', '⁹': '^9'
	};
	const superscriptRegex = /[⁰¹²³⁴⁵⁶⁷⁸⁹]/g;

	// Transaction filter to normalize superscripts back to ^N notation
	const normalizeSuperscripts = EditorState.transactionFilter.of((tr) => {
		if (!tr.docChanged) return tr;

		const newDoc = tr.newDoc.toString();
		const matches = Array.from(newDoc.matchAll(superscriptRegex));
		if (!matches.length) return tr;

		// Return original transaction plus replacement changes
		return [
			tr,
			...matches.map(m => ({
				changes: {
					from: m.index!,
					to: m.index! + 1,
					insert: superscriptMap[m[0]]
				},
				sequential: true
			}))
		];
	});

	// Dark theme for the editor
	const darkTheme = EditorView.theme({
		'&': {
			backgroundColor: 'var(--bg-primary)',
			color: 'var(--text-primary)',
			height: '100%'
		},
		'.cm-content': {
			fontFamily: 'var(--font-mono)',
			fontSize: '14px',
			padding: 'var(--spacing-md) 0'
		},
		'.cm-line': {
			padding: '0 var(--spacing-md)'
		},
		'.cm-gutters': {
			backgroundColor: 'var(--bg-secondary)',
			color: 'var(--text-muted)',
			border: 'none',
			borderRight: '1px solid var(--border-muted)'
		},
		'.cm-activeLineGutter': {
			backgroundColor: 'var(--bg-tertiary)'
		},
		'.cm-activeLine': {
			backgroundColor: 'rgba(255, 255, 255, 0.03)'
		},
		'.cm-cursor': {
			borderLeftColor: 'var(--accent-primary)',
			borderLeftWidth: '2px'
		},
		'.cm-selectionBackground': {
			backgroundColor: 'rgba(88, 166, 255, 0.25)'
		},
		'&.cm-focused .cm-selectionBackground': {
			backgroundColor: 'rgba(88, 166, 255, 0.35)'
		},
		'.cm-matchingBracket': {
			backgroundColor: 'rgba(88, 166, 255, 0.2)',
			outline: '1px solid var(--accent-primary)'
		}
	}, { dark: true });

	// Custom keybindings for evaluate, stop, and help
	const evaluateKeymap = keymap.of([
		{
			key: 'Ctrl-Enter',
			mac: 'Cmd-Enter',
			run: () => {
				// Sync editor content to store before evaluating
				if (view) {
					editorStore.setCode(view.state.doc.toString());
				}
				editorStore.evaluate();
				return true;
			}
		},
		{
			key: 'Escape',
			run: () => {
				audioEngine.stop();
				return true;
			}
		},
		{
			key: 'F1',
			run: (editorView) => {
				const word = getWordAtCursor(editorView);
				if (word) {
					const found = triggerF1Help(word);
					if (found) {
						window.dispatchEvent(new CustomEvent('nkido:f1-help', { detail: { word } }));
					}
				}
				return true; // Prevent browser help
			}
		}
	]);

	onMount(() => {
		const state = EditorState.create({
			doc: editorStore.code,
			extensions: [
				lineNumbers(),
				highlightActiveLineGutter(),
				highlightSpecialChars(),
				history(),
				foldGutter(),
				drawSelection(),
				dropCursor(),
				EditorState.allowMultipleSelections.of(true),
				syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
				bracketMatching(),
				closeBrackets(),
				autocompletion(),
				rectangularSelection(),
				crosshairCursor(),
				highlightActiveLine(),
				evaluateKeymap,
				keymap.of([
					...closeBracketsKeymap,
					...defaultKeymap,
					...historyKeymap,
					...foldKeymap,
					...completionKeymap,
					indentWithTab
				]),
				darkTheme,
				// Disable OS-level text substitutions (e.g., ^2 → ²)
				EditorView.contentAttributes.of({
					spellcheck: 'false',
					autocorrect: 'off',
					autocapitalize: 'off',
					'data-gramm': 'false',
					'data-gramm_editor': 'false'
				}),
				// Normalize superscripts back to ^N if OS still substitutes
				normalizeSuperscripts,
				EditorView.updateListener.of((update) => {
					if (update.docChanged) {
						editorStore.setCode(update.state.doc.toString());
					}
				}),
				// Linter for inline error display
				...linterExtensions
			]
		});

		view = new EditorView({
			state,
			parent: editorContainer
		});

		// Initial diagnostics update if any exist
		if (editorStore.diagnostics.length > 0) {
			updateEditorDiagnostics(view, editorStore.diagnostics);
		}
	});

	// Update diagnostics when they change
	$effect(() => {
		const diagnostics = editorStore.diagnostics;
		if (view) {
			updateEditorDiagnostics(view, diagnostics);
		}
	});

	onDestroy(() => {
		view?.destroy();
	});
</script>

<div class="editor-wrapper">
	<div class="editor" bind:this={editorContainer}></div>
	{#if editorStore.diagnostics.length > 0}
		<div class="error-log">
			<div class="error-log-header">
				<span class="error-log-title">
					Problems ({editorStore.diagnostics.length})
				</span>
				<button class="error-log-clear" onclick={() => editorStore.setDiagnostics([])}>
					Clear
				</button>
			</div>
			<div class="error-log-content">
				{#each editorStore.diagnostics as diagnostic}
					<div class="error-log-item" class:error={diagnostic.severity === 2} class:warning={diagnostic.severity === 1}>
						<span class="error-line">Ln {diagnostic.line}, Col {diagnostic.column}</span>
						<span class="error-message">{diagnostic.message}</span>
					</div>
				{/each}
			</div>
		</div>
	{/if}
	<div class="status-bar">
		<div class="status-left">
			{#if editorStore.hasUnsavedChanges}
				<span class="status-indicator modified" title="Unsaved changes">Modified</span>
			{:else if editorStore.lastCompileTime}
				<span class="status-indicator compiled" title="Code evaluated">Ready</span>
			{/if}
		</div>
		<div class="status-center">
			<span class="hint">Ctrl+Enter to evaluate | F1 for help</span>
		</div>
		<div class="status-right">
			{#if editorStore.diagnostics.length > 0}
				<span class="status-error">{editorStore.diagnostics.length} error{editorStore.diagnostics.length > 1 ? 's' : ''}</span>
			{/if}
		</div>
	</div>
</div>

<style>
	.editor-wrapper {
		display: flex;
		flex-direction: column;
		height: 100%;
		overflow: hidden;
	}

	.editor {
		flex: 1;
		overflow: hidden;
	}

	.editor :global(.cm-editor) {
		height: 100%;
	}

	.editor :global(.cm-scroller) {
		overflow: auto;
	}

	.status-bar {
		display: flex;
		align-items: center;
		justify-content: space-between;
		height: 24px;
		padding: 0 var(--spacing-md);
		background-color: var(--bg-secondary);
		border-top: 1px solid var(--border-muted);
		font-size: 12px;
	}

	.status-left, .status-right {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
	}

	.status-center {
		color: var(--text-muted);
	}

	.hint {
		font-family: var(--font-mono);
		font-size: 11px;
	}

	.status-indicator {
		padding: 1px 6px;
		border-radius: 3px;
		font-size: 11px;
		font-weight: 500;
	}

	.status-indicator.modified {
		background-color: rgba(210, 153, 34, 0.2);
		color: var(--accent-warning);
	}

	.status-indicator.compiled {
		background-color: rgba(63, 185, 80, 0.2);
		color: var(--accent-secondary);
	}

	.status-error {
		color: var(--accent-error);
		font-family: var(--font-mono);
		font-size: 11px;
	}

	/* Error Log Panel */
	.error-log {
		display: flex;
		flex-direction: column;
		max-height: 150px;
		background-color: var(--bg-secondary);
		border-top: 1px solid var(--border-muted);
	}

	.error-log-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		padding: var(--spacing-xs) var(--spacing-md);
		background-color: var(--bg-tertiary);
		border-bottom: 1px solid var(--border-muted);
	}

	.error-log-title {
		font-size: 11px;
		font-weight: 600;
		color: var(--accent-error);
		text-transform: uppercase;
		letter-spacing: 0.5px;
	}

	.error-log-clear {
		font-size: 11px;
		color: var(--text-muted);
		padding: 2px 8px;
		border-radius: 3px;
		transition: background-color var(--transition-fast), color var(--transition-fast);
	}

	.error-log-clear:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.error-log-content {
		overflow-y: auto;
		padding: var(--spacing-xs) 0;
	}

	.error-log-item {
		display: flex;
		align-items: flex-start;
		gap: var(--spacing-md);
		padding: var(--spacing-xs) var(--spacing-md);
		font-family: var(--font-mono);
		font-size: 12px;
		border-left: 3px solid transparent;
	}

	.error-log-item:hover {
		background-color: var(--bg-hover);
	}

	.error-log-item.error {
		border-left-color: var(--accent-error);
	}

	.error-log-item.warning {
		border-left-color: var(--accent-warning);
	}

	.error-line {
		color: var(--text-muted);
		white-space: nowrap;
		min-width: 90px;
	}

	.error-message {
		color: var(--text-primary);
	}

	.error-log-item.error .error-message {
		color: var(--accent-error);
	}

	.error-log-item.warning .error-message {
		color: var(--accent-warning);
	}
</style>
