<script lang="ts">
	import { editorStore } from '$lib/stores/editor.svelte';
	import { audioEngine } from '$lib/stores/audio.svelte';
	import { docsStore } from '$lib/stores/docs.svelte';

	interface Props {
		code: string;
	}

	let { code }: Props = $props();

	let isPlaying = $state(false);
	let copied = $state(false);

	async function play() {
		// Save current editor code
		if (!docsStore.hasSavedCode()) {
			docsStore.saveEditorCode(editorStore.code);
		}

		// Load this example into editor and run
		editorStore.setCode(code);
		await editorStore.evaluate();
		isPlaying = true;
	}

	function stop() {
		audioEngine.stop();
		isPlaying = false;
	}

	async function copyCode() {
		await navigator.clipboard.writeText(code);
		copied = true;
		setTimeout(() => (copied = false), 1500);
	}
</script>

<div class="code-widget">
	<pre><code>{code}</code></pre>
	<div class="controls">
		<button class="btn copy" onclick={copyCode} title={copied ? 'Copied!' : 'Copy'}>
			{#if copied}
				<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5">
					<polyline points="20 6 9 17 4 12" />
				</svg>
			{:else}
				<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
					<rect x="9" y="9" width="13" height="13" rx="2" />
					<path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1" />
				</svg>
			{/if}
		</button>
		{#if isPlaying}
			<button class="btn stop" onclick={stop} title="Stop">
				<svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor">
					<rect x="4" y="4" width="16" height="16" rx="2" />
				</svg>
			</button>
		{:else}
			<button class="btn play" onclick={play} title="Play">
				<svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor">
					<polygon points="5 3 19 12 5 21 5 3" />
				</svg>
			</button>
		{/if}
	</div>
</div>

<style>
	.code-widget {
		position: relative;
		margin: var(--spacing-sm) 0;
	}

	pre {
		margin: 0;
		padding: 8px 10px;
		padding-right: 58px;
		font-family: var(--font-mono);
		font-size: 12px;
		line-height: 1.4;
		background: var(--bg-tertiary);
		border-radius: 4px;
		overflow-x: auto;
	}

	code {
		color: var(--text-primary);
	}

	.controls {
		position: absolute;
		top: 6px;
		right: 6px;
		display: flex;
		gap: 4px;
	}

	.btn {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 22px;
		height: 22px;
		border-radius: 4px;
		transition: all var(--transition-fast);
	}

	.btn.play {
		color: var(--accent-secondary);
		background: rgba(63, 185, 80, 0.15);
	}

	.btn.play:hover {
		background: rgba(63, 185, 80, 0.3);
	}

	.btn.stop {
		color: var(--accent-error);
		background: rgba(248, 81, 73, 0.15);
	}

	.btn.stop:hover {
		background: rgba(248, 81, 73, 0.3);
	}

	.btn.copy {
		color: var(--text-muted);
		background: rgba(128, 128, 128, 0.1);
	}

	.btn.copy:hover {
		color: var(--text-secondary);
		background: rgba(128, 128, 128, 0.2);
	}
</style>
