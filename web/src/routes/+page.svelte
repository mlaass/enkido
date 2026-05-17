<script lang="ts">
	import Transport from '$components/Transport/Transport.svelte';
	import Editor from '$components/Editor/Editor.svelte';
	import EditorTabs from '$components/EditorTabs.svelte';
	import SidePanel from '$components/Panel/SidePanel.svelte';
	import Logo from '$components/Logo/Logo.svelte';
	import ShareDialog from '$components/ShareDialog.svelte';
	import ReportDialog from '$components/ReportDialog.svelte';
	import { audioEngine } from '$stores/audio.svelte';
	import { settingsStore } from '$stores/settings.svelte';
	import { draftsStore } from '$stores/drafts.svelte';
	import { getProvider } from '$lib/ide/storage';
	import { hasReported } from '$lib/ide/share/reported-slugs';
	import { Eye, Settings, BookmarkPlus, Share2, GitFork, Flag } from 'lucide-svelte';

	let panelPosition = $derived(settingsStore.panelPosition);
	const provider = getProvider();
	let activeDraft = $derived(draftsStore.activeDraft);
	let activeIsPhantom = $derived(activeDraft?.isPhantom ?? false);
	// Only slug-phantoms are reportable. Inline phantoms (phantomSlug === null)
	// have no server identity to flag — PRD §3.2 non-goal 6.
	let activePhantomSlug = $derived(
		activeIsPhantom ? (activeDraft?.phantomSlug ?? null) : null
	);
	const canShare = typeof provider.share === 'function';
	const canReport = typeof provider.reportShare === 'function';
	let canReportActive = $derived(canReport && activePhantomSlug !== null);

	let shareOpen = $state(false);
	let reportOpen = $state(false);

	// Header toast — sibling of the modal stack so it can announce results
	// after a dialog dismisses (e.g. "Reported.", "Already reported").
	let headerToast = $state<string | null>(null);
	let headerToastTimer: ReturnType<typeof setTimeout> | null = null;
	function showHeaderToast(msg: string) {
		headerToast = msg;
		if (headerToastTimer) clearTimeout(headerToastTimer);
		headerToastTimer = setTimeout(() => (headerToast = null), 1800);
	}

	function openSettings() {
		settingsStore.setPanelCollapsed(false);
		settingsStore.setActiveTab('settings');
	}

	function onKeep() {
		const active = draftsStore.activeDraft;
		if (!active) return;
		const suggested = active.name.startsWith('From inline link') ? 'Inline patch' : `Fork of ${active.name}`;
		const name = window.prompt('Name this patch:', suggested);
		if (!name || !name.trim()) return;
		draftsStore.keepActiveAsNamed(name.trim());
	}

	function openShare() {
		shareOpen = true;
	}

	function onReportClick() {
		const slug = activePhantomSlug;
		if (!slug) return;
		if (hasReported(slug)) {
			showHeaderToast('Already reported');
			return;
		}
		reportOpen = true;
	}

	function onReported() {
		reportOpen = false;
		showHeaderToast('Reported.');
	}
</script>

