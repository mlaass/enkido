<script lang="ts">
	/**
	 * Offline render dialog (prd-studio-daw-core §4.5/§5.5, G5).
	 *
	 * Renders on a FRESH offline engine (never the live one), faster than
	 * realtime, deterministic with the live path. Duration in cycles or seconds;
	 * master-only or master + per-bus stems, written to `renders/<take>/` with
	 * the same bus-keyed layout as captures. Cancel leaves no partial files —
	 * the native side writes only after a completed render.
	 *
	 * Programs that read live externals (`in()`, live `midi()`) render those as
	 * silence/absence; the note below states that up front.
	 */
	import { studio } from './studio-bridge.svelte';

	interface Props {
		open: boolean;
		source: string;
		onclose: () => void;
	}
	let { open, source, onclose }: Props = $props();

	let unit = $state<'seconds' | 'cycles'>('seconds');
	let amount = $state(30);
	let sampleRate = $state(48000);
	let stems = $state(true);

	async function run() {
		const seconds = unit === 'seconds' ? amount : 0;
		const cycles = unit === 'cycles' ? amount : 0;
		const ok = await studio.render(source, seconds, cycles, sampleRate, stems);
		if (ok) onclose();
	}
</script>

<svelte:window
	onkeydown={(e) => {
		if (open && e.key === 'Escape' && !studio.rendering) onclose();
	}}
/>

{#if open}
	<div class="layer">
		<!-- A real button, so dismissing by clicking outside is keyboard-reachable. -->
		<button class="scrim" aria-label="Close render dialog" onclick={onclose} disabled={studio.rendering}
		></button>
		<div class="dialog" role="dialog" aria-modal="true" aria-label="Render" tabindex="-1">
			<h2>Render</h2>

			<label>
				Duration
				<input type="number" min="1" step="1" bind:value={amount} disabled={studio.rendering} />
				<select bind:value={unit} disabled={studio.rendering}>
					<option value="seconds">seconds</option>
					<option value="cycles">cycles</option>
				</select>
			</label>

			<label>
				Sample rate
				<select bind:value={sampleRate} disabled={studio.rendering}>
					<option value={44100}>44100</option>
					<option value={48000}>48000</option>
					<option value={96000}>96000</option>
				</select>
			</label>

			<label class="check">
				<input type="checkbox" bind:checked={stems} disabled={studio.rendering} />
				Master + per-bus stems
			</label>

			<p class="note">
				32-bit float WAV into <code>renders/</code>. Live inputs (<code>in()</code>,
				live <code>midi()</code>) render as silence.
			</p>

			<div class="actions">
				<button onclick={onclose} disabled={studio.rendering}>Cancel</button>
				<button class="primary" onclick={run} disabled={studio.rendering}>
					{studio.rendering ? 'Rendering…' : 'Render'}
				</button>
			</div>
		</div>
	</div>
{/if}

<style>
	.layer {
		position: fixed;
		inset: 0;
		display: grid;
		place-items: center;
		z-index: 50;
	}
	.scrim {
		position: absolute;
		inset: 0;
		border: none;
		padding: 0;
		background: rgba(0, 0, 0, 0.55);
		cursor: default;
	}
	.dialog {
		position: relative;
		min-width: 320px;
		padding: 1rem 1.2rem;
		border-radius: 8px;
		background: var(--bg-panel, #1e1e1e);
		border: 1px solid var(--border, #3a3a3a);
		font-size: 0.82rem;
	}
	h2 {
		margin: 0 0 0.8rem;
		font-size: 0.9rem;
	}
	label {
		display: flex;
		align-items: center;
		gap: 0.4rem;
		margin-bottom: 0.6rem;
		color: var(--fg-muted, #aaa);
	}
	label.check {
		gap: 0.35rem;
	}
	input[type='number'],
	select {
		background: #141414;
		color: inherit;
		border: 1px solid var(--border, #3a3a3a);
		border-radius: 3px;
		padding: 0.2rem 0.3rem;
	}
	input[type='number'] {
		width: 5rem;
	}
	.note {
		color: var(--fg-muted, #777);
		font-size: 0.72rem;
		margin: 0.4rem 0 0.9rem;
	}
	.actions {
		display: flex;
		justify-content: flex-end;
		gap: 0.5rem;
	}
	button {
		padding: 0.3rem 0.7rem;
		border-radius: 3px;
		border: 1px solid var(--border, #444);
		background: #2c2c2c;
		color: #ddd;
		cursor: pointer;
	}
	button.primary {
		background: var(--accent, #3a6ea5);
		border-color: var(--accent, #3a6ea5);
		color: #fff;
	}
	button:disabled {
		opacity: 0.5;
		cursor: default;
	}
</style>
