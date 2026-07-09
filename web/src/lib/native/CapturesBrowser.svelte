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
		border: 1px solid var(--border, #333);
		border-radius: 6px;
		background: var(--bg-panel, #1b1b1b);
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
		color: var(--fg-muted, #999);
		cursor: pointer;
		font-size: 0.75rem;
	}
	button[aria-selected='true'] {
		background: #2c2c2c;
		border-color: var(--border, #444);
		color: #ddd;
	}
	.refresh {
		color: var(--fg-muted, #888);
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
		border-bottom: 1px solid #262626;
	}
	li.empty {
		color: var(--fg-muted, #666);
		border: none;
	}
</style>
