<script lang="ts">
	import { docsStore } from '$lib/stores/docs.svelte';
	import { navigation } from '../manifest';
	import type { DocCategory } from '../types';

	interface Props {
		category: DocCategory;
	}

	let { category }: Props = $props();

	// Get navigation items for current category from manifest
	function getNavItems(cat: DocCategory): Array<{ label: string; slug: string }> {
		const items = navigation[cat] || [];
		return items.map((item) => ({ label: item.title, slug: item.slug }));
	}

	const categoryDescriptions: Record<DocCategory, string> = {
		builtins: 'Built-in functions for audio synthesis and processing',
		language: 'Core language syntax and operators',
		'mini-notation': 'Compact pattern notation for rhythms and sequences',
		concepts: 'Key concepts for understanding Akkado',
		tutorials: 'Step-by-step guides from basics to advanced synthesis'
	};
</script>

<div class="navigation">
	<p class="category-description">{categoryDescriptions[category]}</p>

	<div class="nav-items">
		{#each getNavItems(category) as item}
			<button
				class="nav-item"
				class:active={docsStore.activeSlug === item.slug}
				onclick={() => docsStore.setDocument(item.slug)}
			>
				{item.label}
			</button>
		{/each}
	</div>
</div>

<style>
	.navigation {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
	}

	.category-description {
		color: var(--text-muted);
		font-size: 12px;
		margin: 0 0 var(--spacing-sm) 0;
	}

	.nav-items {
		display: flex;
		flex-direction: column;
		gap: 2px;
	}

	.nav-item {
		padding: 8px 10px;
		text-align: left;
		font-size: 13px;
		color: var(--text-secondary);
		background: transparent;
		border-radius: 4px;
		transition: all var(--transition-fast);
	}

	.nav-item:hover {
		color: var(--text-primary);
		background: var(--bg-tertiary);
	}

	.nav-item.active {
		color: var(--accent-primary);
		background: rgba(88, 166, 255, 0.1);
	}
</style>
