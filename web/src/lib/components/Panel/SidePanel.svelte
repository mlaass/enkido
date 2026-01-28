<script lang="ts">
	import { onMount } from 'svelte';
	import DocsPanel from '$lib/docs/components/DocsPanel.svelte';
	import ResizeHandle from './ResizeHandle.svelte';
	import ThemeSelector from '$lib/components/Theme/ThemeSelector.svelte';
	import { ParamsPanel } from '$lib/components/Params';
	import DebugPanel from './DebugPanel.svelte';
	import { settingsStore } from '$lib/stores/settings.svelte';
	import { audioEngine } from '$lib/stores/audio.svelte';

	interface Props {
		position?: 'left' | 'right';
	}

	let { position = 'left' }: Props = $props();

	// Bind to settings store
	let collapsed = $derived(settingsStore.panelCollapsed);
	let activeTab = $derived(settingsStore.activeTab);
	let width = $derived(settingsStore.panelWidth);
	let showDebugTab = $derived(settingsStore.showDebugTab);

	// Local state for resize
	let currentWidth = $state(settingsStore.panelWidth);

	// Scroll container refs
	let controlsScrollEl: HTMLElement | null = $state(null);
	let settingsScrollEl: HTMLElement | null = $state(null);
	let docsScrollEl: HTMLElement | null = $state(null);
	let debugScrollEl: HTMLElement | null = $state(null);

	function getScrollEl(tab: string): HTMLElement | null {
		if (tab === 'controls') return controlsScrollEl;
		if (tab === 'settings') return settingsScrollEl;
		if (tab === 'docs') return docsScrollEl;
		if (tab === 'debug') return debugScrollEl;
		return null;
	}

	function saveScrollPosition(tab: string) {
		const el = getScrollEl(tab);
		if (el) {
			settingsStore.setScrollPosition(tab, el.scrollTop);
		}
	}

	function restoreScrollPosition(tab: string) {
		const el = getScrollEl(tab);
		const pos = settingsStore.scrollPositions[tab] || 0;
		if (el) {
			el.scrollTop = pos;
		}
	}

	function handleTabChange(tab: 'controls' | 'settings' | 'docs' | 'debug') {
		saveScrollPosition(activeTab);
		settingsStore.setActiveTab(tab);
		// Restore scroll after DOM updates
		setTimeout(() => restoreScrollPosition(tab), 0);
	}

	function handleResize(newWidth: number) {
		currentWidth = newWidth;
	}

	// Sync currentWidth with store
	$effect(() => {
		currentWidth = width;
	});

	// Listen for F1 help events from the editor
	onMount(() => {
		function handleF1Help() {
			settingsStore.setActiveTab('docs');
			settingsStore.setPanelCollapsed(false);
		}

		window.addEventListener('nkido:f1-help', handleF1Help);
		return () => window.removeEventListener('nkido:f1-help', handleF1Help);
	});
</script>

<aside
	class="panel"
	class:collapsed
	class:left={position === 'left'}
	class:right={position === 'right'}
	style:width={collapsed ? 'var(--panel-collapsed-width)' : `${currentWidth}px`}
