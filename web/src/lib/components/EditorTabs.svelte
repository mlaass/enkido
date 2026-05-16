<script lang="ts">
	import { Plus, X } from 'lucide-svelte';
	import { draftsStore as drafts } from '$stores/drafts.svelte';
	import type { DraftSummary } from '$lib/ide/storage/types';

	// Build the tab list from openTabIds → matched DraftSummary entries.
	let openTabs = $derived(
		drafts.openTabIds
			.map((id) => drafts.drafts.find((d) => d.id === id))
			.filter((d): d is DraftSummary => d !== undefined)
	);

	function onTabClick(id: string) {
		drafts.setActive(id);
	}

	function onCloseClick(e: MouseEvent, id: string) {
		e.stopPropagation();
		drafts.closeTab(id);
	}

	function onMiddleClick(e: MouseEvent, id: string) {
		// middle button = close
		if (e.button === 1) {
			e.preventDefault();
			drafts.closeTab(id);
		}
	}

	function onNewClick() {
		drafts.createDraft({});
	}

	function isPhantomDirty(draft: DraftSummary): boolean {
		// Phantoms are always shown with `●` since they are unsaved-to-share.
		// (A draft becomes phantom only via inline-URL visit; first edit
		// materializes it.)
		return draft.isPhantom;
	}
</script>

<div class="tab-bar" role="tablist">
	{#each openTabs as tab (tab.id)}
		<button
			class="tab"
			class:active={tab.id === drafts.activeTabId}
			class:phantom={tab.isPhantom}
			role="tab"
			aria-selected={tab.id === drafts.activeTabId}
			title={tab.name + (tab.isPhantom ? ' (unsaved phantom — Keep to save)' : '')}
			onclick={() => onTabClick(tab.id)}
			onmousedown={(e) => onMiddleClick(e, tab.id)}
		>
			<span class="label">{tab.name}</span>
			{#if isPhantomDirty(tab)}
				<span class="dirty-dot" aria-hidden="true">●</span>
			{/if}
			<span
				class="close"
				role="button"
				tabindex="-1"
				title="Close tab"
				aria-label="Close {tab.name}"
				onclick={(e) => onCloseClick(e, tab.id)}
				onkeydown={(e) => { if (e.key === 'Enter' || e.key === ' ') onCloseClick(e as any, tab.id); }}
			>
				<X size={12} strokeWidth={2.5} />
			</span>
		</button>
	{/each}
	<button class="new-tab" title="New draft (Ctrl+T)" onclick={onNewClick} aria-label="New draft">
		<Plus size={14} strokeWidth={2.5} />
	</button>
</div>

<style>
	.tab-bar {
		display: flex;
		align-items: stretch;
		overflow-x: auto;
		background-color: var(--bg-secondary);
		border-bottom: 1px solid var(--border-default);
		flex-shrink: 0;
		min-height: 32px;
		scrollbar-width: thin;
	}

	.tab-bar::-webkit-scrollbar {
		height: 4px;
	}
	.tab-bar::-webkit-scrollbar-thumb {
		background: var(--border-default);
		border-radius: 2px;
	}

	.tab {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		padding: 4px 6px 4px 12px;
		font-size: 12px;
		font-family: var(--font-mono);
		color: var(--text-secondary);
		background: transparent;
		border: none;
		border-right: 1px solid var(--border-muted);
		cursor: pointer;
		max-width: 220px;
		min-width: 90px;
		white-space: nowrap;
		transition: background-color var(--transition-fast), color var(--transition-fast);
	}

	.tab:hover {
		background-color: var(--bg-tertiary);
		color: var(--text-primary);
	}

	.tab.active {
		background-color: var(--bg-primary);
		color: var(--text-primary);
		border-bottom: 2px solid var(--accent-primary);
		padding-bottom: 2px;
	}

	.tab.phantom .label {
		font-style: italic;
	}

	.label {
		overflow: hidden;
		text-overflow: ellipsis;
		min-width: 0;
	}

	.dirty-dot {
		color: var(--accent-warning);
		font-size: 14px;
		line-height: 1;
	}

	.close {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 18px;
		height: 18px;
		border-radius: 3px;
		color: var(--text-muted);
		opacity: 0;
		transition: opacity var(--transition-fast), background-color var(--transition-fast), color var(--transition-fast);
	}

	.tab:hover .close,
	.tab.active .close {
		opacity: 1;
	}

	.close:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.new-tab {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 32px;
		color: var(--text-muted);
		background: transparent;
		border: none;
		border-right: 1px solid var(--border-muted);
		cursor: pointer;
		transition: background-color var(--transition-fast), color var(--transition-fast);
	}

	.new-tab:hover {
		background-color: var(--bg-tertiary);
		color: var(--text-primary);
	}
</style>
