<script lang="ts">
	/**
	 * Captures + renders browser (prd-studio-daw-core §4.6).
	 *
	 * Lists the finalized takes in the open `.nkido` bundle. Takes are directories
	 * named by ISO start time, each holding `master.wav` + one WAV per bus (named
	 * from the code's bus label when it has one) plus `capture.json` /
	 * `render.json` metadata. Finalized takes exist on disk regardless of save.
	 */
	import { studio } from './studio-bridge.svelte';

	let tab = $state<'captures' | 'renders'>('captures');
	const items = $derived(tab === 'captures' ? studio.captures : studio.renders);
</script>

<section class="browser" aria-label="Takes">
	<header>
		<div class="tabs" role="tablist">
			<button role="tab" aria-selected={tab === 'captures'} onclick={() => (tab = 'captures')}>
				Captures ({studio.captures.length})
			</button>
			<button role="tab" aria-selected={tab === 'renders'} onclick={() => (tab = 'renders')}>
				Renders ({studio.renders.length})
			</button>
		</div>
		<button class="refresh" onclick={() => studio.refreshTakes()} aria-label="Refresh">⟳</button>
	</header>

	<ul>
		{#each items as take (take)}
			<li>{take}</li>
		{:else}
			<li class="empty">
				{tab === 'captures' ? 'Press Keep or Record to make a take.' : 'File → Render to bounce.'}
			</li>
		{/each}
	</ul>
</section>

<style>
	.browser {
		border: 1px solid var(--border-default);
		border-radius: 6px;
		background: var(--bg-secondary);
		padding: 0.5rem 0.75rem 0.6rem;
		font-size: 0.78rem;
	}
	header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		margin-bottom: 0.4rem;
	}
	.tabs {
		display: flex;
		gap: 0.3rem;
	}
	button {
		padding: 0.2rem 0.5rem;
		border-radius: 3px;
		border: 1px solid transparent;
		background: transparent;
		color: var(--text-secondary);
		cursor: pointer;
		font-size: 0.75rem;
	}
	button[aria-selected='true'] {
		background: var(--bg-tertiary);
		border-color: var(--border-default);
		color: var(--text-primary);
	}
	.refresh {
		color: var(--text-muted);
	}
	ul {
		list-style: none;
		margin: 0;
		padding: 0;
		max-height: 140px;
		overflow-y: auto;
	}
	li {
		padding: 0.2rem 0.1rem;
		font-variant-numeric: tabular-nums;
		border-bottom: 1px solid var(--border-muted);
	}
	li.empty {
		color: var(--text-muted);
		border: none;
	}
</style>
