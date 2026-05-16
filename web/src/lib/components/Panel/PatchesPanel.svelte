<script lang="ts">
	import { Plus, Trash2, Pencil, Save, X, Share2 } from 'lucide-svelte';
	import { draftsStore as drafts } from '$stores/drafts.svelte';
	import { getProvider } from '$lib/ide/storage';
	import type { DraftSummary, RecentlyVisited } from '$lib/ide/storage/types';

	let renamingId = $state<string | null>(null);
	let renameValue = $state('');
	let loadingSlug = $state<string | null>(null);
	let visitError = $state<string | null>(null);

	let myDrafts = $derived(
		drafts.drafts
			.slice()
			.filter((d) => !d.isPhantom)
			.sort((a, b) => b.updatedAt - a.updatedAt)
	);

	let recentlyVisited = $derived(drafts.recentlyVisited);

	function startRename(d: DraftSummary) {
		renamingId = d.id;
		renameValue = d.name;
	}

	function commitRename() {
		if (renamingId && renameValue.trim().length > 0) {
			drafts.renameDraft(renamingId, renameValue.trim());
		}
		renamingId = null;
	}

	function cancelRename() { renamingId = null; }

	function onRenameKey(e: KeyboardEvent) {
		if (e.key === 'Enter') {
			e.preventDefault();
			commitRename();
		} else if (e.key === 'Escape') {
			e.preventDefault();
			cancelRename();
		}
	}

	function onDelete(d: DraftSummary, e: MouseEvent) {
		e.stopPropagation();
		const ok = window.confirm(`Delete "${d.name}"? This cannot be undone.`);
		if (ok) drafts.deleteDraft(d.id);
	}

	function onOpen(d: DraftSummary) { drafts.openTab(d.id); }

	function onNew() { drafts.createDraft({}); }

	async function onOpenVisited(entry: RecentlyVisited) {
		// If a local slug-phantom already exists, just focus it — no refetch.
		const existing = drafts.findSlugPhantom(entry.slug);
		if (existing) {
			drafts.openTab(existing.id);
			return;
		}
		const provider = getProvider();
		if (typeof provider.fetchShare !== 'function') {
			visitError = 'Sharing is not configured on this build.';
			return;
		}
		loadingSlug = entry.slug;
		visitError = null;
		try {
			const share = await provider.fetchShare(entry.slug);
			if (!share) {
				visitError = `Patch ${entry.slug} no longer exists. Removing from recent.`;
				await drafts.forgetVisited(entry.slug);
				return;
			}
			drafts.openSlugPhantomTab({ slug: share.slug, code: share.code, title: share.title });
			drafts.recordVisited(share.slug, share.title).catch(() => {});
		} catch (e) {
			visitError = `Couldn't load /p/${entry.slug}: ${(e as Error).message}`;
		} finally {
			loadingSlug = null;
		}
	}

	function onForget(entry: RecentlyVisited, e: MouseEvent) {
		e.stopPropagation();
		const ok = window.confirm(`Forget /p/${entry.slug}? Any local edits will be deleted.`);
		if (!ok) return;
		drafts.deleteSlugPhantom(entry.slug);
		drafts.forgetVisited(entry.slug);
	}

	function relativeTime(ms: number): string {
		const diff = Date.now() - ms;
		if (diff < 60_000) return 'just now';
		if (diff < 3_600_000) return Math.floor(diff / 60_000) + 'm ago';
		if (diff < 86_400_000) return Math.floor(diff / 3_600_000) + 'h ago';
		return Math.floor(diff / 86_400_000) + 'd ago';
	}

	function hasLocalEdits(slug: string): boolean {
		return drafts.findSlugPhantom(slug) !== null;
	}
</script>

