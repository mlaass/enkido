<script lang="ts">
	import { page } from '$app/stores';
	import { navigation } from '$lib/docs/manifest';
	import DocMarkdown from '$lib/docs/components/DocMarkdown.svelte';
	import { ChevronLeft, ChevronRight } from 'lucide-svelte';

	let category = $derived($page.params.category ?? '');
	let slug = $derived($page.params.slug ?? '');
	let anchor = $derived($page.url.hash?.slice(1) || null);

	let docs = $derived(navigation[category] ?? []);
	let currentIndex = $derived(docs.findIndex((d: { slug: string }) => d.slug === slug));

	let prevDoc = $derived(currentIndex > 0 ? docs[currentIndex - 1] : null);
	let nextDoc = $derived(currentIndex < docs.length - 1 ? docs[currentIndex + 1] : null);
</script>

<div class="document-page">
	<DocMarkdown {slug} {anchor} fullPage={true} />

	<nav class="doc-nav">
		{#if prevDoc}
			<a href="/docs/{category}/{prevDoc.slug}" class="nav-link prev">
				<ChevronLeft size={16} />
				<div class="nav-content">
					<span class="nav-label">Previous</span>
					<span class="nav-title">{prevDoc.title}</span>
				</div>
			</a>
		{:else}
			<div class="nav-placeholder"></div>
		{/if}

		{#if nextDoc}
			<a href="/docs/{category}/{nextDoc.slug}" class="nav-link next">
				<div class="nav-content">
					<span class="nav-label">Next</span>
					<span class="nav-title">{nextDoc.title}</span>
				</div>
				<ChevronRight size={16} />
			</a>
		{:else}
			<div class="nav-placeholder"></div>
		{/if}
	</nav>
</div>

<style>
	.document-page {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xl);
	}

	.doc-nav {
		display: flex;
		justify-content: space-between;
		gap: var(--spacing-md);
		padding-top: var(--spacing-lg);
		border-top: 1px solid var(--border-muted);
		margin-top: var(--spacing-lg);
	}

	.nav-link {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
		padding: var(--spacing-sm) var(--spacing-md);
		background: var(--bg-secondary);
		border: 1px solid var(--border-default);
		border-radius: 8px;
		text-decoration: none;
		color: var(--text-secondary);
		transition: all var(--transition-fast);
		max-width: 45%;
	}

	.nav-link:hover {
		border-color: var(--accent-primary);
		color: var(--text-primary);
	}

	.nav-link.prev {
		text-align: left;
	}

	.nav-link.next {
		text-align: right;
		margin-left: auto;
	}

	.nav-content {
		display: flex;
		flex-direction: column;
		gap: 2px;
		min-width: 0;
	}

	.nav-label {
		font-size: 11px;
		text-transform: uppercase;
		letter-spacing: 0.5px;
		color: var(--text-muted);
	}

	.nav-title {
		font-size: 14px;
		font-weight: 500;
		color: inherit;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.nav-placeholder {
		flex: 1;
	}
</style>
