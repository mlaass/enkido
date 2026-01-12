<script lang="ts">
	import { onMount, tick } from 'svelte';
	import { docsStore } from '$lib/stores/docs.svelte';
	import { renderMarkdown } from '../parser';
	import { slugToPath } from '../manifest';
	import DocCodeWidget from './DocCodeWidget.svelte';

	interface Props {
		slug: string;
		anchor?: string | null;
		fullPage?: boolean;
	}

	let { slug, anchor = null, fullPage = false }: Props = $props();

	let contentElement: HTMLDivElement;
	let htmlContent = $state('');
	let codeBlocks = $state<Array<{ id: string; code: string }>>([]);
	let isLoading = $state(true);
	let error = $state<string | null>(null);

	// Load and render the markdown document
	async function loadDocument() {
		isLoading = true;
		error = null;

		try {
			const path = slugToPath[slug];
			if (!path) {
				htmlContent = `<p class="doc-placeholder">Documentation for <code>${slug}</code> is not yet available.</p>`;
				codeBlocks = [];
				isLoading = false;
				return;
			}

			const response = await fetch(`/docs/${path}`);
			if (!response.ok) {
				throw new Error(`Failed to load: ${response.status}`);
			}

			const raw = await response.text();
			// Parse out frontmatter and get content
			const match = raw.match(/^---\n[\s\S]*?\n---\n([\s\S]*)$/);
			const content = match ? match[1] : raw;

			const rendered = renderMarkdown(content);
			processRenderedContent(rendered);
		} catch (err) {
			error = err instanceof Error ? err.message : 'Failed to load documentation';
			htmlContent = '';
			codeBlocks = [];
		} finally {
			isLoading = false;
		}
	}

	// Content segments: alternating html and code blocks
	let segments = $state<Array<{ type: 'html' | 'code'; content: string }>>([]);

	// Process rendered HTML and split into segments
	function processRenderedContent(html: string) {
		const result: Array<{ type: 'html' | 'code'; content: string }> = [];
		const regex = /<div class="akk-code-block" data-code="([^"]+)">[\s\S]*?<\/div>/g;

		let lastIndex = 0;
		let match;

		while ((match = regex.exec(html)) !== null) {
			// Add HTML before this code block
			if (match.index > lastIndex) {
				result.push({ type: 'html', content: html.slice(lastIndex, match.index) });
			}
			// Add the code block
			result.push({ type: 'code', content: decodeURIComponent(match[1]) });
			lastIndex = match.index + match[0].length;
		}

		// Add remaining HTML after last code block
		if (lastIndex < html.length) {
			result.push({ type: 'html', content: html.slice(lastIndex) });
		}

		segments = result;
		htmlContent = ''; // Not used anymore
		codeBlocks = []; // Not used anymore
	}

	// Scroll to anchor if specified
	async function scrollToAnchor() {
		if (!anchor || !contentElement) return;

		await tick();

		const element = contentElement.querySelector(`#${anchor}`);
		if (element) {
			element.scrollIntoView({ behavior: 'smooth', block: 'start' });
			element.classList.add('highlight-flash');
			setTimeout(() => element.classList.remove('highlight-flash'), 2000);
		}
	}

	// Navigate back to category navigation
	function goBack() {
		docsStore.setDocument('');
	}

	// Load document when slug changes
	$effect(() => {
		slug; // Track slug changes
		loadDocument();
	});

	$effect(() => {
		if (anchor && !isLoading) {
			scrollToAnchor();
		}
	});
</script>

<div class="doc-container" class:full-page={fullPage}>
	{#if !fullPage}
		<button class="back-btn" onclick={goBack}>
			<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
				<path d="m15 18-6-6 6-6" />
			</svg>
			Back
		</button>
	{/if}

	{#if isLoading}
		<div class="loading">Loading...</div>
	{:else if error}
		<div class="error">{error}</div>
	{:else}
		<div class="doc-content" bind:this={contentElement}>
			{#each segments as segment, i (i)}
				{#if segment.type === 'html'}
					{@html segment.content}
				{:else}
					<DocCodeWidget code={segment.content} />
				{/if}
			{/each}
		</div>
	{/if}
</div>

<style>
	.doc-container {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
	}

	.doc-container.full-page {
		max-width: 800px;
	}

	.doc-container.full-page .doc-content {
		font-size: 14px;
	}

	.doc-container.full-page :global(h1) {
		font-size: 1.75rem;
		margin-bottom: var(--spacing-lg);
	}

	.doc-container.full-page :global(h2) {
		font-size: 1.25rem;
		margin-top: var(--spacing-xl);
	}

	.doc-container.full-page :global(h3) {
		font-size: 1rem;
	}

	.back-btn {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		padding: 4px 8px;
		font-size: 12px;
		color: var(--text-secondary);
		background: transparent;
		border-radius: 4px;
		transition: all var(--transition-fast);
		align-self: flex-start;
	}

	.back-btn:hover {
		color: var(--text-primary);
		background: var(--bg-tertiary);
	}

	.loading,
	.error {
		padding: var(--spacing-md);
		text-align: center;
		font-size: 13px;
	}

	.loading {
		color: var(--text-muted);
	}

	.error {
		color: var(--accent-error);
	}

	.doc-content {
		font-size: 13px;
		line-height: 1.6;
		color: var(--text-primary);
	}

	/* Markdown styling */
	.doc-content :global(h1) {
		font-size: 18px;
		font-weight: 600;
		margin: 0 0 var(--spacing-md) 0;
		color: var(--text-primary);
	}

	.doc-content :global(h2) {
		font-size: 15px;
		font-weight: 600;
		margin: var(--spacing-lg) 0 var(--spacing-sm) 0;
		padding-top: var(--spacing-sm);
		border-top: 1px solid var(--border-muted);
		color: var(--text-primary);
	}

	.doc-content :global(h3) {
		font-size: 13px;
		font-weight: 600;
		margin: var(--spacing-md) 0 var(--spacing-xs) 0;
		color: var(--text-primary);
	}

	.doc-content :global(p) {
		margin: 0 0 var(--spacing-sm) 0;
	}

	.doc-content :global(code) {
		font-family: var(--font-mono);
		font-size: 12px;
		padding: 1px 4px;
		background: var(--bg-tertiary);
		border-radius: 3px;
	}

	.doc-content :global(pre) {
		font-family: var(--font-mono);
		font-size: 12px;
		padding: var(--spacing-sm);
		background: var(--bg-tertiary);
		border-radius: 4px;
		overflow-x: auto;
		margin: var(--spacing-sm) 0;
	}

	.doc-content :global(pre code) {
		padding: 0;
		background: transparent;
	}

	.doc-content :global(table) {
		width: 100%;
		border-collapse: collapse;
		font-size: 12px;
		margin: var(--spacing-sm) 0;
	}

	.doc-content :global(th),
	.doc-content :global(td) {
		padding: 6px 8px;
		text-align: left;
		border: 1px solid var(--border-muted);
	}

	.doc-content :global(th) {
		background: var(--bg-tertiary);
		font-weight: 600;
	}

	.doc-content :global(a) {
		color: var(--accent-primary);
		text-decoration: none;
	}

	.doc-content :global(a:hover) {
		text-decoration: underline;
	}

	.doc-content :global(.highlight-flash) {
		animation: flash 2s ease-out;
	}

	@keyframes flash {
		0% { background-color: rgba(88, 166, 255, 0.3); }
		100% { background-color: transparent; }
	}

	.doc-content :global(.doc-placeholder) {
		color: var(--text-muted);
		font-style: italic;
	}
</style>
