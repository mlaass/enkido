<script lang="ts">
	import { audioEngine, type ParamDecl } from '$stores/audio.svelte';

	interface Props {
		param: ParamDecl;
	}

	let { param }: Props = $props();

	// Local state for immediate UI updates
	let selectedIndex = $state(Math.round(param.defaultValue));

	// Sync from store when param changes
	$effect(() => {
		const storeValue = audioEngine.paramValues.get(param.name);
		if (storeValue !== undefined) {
			selectedIndex = Math.round(storeValue);
		}
	});

	function handleChange(e: Event) {
		const target = e.target as HTMLSelectElement;
		const value = parseInt(target.value, 10);
		selectedIndex = value;
		audioEngine.setParamValue(param.name, value);
	}
</script>

<div class="param-select">
	<label class="param-label" for={`param-${param.name}`}>{param.name}</label>
	<select
		id={`param-${param.name}`}
		value={selectedIndex}
		onchange={handleChange}
	>
		{#each param.options as option, index}
			<option value={index}>{option}</option>
		{/each}
	</select>
</div>

<style>
	.param-select {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xs);
	}

	.param-label {
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
	}

	select {
		width: 100%;
		padding: var(--spacing-sm) var(--spacing-md);
		font-size: 13px;
		color: var(--text-primary);
		background-color: var(--bg-tertiary);
		border: 1px solid var(--border-default);
		border-radius: 6px;
		cursor: pointer;
		appearance: none;
		background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 24 24' fill='none' stroke='%23888' stroke-width='2'%3E%3Cpolyline points='6,9 12,15 18,9'/%3E%3C/svg%3E");
		background-repeat: no-repeat;
		background-position: right var(--spacing-sm) center;
		padding-right: calc(var(--spacing-md) + 16px);
	}

	select:hover {
		background-color: var(--bg-hover);
		border-color: var(--text-muted);
	}

	select:focus {
		outline: none;
		border-color: var(--accent-primary);
	}
</style>
