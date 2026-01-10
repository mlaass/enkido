<script lang="ts">
	import { editorStore } from '$lib/stores/editor.svelte';
	import { audioEngine } from '$lib/stores/audio.svelte';
	import { docsStore } from '$lib/stores/docs.svelte';

	interface Props {
		code: string;
	}

	let { code }: Props = $props();

	let isPlaying = $state(false);

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
</script>

<div class="code-widget">
	<pre><code>{code}</code></pre>
	<div class="controls">
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
		padding-right: 36px;
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
</style>
