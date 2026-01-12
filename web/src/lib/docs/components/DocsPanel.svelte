<script lang="ts">
	import { goto } from '$app/navigation';
	import { docsStore } from '$lib/stores/docs.svelte';
	import { previews } from '../manifest';
	import DocSearch from './DocSearch.svelte';
	import DocNavigation from './DocNavigation.svelte';
	import DocMarkdown from './DocMarkdown.svelte';
	import type { DocCategory } from '../types';
	import { Maximize2 } from 'lucide-svelte';

	const categories: Array<{ id: DocCategory; label: string }> = [
		{ id: 'builtins', label: 'Builtins' },
		{ id: 'language', label: 'Language' },
		{ id: 'mini-notation', label: 'Mini-notation' },
		{ id: 'concepts', label: 'Concepts' },
		{ id: 'tutorials', label: 'Tutorials' }
	];

	function expandToDocs() {
		if (docsStore.activeSlug) {
			goto(`/docs/${docsStore.activeCategory}/${docsStore.activeSlug}`);
		} else {
			goto(`/docs/${docsStore.activeCategory}`);
		}
	}
</script>

<div class="docs-panel">
	<div class="docs-header">
		<DocSearch />
		<button class="expand-btn" onclick={expandToDocs} title="Open full documentation">
			<Maximize2 size={14} />
		</button>
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
							<div class="result-content">
								<span class="result-title">{result.title}</span>
								{#if previews[result.slug]}
									<span class="result-preview">{previews[result.slug]}</span>
								{/if}
							</div>
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
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
		padding: var(--spacing-sm);
		border-bottom: 1px solid var(--border-muted);
	}

	.expand-btn {
		flex-shrink: 0;
		padding: 6px;
		color: var(--text-muted);
		background: transparent;
		border-radius: 4px;
		transition: all var(--transition-fast);
	}

	.expand-btn:hover {
		color: var(--text-primary);
		background: var(--bg-tertiary);
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
		align-items: flex-start;
		gap: var(--spacing-sm);
		padding: 8px;
		text-align: left;
		background: var(--bg-tertiary);
		border-radius: 4px;
		transition: background var(--transition-fast);
	}

	.result-item:hover {
		background: var(--bg-hover);
	}

	.result-content {
		display: flex;
		flex-direction: column;
		gap: 2px;
		flex: 1;
		min-width: 0;
	}

	.result-title {
		color: var(--text-primary);
		font-size: 13px;
		font-weight: 500;
	}

	.result-preview {
		color: var(--text-muted);
		font-size: 11px;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.result-category {
		color: var(--text-muted);
		font-size: 11px;
		flex-shrink: 0;
	}
</style>
