<script lang="ts">
	import { Plus, Trash2, Pencil, Save, X } from 'lucide-svelte';
	import { draftsStore as drafts } from '$stores/drafts.svelte';
	import type { DraftSummary } from '$lib/ide/storage/types';

	let renamingId = $state<string | null>(null);
	let renameValue = $state('');

	let myDrafts = $derived(
		drafts.drafts
			.slice()
			.sort((a, b) => b.updatedAt - a.updatedAt)
	);

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

	function cancelRename() {
		renamingId = null;
	}

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
		// guard: confirm destructive action
		const label = d.isPhantom ? `phantom "${d.name}"` : `"${d.name}"`;
		const ok = window.confirm(`Delete ${label}? This cannot be undone.`);
		if (ok) drafts.deleteDraft(d.id);
	}

	function onOpen(d: DraftSummary) {
		drafts.openTab(d.id);
	}

	function onNew() {
		drafts.createDraft({});
	}

	function relativeTime(ms: number): string {
		const diff = Date.now() - ms;
		if (diff < 60_000) return 'just now';
		if (diff < 3_600_000) return Math.floor(diff / 60_000) + 'm ago';
		if (diff < 86_400_000) return Math.floor(diff / 3_600_000) + 'h ago';
		return Math.floor(diff / 86_400_000) + 'd ago';
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
					<li
						class="row"
						class:active={d.id === drafts.activeTabId}
						class:phantom={d.isPhantom}
					>
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
								title={(d.isPhantom ? 'Phantom: ' : '') + d.name + ' — last edit ' + relativeTime(d.updatedAt)}
								onclick={() => onOpen(d)}
							>
								{#if d.isPhantom}<span class="dirty-dot">●</span>{/if}
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
		<div class="empty muted">
			Worker shares (coming in Phase 2) will appear here.
		</div>
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

	.row:hover {
		background-color: var(--bg-tertiary);
	}

	.row.active {
		background-color: var(--bg-tertiary);
		box-shadow: inset 2px 0 0 var(--accent-primary);
	}

	.row.phantom .name {
		font-style: italic;
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

	.dirty-dot {
		color: var(--accent-warning);
		font-size: 12px;
		line-height: 1;
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

	.row-action {
		opacity: 0;
	}

	.row:hover .row-action,
	.row.active .row-action {
		opacity: 1;
	}

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

	.empty.muted {
		opacity: 0.7;
	}
</style>
