<script lang="ts">
	import { onMount } from 'svelte';
	import { page } from '$app/stores';
	import { goto } from '$app/navigation';
	import { decodeInlineHash, inlineHashKey } from '$lib/ide/share/inline-url';
	import { draftsStore } from '$stores/drafts.svelte';
	import { getProvider } from '$lib/ide/storage';
	import { WorkerShareApiError } from '$lib/ide/storage/worker-share';

	type ViewState =
		| { kind: 'loading' }
		| { kind: 'redirect-home' }
		| { kind: 'decode-failed'; reason: string }
		| { kind: 'fetch-failed'; slug: string; reason: string }
		| { kind: 'not-found'; slug: string }
		| { kind: 'no-share-backend'; slug: string };

	let state = $state<ViewState>({ kind: 'loading' });

	onMount(async () => {
		// 1) Slug branch wins over hash (PRD §10.9).
		const slug = $page.params.slug;
		if (slug) {
			await openSlug(slug);
			return;
		}

		// 2) hash carries an inline link.
		const hash = typeof window !== 'undefined' ? window.location.hash : '';
		if (hash && hash.includes('code=')) {
			await openInline(hash);
			return;
		}

		// 3) no slug, no hash — drop to /.
		state = { kind: 'redirect-home' };
		await goto('/', { replaceState: true });
	});

	async function openInline(hash: string) {
		const code = decodeInlineHash(hash);
		if (!code) {
			state = { kind: 'decode-failed', reason: 'invalid or truncated payload' };
			return;
		}
		const key = await inlineHashKey(code);
		const tabId = 'inline:' + key;
		draftsStore.openInlinePhantomTab({ key: tabId, code, title: 'From inline link' });
		state = { kind: 'redirect-home' };
		await goto('/', { replaceState: true });
	}

	async function openSlug(slug: string) {
		const provider = getProvider();
		if (typeof provider.fetchShare !== 'function') {
			state = { kind: 'no-share-backend', slug };
			return;
		}

		// If we already have local edits for this slug, reuse the phantom and
		// skip the re-fetch entirely. The user's edits stay intact; the original
		// share code is reachable by Forget-then-revisit (PatchesPanel).
		const existing = draftsStore.findSlugPhantom(slug);
		if (existing) {
			draftsStore.openSlugPhantomTab({
				slug,
				code: existing.code,
				title: existing.name
			});
			state = { kind: 'redirect-home' };
			await goto('/', { replaceState: true });
			return;
		}

		try {
			const share = await provider.fetchShare(slug);
			if (!share) {
				state = { kind: 'not-found', slug };
				return;
			}
			draftsStore.openSlugPhantomTab({
				slug: share.slug,
				code: share.code,
				title: share.title
			});
			draftsStore.recordVisited(share.slug, share.title).catch(() => {
				/* recording is best-effort; never block the viewer flow */
			});
			state = { kind: 'redirect-home' };
			await goto('/', { replaceState: true });
		} catch (e) {
			const reason = e instanceof WorkerShareApiError
				? `${e.code} (HTTP ${e.status})`
				: (e as Error).message;
			state = { kind: 'fetch-failed', slug, reason };
		}
	}

	function goHome() { goto('/'); }
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
				Try copying the full link again, or ask the sender for a Permalink.
			</p>
			<button class="primary" onclick={goHome}>Back to editor</button>
		</div>
	{:else if state.kind === 'not-found'}
		<div class="card">
			<h1>Patch not found</h1>
			<p class="msg">
				<code>/p/{state.slug}</code> does not exist or was removed.
			</p>
			<button class="primary" onclick={goHome}>Back to editor</button>
		</div>
	{:else if state.kind === 'fetch-failed'}
		{@const failedSlug = state.slug}
		<div class="card">
			<h1>Couldn't load patch</h1>
			<p class="msg">
				<code>/p/{failedSlug}</code>: {state.reason}
			</p>
			<button class="primary" onclick={() => openSlug(failedSlug)}>Retry</button>
			<button class="secondary" onclick={goHome}>Back to editor</button>
		</div>
	{:else if state.kind === 'no-share-backend'}
		<div class="card">
			<h1>Sharing is not configured</h1>
			<p class="msg">
				<code>PUBLIC_SHARE_API_BASE</code> is unset on this build, so slug
				shares like <code>/p/{state.slug}</code> can't be loaded.
				Inline links (<code>/p#code=…</code>) still work.
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
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
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

	.primary,
	.secondary {
		padding: 6px 16px;
		font-size: 13px;
		font-weight: 500;
		border-radius: 4px;
		border: 1px solid transparent;
		cursor: pointer;
	}

	.primary {
		color: var(--bg-primary);
		background-color: var(--accent-primary);
	}

	.primary:hover { opacity: 0.9; }

	.secondary {
		color: var(--text-primary);
		background-color: var(--bg-primary);
		border-color: var(--border-default);
	}

	.secondary:hover { background-color: var(--bg-hover); }

	code {
		font-family: var(--font-mono);
		font-size: 12px;
		background: var(--bg-primary);
		padding: 1px 4px;
		border-radius: 2px;
	}
</style>
