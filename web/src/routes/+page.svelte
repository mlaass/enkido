<script lang="ts">
	import Transport from '$components/Transport/Transport.svelte';
	import Editor from '$components/Editor/Editor.svelte';
	import EditorTabs from '$components/EditorTabs.svelte';
	import SidePanel from '$components/Panel/SidePanel.svelte';
	import Logo from '$components/Logo/Logo.svelte';
	import ShareDialog from '$components/ShareDialog.svelte';
	import { audioEngine } from '$stores/audio.svelte';
	import { settingsStore } from '$stores/settings.svelte';
	import { draftsStore } from '$stores/drafts.svelte';
	import { getProvider } from '$lib/ide/storage';
	import { Eye, Settings, BookmarkPlus, Share2, GitFork } from 'lucide-svelte';

	let panelPosition = $derived(settingsStore.panelPosition);
	let activeIsPhantom = $derived(
		draftsStore.drafts.find((d) => d.id === draftsStore.activeTabId)?.isPhantom ?? false
	);
	const canShare = typeof getProvider().share === 'function';
	let shareOpen = $state(false);

	function openSettings() {
		settingsStore.setPanelCollapsed(false);
		settingsStore.setActiveTab('settings');
	}

	function onKeep() {
		const active = draftsStore.drafts.find((d) => d.id === draftsStore.activeTabId);
		if (!active) return;
		const suggested = active.name.startsWith('From inline link') ? 'Inline patch' : `Fork of ${active.name}`;
		const name = window.prompt('Name this patch:', suggested);
		if (!name || !name.trim()) return;
		draftsStore.keepActiveAsNamed(name.trim());
	}

	function openShare() {
		shareOpen = true;
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
</style>
