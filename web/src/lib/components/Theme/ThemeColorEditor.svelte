<script lang="ts">
	import type { ThemeColors } from '$lib/types/theme';
	import { COLOR_GROUPS, COLOR_LABELS } from '$lib/types/theme';
	import ColorInput from './ColorInput.svelte';

	interface Props {
		colors: ThemeColors;
		onchange: (key: keyof ThemeColors, value: string) => void;
		onSave: () => void;
		onReset: () => void;
		onDelete?: () => void;
		canDelete?: boolean;
		hasChanges?: boolean;
	}

	let {
		colors,
		onchange,
		onSave,
		onReset,
		onDelete,
		canDelete = false,
		hasChanges = false
	}: Props = $props();
</script>

<div class="color-editor">
	{#each COLOR_GROUPS as group}
		<div class="color-group">
			<div class="group-header">{group.label}</div>
			<div class="color-list">
				{#each group.keys as key}
					<ColorInput
						label={COLOR_LABELS[key]}
						value={colors[key]}
						onchange={(value) => onchange(key, value)}
					/>
				{/each}
			</div>
		</div>
	{/each}

	<div class="actions">
		<button
			class="action-button save"
			onclick={onSave}
			disabled={!hasChanges}
		>
			Save as New
		</button>
		<button class="action-button reset" onclick={onReset}>
			Reset
		</button>
		{#if canDelete && onDelete}
			<button class="action-button delete" onclick={onDelete}>
				Delete
			</button>
		{/if}
	</div>
</div>

<style>
	.color-editor {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-md);
	}

	.color-group {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xs);
	}

	.group-header {
		font-size: 11px;
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.5px;
		color: var(--text-muted);
		padding-bottom: var(--spacing-xs);
		border-bottom: 1px solid var(--border-muted);
	}

	.color-list {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xs);
		padding-left: var(--spacing-xs);
	}

	.actions {
		display: flex;
		gap: var(--spacing-sm);
		padding-top: var(--spacing-sm);
		border-top: 1px solid var(--border-muted);
	}

	.action-button {
		flex: 1;
		padding: var(--spacing-xs) var(--spacing-sm);
		font-size: 12px;
		font-weight: 500;
		border-radius: 4px;
		transition: all var(--transition-fast);
	}

	.action-button.save {
		background-color: var(--accent-primary);
		color: var(--bg-primary);
	}

	.action-button.save:hover:not(:disabled) {
		filter: brightness(1.1);
	}

	.action-button.save:disabled {
		opacity: 0.5;
		cursor: not-allowed;
	}

	.action-button.reset {
		background-color: var(--bg-tertiary);
		color: var(--text-secondary);
	}

	.action-button.reset:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.action-button.delete {
		background-color: transparent;
		color: var(--accent-error);
		border: 1px solid var(--accent-error);
	}

	.action-button.delete:hover {
		background-color: var(--accent-error);
		color: var(--bg-primary);
	}
</style>
