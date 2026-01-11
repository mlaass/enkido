<script lang="ts">
	import type { Theme, ThemeColors } from '$lib/types/theme';
	import { themeStore } from '$lib/stores/theme.svelte';
	import ThemeColorEditor from './ThemeColorEditor.svelte';

	let showEditor = $state(false);
	let editingColors = $state<ThemeColors>({ ...themeStore.activeTheme.colors });
	let hasChanges = $state(false);

	// Track the original colors when editing starts
	let originalColors = $state<ThemeColors>({ ...themeStore.activeTheme.colors });

	function handleThemeChange(e: Event) {
		const select = e.target as HTMLSelectElement;
		themeStore.setActiveTheme(select.value);
		// Reset editing state when theme changes
		editingColors = { ...themeStore.activeTheme.colors };
		originalColors = { ...themeStore.activeTheme.colors };
		hasChanges = false;
	}

	function handleFollowSystemChange(e: Event) {
		const checkbox = e.target as HTMLInputElement;
		themeStore.setFollowSystem(checkbox.checked);
	}

	function handleColorChange(key: keyof ThemeColors, value: string) {
		editingColors = { ...editingColors, [key]: value };
		hasChanges = true;
		// Apply live preview
		themeStore.applyColors(editingColors);
	}

	function handleSave() {
		const baseName = themeStore.activeTheme.name;
		const timestamp = Date.now();
		const newTheme: Theme = {
			id: `custom-${timestamp}`,
			name: `${baseName} (Custom)`,
			isBuiltin: false,
			isDark: themeStore.activeTheme.isDark,
			colors: { ...editingColors }
		};
		themeStore.saveCustomTheme(newTheme);
		themeStore.setActiveTheme(newTheme.id);
		originalColors = { ...editingColors };
		hasChanges = false;
	}

	function handleReset() {
		editingColors = { ...originalColors };
		hasChanges = false;
		themeStore.applyColors(originalColors);
	}

	function handleDelete() {
		if (themeStore.activeTheme.isBuiltin) return;
		themeStore.deleteCustomTheme(themeStore.activeThemeId);
		editingColors = { ...themeStore.activeTheme.colors };
		originalColors = { ...themeStore.activeTheme.colors };
		hasChanges = false;
	}

	function toggleEditor() {
		showEditor = !showEditor;
		if (showEditor) {
			editingColors = { ...themeStore.activeTheme.colors };
			originalColors = { ...themeStore.activeTheme.colors };
			hasChanges = false;
		} else if (hasChanges) {
			// Revert changes when closing without saving
			themeStore.applyColors(originalColors);
			hasChanges = false;
		}
	}

	// Sync editing colors when theme changes externally
	$effect(() => {
		if (!showEditor) {
			editingColors = { ...themeStore.activeTheme.colors };
			originalColors = { ...themeStore.activeTheme.colors };
		}
	});
</script>

<div class="theme-selector">
	<div class="theme-row">
		<select
			class="theme-dropdown"
			value={themeStore.activeThemeId}
			onchange={handleThemeChange}
			disabled={themeStore.followSystem}
		>
			<optgroup label="Presets">
				{#each themeStore.presetThemes as theme}
					<option value={theme.id}>{theme.name}</option>
				{/each}
			</optgroup>
			{#if themeStore.customThemes.length > 0}
				<optgroup label="Custom">
					{#each themeStore.customThemes as theme}
						<option value={theme.id}>{theme.name}</option>
					{/each}
				</optgroup>
			{/if}
		</select>
		<button
			class="customize-button"
			class:active={showEditor}
			onclick={toggleEditor}
			title={showEditor ? 'Hide color editor' : 'Customize colors'}
		>
			<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
				<circle cx="12" cy="12" r="3" />
				<path d="M12 1v6m0 6v6m11-7h-6m-6 0H1m15.36-5.36l-4.24 4.24m-4.24 0L2.64 6.64m0 10.72l4.24-4.24m4.24 0l4.24 4.24" />
			</svg>
		</button>
	</div>

	<label class="follow-system">
		<input
			type="checkbox"
			checked={themeStore.followSystem}
			onchange={handleFollowSystemChange}
		/>
		<span>Follow system preference</span>
	</label>

	{#if showEditor}
		<div class="editor-container">
			<ThemeColorEditor
				colors={editingColors}
				onchange={handleColorChange}
				onSave={handleSave}
				onReset={handleReset}
				onDelete={handleDelete}
				canDelete={!themeStore.activeTheme.isBuiltin}
				{hasChanges}
			/>
		</div>
	{/if}
</div>

<style>
	.theme-selector {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
	}

	.theme-row {
		display: flex;
		gap: var(--spacing-xs);
	}

	.theme-dropdown {
		flex: 1;
		padding: var(--spacing-xs) var(--spacing-sm);
		font-size: 13px;
		background-color: var(--bg-tertiary);
		border: 1px solid var(--border-default);
		border-radius: 4px;
		color: var(--text-primary);
		cursor: pointer;
	}

	.theme-dropdown:disabled {
		opacity: 0.6;
		cursor: not-allowed;
	}

	.theme-dropdown:focus {
		border-color: var(--accent-primary);
		outline: none;
	}

	.customize-button {
		display: flex;
		align-items: center;
		justify-content: center;
		width: 32px;
		height: 32px;
		background-color: var(--bg-tertiary);
		border: 1px solid var(--border-default);
		border-radius: 4px;
		color: var(--text-secondary);
		transition: all var(--transition-fast);
	}

	.customize-button:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.customize-button.active {
		background-color: var(--accent-primary);
		border-color: var(--accent-primary);
		color: var(--bg-primary);
	}

	.follow-system {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		font-size: 12px;
		color: var(--text-secondary);
		cursor: pointer;
	}

	.follow-system input {
		width: 14px;
		height: 14px;
		accent-color: var(--accent-primary);
	}

	.follow-system:hover span {
		color: var(--text-primary);
	}

	.editor-container {
		margin-top: var(--spacing-xs);
		padding: var(--spacing-sm);
		background-color: var(--bg-tertiary);
		border-radius: 6px;
		border: 1px solid var(--border-muted);
	}
</style>
