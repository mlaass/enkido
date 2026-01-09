<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { EditorView, keymap, lineNumbers, highlightActiveLineGutter, highlightSpecialChars, drawSelection, dropCursor, rectangularSelection, crosshairCursor, highlightActiveLine } from '@codemirror/view';
	import { EditorState } from '@codemirror/state';
	import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands';
	import { syntaxHighlighting, defaultHighlightStyle, bracketMatching, foldGutter, foldKeymap } from '@codemirror/language';
	import { autocompletion, completionKeymap, closeBrackets, closeBracketsKeymap } from '@codemirror/autocomplete';
	import { editorStore } from '$stores/editor.svelte';
	import { audioEngine } from '$stores/audio.svelte';
	import { compile } from '$lib/compiler/akkado';

	let editorContainer: HTMLDivElement;
	let view: EditorView | null = null;

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

	// Custom keybinding for evaluate (Ctrl+Enter)
	const evaluateKeymap = keymap.of([
		{
			key: 'Ctrl-Enter',
			mac: 'Cmd-Enter',
			run: () => {
				evaluate();
				return true;
			}
		}
	]);

	async function evaluate() {
		console.log('[Editor] evaluate() called');
		if (!view) {
			console.log('[Editor] No view, returning');
			return;
		}

		const code = view.state.doc.toString();
		console.log('[Editor] Code to compile:', code.substring(0, 100) + '...');
		editorStore.setCode(code);

		try {
			// Compile with akkado.wasm
			console.log('[Editor] Calling compile()...');
			const result = await compile(code);
			console.log('[Editor] Compile result:', result);

			if (result.success && result.bytecode) {
				// Ensure audio engine is initialized before loading program
				if (!audioEngine.isInitialized) {
					console.log('[Editor] Initializing audio engine first...');
					await audioEngine.play();
					// Give the worklet time to initialize
					await new Promise(resolve => setTimeout(resolve, 500));
				}
				// Send bytecode to Cedar VM
				audioEngine.loadProgram(result.bytecode);
				editorStore.markCompiled();
				editorStore.setCompileError(null);
				console.log('[Editor] Compiled and loaded bytecode:', result.bytecode.length, 'bytes');
			} else {
				// Show first error
				const firstError = result.diagnostics.find(d => d.severity === 2);
				const errorMsg = firstError
					? `${firstError.message} (line ${firstError.line})`
					: 'Compilation failed';
				editorStore.setCompileError(errorMsg);
				console.error('[Editor] Compile error:', errorMsg);
			}
		} catch (err) {
			const errorMsg = err instanceof Error ? err.message : String(err);
			editorStore.setCompileError(errorMsg);
			console.error('[Editor] Compile exception:', err);
			console.error('[Editor] Stack:', err instanceof Error ? err.stack : 'N/A');
		}
		console.log('[Editor] evaluate() finished');

		// Start playback if not already playing
		if (!audioEngine.isPlaying) {
			audioEngine.play();
		}
	}

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
				EditorView.updateListener.of((update) => {
					if (update.docChanged) {
						editorStore.setCode(update.state.doc.toString());
					}
				})
			]
		});

		view = new EditorView({
			state,
			parent: editorContainer
		});
	});

	onDestroy(() => {
		view?.destroy();
	});
</script>

<div class="editor-wrapper">
	<div class="editor" bind:this={editorContainer}></div>
	<div class="status-bar">
		<div class="status-left">
			{#if editorStore.hasUnsavedChanges}
				<span class="status-indicator modified" title="Unsaved changes">Modified</span>
			{:else if editorStore.lastCompileTime}
				<span class="status-indicator compiled" title="Code evaluated">Ready</span>
			{/if}
		</div>
		<div class="status-center">
			<span class="hint">Ctrl+Enter to evaluate</span>
		</div>
		<div class="status-right">
			{#if editorStore.lastCompileError}
				<span class="status-error">{editorStore.lastCompileError}</span>
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
</style>