<div class="app">
	<header class="header">
		<div class="header-left">
			<span class="logo"><Logo size={24} /></span>
		</div>
		<Transport />
		<div class="header-right">
			{#if activeIsPhantom}
				<button
					class="keep-button"
					title="Save this patch to your local drafts"
					onclick={onKeep}
				>
					<BookmarkPlus size={16} />
					<span>Keep</span>
				</button>
				{#if canShare}
					<button
						class="share-button"
						title="Publish your edits as a new public patch"
						onclick={openShare}
					>
						<GitFork size={16} />
						<span>Fork</span>
					</button>
				{/if}
				{#if canReportActive}
					<button
						class="icon-button"
						title="Report this patch to operators"
						aria-label="Report patch"
						onclick={onReportClick}
					>
						<Flag size={18} />
					</button>
				{/if}
			{:else}
				<button
					class="share-button"
					title="Share this patch"
					onclick={openShare}
				>
					<Share2 size={16} />
					<span>Share</span>
				</button>
			{/if}
			<button
				class="icon-button"
				title="Toggle visualizations"
				onclick={() => audioEngine.toggleVisualizations()}
			>
				<Eye size={20} />
			</button>
			<button class="icon-button" title="Settings" onclick={openSettings}>
				<Settings size={20} />
			</button>
		</div>
	</header>

	{#if shareOpen}
		<ShareDialog onClose={() => (shareOpen = false)} />
	{/if}

	{#if reportOpen && activePhantomSlug}
		<ReportDialog
			slug={activePhantomSlug}
			onClose={() => (reportOpen = false)}
			onReported={onReported}
		/>
	{/if}

	{#if headerToast}
		<div class="header-toast" role="status">{headerToast}</div>
	{/if}

	<main class="main" class:panel-left={panelPosition === 'left'} class:panel-right={panelPosition === 'right'}>
		{#if panelPosition === 'left'}
			<SidePanel position="left" />
		{/if}

		<div class="editor-container">
			<EditorTabs />
			<Editor />
		</div>

		{#if panelPosition === 'right'}
			<SidePanel position="right" />
		{/if}
	</main>
</div>

<style>
	.app {
		display: flex;
		flex-direction: column;
		height: 100vh;
		overflow: hidden;
	}

	.header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		height: var(--header-height);
		padding: 0 var(--spacing-md);
		background-color: var(--bg-secondary);
		border-bottom: 1px solid var(--border-default);
		flex-shrink: 0;
	}

	.header-left, .header-right {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
	}

	.logo {
		font-family: var(--font-mono);
		font-size: 18px;
		font-weight: 600;
		color: var(--accent-primary);
		letter-spacing: -0.5px;
	}

	.icon-button {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 32px;
		height: 32px;
		border-radius: 6px;
		color: var(--text-secondary);
		transition: all var(--transition-fast);
	}

	.icon-button:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.keep-button {
		display: inline-flex;
		align-items: center;
		gap: 6px;
		padding: 4px 10px;
		font-size: 12px;
		font-weight: 500;
		color: var(--bg-primary);
		background-color: var(--accent-warning);
		border: none;
		border-radius: 6px;
		cursor: pointer;
		transition: opacity var(--transition-fast);
	}

	.keep-button:hover {
		opacity: 0.85;
	}

	.share-button {
		display: inline-flex;
		align-items: center;
		gap: 6px;
		padding: 4px 10px;
		font-size: 12px;
		font-weight: 500;
		color: var(--text-primary);
		background-color: var(--bg-primary);
		border: 1px solid var(--border-default);
		border-radius: 6px;
		cursor: pointer;
		transition: background-color var(--transition-fast);
	}

	.share-button:hover {
		background-color: var(--bg-hover);
	}

	.main {
		display: flex;
		flex: 1;
		overflow: hidden;
	}

	.editor-container {
		flex: 1;
		overflow: hidden;
		display: flex;
		flex-direction: column;
	}

	.header-toast {
		position: fixed;
		left: 50%;
		bottom: 24px;
		transform: translateX(-50%);
		padding: 6px 12px;
		background: var(--text-primary);
		color: var(--bg-primary);
		font-size: 12px;
		border-radius: 4px;
		box-shadow: 0 4px 16px rgba(0, 0, 0, 0.2);
		z-index: 1100;
		pointer-events: none;
	}

	/* Narrow viewports (tablet / phone): collapse the side panel into a
	   bottom strip so the editor keeps full width. Overrides the
	   user's saved panel-position silently. */
	@media (max-width: 768px) {
		.main {
			flex-direction: column;
		}
		/* Panel goes below the editor regardless of left/right setting. */
		.main :global(.panel.left),
		.main :global(.panel.right) {
			order: 2;
		}
		.editor-container {
			order: 1;
			min-height: 0;
		}
	}

	/* Phone: pull header padding in and hide non-essential icon-button
	   labels so transport stays usable. */
	@media (max-width: 480px) {
		.header {
			padding: 0 var(--spacing-sm);
			gap: var(--spacing-xs);
		}
		.header-left :global(.logo svg) {
			width: 20px;
			height: 20px;
		}
		.keep-button span,
		.share-button span {
			display: none;
		}
		.keep-button,
		.share-button {
			padding: 6px 8px;
		}
	}
</style>
