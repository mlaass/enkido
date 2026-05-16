<script lang="ts">
	import { audioEngine, type SoundFontInfo } from '$lib/stores/audio.svelte';

	let soundfonts = $derived(audioEngine.loadedSoundfonts);
	let sfUrlInput = $state('');
	let loading = $state(false);
	let expandedSf = $state<number | null>(null);

	function toggleExpand(sfId: number) {
		expandedSf = expandedSf === sfId ? null : sfId;
	}

	async function handleSfUrlLoad() {
		const url = sfUrlInput.trim();
		if (!url) return;

		loading = true;
		try {
			// Extract filename from URL for display name
			const name = url.split('/').pop() || url;
			await audioEngine.loadAsset(url, 'soundfont', name);
			sfUrlInput = '';
		} catch (e) {
			console.error('Failed to load SoundFont:', e);
		} finally {
			loading = false;
		}
	}

	// Drop / drag handlers moved to FilesPanel (PRD prd-midi-input §7.6) —
	// SampleBrowser is now a list/details view only.
</script>

<div class="sample-browser">
	<!-- SoundFonts section -->
	<div class="section">
		<h3 class="section-title">SoundFonts</h3>

		{#if soundfonts.length === 0}
			<div class="empty-state">
				<p>No SoundFonts loaded</p>
				<p class="hint">Drop .sf2/.sf3 files in the Files tab or enter a URL below</p>
			</div>
		{:else}
			<div class="sf-list">
				{#each soundfonts as sf (sf.sfId)}
					<div class="sf-card">
						<button class="sf-header" onclick={() => toggleExpand(sf.sfId)}>
							<span class="sf-name">{sf.name}</span>
							<span class="sf-meta">{sf.presetCount} presets</span>
							<span class="expand-icon" class:expanded={expandedSf === sf.sfId}>
								<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
									<polyline points="6,9 12,15 18,9" />
								</svg>
							</span>
						</button>
						{#if expandedSf === sf.sfId}
							<div class="sf-presets">
								{#each sf.presets as preset, i}
									<div class="preset-row">
										<span class="preset-index">{i}</span>
										<span class="preset-name">{preset.name}</span>
										<span class="preset-bank">{preset.bank}:{preset.program}</span>
									</div>
								{/each}
							</div>
						{/if}
					</div>
				{/each}
			</div>
		{/if}

		<!-- URL loader -->
		<div class="url-loader">
			<input
				type="text"
				placeholder="SF2 URL..."
				bind:value={sfUrlInput}
				onkeydown={(e) => e.key === 'Enter' && handleSfUrlLoad()}
				disabled={loading}
			/>
			<button onclick={handleSfUrlLoad} disabled={loading || !sfUrlInput.trim()}>
				{loading ? '...' : 'Load'}
			</button>
		</div>
	</div>

	<!-- Usage hint -->
	<div class="usage-hint">
		<p>Use in code:</p>
		<code>pat("c4 e4 g4") |> soundfont(%, "gm", 0)</code>
	</div>
</div>

<style>
	.sample-browser {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-md);
		padding: var(--spacing-sm);
	}

	.section-title {
		font-size: 11px;
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.05em;
		color: var(--text-secondary);
		margin: 0 0 var(--spacing-xs) 0;
	}

	.empty-state {
		padding: var(--spacing-md);
		text-align: center;
		color: var(--text-secondary);
		font-size: 12px;
	}

	.empty-state .hint {
		font-size: 11px;
		color: var(--text-muted);
		margin-top: var(--spacing-xs);
	}

	.sf-list {
		display: flex;
		flex-direction: column;
		gap: 2px;
	}

	.sf-card {
		background-color: var(--bg-tertiary);
		border-radius: 4px;
		overflow: hidden;
	}

	.sf-header {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		width: 100%;
		padding: var(--spacing-xs) var(--spacing-sm);
		font-size: 12px;
		color: var(--text-primary);
		text-align: left;
		transition: background-color var(--transition-fast);
	}

	.sf-header:hover {
		background-color: var(--bg-hover);
	}

	.sf-name {
		flex: 1;
		font-weight: 500;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.sf-meta {
		font-size: 11px;
		color: var(--text-muted);
		flex-shrink: 0;
	}

	.expand-icon {
		transition: transform var(--transition-fast);
		color: var(--text-muted);
	}

	.expand-icon.expanded {
		transform: rotate(180deg);
	}

	.sf-presets {
		border-top: 1px solid var(--border-muted);
		max-height: 200px;
		overflow-y: auto;
	}

	.preset-row {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		padding: 2px var(--spacing-sm);
		font-size: 11px;
		color: var(--text-secondary);
	}

	.preset-row:hover {
		background-color: var(--bg-hover);
	}

	.preset-index {
		width: 24px;
		text-align: right;
		color: var(--text-muted);
		font-variant-numeric: tabular-nums;
	}

	.preset-name {
		flex: 1;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}

	.preset-bank {
		color: var(--text-muted);
		font-variant-numeric: tabular-nums;
	}

	.url-loader {
		display: flex;
		gap: 4px;
	}

	.url-loader input {
		flex: 1;
		padding: 4px 8px;
		font-size: 12px;
		min-width: 0;
	}

	.url-loader button {
		padding: 4px 10px;
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
		background-color: var(--bg-tertiary);
		border-radius: 4px;
		transition: all var(--transition-fast);
		flex-shrink: 0;
	}

	.url-loader button:hover:not(:disabled) {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.url-loader button:disabled {
		opacity: 0.5;
	}

	.usage-hint {
		padding: var(--spacing-sm);
		background-color: var(--bg-tertiary);
		border-radius: 4px;
		font-size: 11px;
		color: var(--text-muted);
	}

	.usage-hint p {
		margin: 0 0 4px 0;
	}

	.usage-hint code {
		display: block;
		font-size: 11px;
		color: var(--text-secondary);
		word-break: break-all;
	}
</style>
