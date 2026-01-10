<script lang="ts">
	import DocsPanel from '$lib/docs/components/DocsPanel.svelte';

	interface Props {
		collapsed?: boolean;
		position?: 'left' | 'right';
	}

	let { collapsed = $bindable(false), position = 'left' }: Props = $props();

	let activeTab: 'controls' | 'settings' | 'docs' = $state('controls');
</script>

<aside class="panel" class:collapsed class:left={position === 'left'} class:right={position === 'right'}>
	<div class="panel-toggle">
		<button onclick={() => collapsed = !collapsed} title={collapsed ? 'Expand panel' : 'Collapse panel'}>
			<svg
				width="16"
				height="16"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="2"
				style:transform={collapsed
					? (position === 'left' ? 'rotate(0deg)' : 'rotate(180deg)')
					: (position === 'left' ? 'rotate(180deg)' : 'rotate(0deg)')}
			>
				<polyline points="15,18 9,12 15,6" />
			</svg>
		</button>
	</div>

	{#if !collapsed}
		<div class="panel-tabs">
			<button
				class="tab"
				class:active={activeTab === 'controls'}
				onclick={() => activeTab = 'controls'}
			>
				Controls
			</button>
			<button
				class="tab"
				class:active={activeTab === 'settings'}
				onclick={() => activeTab = 'settings'}
			>
				Settings
			</button>
			<button
				class="tab"
				class:active={activeTab === 'docs'}
				onclick={() => activeTab = 'docs'}
			>
				Docs
			</button>
		</div>

		<div class="panel-content">
			{#if activeTab === 'controls'}
				<div class="tab-content">
					<p class="placeholder">Control panel coming soon...</p>
					<p class="hint">Add knobs and faders that bind to Akkado variables</p>
				</div>
			{:else if activeTab === 'settings'}
				<div class="tab-content">
					<p class="placeholder">Settings coming soon...</p>
					<p class="hint">Theme, audio output, buffer size</p>
				</div>
			{:else if activeTab === 'docs'}
				<DocsPanel />
			{/if}
		</div>
	{/if}
</aside>

<style>
	.panel {
		display: flex;
		flex-direction: column;
		width: var(--panel-width);
		background-color: var(--bg-secondary);
		border-color: var(--border-default);
		transition: width var(--transition-normal);
		flex-shrink: 0;
	}

	.panel.left {
		border-right: 1px solid var(--border-default);
	}

	.panel.right {
		border-left: 1px solid var(--border-default);
	}

	.panel.collapsed {
		width: var(--panel-collapsed-width);
	}

	.panel-toggle {
		display: flex;
		justify-content: center;
		padding: var(--spacing-sm);
		border-bottom: 1px solid var(--border-muted);
	}

	.panel-toggle button {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 28px;
		height: 28px;
		border-radius: 4px;
		color: var(--text-secondary);
		transition: all var(--transition-fast);
	}

	.panel-toggle button:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.panel-toggle button svg {
		transition: transform var(--transition-fast);
	}

	.panel-tabs {
		display: flex;
		border-bottom: 1px solid var(--border-muted);
	}

	.tab {
		flex: 1;
		padding: var(--spacing-sm) var(--spacing-xs);
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
		border-bottom: 2px solid transparent;
		transition: all var(--transition-fast);
	}

	.tab:hover {
		color: var(--text-primary);
		background-color: var(--bg-tertiary);
	}

	.tab.active {
		color: var(--accent-primary);
		border-bottom-color: var(--accent-primary);
	}

	.panel-content {
		flex: 1;
		overflow-y: auto;
		padding: var(--spacing-md);
	}

	.tab-content {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
	}

	.placeholder {
		color: var(--text-secondary);
		font-size: 13px;
		margin: 0;
	}

	.hint {
		color: var(--text-muted);
		font-size: 12px;
		margin: 0;
	}
</style>
