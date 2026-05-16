<script lang="ts">
	import { onMount } from 'svelte';
	import { page } from '$app/stores';
	import { goto } from '$app/navigation';
	import { decodeInlineHash, inlineHashKey } from '$lib/ide/share/inline-url';
	import { draftsStore } from '$stores/drafts.svelte';

	type ViewState =
		| { kind: 'loading' }
		| { kind: 'redirect-home' }
		| { kind: 'decode-failed'; reason: string }
		| { kind: 'slug-stub'; slug: string };

	let state = $state<ViewState>({ kind: 'loading' });

	onMount(async () => {
		// 1) hash takes priority — that's the inline-link transport.
		const hash = typeof window !== 'undefined' ? window.location.hash : '';
		if (hash && hash.includes('code=')) {
			const code = decodeInlineHash(hash);
			if (!code) {
				state = { kind: 'decode-failed', reason: 'invalid or truncated payload' };
				return;
			}
			const key = await inlineHashKey(code);
			const tabId = 'inline:' + key;
			draftsStore.openInlinePhantomTab({
				key: tabId,
				code,
				title: 'From inline link'
			});
			state = { kind: 'redirect-home' };
			// Clean the hash from the URL by navigating to /; the phantom tab is
			// already active in the drafts store. Use replaceState so back-button
			// doesn't bounce here.
			await goto('/', { replaceState: true });
			return;
		}

		// 2) slug branch — Phase 2 stub.
		const slug = $page.params.slug;
		if (slug) {
			state = { kind: 'slug-stub', slug };
			return;
		}

		// 3) no slug, no hash — same as Phase 2 stub but generic.
		state = { kind: 'slug-stub', slug: '' };
	});

	function goHome() {
		goto('/');
	}
</script>

<svelte:head>
	<title>Loading patch — nkido</title>
</svelte:head>

<div class="viewer">
	{#if state.kind === 'loading' || state.kind === 'redirect-home'}
		<div class="card">
			<p class="msg">Loading patch…</p>
		</div>
	{:else if state.kind === 'decode-failed'}
		<div class="card">
			<h1>Couldn't decode inline link</h1>
			<p class="msg">
				The shared URL appears malformed or was truncated ({state.reason}).
				Try copying the full link again, or ask the sender to use a
				Permalink (coming in Phase 2) instead.
			</p>
			<button class="primary" onclick={goHome}>Back to editor</button>
		</div>
	{:else if state.kind === 'slug-stub'}
		<div class="card">
			<h1>{state.slug ? `Slug share /p/${state.slug}` : 'Patch viewer'}</h1>
			<p class="msg">
				Slug-based Worker shares are coming in Phase&nbsp;2 of
				<a href="/docs/architecture/shareable-patches">shareable patches</a>.
				For now, share patches via inline links from the editor.
			</p>
			<button class="primary" onclick={goHome}>Back to editor</button>
		</div>
	{/if}
</div>

<style>
	.viewer {
		display: flex;
		align-items: center;
		justify-content: center;
		min-height: 100vh;
		padding: var(--spacing-lg);
		background-color: var(--bg-primary);
	}

	.card {
		max-width: 480px;
		padding: var(--spacing-lg);
		background-color: var(--bg-secondary);
		border: 1px solid var(--border-default);
		border-radius: 8px;
	}

	h1 {
		margin: 0 0 var(--spacing-md);
		font-size: 18px;
		color: var(--text-primary);
	}

	.msg {
		margin: 0 0 var(--spacing-md);
		font-size: 14px;
		color: var(--text-secondary);
		line-height: 1.5;
	}

	.primary {
		padding: 6px 16px;
		font-size: 13px;
		font-weight: 500;
		color: var(--bg-primary);
		background-color: var(--accent-primary);
		border: none;
		border-radius: 4px;
		cursor: pointer;
	}

	.primary:hover {
		opacity: 0.9;
	}
</style>
