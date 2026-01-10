<script lang="ts">
	import { docsStore } from '$lib/stores/docs.svelte';
	import DocSearch from './DocSearch.svelte';
	import DocNavigation from './DocNavigation.svelte';
	import DocMarkdown from './DocMarkdown.svelte';
	import type { DocCategory } from '../types';

	const categories: Array<{ id: DocCategory; label: string }> = [
		{ id: 'builtins', label: 'Builtins' },
		{ id: 'language', label: 'Language' },
		{ id: 'mini-notation', label: 'Mini-notation' },
		{ id: 'concepts', label: 'Concepts' },
		{ id: 'tutorials', label: 'Tutorials' }
	];
</script>

<div class="docs-panel">
	<div class="docs-header">
		<DocSearch />
	</div>

	<div class="docs-categories">
		{#each categories as cat}
			<button
				class="category-btn"
				class:active={docsStore.activeCategory === cat.id}
				onclick={() => docsStore.setCategory(cat.id)}
			>
				{cat.label}
			</button>
		{/each}
	</div>

	<div class="docs-content">
		{#if docsStore.searchQuery.length >= 2}
			<!-- Search results -->
			<div class="search-results">
				{#if docsStore.searchResults.length === 0}
					<p class="no-results">No results for "{docsStore.searchQuery}"</p>
				{:else}
					{#each docsStore.searchResults as result}
						<button
							class="result-item"
							onclick={() => docsStore.setDocument(result.slug, result.anchor)}
						>
							<span class="result-title">{result.title}</span>
							<span class="result-category">{result.category}</span>
						</button>
					{/each}
				{/if}
			</div>
		{:else if docsStore.activeSlug}
			<!-- Document content -->
			<DocMarkdown slug={docsStore.activeSlug} anchor={docsStore.activeAnchor} />
		{:else}
			<!-- Navigation for current category -->
			<DocNavigation category={docsStore.activeCategory} />
		{/if}
	</div>
</div>

<style>
	.docs-panel {
		display: flex;
		flex-direction: column;
		height: 100%;
		overflow: hidden;
	}

	.docs-header {
		padding: var(--spacing-sm);
		border-bottom: 1px solid var(--border-muted);
	}

	.docs-categories {
		display: flex;
		flex-wrap: wrap;
		gap: 4px;
		padding: var(--spacing-sm);
		border-bottom: 1px solid var(--border-muted);
	}

	.category-btn {
		padding: 4px 8px;
		font-size: 11px;
		font-weight: 500;
		color: var(--text-secondary);
		background: var(--bg-tertiary);
		border-radius: 4px;
		transition: all var(--transition-fast);
	}

	.category-btn:hover {
		color: var(--text-primary);
		background: var(--bg-hover);
	}

	.category-btn.active {
		color: var(--accent-primary);
		background: rgba(88, 166, 255, 0.15);
	}

	.docs-content {
		flex: 1;
		overflow-y: auto;
		padding: var(--spacing-sm);
	}

	.search-results {
		display: flex;
		flex-direction: column;
		gap: 4px;
	}

	.no-results {
		color: var(--text-muted);
		font-size: 13px;
		text-align: center;
		padding: var(--spacing-md);
	}

	.result-item {
		display: flex;
		justify-content: space-between;
		align-items: center;
		padding: 8px;
		text-align: left;
		background: var(--bg-tertiary);
		border-radius: 4px;
		transition: background var(--transition-fast);
	}

	.result-item:hover {
		background: var(--bg-hover);
	}

	.result-title {
		color: var(--text-primary);
		font-size: 13px;
		font-weight: 500;
	}

	.result-category {
		color: var(--text-muted);
		font-size: 11px;
	}
</style>
