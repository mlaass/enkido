<script lang="ts">
	import Transport from '$components/Transport/Transport.svelte';
	import Editor from '$components/Editor/Editor.svelte';
	import SidePanel from '$components/Panel/SidePanel.svelte';
	import { audioEngine } from '$stores/audio.svelte';

	let panelPosition: 'left' | 'right' = $state('left');
	let panelCollapsed = $state(false);
</script>

<div class="app">
	<header class="header">
		<div class="header-left">
			<span class="logo">enkido</span>
		</div>
		<Transport />
		<div class="header-right">
			<button
				class="icon-button"
				title="Toggle visualizations"
				onclick={() => audioEngine.toggleVisualizations()}
			>
				<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
					<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z" />
					<circle cx="12" cy="12" r="3" />
				</svg>
			</button>
			<button class="icon-button" title="Settings">
				<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
					<circle cx="12" cy="12" r="3" />
					<path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z" />
				</svg>
			</button>
		</div>
	</header>

	<main class="main" class:panel-left={panelPosition === 'left'} class:panel-right={panelPosition === 'right'}>
		{#if panelPosition === 'left'}
			<SidePanel bind:collapsed={panelCollapsed} position="left" />
		{/if}

		<div class="editor-container">
			<Editor />
		</div>

		{#if panelPosition === 'right'}
			<SidePanel bind:collapsed={panelCollapsed} position="right" />
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
