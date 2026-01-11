<script lang="ts">
	interface Props {
		label: string;
		value: string;
		onchange: (color: string) => void;
	}

	let { label, value, onchange }: Props = $props();

	function handleTextChange(e: Event) {
		const input = e.target as HTMLInputElement;
		let color = input.value.trim();
		// Add # if missing
		if (color && !color.startsWith('#')) {
			color = '#' + color;
		}
		// Validate hex color
		if (/^#[0-9A-Fa-f]{6}$/.test(color)) {
			onchange(color);
		}
	}

	function handleColorChange(e: Event) {
		const input = e.target as HTMLInputElement;
		onchange(input.value);
	}
</script>

<div class="color-input">
	<span class="label">{label}</span>
	<div class="inputs">
		<input
			type="text"
			class="hex-input"
			{value}
			onchange={handleTextChange}
			maxlength="7"
			placeholder="#000000"
		/>
		<input
			type="color"
			class="color-picker"
			{value}
			oninput={handleColorChange}
		/>
	</div>
</div>

<style>
	.color-input {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: var(--spacing-sm);
	}

	.label {
		font-size: 12px;
		color: var(--text-secondary);
		flex-shrink: 0;
		min-width: 70px;
	}

	.inputs {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
	}

	.hex-input {
		width: 72px;
		padding: 4px 6px;
		font-size: 11px;
		font-family: var(--font-mono);
		text-transform: lowercase;
		background-color: var(--bg-tertiary);
		border: 1px solid var(--border-default);
		border-radius: 4px;
		color: var(--text-primary);
	}

	.hex-input:focus {
		border-color: var(--accent-primary);
		outline: none;
	}

	.color-picker {
		width: 24px;
		height: 24px;
		padding: 0;
		border: 1px solid var(--border-default);
		border-radius: 4px;
		cursor: pointer;
		background: none;
	}

	.color-picker::-webkit-color-swatch-wrapper {
		padding: 2px;
	}

	.color-picker::-webkit-color-swatch {
		border: none;
		border-radius: 2px;
	}

	.color-picker::-moz-color-swatch {
		border: none;
		border-radius: 2px;
	}
</style>
