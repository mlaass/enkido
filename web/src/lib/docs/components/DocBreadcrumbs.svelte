<script lang="ts">
	import { navigation } from '../manifest';
	import { ChevronRight } from 'lucide-svelte';

	interface Props {
		category?: string;
		slug?: string;
	}

	let { category, slug }: Props = $props();

	const categoryLabels: Record<string, string> = {
		builtins: 'Builtins',
		language: 'Language',
		'mini-notation': 'Mini-notation',
		concepts: 'Concepts',
		tutorials: 'Tutorials'
	};

	function getDocTitle(cat: string, s: string): string {
		return navigation[cat]?.find((d) => d.slug === s)?.title ?? s;
	}
</script>

<nav class="breadcrumbs" aria-label="Breadcrumb">
	<a href="/docs" class="crumb">Home</a>
	{#if category}
		<ChevronRight size={14} class="separator" />
		<a href="/docs/{category}" class="crumb">{categoryLabels[category] ?? category}</a>
	{/if}
	{#if slug}
		<ChevronRight size={14} class="separator" />
		<span class="crumb current">{getDocTitle(category!, slug)}</span>
	{/if}
</nav>

<style>
	.breadcrumbs {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		font-size: 13px;
	}

	.crumb {
		color: var(--text-secondary);
		text-decoration: none;
		transition: color var(--transition-fast);
	}

	.crumb:hover:not(.current) {
		color: var(--text-primary);
	}

	.crumb.current {
		color: var(--text-primary);
		font-weight: 500;
	}

	:global(.separator) {
		color: var(--text-muted);
		flex-shrink: 0;
	}
</style>
