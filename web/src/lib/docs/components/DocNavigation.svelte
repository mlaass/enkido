<script lang="ts">
	import { docsStore } from '$lib/stores/docs.svelte';
	import type { DocCategory } from '../types';

	interface Props {
		category: DocCategory;
	}

	let { category }: Props = $props();

	// Static navigation structure until we load actual documents
	const navigationByCategory: Record<DocCategory, Array<{ label: string; slug: string }>> = {
		builtins: [
			{ label: 'Oscillators', slug: 'oscillators' },
			{ label: 'Filters', slug: 'filters' },
			{ label: 'Envelopes', slug: 'envelopes' },
			{ label: 'Effects', slug: 'effects' },
			{ label: 'Distortion', slug: 'distortion' },
			{ label: 'Dynamics', slug: 'dynamics' },
			{ label: 'Math', slug: 'math' },
			{ label: 'Utility', slug: 'utility' },
			{ label: 'Sequencing', slug: 'sequencing' }
		],
		language: [
			{ label: 'Pipes & Holes', slug: 'pipes' },
			{ label: 'Closures', slug: 'closures' },
			{ label: 'Operators', slug: 'operators' },
			{ label: 'Patterns', slug: 'patterns' }
		],
		'mini-notation': [
			{ label: 'Basics', slug: 'basics' },
			{ label: 'Modifiers', slug: 'modifiers' },
			{ label: 'Euclidean Rhythms', slug: 'euclidean' },
			{ label: 'Advanced', slug: 'advanced' }
		],
		concepts: [
			{ label: 'Signal Flow', slug: 'signal-flow' },
			{ label: 'Clock & Timing', slug: 'clock-timing' },
			{ label: 'Chords', slug: 'chords' },
			{ label: 'Hot-Swapping', slug: 'hot-swap' }
		],
		tutorials: [
			{ label: '1. Hello Sine', slug: '01-hello-sine' },
			{ label: '2. Filters', slug: '02-filters' },
			{ label: '3. Envelopes', slug: '03-envelopes' },
			{ label: '4. Patterns', slug: '04-patterns' },
			{ label: '5. Modulation', slug: '05-modulation' },
			{ label: '6. Synthesis', slug: '06-synthesis' }
		]
	};

	const categoryDescriptions: Record<DocCategory, string> = {
		builtins: 'Built-in functions for audio synthesis and processing',
		language: 'Core language syntax and operators',
		'mini-notation': 'Compact pattern notation for rhythms and sequences',
		concepts: 'Key concepts for understanding Akkado',
		tutorials: 'Step-by-step guides from basics to advanced synthesis'
	};

	$effect(() => {
		// When category changes, we could load documents here
	});
</script>

<div class="navigation">
	<p class="category-description">{categoryDescriptions[category]}</p>

	<div class="nav-items">
		{#each navigationByCategory[category] as item}
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
