<script lang="ts">
	import { docsStore } from '$lib/stores/docs.svelte';

	let inputValue = $state('');

	function handleInput(e: Event) {
		const target = e.target as HTMLInputElement;
		inputValue = target.value;
		docsStore.setSearchQuery(target.value);
	}

	function handleClear() {
		inputValue = '';
		docsStore.setSearchQuery('');
	}

	function handleKeydown(e: KeyboardEvent) {
		if (e.key === 'Escape') {
			handleClear();
		}
	}
</script>

<div class="search-container">
	<svg class="search-icon" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
		<circle cx="11" cy="11" r="8" />
		<path d="m21 21-4.35-4.35" />
	</svg>
	<input
		type="text"
		class="search-input"
		placeholder="Search docs..."
		value={inputValue}
		oninput={handleInput}
		onkeydown={handleKeydown}
	/>
	{#if inputValue}
		<button class="clear-btn" onclick={handleClear} title="Clear search">
			<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
				<path d="M18 6 6 18M6 6l12 12" />
			</svg>
		</button>
	{/if}
</div>

<style>
	.search-container {
		position: relative;
		display: flex;
		align-items: center;
	}

	.search-icon {
		position: absolute;
		left: 8px;
		color: var(--text-muted);
		pointer-events: none;
	}

	.search-input {
		width: 100%;
		padding: 6px 28px 6px 28px;
		font-size: 12px;
		font-family: var(--font-sans);
		color: var(--text-primary);
		background: var(--bg-tertiary);
		border: 1px solid var(--border-muted);
		border-radius: 4px;
		outline: none;
		transition: all var(--transition-fast);
	}

	.search-input::placeholder {
		color: var(--text-muted);
	}

	.search-input:focus {
		border-color: var(--accent-primary);
		background: var(--bg-primary);
	}

	.clear-btn {
		position: absolute;
		right: 6px;
		display: flex;
		align-items: center;
		justify-content: center;
		width: 18px;
		height: 18px;
		color: var(--text-muted);
		background: transparent;
		border-radius: 2px;
		transition: all var(--transition-fast);
	}

	.clear-btn:hover {
		color: var(--text-primary);
		background: var(--bg-hover);
	}
</style>
