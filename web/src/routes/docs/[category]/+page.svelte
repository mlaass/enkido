<script lang="ts">
	import { page } from '$app/stores';
	import { navigation, previews } from '$lib/docs/manifest';

	let category = $derived($page.params.category ?? '');

	const categoryInfo: Record<string, { label: string; description: string }> = {
		tutorials: {
			label: 'Tutorials',
			description: 'Step-by-step guides to learn Akkado from the basics to advanced techniques.'
		},
		builtins: {
			label: 'Builtins',
			description:
				'Complete reference for all built-in functions including oscillators, filters, effects, and more.'
		},
		language: {
			label: 'Language',
			description:
				'Learn about pipes, variables, operators, and closures that make up the Akkado language.'
		},
		'mini-notation': {
			label: 'Mini-notation',
			description: 'Pattern syntax for sequencing, rhythm, and musical structures.'
		},
		concepts: {
			label: 'Concepts',
			description: 'Core ideas and architectural concepts behind NKIDO.'
		}
	};

	let docs = $derived(navigation[category] ?? []);
	let info = $derived(categoryInfo[category] ?? { label: category, description: '' });
</script>

<div class="category-page">
	<header class="category-header">
		<h1>{info.label}</h1>
		<p>{info.description}</p>
	</header>

	<div class="doc-grid">
		{#each docs as doc}
			<a href="/docs/{category}/{doc.slug}" class="doc-card">
				<h2>{doc.title}</h2>
				{#if previews[doc.slug]}
					<p class="preview">{previews[doc.slug]}</p>
				{/if}
			</a>
		{/each}
	</div>

	{#if docs.length === 0}
		<p class="empty">No documentation in this category yet.</p>
	{/if}
</div>

<style>
	.category-page {
		max-width: 900px;
		margin: 0 auto;
	}

	.category-header {
		margin-bottom: var(--spacing-xl);
	}

	.category-header h1 {
		font-size: 1.75rem;
		font-weight: 600;
		color: var(--text-primary);
		margin-bottom: var(--spacing-sm);
	}

	.category-header p {
		color: var(--text-secondary);
		font-size: 1rem;
		line-height: 1.5;
	}

	.doc-grid {
		display: grid;
		grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
		gap: var(--spacing-md);
	}

	.doc-card {
		display: block;
		padding: var(--spacing-md);
		background: var(--bg-secondary);
		border: 1px solid var(--border-default);
		border-radius: 8px;
		text-decoration: none;
		transition: all var(--transition-fast);
	}

	.doc-card:hover {
		border-color: var(--accent-primary);
		background: var(--bg-tertiary);
	}

	.doc-card h2 {
		font-size: 1rem;
		font-weight: 600;
		color: var(--text-primary);
		margin-bottom: var(--spacing-xs);
	}

	.doc-card .preview {
		color: var(--text-muted);
		font-size: 13px;
		line-height: 1.4;
		display: -webkit-box;
		-webkit-line-clamp: 2;
		-webkit-box-orient: vertical;
		overflow: hidden;
	}

	.empty {
		color: var(--text-muted);
		text-align: center;
		padding: var(--spacing-xl);
	}
</style>
