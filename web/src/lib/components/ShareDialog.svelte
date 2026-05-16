<script lang="ts">
	import { onMount } from 'svelte';
	import { Copy, X, ExternalLink, AlertTriangle } from 'lucide-svelte';
	import { draftsStore } from '$stores/drafts.svelte';
	import { getProvider } from '$lib/ide/storage';
	import { encodeInlineUrl, type InlineUrl } from '$lib/ide/share/inline-url';

	type Props = { onClose: () => void };
	let { onClose }: Props = $props();

	const provider = getProvider();
	const canPermalink = typeof provider.share === 'function';

	type Tab = 'permalink' | 'inline';
	let activeTab = $state<Tab>(canPermalink ? 'permalink' : 'inline');

	// --- Permalink tab state ------------------------------------------------
	let title = $state('');
	let description = $state('');
	let parentSlug = $state<string | null>(null);
	let publishing = $state(false);
	let publishError = $state<string | null>(null);
	let publishedSlug = $state<string | null>(null);
	let publishedUrl = $state<string | null>(null);

	// --- Inline tab state ---------------------------------------------------
	let inline = $state<InlineUrl>({ url: '', byteLength: 0, status: 'green' });

	// --- Toast --------------------------------------------------------------
	let copyToast = $state<string | null>(null);
	let copyTimer: ReturnType<typeof setTimeout> | null = null;

	onMount(() => {
		// Seed title from active draft name if it isn't an inline-default.
		const active = draftsStore.activeDraft;
		if (active) {
			title = active.isPhantom ? `Fork of ${active.name}` : active.name;
			if (active.isPhantom && active.phantomSlug) {
				parentSlug = active.phantomSlug;
			}
		}
		refreshInline();
	});

	// Recompute inline URL whenever the active code changes.
	$effect(() => {
		const _ = draftsStore.activeCode;
		refreshInline();
	});

	function refreshInline() {
		const code = draftsStore.activeCode;
		if (!code) {
			inline = { url: '', byteLength: 0, status: 'green' };
			return;
		}
		inline = encodeInlineUrl(code);
	}

	async function copy(text: string, label: string) {
		try {
			await navigator.clipboard.writeText(text);
			toast(`${label} copied`);
		} catch {
			toast('Copy failed — select and copy manually');
		}
	}

	function toast(msg: string) {
		copyToast = msg;
		if (copyTimer) clearTimeout(copyTimer);
		copyTimer = setTimeout(() => (copyToast = null), 1800);
	}

	async function publish() {
		if (!provider.share) return;
		publishing = true;
		publishError = null;
		try {
			const result = await provider.share({
				code: draftsStore.activeCode,
				title: title.trim() || undefined,
				description: description.trim() || undefined,
				parentSlug: parentSlug ?? undefined
			});
			publishedSlug = result.slug;
			const apiBase = getApiBase();
			publishedUrl = `${apiBase}/p/${result.slug}`;
		} catch (e) {
			publishError = (e as Error).message;
		} finally {
			publishing = false;
		}
	}

	function getApiBase(): string {
		try {
			const raw = (import.meta as { env?: Record<string, string | undefined> }).env
				?.PUBLIC_SHARE_API_BASE;
			return (raw ?? '').replace(/\/$/, '');
		} catch {
			return '';
		}
	}

	function statusLabel(): string {
		const bytes = inline.byteLength.toLocaleString();
		switch (inline.status) {
			case 'green': return `${bytes} bytes — fits anywhere`;
			case 'yellow': return `${bytes} bytes — Discord/Slack OK; some clients may truncate`;
			case 'red': return `${bytes} bytes — too long for most messengers; use Permalink instead`;
		}
	}

	function onBackdrop(e: MouseEvent) {
		if (e.target === e.currentTarget) onClose();
	}

	function onKey(e: KeyboardEvent) {
		if (e.key === 'Escape') onClose();
	}
</script>

<svelte:window onkeydown={onKey} />

<div
	class="backdrop"
	role="presentation"
	onclick={onBackdrop}
