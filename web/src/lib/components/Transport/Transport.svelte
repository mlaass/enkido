<script lang="ts">
	import { audioEngine } from '$stores/audio.svelte';
	import { editorStore } from '$stores/editor.svelte';

	let bpmInput = $state(audioEngine.bpm.toString());
	$effect(() => { bpmInput = audioEngine.bpm.toString(); });
	let volumePercent = $derived(Math.round(audioEngine.volume * 100));

	async function handlePlayPause() {
		if (audioEngine.isPlaying) {
			audioEngine.pause();
		} else {
			// Use the same evaluate function as Ctrl+Enter
			editorStore.evaluate();
		}
	}

	function handleBpmChange(e: Event) {
		const target = e.target as HTMLInputElement;
		const value = parseInt(target.value, 10);
		if (!isNaN(value)) {
			audioEngine.setBpm(value);
		}
	}

	function handleBpmBlur() {
		bpmInput = audioEngine.bpm.toString();
	}

	function handleVolumeChange(e: Event) {
		const target = e.target as HTMLInputElement;
		audioEngine.setVolume(parseFloat(target.value));
	}
</script>

<div class="transport">
	<div class="transport-controls">
		<button
			class="play-button"
			class:playing={audioEngine.isPlaying}
			onclick={handlePlayPause}
			title={audioEngine.isPlaying ? 'Pause (Space)' : 'Play (Space)'}
		>
			{#if audioEngine.isPlaying}
				<svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
					<rect x="6" y="4" width="4" height="16" />
					<rect x="14" y="4" width="4" height="16" />
				</svg>
			{:else}
				<svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
					<polygon points="5,3 19,12 5,21" />
				</svg>
			{/if}
		</button>

		<button class="stop-button" onclick={() => audioEngine.stop()} title="Stop">
			<svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
				<rect x="4" y="4" width="16" height="16" />
			</svg>
		</button>

		{#if audioEngine.isLoadingSamples}
			<div class="loading-indicator" title="Loading samples...">
				<svg class="spinner" width="16" height="16" viewBox="0 0 24 24" fill="none">
					<circle cx="12" cy="12" r="10" stroke="currentColor" stroke-width="2.5" stroke-opacity="0.25" />
					<path d="M12 2a10 10 0 0 1 10 10" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" />
				</svg>
			</div>
		{/if}
	</div>

	<div class="bpm-control">
		<label for="bpm">BPM</label>
		<input
			id="bpm"
			type="number"
			min="20"
			max="999"
			bind:value={bpmInput}
			onchange={handleBpmChange}
			onblur={handleBpmBlur}
		/>
	</div>

	<div class="volume-control">
		<label for="volume">Vol</label>
		<input
			id="volume"
			type="range"
			min="0"
			max="1"
			step="0.01"
			value={audioEngine.volume}
			oninput={handleVolumeChange}
		/>
		<span class="volume-value">{volumePercent}%</span>
	</div>
</div>

<style>
	.transport {
		display: flex;
		align-items: center;
		gap: var(--spacing-lg);
	}

	.transport-controls {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
	}

	.play-button, .stop-button {
		display: flex;
		align-items: center;
		justify-content: center;
		border-radius: 6px;
		transition: all var(--transition-fast);
	}

	.play-button {
		width: 36px;
		height: 36px;
		background-color: var(--accent-secondary);
		color: var(--bg-primary);
	}

	.play-button:hover {
		filter: brightness(1.1);
	}

	.play-button.playing {
		background-color: var(--accent-warning);
	}

	.stop-button {
		width: 32px;
		height: 32px;
		background-color: var(--bg-tertiary);
		color: var(--text-secondary);
	}

	.stop-button:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.loading-indicator {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 32px;
		height: 32px;
		color: var(--text-secondary);
	}

	.spinner {
		animation: spin 0.8s linear infinite;
	}

	@keyframes spin {
		from { transform: rotate(0deg); }
		to { transform: rotate(360deg); }
	}

	.bpm-control, .volume-control {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
	}

	.bpm-control label, .volume-control label {
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
		text-transform: uppercase;
		letter-spacing: 0.5px;
	}

	.bpm-control input {
		width: 60px;
		text-align: center;
		font-family: var(--font-mono);
		font-size: 14px;
	}

	.volume-control input[type="range"] {
		width: 80px;
		height: 4px;
		background: var(--bg-tertiary);
		border-radius: 2px;
		appearance: none;
		cursor: pointer;
	}

	.volume-control input[type="range"]::-webkit-slider-thumb {
		appearance: none;
		width: 12px;
		height: 12px;
		background: var(--text-primary);
		border-radius: 50%;
		cursor: pointer;
	}

	.volume-value {
		font-family: var(--font-mono);
		font-size: 12px;
		color: var(--text-secondary);
		width: 36px;
		text-align: right;
	}

	/* Phone: tighten transport — drop volume-control entirely (still
	   adjustable from the settings panel) and shrink BPM. Keeps play/stop
	   as primary tap targets at full size. */
	@media (max-width: 480px) {
		.transport {
			gap: var(--spacing-sm);
		}
		.volume-control {
			display: none;
		}
		.bpm-control label {
			display: none;
		}
		.bpm-control input {
			width: 48px;
		}
	}

	/* Touch input: bump play/stop hit targets above the 44px guideline. */
	@media (pointer: coarse) {
		.play-button {
			width: 44px;
			height: 44px;
		}
		.stop-button {
			width: 40px;
			height: 40px;
		}
	}
</style>
