<script lang="ts">
	import { audioEngine } from '$stores/audio.svelte';

	let bpmInput = $state(audioEngine.bpm.toString());
	let volumePercent = $derived(Math.round(audioEngine.volume * 100));

	function handlePlayPause() {
		if (audioEngine.isPlaying) {
			audioEngine.pause();
		} else {
			audioEngine.play();
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
</style>
