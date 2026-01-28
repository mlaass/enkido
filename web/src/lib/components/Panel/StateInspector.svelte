<script lang="ts">
	import { audioEngine } from '$lib/stores/audio.svelte';
	import type { StateInspection } from '$lib/stores/audio.svelte';

	interface Props {
		stateId: number | null;
		onClose: () => void;
	}

	let { stateId, onClose }: Props = $props();

	let stateData = $state<StateInspection | null>(null);
	let pollInterval: ReturnType<typeof setInterval> | null = null;

	// Start/stop polling based on stateId and playback
	$effect(() => {
		if (stateId !== null && audioEngine.isPlaying) {
			// Start polling at ~20Hz
			pollInterval = setInterval(async () => {
				if (stateId !== null) {
					stateData = await audioEngine.inspectState(stateId);
				}
			}, 50);

			// Initial fetch
			audioEngine.inspectState(stateId).then((data) => {
				stateData = data;
			});
		} else {
			// Stop polling
			if (pollInterval) {
				clearInterval(pollInterval);
				pollInterval = null;
			}
			// Do one final fetch if we have a stateId but not playing
			if (stateId !== null && !audioEngine.isPlaying) {
				audioEngine.inspectState(stateId).then((data) => {
					stateData = data;
				});
			}
		}

		return () => {
			if (pollInterval) {
				clearInterval(pollInterval);
				pollInterval = null;
			}
		};
	});

	function formatValue(value: unknown): string {
		if (typeof value === 'number') {
			// Format with appropriate precision
			if (Number.isInteger(value)) {
				return value.toString();
			}
			// Show 4 decimal places for floats
			return value.toFixed(4);
		}
		if (typeof value === 'boolean') {
			return value ? 'true' : 'false';
		}
		if (Array.isArray(value)) {
			return '[' + value.map(formatValue).join(', ') + ']';
		}
		return String(value);
	}

	function formatStateId(id: number): string {
		return `0x${id.toString(16).toUpperCase().padStart(8, '0')}`;
	}

	// Get fields to display (exclude 'type')
	function getFields(data: StateInspection): Array<[string, unknown]> {
		return Object.entries(data).filter(([key]) => key !== 'type');
	}
</script>

{#if stateId !== null}
	<div class="state-inspector">
		<div class="inspector-header">
			<div class="inspector-title">
				<span class="state-type">{stateData?.type ?? 'Loading...'}</span>
				<span class="state-id">{formatStateId(stateId)}</span>
			</div>
			<button class="close-button" onclick={onClose} title="Close inspector">
				<svg width="12" height="12" viewBox="0 0 12 12" fill="currentColor">
					<path d="M6 4.586L10.293.293a1 1 0 011.414 1.414L7.414 6l4.293 4.293a1 1 0 01-1.414 1.414L6 7.414l-4.293 4.293a1 1 0 01-1.414-1.414L4.586 6 .293 1.707A1 1 0 011.707.293L6 4.586z"/>
				</svg>
			</button>
		</div>

		{#if stateData}
			<div class="inspector-fields">
				{#each getFields(stateData) as [key, value] (key)}
					<div class="field-row">
						<span class="field-name">{key}</span>
						<span class="field-value">{formatValue(value)}</span>
					</div>
				{/each}
			</div>
			{#if !audioEngine.isPlaying}
				<div class="paused-hint">Play to see live updates</div>
			{/if}
		{:else}
			<div class="no-data">State not found</div>
		{/if}
	</div>
{/if}

<style>
	.state-inspector {
		background: var(--bg-tertiary);
		border: 1px solid var(--border-default);
		border-radius: 6px;
		padding: var(--spacing-sm);
		margin-top: var(--spacing-sm);
	}

	.inspector-header {
		display: flex;
		justify-content: space-between;
		align-items: center;
		margin-bottom: var(--spacing-sm);
		padding-bottom: var(--spacing-xs);
		border-bottom: 1px solid var(--border-muted);
	}

	.inspector-title {
		display: flex;
		flex-direction: column;
		gap: 2px;
	}

	.state-type {
		font-weight: 600;
		color: var(--syntax-function);
		font-size: 12px;
	}

	.state-id {
		font-family: var(--font-mono);
		font-size: 10px;
		color: var(--text-muted);
	}

	.close-button {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 20px;
		height: 20px;
		border-radius: 4px;
		color: var(--text-muted);
		transition: background-color var(--transition-fast), color var(--transition-fast);
	}

	.close-button:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.inspector-fields {
		display: flex;
		flex-direction: column;
		gap: 4px;
	}

	.field-row {
		display: flex;
		justify-content: space-between;
		align-items: center;
		padding: 2px 4px;
		border-radius: 3px;
		font-size: 11px;
	}

	.field-row:hover {
		background: var(--bg-hover);
	}

	.field-name {
		color: var(--text-secondary);
		font-family: var(--font-mono);
	}

	.field-value {
		color: var(--syntax-number);
		font-family: var(--font-mono);
		text-align: right;
	}

	.paused-hint {
		margin-top: var(--spacing-sm);
		font-size: 10px;
		color: var(--text-muted);
		text-align: center;
		font-style: italic;
	}

	.no-data {
		color: var(--text-muted);
		font-size: 11px;
		text-align: center;
		padding: var(--spacing-sm);
	}
</style>
