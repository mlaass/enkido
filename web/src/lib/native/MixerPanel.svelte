<script lang="ts">
	/**
	 * Mixer panel (prd-studio-daw-core §5.3, G3). One strip per bus the running
	 * program references — bus 0 is Master. There is no add/remove-track UI: you
	 * add a track by writing `bus(N, …)` in code. Strips are keyed by bus index,
	 * so state survives hot-swaps; a bus that disappears greys out but keeps its
	 * fader (the native side retains it).
	 *
	 * Meters come from the single batched `meters` event; fader/mute/solo drive
	 * the engine's per-bus trim through the native bridge.
	 */
	import { studio } from './studio-bridge.svelte';

	// dB → 0..1 for the fader track (−60 dB floor, +6 dB headroom).
	const MIN_DB = -60;
	const MAX_DB = 6;

	function dbToPos(db: number): number {
		return (db - MIN_DB) / (MAX_DB - MIN_DB);
	}
	function posToDb(pos: number): number {
		return MIN_DB + pos * (MAX_DB - MIN_DB);
	}
	/** Peak (0..1 linear) → meter height, log-scaled so quiet signals are visible. */
	function meterHeight(peak: number): number {
		if (peak <= 0) return 0;
		const db = 20 * Math.log10(peak);
		return Math.max(0, Math.min(1, (db - MIN_DB) / (MAX_DB - MIN_DB)));
	}

	const anySolo = $derived(studio.strips.some((s) => s.index !== 0 && s.solo));

	/** A channel is audibly silenced when muted, or when another bus is soloed. */
	function dimmed(index: number, mute: boolean, solo: boolean): boolean {
		if (mute) return true;
		return index !== 0 && anySolo && !solo;
	}
</script>

<section class="mixer" aria-label="Mixer">
	<header>
		<h2>Mixer</h2>
		<span class="hint">strips follow the code’s buses</span>
	</header>

	<div class="strips">
		{#each studio.strips as strip (strip.index)}
			{@const m = studio.meters[strip.index] ?? { peak: 0, rms: 0 }}
			<div class="strip" class:master={strip.index === 0} class:dim={dimmed(strip.index, strip.mute, strip.solo)}>
				<div class="label" title={strip.label}>{strip.label}</div>

				<div class="meter-row">
					<div class="meter" role="img" aria-label="{strip.label} level">
						<div class="rms" style="height: {meterHeight(m.rms) * 100}%"></div>
						<div class="peak" style="bottom: {meterHeight(m.peak) * 100}%"></div>
					</div>

					<input
						class="fader"
						type="range"
						min="0"
						max="1"
						step="0.001"
						aria-label="{strip.label} fader"
						value={dbToPos(strip.faderDb)}
						oninput={(e) =>
							studio.setStrip(strip.index, {
								faderDb: posToDb(Number(e.currentTarget.value))
							})}
					/>
				</div>

				<div class="db">{strip.faderDb <= MIN_DB ? '−∞' : strip.faderDb.toFixed(1)} dB</div>

				<div class="buttons">
					<button
						class:on={strip.mute}
						aria-pressed={strip.mute}
						onclick={() => studio.setStrip(strip.index, { mute: !strip.mute })}>M</button
					>
					{#if strip.index !== 0}
						<button
							class="solo"
							class:on={strip.solo}
							aria-pressed={strip.solo}
							onclick={() => studio.setStrip(strip.index, { solo: !strip.solo })}>S</button
						>
					{:else}
						<!-- Bus 0 is the master sum, not a channel: it has no solo. -->
						<button class="solo" disabled aria-hidden="true">S</button>
					{/if}
				</div>
			</div>
		{:else}
			<p class="empty">No buses yet — write <code>bus(1, …)</code> in your code.</p>
		{/each}
	</div>
</section>

<style>
	.mixer {
		border: 1px solid var(--border, #333);
		border-radius: 6px;
		background: var(--bg-panel, #1b1b1b);
		padding: 0.5rem 0.75rem 0.75rem;
	}
	header {
		display: flex;
		align-items: baseline;
		gap: 0.75rem;
	}
	h2 {
		font-size: 0.8rem;
		text-transform: uppercase;
		letter-spacing: 0.08em;
		margin: 0 0 0.5rem;
		color: var(--fg-muted, #999);
	}
	.hint {
		font-size: 0.7rem;
		color: var(--fg-muted, #666);
	}
	.strips {
		display: flex;
		gap: 0.6rem;
		overflow-x: auto;
	}
	.empty {
		color: var(--fg-muted, #777);
		font-size: 0.8rem;
	}
	.strip {
		display: flex;
		flex-direction: column;
		align-items: center;
		min-width: 62px;
		padding: 0.4rem;
		border-radius: 4px;
		background: var(--bg-strip, #232323);
		transition: opacity 120ms;
	}
	.strip.master {
		background: var(--bg-strip-master, #2a2a2a);
	}
	.strip.dim {
		opacity: 0.45;
	}
	.label {
		font-size: 0.72rem;
		max-width: 58px;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
		margin-bottom: 0.35rem;
	}
	.meter-row {
		display: flex;
		gap: 0.35rem;
		height: 110px;
	}
	.meter {
		position: relative;
		width: 8px;
		background: #111;
		border-radius: 2px;
		overflow: hidden;
	}
	.rms {
		position: absolute;
		bottom: 0;
		width: 100%;
		background: linear-gradient(to top, #3ba55d, #d1c43a 80%, #d14b3a);
	}
	.peak {
		position: absolute;
		width: 100%;
		height: 2px;
		background: #fff;
	}
	.fader {
		writing-mode: vertical-lr;
		direction: rtl;
		width: 20px;
		height: 110px;
		accent-color: var(--accent, #6aa9ff);
	}
	.db {
		font-size: 0.65rem;
		color: var(--fg-muted, #999);
		margin: 0.3rem 0;
		font-variant-numeric: tabular-nums;
	}
	.buttons {
		display: flex;
		gap: 0.25rem;
	}
	button {
		width: 22px;
		height: 20px;
		font-size: 0.65rem;
		border-radius: 3px;
		border: 1px solid var(--border, #444);
		background: #2c2c2c;
		color: #ccc;
		cursor: pointer;
	}
	button.on {
		background: #b3452f;
		color: #fff;
		border-color: #b3452f;
	}
	button.solo.on {
		background: #d1c43a;
		color: #111;
		border-color: #d1c43a;
	}
	button[disabled] {
		opacity: 0.25;
		cursor: default;
	}
</style>