>
	{#if !collapsed}
		<ResizeHandle {position} width={currentWidth} onResize={handleResize} />
	{/if}

	<div class="panel-toggle" class:left={position === 'left'} class:right={position === 'right'}>
		<button
			onclick={() => settingsStore.setPanelCollapsed(!collapsed)}
			title={collapsed ? 'Expand panel' : 'Collapse panel'}
		>
			<svg
				width="16"
				height="16"
				viewBox="0 0 24 24"
				fill="none"
				stroke="currentColor"
				stroke-width="2"
				style:transform={collapsed
					? (position === 'left' ? 'rotate(180deg)' : 'rotate(0deg)')
					: (position === 'left' ? 'rotate(0deg)' : 'rotate(180deg)')}
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
				onclick={() => handleTabChange('controls')}
			>
				Controls
			</button>
			<button
				class="tab"
				class:active={activeTab === 'settings'}
				onclick={() => handleTabChange('settings')}
			>
				Settings
			</button>
			<button
				class="tab"
				class:active={activeTab === 'docs'}
				onclick={() => handleTabChange('docs')}
			>
				Docs
			</button>
			{#if showDebugTab}
				<button
					class="tab"
					class:active={activeTab === 'debug'}
					onclick={() => handleTabChange('debug')}
				>
					Debug
				</button>
			{/if}
		</div>

		<div class="panel-content">
			{#if activeTab === 'controls'}
				<div class="tab-content" bind:this={controlsScrollEl}>
					<ParamsPanel />
				</div>
			{:else if activeTab === 'settings'}
				<div class="tab-content settings-content" bind:this={settingsScrollEl}>
					<!-- Appearance -->
					<div class="setting-group">
						<span class="setting-label">Theme</span>
						<ThemeSelector />
					</div>

					<div class="setting-group">
						<label class="setting-label" for="font-size">Font Size</label>
						<input
							type="number"
							id="font-size"
							min="10"
							max="24"
							value={settingsStore.fontSize}
							onchange={(e) => settingsStore.setFontSize(parseInt((e.target as HTMLInputElement).value))}
						/>
					</div>

					<!-- Layout -->
					<div class="setting-group">
						<span class="setting-label">Panel Position</span>
						<div class="segmented-control">
							<button
								class:active={settingsStore.panelPosition === 'left'}
								onclick={() => settingsStore.setPanelPosition('left')}
							>Left</button>
							<button
								class:active={settingsStore.panelPosition === 'right'}
								onclick={() => settingsStore.setPanelPosition('right')}
							>Right</button>
						</div>
					</div>

					<!-- Audio -->
					<div class="setting-group">
						<label class="setting-label" for="sample-rate">Sample Rate</label>
						<select
							id="sample-rate"
							value={settingsStore.sampleRate}
							onchange={(e) => settingsStore.setSampleRate(parseInt((e.target as HTMLSelectElement).value))}
						>
							<option value={44100}>44100 Hz</option>
							<option value={48000}>48000 Hz</option>
							<option value={88200}>88200 Hz</option>
							<option value={96000}>96000 Hz</option>
						</select>
						{#if audioEngine.activeSampleRate && audioEngine.activeSampleRate !== settingsStore.sampleRate}
							<div class="restart-notice">
								<span>Restart required to apply</span>
								<button class="restart-btn" onclick={() => audioEngine.restartAudio()}>
									Restart Audio
								</button>
							</div>
						{/if}
					</div>

					<!-- Developer -->
					<div class="setting-group">
						<label class="toggle-setting">
							<input
								type="checkbox"
								checked={settingsStore.showDebugTab}
								onchange={(e) => settingsStore.setShowDebugTab((e.target as HTMLInputElement).checked)}
							/>
							<span>Show Debug Tab</span>
						</label>
					</div>

					<button class="reset-button" onclick={() => settingsStore.reset()}>
						Reset to Defaults
					</button>
				</div>
			{:else if activeTab === 'docs'}
				<div bind:this={docsScrollEl}>
					<DocsPanel />
				</div>
			{:else if activeTab === 'debug'}
				<div class="tab-content" bind:this={debugScrollEl}>
					<DebugPanel />
				</div>
			{/if}
		</div>
	{/if}
</aside>

<style>
	.panel {
		position: relative;
		display: flex;
		flex-direction: column;
		background-color: var(--bg-secondary);
		border-color: var(--border-default);
		flex-shrink: 0;
	}

	.panel.left {
		border-right: 1px solid var(--border-default);
	}

	.panel.right {
		border-left: 1px solid var(--border-default);
	}

	.panel-toggle {
		display: flex;
		padding: var(--spacing-sm);
		border-bottom: 1px solid var(--border-muted);
	}

	.panel-toggle.left {
		justify-content: flex-start;
	}

	.panel-toggle.right {
		justify-content: flex-end;
	}

	.panel-toggle button {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 24px;
		height: 24px;
		border-radius: 4px;
		border: 1px solid var(--border-default);
		color: var(--text-secondary);
		transition: all var(--transition-fast);
	}

	.panel-toggle button:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
		border-color: var(--text-muted);
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
	}

	.tab-content {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
	}

	/* Settings styles */
	.settings-content {
		gap: var(--spacing-md);
	}

	.setting-group {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xs);
	}

	.setting-label {
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
	}

	.segmented-control {
		display: flex;
		background-color: var(--bg-tertiary);
		border-radius: 6px;
		padding: 2px;
		gap: 2px;
	}

	.segmented-control button {
		flex: 1;
		padding: var(--spacing-xs) var(--spacing-sm);
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
		border-radius: 4px;
		transition: all var(--transition-fast);
	}

	.segmented-control button:hover {
		color: var(--text-primary);
	}

	.segmented-control button.active {
		background-color: var(--bg-secondary);
		color: var(--accent-primary);
		box-shadow: 0 1px 2px rgba(0, 0, 0, 0.2);
	}

	.setting-group input[type="number"],
	.setting-group select {
		padding: var(--spacing-xs) var(--spacing-sm);
		font-size: 13px;
	}

	.toggle-setting {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
		cursor: pointer;
		font-size: 13px;
		color: var(--text-secondary);
	}

	.toggle-setting:hover {
		color: var(--text-primary);
	}

	.toggle-setting input[type="checkbox"] {
		width: 16px;
		height: 16px;
		margin: 0;
		cursor: pointer;
	}

	.reset-button {
		margin-top: var(--spacing-md);
		padding: var(--spacing-sm) var(--spacing-md);
		font-size: 13px;
		font-weight: 500;
		color: var(--text-secondary);
		background-color: var(--bg-tertiary);
		border-radius: 6px;
		transition: all var(--transition-fast);
	}

	.reset-button:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.restart-notice {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
		padding: var(--spacing-xs) var(--spacing-sm);
		background-color: var(--bg-tertiary);
		border-radius: 4px;
		font-size: 11px;
		color: var(--text-secondary);
	}

	.restart-notice span {
		flex: 1;
	}

	.restart-btn {
		padding: 2px 8px;
		font-size: 11px;
		font-weight: 500;
		color: var(--accent-primary);
		background-color: transparent;
		border: 1px solid var(--accent-primary);
		border-radius: 4px;
		cursor: pointer;
		transition: all var(--transition-fast);
	}

	.restart-btn:hover {
		background-color: var(--accent-primary);
		color: var(--bg-primary);
	}
</style>
