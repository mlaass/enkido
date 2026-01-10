<script lang="ts">
	import { settingsStore } from '$lib/stores/settings.svelte';

	interface Props {
		position: 'left' | 'right';
		width: number;
		onResize: (width: number) => void;
	}

	let { position, width, onResize }: Props = $props();

	const MIN_WIDTH = 180;

	let isDragging = $state(false);
	let startX = 0;
	let startWidth = 0;

	function getMaxWidth() {
		if (typeof window === 'undefined') return 600;
		return window.innerWidth - 200;
	}

	function onPointerDown(e: PointerEvent) {
		isDragging = true;
		startX = e.clientX;
		startWidth = width;
		(e.target as HTMLElement).setPointerCapture(e.pointerId);
		document.body.style.cursor = 'col-resize';
		document.body.style.userSelect = 'none';
	}

	function onPointerMove(e: PointerEvent) {
		if (!isDragging) return;

		const delta = position === 'left'
			? e.clientX - startX
			: startX - e.clientX;

		const maxWidth = getMaxWidth();
		const newWidth = Math.max(MIN_WIDTH, Math.min(maxWidth, startWidth + delta));
		onResize(newWidth);
	}

	function onPointerUp() {
		if (!isDragging) return;
		isDragging = false;
		document.body.style.cursor = '';
		document.body.style.userSelect = '';
		settingsStore.setPanelWidth(width);
	}
</script>

<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
<div
	class="resize-handle"
	class:left={position === 'left'}
	class:right={position === 'right'}
	class:dragging={isDragging}
	role="separator"
	aria-orientation="vertical"
	aria-valuenow={width}
	aria-valuemin={MIN_WIDTH}
	tabindex="0"
	onpointerdown={onPointerDown}
	onpointermove={onPointerMove}
	onpointerup={onPointerUp}
	onpointercancel={onPointerUp}
></div>

<style>
	.resize-handle {
		position: absolute;
		top: 0;
		bottom: 0;
		width: 4px;
		cursor: col-resize;
		background-color: transparent;
		transition: background-color var(--transition-fast);
		z-index: 10;
	}

	.resize-handle.left {
		right: -2px;
	}

	.resize-handle.right {
		left: -2px;
	}

	.resize-handle:hover,
	.resize-handle.dragging {
		background-color: var(--accent-primary);
	}
</style>
