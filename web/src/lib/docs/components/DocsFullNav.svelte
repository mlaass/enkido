<script lang="ts">
	import { navigation, previews } from '../manifest';
	import { docsStore } from '$lib/stores/docs.svelte';
	import { goto } from '$app/navigation';
	import { ChevronDown, ChevronRight, Search } from 'lucide-svelte';

	interface Props {
		activeCategory?: string;
		activeSlug?: string;
	}

	let { activeCategory, activeSlug }: Props = $props();

	const categoryInfo: Record<string, { label: string; order: number }> = {
		tutorials: { label: 'Tutorials', order: 0 },
		builtins: { label: 'Builtins', order: 1 },
		language: { label: 'Language', order: 2 },
		'mini-notation': { label: 'Mini-notation', order: 3 },
		concepts: { label: 'Concepts', order: 4 }
	};

	const sortedCategories = Object.entries(categoryInfo)
		.sort(([, a], [, b]) => a.order - b.order)
		.map(([key]) => key);

	let expandedCategories = $state<Set<string>>(new Set());
	let searchQuery = $state('');

	// Expand the active category when it changes
	$effect(() => {
		if (activeCategory && !expandedCategories.has(activeCategory)) {
			expandedCategories = new Set([...expandedCategories, activeCategory]);
		}
	});

	function toggleCategory(cat: string) {
		const newSet = new Set(expandedCategories);
		if (newSet.has(cat)) {
			newSet.delete(cat);
		} else {
			newSet.add(cat);
		}
		expandedCategories = newSet;
	}

	function handleSearch(e: Event) {
		const value = (e.target as HTMLInputElement).value;
		searchQuery = value;
		docsStore.setSearchQuery(value);
	}

	function handleResultClick(slug: string, category: string, anchor?: string) {
		searchQuery = '';
		docsStore.setSearchQuery('');
		goto(`/docs/${category}/${slug}${anchor ? `#${anchor}` : ''}`);
	}

	function clearSearch() {
		searchQuery = '';
		docsStore.setSearchQuery('');
	}
</script>

<div class="full-nav">
	<div class="search-box">
		<Search size={14} class="search-icon" />
		<input
			type="text"
			placeholder="Search docs..."
			bind:value={searchQuery}
			oninput={handleSearch}
		/>
		{#if searchQuery}
			<button class="clear-btn" onclick={clearSearch}>&times;</button>
		{/if}
	</div>

	{#if searchQuery.length >= 2}
		<div class="search-results">
			{#if docsStore.searchResults.length === 0}
				<p class="no-results">No results</p>
			{:else}
				{#each docsStore.searchResults as result}
					<button
						class="result-item"
						onclick={() => handleResultClick(result.slug, result.category, result.anchor)}
					>
						<span class="result-title">{result.title}</span>
						<span class="result-category">{result.category}</span>
					</button>
				{/each}
			{/if}
		</div>
	{:else}
		<nav class="nav-tree">
			{#each sortedCategories as cat}
				{@const docs = navigation[cat] || []}
				{@const info = categoryInfo[cat]}
				{@const isExpanded = expandedCategories.has(cat)}
				{@const isActive = activeCategory === cat}
				{#if docs.length > 0}
					<div class="nav-category" class:active={isActive}>
						<button class="category-header" onclick={() => toggleCategory(cat)}>
							{#if isExpanded}
								<ChevronDown size={14} />
							{:else}
								<ChevronRight size={14} />
							{/if}
							<span>{info.label}</span>
							<span class="count">{docs.length}</span>
						</button>
						{#if isExpanded}
							<ul class="doc-list">
								{#each docs as doc}
									<li>
										<a
											href="/docs/{cat}/{doc.slug}"
											class="doc-link"
											class:active={activeSlug === doc.slug}
										>
											{doc.title}
										</a>
									</li>
								{/each}
							</ul>
						{/if}
					</div>
				{/if}
			{/each}
		</nav>
	{/if}
</div>

<style>
	.full-nav {
		display: flex;
		flex-direction: column;
		height: 100%;
		padding: var(--spacing-sm);
	}

	.search-box {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		padding: var(--spacing-xs) var(--spacing-sm);
		background: var(--bg-tertiary);
		border: 1px solid var(--border-muted);
		border-radius: 6px;
		margin-bottom: var(--spacing-sm);
	}

	.search-box:focus-within {
		border-color: var(--accent-primary);
	}

	:global(.search-icon) {
		color: var(--text-muted);
		flex-shrink: 0;
	}

	.search-box input {
		flex: 1;
		background: transparent;
		border: none;
		outline: none;
		font-size: 13px;
		color: var(--text-primary);
	}

	.search-box input::placeholder {
		color: var(--text-muted);
	}

	.clear-btn {
		color: var(--text-muted);
		font-size: 16px;
		line-height: 1;
		padding: 0 4px;
	}

	.clear-btn:hover {
		color: var(--text-primary);
	}

	.search-results {
		flex: 1;
		overflow-y: auto;
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
		gap: var(--spacing-sm);
		padding: var(--spacing-xs) var(--spacing-sm);
		width: 100%;
		text-align: left;
		border-radius: 4px;
		transition: background var(--transition-fast);
	}

	.result-item:hover {
		background: var(--bg-hover);
	}

	.result-title {
		color: var(--text-primary);
		font-size: 13px;
	}

	.result-category {
		color: var(--text-muted);
		font-size: 11px;
	}

	.nav-tree {
		flex: 1;
		overflow-y: auto;
	}

	.nav-category {
		margin-bottom: var(--spacing-xs);
	}

	.category-header {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		width: 100%;
		padding: var(--spacing-xs) var(--spacing-sm);
		color: var(--text-secondary);
		font-size: 13px;
		font-weight: 500;
		text-align: left;
		border-radius: 4px;
		transition: all var(--transition-fast);
	}

	.category-header:hover {
		color: var(--text-primary);
		background: var(--bg-hover);
	}

	.nav-category.active > .category-header {
		color: var(--accent-primary);
	}

	.count {
		margin-left: auto;
		color: var(--text-muted);
		font-size: 11px;
		font-weight: 400;
	}

	.doc-list {
		list-style: none;
		padding: 0;
		margin: 0;
	}

	.doc-link {
		display: block;
		padding: 6px var(--spacing-sm) 6px calc(var(--spacing-sm) + 18px);
		color: var(--text-secondary);
		font-size: 13px;
		text-decoration: none;
		border-radius: 4px;
		transition: all var(--transition-fast);
	}

	.doc-link:hover {
		color: var(--text-primary);
		background: var(--bg-hover);
	}

	.doc-link.active {
		color: var(--accent-primary);
		background: rgba(88, 166, 255, 0.1);
	}
</style>
