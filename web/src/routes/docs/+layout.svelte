<script lang="ts">
	import { page } from '$app/stores';
	import { goto } from '$app/navigation';
	import Logo from '$components/Logo/Logo.svelte';
	import DocBreadcrumbs from '$lib/docs/components/DocBreadcrumbs.svelte';
	import DocsFullNav from '$lib/docs/components/DocsFullNav.svelte';
	import { ArrowLeft } from 'lucide-svelte';

	let { children } = $props();

	let category = $derived($page.params.category);
	let slug = $derived($page.params.slug);
</script>

<div class="docs-page">
	<header class="docs-header">
		<div class="header-left">
			<button class="back-btn" onclick={() => goto('/')} title="Back to Editor">
				<ArrowLeft size={18} />
				<span>Editor</span>
			</button>
			<span class="divider"></span>
			<a href="/docs" class="logo-link">
				<Logo size={24} />
				<span class="docs-title">Docs</span>
			</a>
		</div>
		<div class="header-right">
			<DocBreadcrumbs {category} {slug} />
		</div>
	</header>

	<div class="docs-body">
		<aside class="docs-sidebar">
			<DocsFullNav activeCategory={category} activeSlug={slug} />
		</aside>
		<main class="docs-main">
			{@render children()}
		</main>
	</div>
</div>

<style>
	.docs-page {
		display: flex;
		flex-direction: column;
		height: 100vh;
		overflow: hidden;
		background: var(--bg-primary);
	}

	.docs-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		height: var(--header-height);
		padding: 0 var(--spacing-md);
		background-color: var(--bg-secondary);
		border-bottom: 1px solid var(--border-default);
		flex-shrink: 0;
	}

	.header-left {
		display: flex;
		align-items: center;
		gap: var(--spacing-md);
	}

	.back-btn {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		padding: var(--spacing-xs) var(--spacing-sm);
		color: var(--text-secondary);
		background: var(--bg-tertiary);
		border-radius: 4px;
		font-size: 13px;
		transition: all var(--transition-fast);
	}

	.back-btn:hover {
		color: var(--text-primary);
		background: var(--bg-hover);
	}

	.divider {
		width: 1px;
		height: 20px;
		background: var(--border-muted);
	}

	.logo-link {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
		color: var(--text-primary);
		text-decoration: none;
	}

	.docs-title {
		font-family: var(--font-mono);
		font-size: 16px;
		font-weight: 600;
	}

	.header-right {
		display: flex;
		align-items: center;
	}

	.docs-body {
		display: flex;
		flex: 1;
		overflow: hidden;
	}

	.docs-sidebar {
		width: 280px;
		flex-shrink: 0;
		border-right: 1px solid var(--border-default);
		background: var(--bg-secondary);
		overflow-y: auto;
	}

	.docs-main {
		flex: 1;
		overflow-y: auto;
		padding: var(--spacing-lg) var(--spacing-xl);
	}

	@media (max-width: 768px) {
		.docs-sidebar {
			display: none;
		}

		.docs-main {
			padding: var(--spacing-md);
		}
	}
</style>