>
	<div class="modal" role="dialog" aria-modal="true" aria-labelledby="share-title">
		<header class="modal-header">
			<h2 id="share-title">Share patch</h2>
			<button class="icon-button close" title="Close" onclick={onClose}>
				<X size={16} />
			</button>
		</header>

		<div class="tabs" role="tablist">
			<button
				role="tab"
				class="tab"
				class:active={activeTab === 'permalink'}
				class:disabled={!canPermalink}
				disabled={!canPermalink}
				title={canPermalink ? '' : 'Set PUBLIC_SHARE_API_BASE to enable Permalink'}
				onclick={() => canPermalink && (activeTab = 'permalink')}
			>
				Permalink
			</button>
			<button
				role="tab"
				class="tab"
				class:active={activeTab === 'inline'}
				onclick={() => (activeTab = 'inline')}
			>
				Inline link
			</button>
		</div>

		{#if activeTab === 'permalink'}
			<div class="tab-body">
				{#if !canPermalink}
					<p class="info-card warning">
						<AlertTriangle size={14} />
						<span>Worker URL is not configured. Set <code>PUBLIC_SHARE_API_BASE</code> in <code>web/.env</code> to enable permalinks. Inline link still works.</span>
					</p>
				{:else if publishedSlug}
					<label class="field">
						<span class="label">Public URL</span>
						<div class="url-row">
							<input class="url-input" type="text" readonly value={publishedUrl} />
							<button class="primary" onclick={() => copy(publishedUrl!, 'Link')}>
								<Copy size={14} /> Copy
							</button>
							<a class="secondary" href={publishedUrl} target="_blank" rel="noopener noreferrer">
								<ExternalLink size={14} /> Open
							</a>
						</div>
					</label>
					<p class="hint success">
						Shared! Anyone with the link can view this patch. It's immutable —
						to change it, fork.
					</p>
				{:else}
					<label class="field">
						<span class="label">Title</span>
						<input type="text" placeholder="Untitled" bind:value={title} maxlength="200" />
					</label>
					<label class="field">
						<span class="label">Description (optional)</span>
						<textarea
							rows="2"
							placeholder="One-line description"
							bind:value={description}
							maxlength="500"
						></textarea>
					</label>
					{#if parentSlug}
						<p class="hint">
							This share will be recorded as a fork of <code>/p/{parentSlug}</code>.
						</p>
					{/if}
					<p class="warning-line">
						<AlertTriangle size={14} />
						<span>Public and immutable. To change it later, fork.</span>
					</p>
					{#if publishError}
						<p class="error">Couldn't publish: {publishError}</p>
					{/if}
					<div class="actions">
						<button class="secondary" onclick={onClose}>Cancel</button>
						<button class="primary" onclick={publish} disabled={publishing || !draftsStore.activeCode}>
							{publishing ? 'Sharing…' : 'Share publicly'}
						</button>
					</div>
				{/if}
			</div>
		{:else}
			<div class="tab-body">
				<label class="field">
					<span class="label">Inline URL</span>
					<div class="url-row">
						<input class="url-input" type="text" readonly value={inline.url} />
						<button
							class="primary"
							onclick={() => copy(inline.url, 'Inline link')}
							disabled={!inline.url}
						>
							<Copy size={14} /> Copy
						</button>
					</div>
				</label>

				<p class="status" class:s-green={inline.status === 'green'} class:s-yellow={inline.status === 'yellow'} class:s-red={inline.status === 'red'}>
					<span class="dot"></span>
					{statusLabel()}
				</p>

				<p class="info-card">
					Source is packed into the URL hash. No server is contacted. Works
					offline, forever. Does not unfurl in chat apps — for OG previews,
					use Permalink.
				</p>
			</div>
		{/if}

		{#if copyToast}
			<div class="toast" role="status">{copyToast}</div>
		{/if}
	</div>
</div>

<style>
	.backdrop {
		position: fixed;
		inset: 0;
		display: flex;
		align-items: center;
		justify-content: center;
		background: rgba(0, 0, 0, 0.5);
		z-index: 1000;
		padding: var(--spacing-md);
	}

	.modal {
		width: 100%;
		max-width: 520px;
		background: var(--bg-secondary);
		border: 1px solid var(--border-default);
		border-radius: 8px;
		box-shadow: 0 20px 60px rgba(0, 0, 0, 0.4);
		display: flex;
		flex-direction: column;
		max-height: 90vh;
		overflow: hidden;
	}

	.modal-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		padding: 12px 16px;
		border-bottom: 1px solid var(--border-default);
	}

	.modal-header h2 {
		margin: 0;
		font-size: 14px;
		font-weight: 600;
		color: var(--text-primary);
	}

	.icon-button {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 28px;
		height: 28px;
		border: none;
		background: transparent;
		color: var(--text-secondary);
		border-radius: 4px;
		cursor: pointer;
	}

	.icon-button:hover {
		background: var(--bg-hover);
		color: var(--text-primary);
	}

	.tabs {
		display: flex;
		border-bottom: 1px solid var(--border-default);
		background: var(--bg-primary);
	}

	.tab {
		flex: 1;
		padding: 10px 12px;
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
		background: transparent;
		border: none;
		border-bottom: 2px solid transparent;
		cursor: pointer;
	}

	.tab.active {
		color: var(--text-primary);
		border-bottom-color: var(--accent-primary);
		background: var(--bg-secondary);
	}

	.tab.disabled {
		opacity: 0.5;
		cursor: not-allowed;
	}

	.tab-body {
		padding: 16px;
		overflow-y: auto;
	}

	.field {
		display: block;
		margin-bottom: 12px;
	}

	.label {
		display: block;
		margin-bottom: 4px;
		font-size: 11px;
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.04em;
		color: var(--text-secondary);
	}

	.field input[type="text"],
	.field textarea {
		width: 100%;
		padding: 6px 8px;
		font-size: 13px;
		font-family: var(--font-sans);
		color: var(--text-primary);
		background: var(--bg-primary);
		border: 1px solid var(--border-default);
		border-radius: 4px;
		box-sizing: border-box;
	}

	.field textarea {
		resize: vertical;
		min-height: 38px;
	}

	.url-row {
		display: flex;
		gap: 6px;
		align-items: center;
	}

	.url-input {
		flex: 1;
		font-family: var(--font-mono);
		font-size: 11px;
	}

	.primary, .secondary {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		padding: 6px 12px;
		font-size: 12px;
		font-weight: 500;
		border-radius: 4px;
		border: 1px solid transparent;
		cursor: pointer;
		text-decoration: none;
		white-space: nowrap;
	}

	.primary {
		color: var(--bg-primary);
		background: var(--accent-primary);
	}

	.primary:disabled { opacity: 0.5; cursor: not-allowed; }
	.primary:hover:not(:disabled) { opacity: 0.9; }

	.secondary {
		color: var(--text-primary);
		background: var(--bg-primary);
		border-color: var(--border-default);
	}

	.secondary:hover { background: var(--bg-hover); }

	.actions {
		display: flex;
		justify-content: flex-end;
		gap: 8px;
		margin-top: 12px;
	}

	.info-card {
		display: flex;
		align-items: flex-start;
		gap: 6px;
		padding: 8px 10px;
		font-size: 12px;
		color: var(--text-secondary);
		background: var(--bg-primary);
		border: 1px solid var(--border-default);
		border-radius: 4px;
		line-height: 1.4;
		margin: 0;
	}

	.info-card.warning { color: var(--accent-warning, #d4a017); }

	.warning-line {
		display: flex;
		align-items: center;
		gap: 6px;
		font-size: 12px;
		color: var(--accent-warning, #d4a017);
		margin: 4px 0 12px;
	}

	.error {
		font-size: 12px;
		color: var(--accent-danger, #c0392b);
		margin: 4px 0 12px;
	}

	.hint {
		font-size: 12px;
		color: var(--text-secondary);
		margin: 4px 0 8px;
		line-height: 1.4;
	}

	.hint.success { color: var(--text-primary); }

	.status {
		display: flex;
		align-items: center;
		gap: 6px;
		font-size: 12px;
		margin: 0 0 12px;
		color: var(--text-secondary);
	}

	.status .dot {
		width: 8px;
		height: 8px;
		border-radius: 50%;
		background: var(--text-secondary);
	}

	.status.s-green .dot { background: #2ecc71; }
	.status.s-yellow .dot { background: #f39c12; }
	.status.s-red .dot    { background: #e74c3c; }

	.toast {
		position: absolute;
		left: 50%;
		bottom: 16px;
		transform: translateX(-50%);
		padding: 6px 12px;
		background: var(--text-primary);
		color: var(--bg-primary);
		font-size: 12px;
		border-radius: 4px;
		box-shadow: 0 4px 16px rgba(0, 0, 0, 0.2);
		pointer-events: none;
	}

	code {
		font-family: var(--font-mono);
		font-size: 11px;
		background: var(--bg-primary);
		padding: 1px 4px;
		border-radius: 2px;
	}
</style>
