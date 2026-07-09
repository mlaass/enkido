<script lang="ts">
	/**
	 * Transport bar + always-on capture indicator (prd-studio-daw-core §5.4/§4.1).
	 *
	 * The studio owns the clock: play/stop/pause, BPM, and the beat position
	 * (1 cycle = 1 beat). Exactly one external sync source may be active — Link
	 * or MIDI-clock-follow — and enabling one disables the other natively.
	 * MIDI-clock-*out* is orthogonal and may run alongside either.
	 *
	 * "Keep" materialises the last N minutes of the always-on retrospective
	 * buffer into per-bus stems, without interrupting capture.
	 */
	import { studio } from './studio-bridge.svelte';

	let keepMinutes = $state(1);

	const t = $derived(studio.transport);
	const c = $derived(studio.capture);

	function mmss(seconds: number): string {
		const s = Math.max(0, Math.floor(seconds));
		return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
	}

	/** Keep is bounded by what is actually buffered. */
	const keepSeconds = $derived(Math.min(keepMinutes * 60, c.bufferedSeconds));
	const canKeep = $derived(c.retroOn && keepSeconds > 0.5);
</script>

<section class="bar" aria-label="Transport">
	<div class="group">
		<button class="play" class:on={t.playing} onclick={() => studio.command(t.playing ? 'pause' : 'start')}>
			{t.playing ? '❚❚' : '▶'}
		</button>
		<button onclick={() => studio.command('stop')} aria-label="Stop">■</button>
	</div>

	<div class="group">
		<label>
			BPM
			<input
				type="number"
				min="20"
				max="300"
				step="0.1"
				value={t.bpm}
				onchange={(e) => studio.command('bpm', Number(e.currentTarget.value))}
			/>
		</label>
		<span class="pos" title="beats (1 cycle = 1 beat)">{t.positionBeats.toFixed(2)}</span>
	</div>

	<div class="group">
		<label>
			Sync
			<select value={t.sync} onchange={(e) => studio.command('sync', e.currentTarget.value)}>
				<option value="none">None</option>
				<option value="link" disabled={!t.linkAvailable}>
					Ableton Link{t.linkAvailable ? '' : ' (unavailable)'}
				</option>
				<option value="midi-follow">MIDI clock in</option>
			</select>
		</label>
		<label class="check">
			<input
				type="checkbox"
				checked={t.midiClockOut}
				onchange={(e) => studio.command('clockOut', e.currentTarget.checked)}
			/>
			MIDI clock out
		</label>
	</div>

	<div class="group capture">
		<span class="dot" class:rec={c.recording} class:armed={c.retroOn}></span>
		<span class="buf" title="retrospective buffer">
			{mmss(c.bufferedSeconds)} / {mmss(c.windowSeconds)}{c.spilling ? ' ⤓' : ''}
		</span>

		<label class="keep">
			<input type="number" min="1" max="60" step="1" bind:value={keepMinutes} />
			min
		</label>
		<button disabled={!canKeep} onclick={() => studio.keep(keepSeconds)}>Keep</button>

		<button class="rec" class:on={c.recording} onclick={() => studio.record(!c.recording)}>
			{c.recording ? 'Stop' : 'Record'}
		</button>

		{#if c.overflow > 0}
			<span class="warn" title="the disk could not keep up; the take marks the dropout">
				disk behind ({c.overflow})
			</span>
		{/if}
	</div>

	{#if studio.notice}
		<span class="notice">{studio.notice}</span>
	{/if}
</section>

<style>
	.bar {
		display: flex;
		align-items: center;
		gap: 1rem;
		flex-wrap: wrap;
		padding: 0.4rem 0.6rem;
		border: 1px solid var(--border, #333);
		border-radius: 6px;
		background: var(--bg-panel, #1b1b1b);
		font-size: 0.78rem;
	}
	.group {
		display: flex;
		align-items: center;
		gap: 0.4rem;
	}
	label {
		display: flex;
		align-items: center;
		gap: 0.3rem;
		color: var(--fg-muted, #999);
	}
	label.check {
		gap: 0.25rem;
	}
	input[type='number'] {
		width: 4.5rem;
		background: #141414;
		color: inherit;
		border: 1px solid var(--border, #3a3a3a);
		border-radius: 3px;
		padding: 0.15rem 0.25rem;
	}
	.keep input {
		width: 2.8rem;
	}
	select {
		background: #141414;
		color: inherit;
		border: 1px solid var(--border, #3a3a3a);
		border-radius: 3px;
		padding: 0.15rem;
	}
	button {
		min-width: 2rem;
		padding: 0.25rem 0.5rem;
		border-radius: 3px;
		border: 1px solid var(--border, #444);
		background: #2c2c2c;
		color: #ddd;
		cursor: pointer;
	}
	button:disabled {
		opacity: 0.4;
		cursor: default;
	}
	button.play.on {
		background: #3ba55d;
		border-color: #3ba55d;
		color: #fff;
	}
	button.rec.on {
		background: #b3452f;
		border-color: #b3452f;
		color: #fff;
	}
	.pos {
		font-variant-numeric: tabular-nums;
		color: var(--fg-muted, #888);
		min-width: 3.5rem;
	}
	.capture .dot {
		width: 9px;
		height: 9px;
		border-radius: 50%;
		background: #444;
	}
	.capture .dot.armed {
		background: #3ba55d;
	}
	.capture .dot.rec {
		background: #e0402a;
		animation: pulse 1.2s infinite;
	}
	@keyframes pulse {
		50% {
			opacity: 0.3;
		}
	}
	.buf {
		font-variant-numeric: tabular-nums;
		color: var(--fg-muted, #888);
	}
	.warn {
		color: #e0a02a;
	}
	.notice {
		color: var(--fg-muted, #8a8);
		margin-left: auto;
	}
</style>
