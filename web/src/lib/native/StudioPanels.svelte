<script lang="ts">
	/**
	 * Studio panel host (prd-studio-daw-core). Mounted only in the native bundle
	 * by `initNativeHost()`, and only once the native bridge answers — the same
	 * shared IDE served to the browser never renders these.
	 *
	 * Layout: a docked strip along the bottom of the window holding the transport
	 * bar (with the always-on capture indicator), the bus mixer, and the takes
	 * browser. The render dialog is modal.
	 */
	import { studio } from './studio-bridge.svelte';
	import { editorStore } from '$lib/stores/editor.svelte';
	import { themeStore } from '$lib/stores/theme.svelte';
	import TransportBar from './TransportBar.svelte';
	import MixerPanel from './MixerPanel.svelte';
	import CapturesBrowser from './CapturesBrowser.svelte';
	import RenderDialog from './RenderDialog.svelte';

	let renderOpen = $state(false);
	let collapsed = $state(false);

	// Native chrome (menu bar, window background) follows the web theme — runs
	// once on mount and again on every theme change.
	$effect(() => {
		void studio.pushTheme({ ...themeStore.activeTheme.colors });
	});
</script>

{#if studio.available}
	<div class="dock" class:collapsed>
		<button class="handle" onclick={() => (collapsed = !collapsed)} aria-expanded={!collapsed}>
			{collapsed ? '▲ studio' : '▼ studio'}
		</button>

		{#if !collapsed}
			<div class="rows">
				<TransportBar />
				<div class="lower">
					<MixerPanel />
					<div class="side">
						<button class="render" onclick={() => (renderOpen = true)}>Render…</button>
						<CapturesBrowser />
					</div>
				</div>
			</div>
		{/if}
	</div>

	<RenderDialog open={renderOpen} source={editorStore.code} onclose={() => (renderOpen = false)} />
{/if}

<style>
	.dock {
		position: fixed;
		left: 0;
		right: 0;
		bottom: 0;
		z-index: 30;
		display: flex;
		flex-direction: column;
		background: var(--bg-secondary);
		border-top: 1px solid var(--border-default);
		box-shadow: 0 -4px 18px rgba(0, 0, 0, 0.35);
	}
	.handle {
		align-self: flex-start;
		margin: 0.15rem 0.4rem;
		padding: 0.1rem 0.5rem;
		font-size: 0.68rem;
		letter-spacing: 0.06em;
		text-transform: uppercase;
		background: transparent;
		border: none;
		color: var(--text-muted);
		cursor: pointer;
	}
	.rows {
		display: flex;
		flex-direction: column;
		gap: 0.5rem;
		padding: 0 0.5rem 0.5rem;
	}
	.lower {
		display: flex;
		gap: 0.5rem;
		align-items: stretch;
	}
	.lower :global(.mixer) {
		flex: 1 1 auto;
		min-width: 0;
	}
	.side {
		display: flex;
		flex-direction: column;
		gap: 0.4rem;
		width: 230px;
	}
	.render {
		padding: 0.3rem;
		border-radius: 4px;
		border: 1px solid var(--border-default);
		background: var(--bg-tertiary);
		color: var(--text-primary);
		cursor: pointer;
		font-size: 0.78rem;
	}
</style>
