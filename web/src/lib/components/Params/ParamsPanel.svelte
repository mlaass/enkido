<script lang="ts">
	import { audioEngine, ParamType } from '$stores/audio.svelte';
	import ParamSlider from './ParamSlider.svelte';
	import ParamButton from './ParamButton.svelte';
	import ParamToggle from './ParamToggle.svelte';
	import ParamSelect from './ParamSelect.svelte';

	let params = $derived(audioEngine.params);
</script>

<div class="params-panel">
	{#if params.length === 0}
		<div class="empty-state">
			<p class="placeholder">No parameters declared</p>
			<p class="hint">Use param(), button(), toggle(), or dropdown() in your code to add controls here</p>
		</div>
	{:else}
		<div class="params-list">
			{#each params as param (param.name)}
				{#if param.type === ParamType.Continuous}
					<ParamSlider {param} />
				{:else if param.type === ParamType.Button}
					<ParamButton {param} />
				{:else if param.type === ParamType.Toggle}
					<ParamToggle {param} />
				{:else if param.type === ParamType.Select}
					<ParamSelect {param} />
				{/if}
			{/each}
		</div>
	{/if}
</div>

<style>
	.params-panel {
		padding: var(--spacing-md);
	}

	.empty-state {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xs);
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

	.params-list {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-md);
	}
</style>
