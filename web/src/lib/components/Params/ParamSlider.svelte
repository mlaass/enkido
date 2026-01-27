<script lang="ts">
	import { audioEngine, type ParamDecl } from '$stores/audio.svelte';

	interface Props {
		param: ParamDecl;
	}

	let { param }: Props = $props();

	// Local state for immediate UI updates
	let currentValue = $state(param.defaultValue);

	// Sync from store when param changes (e.g., on hot-swap)
	$effect(() => {
		const storeValue = audioEngine.paramValues.get(param.name);
		if (storeValue !== undefined) {
			currentValue = storeValue;
		}
	});

	// Step size: ~1000 steps across any range
	let stepSize = $derived((param.max - param.min) / 1000);

	// Format display value based on step size
	let displayValue = $derived(() => {
		if (stepSize >= 1) return Math.round(currentValue).toString();
		if (stepSize >= 0.1) return currentValue.toFixed(1);
		if (stepSize >= 0.01) return currentValue.toFixed(2);
		return currentValue.toFixed(3);
	});

	function handleInput(e: Event) {
		const target = e.target as HTMLInputElement;
		const value = parseFloat(target.value);
		currentValue = value;  // Update local state immediately
		audioEngine.setParamValue(param.name, value);
	}

	function handleDoubleClick() {
		currentValue = param.defaultValue;
		audioEngine.resetParam(param.name);
	}
</script>

<div class="param-slider">
	<div class="param-header">
		<span class="param-name">{param.name}</span>
		<span class="param-value">{displayValue()}</span>
	</div>
	<input
		type="range"
		min={param.min}
		max={param.max}
		step={stepSize}
		value={currentValue}
		oninput={handleInput}
		ondblclick={handleDoubleClick}
		title="Double-click to reset to default"
	/>
</div>

<style>
	.param-slider {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xs);
	}

	.param-header {
		display: flex;
		justify-content: space-between;
		align-items: baseline;
	}

	.param-name {
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
	}

	.param-value {
		font-family: var(--font-mono);
		font-size: 11px;
		color: var(--text-muted);
	}

	input[type="range"] {
		width: 100%;
		height: 6px;
		background: var(--bg-tertiary);
		border-radius: 3px;
		appearance: none;
		cursor: pointer;
	}

	input[type="range"]::-webkit-slider-thumb {
		appearance: none;
		width: 14px;
		height: 14px;
		background: var(--accent-primary);
		border-radius: 50%;
		cursor: pointer;
		transition: transform var(--transition-fast);
	}

	input[type="range"]::-webkit-slider-thumb:hover {
		transform: scale(1.1);
	}

	input[type="range"]::-moz-range-thumb {
		width: 14px;
		height: 14px;
		background: var(--accent-primary);
		border-radius: 50%;
		border: none;
		cursor: pointer;
	}
</style>
