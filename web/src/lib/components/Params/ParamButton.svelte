<script lang="ts">
	import { audioEngine, type ParamDecl } from '$stores/audio.svelte';

	interface Props {
		param: ParamDecl;
	}

	let { param }: Props = $props();

	// Local state for immediate UI updates
	let isPressed = $state(false);

	function handleMouseDown() {
		isPressed = true;
		audioEngine.pressButton(param.name);
	}

	function handleMouseUp() {
		isPressed = false;
		audioEngine.releaseButton(param.name);
	}

	function handleMouseLeave() {
		if (isPressed) {
			isPressed = false;
			audioEngine.releaseButton(param.name);
		}
	}
</script>

<button
	class="param-button"
	class:pressed={isPressed}
	onmousedown={handleMouseDown}
	onmouseup={handleMouseUp}
	onmouseleave={handleMouseLeave}
	ontouchstart={handleMouseDown}
	ontouchend={handleMouseUp}
>
	{param.name}
</button>

<style>
	.param-button {
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
		user-select: none;
	}

	.param-button:hover {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}

	.param-button.pressed {
		background-color: var(--accent-primary);
		color: var(--bg-primary);
		border-color: var(--accent-primary);
		transform: scale(0.98);
	}
</style>
