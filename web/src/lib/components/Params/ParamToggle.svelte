<script lang="ts">
	import { audioEngine, type ParamDecl } from '$stores/audio.svelte';

	interface Props {
		param: ParamDecl;
	}

	let { param }: Props = $props();

	// Local state for immediate UI updates
	let isOn = $state(param.defaultValue > 0.5);

	// Sync from store when param changes
	$effect(() => {
		const storeValue = audioEngine.paramValues.get(param.name);
		if (storeValue !== undefined) {
			isOn = storeValue > 0.5;
		}
	});

	function handleClick() {
		isOn = !isOn;
		audioEngine.toggleParam(param.name);
	}
</script>

<button
	class="param-toggle"
	class:on={isOn}
	onclick={handleClick}
	role="switch"
	aria-checked={isOn}
>
	<span class="toggle-indicator"></span>
	<span class="toggle-label">{param.name}</span>
</button>

<style>
	.param-toggle {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
		width: 100%;
		padding: var(--spacing-sm) var(--spacing-md);
		font-size: 13px;
		font-weight: 500;
		color: var(--text-secondary);
		background-color: var(--bg-tertiary);
		border: 1px solid var(--border-default);
		border-radius: 6px;
		cursor: pointer;
		transition: all var(--transition-fast);
	}

	.param-toggle:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.toggle-indicator {
		width: 10px;
		height: 10px;
		border-radius: 50%;
		background-color: var(--text-muted);
		transition: all var(--transition-fast);
		flex-shrink: 0;
	}

	.param-toggle.on .toggle-indicator {
		background-color: var(--accent-secondary);
		box-shadow: 0 0 8px var(--accent-secondary);
	}

	.toggle-label {
		flex: 1;
		text-align: left;
	}
</style>
