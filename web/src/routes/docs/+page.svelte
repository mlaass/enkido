<script lang="ts">
	import { navigation, previews } from '$lib/docs/manifest';
	import DocSearch from '$lib/docs/components/DocSearch.svelte';
	import { docsStore } from '$lib/stores/docs.svelte';
	import { goto } from '$app/navigation';

	const categoryInfo: Record<string, { label: string; description: string }> = {
		tutorials: {
			label: 'Tutorials',
			description: 'Step-by-step guides to learn Akkado from scratch'
		},
		builtins: {
			label: 'Builtins',
			description: 'Reference for oscillators, filters, effects, and more'
		},
		language: {
			label: 'Language',
			description: 'Pipes, variables, operators, and closures'
		},
		'mini-notation': {
			label: 'Mini-notation',
			description: 'Pattern syntax for sequencing and rhythm'
		},
		concepts: {
			label: 'Concepts',
			description: 'Core ideas and architecture'
		}
	};

	const categoryOrder = ['tutorials', 'builtins', 'language', 'mini-notation', 'concepts'];

	function handleResultClick(slug: string, category: string, anchor?: string) {
		goto(`/docs/${category}/${slug}${anchor ? `#${anchor}` : ''}`);
	}
</script>

<div class="landing">
	<div class="hero">
		<h1>NKIDO Documentation</h1>
		<p class="subtitle">Learn to create music with the Akkado language and Cedar audio engine</p>
	</div>

	<div class="search-container">
		<DocSearch />
		{#if docsStore.searchQuery.length >= 2}
			<div class="search-results">
				{#if docsStore.searchResults.length === 0}
					<p class="no-results">No results for "{docsStore.searchQuery}"</p>
				{:else}
					{#each docsStore.searchResults as result}
						<button
							class="result-item"
							onclick={() => handleResultClick(result.slug, result.category, result.anchor)}
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
		{/if}
	</div>

	{#if docsStore.searchQuery.length < 2}
		<div class="categories">
			{#each categoryOrder as cat}
				{@const info = categoryInfo[cat]}
				{@const docs = navigation[cat] || []}
				{#if docs.length > 0}
					<a href="/docs/{cat}" class="category-card">
						<h2>{info.label}</h2>
						<p>{info.description}</p>
						<span class="doc-count">{docs.length} {docs.length === 1 ? 'doc' : 'docs'}</span>
					</a>
				{/if}
			{/each}
		</div>

		<div class="quick-start">
			<h2>Quick Start</h2>
			<p>New to NKIDO? Start with these tutorials:</p>
			<div class="quick-links">
				{#each navigation['tutorials']?.slice(0, 3) || [] as doc}
					<a href="/docs/tutorials/{doc.slug}" class="quick-link">
						<span class="link-title">{doc.title}</span>
						{#if previews[doc.slug]}
							<span class="link-preview">{previews[doc.slug]}</span>
						{/if}
					</a>
				{/each}
			</div>
		</div>
	{/if}
</div>

<style>
	.landing {
		max-width: 900px;
		margin: 0 auto;
	}

	.hero {
		text-align: center;
		margin-bottom: var(--spacing-xl);
	}

	.hero h1 {
		font-size: 2rem;
		font-weight: 600;
		color: var(--text-primary);
		margin-bottom: var(--spacing-sm);
	}

	.subtitle {
		color: var(--text-secondary);
		font-size: 1.1rem;
	}

	.search-container {
		max-width: 500px;
		margin: 0 auto var(--spacing-xl);
	}

	.search-results {
		margin-top: var(--spacing-md);
		background: var(--bg-secondary);
		border: 1px solid var(--border-default);
		border-radius: 8px;
		overflow: hidden;
	}

	.no-results {
		padding: var(--spacing-md);
		color: var(--text-muted);
		text-align: center;
	}

	.result-item {
		display: flex;
		justify-content: space-between;
		align-items: flex-start;
		gap: var(--spacing-md);
		padding: var(--spacing-sm) var(--spacing-md);
		width: 100%;
		text-align: left;
		background: transparent;
		border-bottom: 1px solid var(--border-muted);
		transition: background var(--transition-fast);
	}

	.result-item:last-child {
		border-bottom: none;
	}

	.result-item:hover {
		background: var(--bg-hover);
	}

	.result-content {
		display: flex;
		flex-direction: column;
		gap: 4px;
		flex: 1;
		min-width: 0;
	}

	.result-title {
		color: var(--text-primary);
		font-weight: 500;
	}

	.result-preview {
		color: var(--text-muted);
		font-size: 12px;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.result-category {
		color: var(--text-muted);
		font-size: 11px;
		background: var(--bg-tertiary);
		padding: 2px 6px;
		border-radius: 4px;
		flex-shrink: 0;
	}

	.categories {
		display: grid;
		grid-template-columns: repeat(auto-fill, minmax(250px, 1fr));
		gap: var(--spacing-md);
		margin-bottom: var(--spacing-xl);
	}

	.category-card {
		display: block;
		padding: var(--spacing-md);
		background: var(--bg-secondary);
		border: 1px solid var(--border-default);
		border-radius: 8px;
		text-decoration: none;
		transition: all var(--transition-fast);
	}

	.category-card:hover {
		border-color: var(--accent-primary);
		background: var(--bg-tertiary);
	}

	.category-card h2 {
		font-size: 1.1rem;
		font-weight: 600;
		color: var(--text-primary);
		margin-bottom: var(--spacing-xs);
	}

	.category-card p {
		color: var(--text-secondary);
		font-size: 13px;
		margin-bottom: var(--spacing-sm);
	}

	.doc-count {
		color: var(--text-muted);
		font-size: 12px;
	}

	.quick-start {
		background: var(--bg-secondary);
		border: 1px solid var(--border-default);
		border-radius: 8px;
		padding: var(--spacing-lg);
	}

	.quick-start h2 {
		font-size: 1.1rem;
		font-weight: 600;
		color: var(--text-primary);
		margin-bottom: var(--spacing-xs);
	}

	.quick-start > p {
		color: var(--text-secondary);
		margin-bottom: var(--spacing-md);
	}

	.quick-links {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
	}

	.quick-link {
		display: flex;
		flex-direction: column;
		gap: 4px;
		padding: var(--spacing-sm) var(--spacing-md);
		background: var(--bg-tertiary);
		border-radius: 6px;
		text-decoration: none;
		transition: background var(--transition-fast);
	}

	.quick-link:hover {
		background: var(--bg-hover);
	}

	.link-title {
		color: var(--text-primary);
		font-weight: 500;
	}

	.link-preview {
		color: var(--text-muted);
		font-size: 12px;
	}
</style>
