<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { EditorView, keymap, lineNumbers, highlightActiveLineGutter, highlightSpecialChars, drawSelection, dropCursor, rectangularSelection, crosshairCursor, highlightActiveLine } from '@codemirror/view';
	import { EditorState } from '@codemirror/state';
	import { defaultKeymap, history, historyKeymap, indentWithTab, toggleComment, moveLineUp, moveLineDown, copyLineDown } from '@codemirror/commands';
	import { bracketMatching, foldGutter, foldKeymap } from '@codemirror/language';
	import { akkadoLanguage } from '$lib/editor/akkado-language';
	import { autocompletion, completionKeymap, closeBrackets, closeBracketsKeymap } from '@codemirror/autocomplete';
	import { editorStore } from '$stores/editor.svelte';
	import { audioEngine } from '$stores/audio.svelte';
	import { draftsStore } from '$stores/drafts.svelte';
	import { triggerF1Help, getWordAtCursor } from '$lib/docs/lookup';
	import { linterExtensions, updateEditorDiagnostics } from './editor-linter';
	import { akkadoCompletions } from '$lib/editor/akkado-completions';
	import {
		scheduleShapeIndex,
		setShapeIndexFetcher,
		utf8ByteOffset
	} from '$lib/editor/akkado-shape-index';
	import { signatureHelp } from '$lib/editor/signature-help';

	// Phase 2 records-system-unification: connect the shape-index module to
	// the audio engine. The fetcher is wired once per Editor mount; the
	// debounced scheduler in `EditorView.updateListener` calls it on idle.
	setShapeIndexFetcher((source, cursor) => audioEngine.getShapeIndex(source, cursor));
	import { stepHighlight } from '$lib/editor/step-highlight';
	import { instructionHighlight, highlightInstruction } from '$lib/editor/instruction-highlight';
	import { visualizationWidgets } from '$lib/editor/visualization-widgets';
	import { patternHighlightStore } from '$stores/pattern-highlight.svelte';
	import type { SourceLocation } from '$stores/audio.svelte';

	let editorContainer: HTMLDivElement;
	let view: EditorView | null = null;
	let pulseKey = $state(0);
	// Set while applying an externally-driven doc change (e.g. tab switch).
	// Suppresses the update listener's write-back into the editor store so we
	// don't bounce the new code back into the drafts store.
	let applyingExternal = false;

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
			backgroundColor: 'transparent',
			borderBottom: '1px solid var(--accent-primary)'
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
		},
		{
			key: 'Ctrl-t',
			mac: 'Cmd-t',
			preventDefault: true,
			run: () => {
				draftsStore.createDraft({});
				return true;
			}
		},
		{
			key: 'Ctrl-Shift-w',
			mac: 'Cmd-Shift-w',
			preventDefault: true,
			run: () => {
				const id = draftsStore.activeTabId;
				if (id) draftsStore.closeTab(id);
				return true;
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
				...akkadoLanguage(),
				bracketMatching(),
				closeBrackets(),
				autocompletion({
					override: [akkadoCompletions],
					activateOnTyping: true
				}),
				signatureHelp(),
				rectangularSelection(),
				crosshairCursor(),
				highlightActiveLine(),
				EditorState.languageData.of(() => [{ commentTokens: { line: '//' } }]),
				evaluateKeymap,
				keymap.of([
					{ key: 'Ctrl-/', mac: 'Cmd-/', run: toggleComment },
					{ key: 'Ctrl-Alt-ArrowUp', run: moveLineUp },
					{ key: 'Ctrl-Alt-ArrowDown', run: moveLineDown },
					{ key: 'Ctrl-d', mac: 'Cmd-d', run: copyLineDown },
				]),
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
					if (update.docChanged && !applyingExternal) {
						editorStore.setCode(update.state.doc.toString());
					}
					// Phase 2 records-system-unification: refresh the shape
					// index used by `r.` / `%.` completions on every doc /
					// selection change. The scheduler debounces internally.
					if (update.docChanged || update.selectionSet) {
						const text = update.state.doc.toString();
						const charPos = update.state.selection.main.head;
						scheduleShapeIndex(text, utf8ByteOffset(text, charPos));
					}
				}),
				// Linter for inline error display
				...linterExtensions,
				// Step highlighting for active pattern notes
				...stepHighlight(),
				// Visualization block widgets (pianoroll, oscilloscope, etc.)
				visualizationWidgets(),
				// Instruction highlighting for debug panel
				...instructionHighlight()
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

	// Update pattern previews after successful compilation
	$effect(() => {
		const compileTime = editorStore.lastCompileTime;
		const hasErrors = editorStore.diagnostics.some(d => d.severity === 2);
		if (compileTime && !hasErrors) {
			// Small delay to ensure WASM state is ready
			setTimeout(() => {
				patternHighlightStore.updatePreviews();
			}, 50);
		}
	});

	// Rebind CodeMirror doc when the active draft's code changes externally
	// (tab switch, Keep promotion, inline-URL phantom open). Compares against
	// the current view doc to skip noop updates and avoid feedback loops.
	$effect(() => {
		const externalCode = editorStore.code;
		if (!view) return;
		const current = view.state.doc.toString();
		if (current === externalCode) return;
		applyingExternal = true;
		try {
			view.dispatch({
				changes: { from: 0, to: current.length, insert: externalCode }
			});
		} finally {
			applyingExternal = false;
		}
	});

	// Bump pulseKey on every successful evaluate so the status indicator
	// flashes — confirms Ctrl+Enter ran even when the audio output is
	// identical (e.g. unchanged code or stylistic edits).
	$effect(() => {
		const compileTime = editorStore.lastCompileTime;
		if (compileTime) {
			pulseKey = compileTime;
		}
	});

	// Listen for instruction highlight events from DebugPanel
	function handleInstructionHighlight(event: CustomEvent<{ source: SourceLocation | null }>) {
		if (view) {
			highlightInstruction(view, event.detail.source);
		}
	}

	$effect(() => {
		// Set up event listener on mount
		window.addEventListener('nkido:instruction-highlight', handleInstructionHighlight as EventListener);

		// Clean up on unmount
		return () => {
			window.removeEventListener('nkido:instruction-highlight', handleInstructionHighlight as EventListener);
		};
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
				{#key pulseKey}
					<span class="status-indicator compiled pulse" title="Code evaluated">Ready</span>
				{/key}
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

	.status-indicator.pulse {
		animation: refresh-pulse 600ms ease-out;
	}

	@keyframes refresh-pulse {
		0% {
			background-color: rgba(63, 185, 80, 0.7);
			box-shadow: 0 0 0 0 rgba(63, 185, 80, 0.5);
		}
		100% {
			background-color: rgba(63, 185, 80, 0.2);
			box-shadow: 0 0 0 6px rgba(63, 185, 80, 0);
		}
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
