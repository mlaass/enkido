<script lang="ts">
	import Transport from '$components/Transport/Transport.svelte';
	import Editor from '$components/Editor/Editor.svelte';
	import SidePanel from '$components/Panel/SidePanel.svelte';
	import Logo from '$components/Logo/Logo.svelte';
	import { audioEngine } from '$stores/audio.svelte';
	import { settingsStore } from '$stores/settings.svelte';
	import { Eye, Settings } from 'lucide-svelte';

	let panelPosition = $derived(settingsStore.panelPosition);

	function openSettings() {
		settingsStore.setPanelCollapsed(false);
		settingsStore.setActiveTab('settings');
	}
</script>

<div class="app">
	<header class="header">
		<div class="header-left">
			<span class="logo"><Logo size={24} /></span>
		</div>
		<Transport />
		<div class="header-right">
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

	<main class="main" class:panel-left={panelPosition === 'left'} class:panel-right={panelPosition === 'right'}>
		{#if panelPosition === 'left'}
			<SidePanel position="left" />
		{/if}

		<div class="editor-container">
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