<div class="patches-panel">
	<section class="section">
		<header class="section-header">
			<span class="section-title">My drafts</span>
			<button class="icon-btn" title="New draft (Ctrl+T)" aria-label="New draft" onclick={onNew}>
				<Plus size={14} strokeWidth={2.5} />
			</button>
		</header>

		{#if myDrafts.length === 0}
			<div class="empty">No drafts yet.</div>
		{:else}
			<ul class="list">
				{#each myDrafts as d (d.id)}
					<li class="row" class:active={d.id === drafts.activeTabId}>
						{#if renamingId === d.id}
							<input
								class="rename-input"
								type="text"
								bind:value={renameValue}
								onblur={commitRename}
								onkeydown={onRenameKey}
							/>
							<button class="icon-btn" title="Save name" aria-label="Save name" onclick={commitRename}>
								<Save size={12} strokeWidth={2.5} />
							</button>
							<button class="icon-btn" title="Cancel" aria-label="Cancel rename" onclick={cancelRename}>
								<X size={12} strokeWidth={2.5} />
							</button>
						{:else}
							<button
								class="row-main"
								title={d.name + ' — last edit ' + relativeTime(d.updatedAt)}
								onclick={() => onOpen(d)}
							>
								<span class="name">{d.name}</span>
								<span class="time">{relativeTime(d.updatedAt)}</span>
							</button>
							<button
								class="icon-btn row-action"
								title="Rename"
								aria-label="Rename {d.name}"
								onclick={(e) => { e.stopPropagation(); startRename(d); }}
							>
								<Pencil size={12} strokeWidth={2.5} />
							</button>
							<button
								class="icon-btn row-action"
								title="Delete"
								aria-label="Delete {d.name}"
								onclick={(e) => onDelete(d, e)}
							>
								<Trash2 size={12} strokeWidth={2.5} />
							</button>
						{/if}
					</li>
				{/each}
			</ul>
		{/if}
	</section>

	<section class="section">
		<header class="section-header">
			<span class="section-title">Recently visited</span>
		</header>
		{#if recentlyVisited.length === 0}
			<div class="empty muted">
				Slug shares you visit will appear here.
			</div>
		{:else}
			<ul class="list">
				{#each recentlyVisited as entry (entry.slug)}
					<li class="row" class:active={drafts.activeTabId === 'slug:' + entry.slug}>
						<button
							class="row-main"
							title={`/p/${entry.slug} — visited ${relativeTime(entry.visitedAt)}`}
							disabled={loadingSlug === entry.slug}
							onclick={() => onOpenVisited(entry)}
						>
							{#if hasLocalEdits(entry.slug)}<span class="dirty-dot" title="Local edits">●</span>{/if}
							<Share2 size={11} strokeWidth={2} class="row-icon" />
							<span class="name">
								{entry.title?.trim() || `/p/${entry.slug}`}
							</span>
							<span class="time">{loadingSlug === entry.slug ? '…' : relativeTime(entry.visitedAt)}</span>
						</button>
						<button
							class="icon-btn row-action"
							title="Forget"
							aria-label="Forget /p/{entry.slug}"
							onclick={(e) => onForget(entry, e)}
						>
							<X size={12} strokeWidth={2.5} />
						</button>
					</li>
				{/each}
			</ul>
		{/if}
		{#if visitError}
			<div class="error">{visitError}</div>
		{/if}
	</section>
</div>

<style>
	.patches-panel {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-md);
		padding: var(--spacing-sm);
	}

	.section {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xs);
	}

	.section-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		padding: 0 var(--spacing-xs);
	}

	.section-title {
		font-size: 11px;
		font-weight: 600;
		color: var(--text-muted);
		text-transform: uppercase;
		letter-spacing: 0.5px;
	}

	.icon-btn {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 22px;
		height: 22px;
		border-radius: 4px;
		color: var(--text-muted);
		background: transparent;
		border: none;
		cursor: pointer;
		transition: background-color var(--transition-fast), color var(--transition-fast);
	}

	.icon-btn:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.list {
		list-style: none;
		padding: 0;
		margin: 0;
		display: flex;
		flex-direction: column;
		gap: 1px;
	}

	.row {
		display: flex;
		align-items: center;
		gap: 2px;
		padding: 2px 2px 2px 4px;
		border-radius: 4px;
		transition: background-color var(--transition-fast);
	}

	.row:hover { background-color: var(--bg-tertiary); }

	.row.active {
		background-color: var(--bg-tertiary);
		box-shadow: inset 2px 0 0 var(--accent-primary);
	}

	.row-main {
		flex: 1;
		display: flex;
		align-items: center;
		gap: 6px;
		padding: 4px 6px;
		min-width: 0;
		background: transparent;
		border: none;
		color: var(--text-primary);
		font-family: var(--font-mono);
		font-size: 12px;
		text-align: left;
		cursor: pointer;
	}

	.row-main:disabled { opacity: 0.6; cursor: progress; }

	.dirty-dot {
		color: var(--accent-warning);
		font-size: 12px;
		line-height: 1;
		flex-shrink: 0;
	}

	:global(.row-icon) {
		color: var(--text-muted);
		flex-shrink: 0;
	}

	.name {
		flex: 1;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
		min-width: 0;
	}

	.time {
		color: var(--text-muted);
		font-size: 11px;
		flex-shrink: 0;
	}

	.row-action { opacity: 0; }

	.row:hover .row-action,
	.row.active .row-action { opacity: 1; }

	.rename-input {
		flex: 1;
		padding: 2px 6px;
		background-color: var(--bg-primary);
		border: 1px solid var(--accent-primary);
		border-radius: 3px;
		color: var(--text-primary);
		font-family: var(--font-mono);
		font-size: 12px;
	}

	.empty {
		padding: var(--spacing-sm) var(--spacing-md);
		font-size: 12px;
		color: var(--text-muted);
		font-style: italic;
	}

	.empty.muted { opacity: 0.7; }

	.error {
		padding: var(--spacing-xs) var(--spacing-md);
		font-size: 11px;
		color: var(--accent-danger, #c0392b);
	}
</style>
