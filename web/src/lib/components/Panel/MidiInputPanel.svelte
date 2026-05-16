<script lang="ts">
	import { audioEngine } from '$lib/stores/audio.svelte';

	// Pull reactive state straight off the controller — getters keep these
	// hooked into Svelte's reactivity graph.
	const midi = $derived(audioEngine.midi);
	const status = $derived(midi.status);
	const devices = $derived(midi.devices);
	const defaultDeviceName = $derived(midi.defaultDeviceName);
	const error = $derived(midi.error);
	const lastEventTime = $derived(midi.lastEventTime);

	let nowMs = $state(Date.now());
	// Tick once per second so the activity LED fades back to idle
	// without manual recomputation on every event.
	$effect(() => {
		const id = setInterval(() => { nowMs = Date.now(); }, 250);
		return () => clearInterval(id);
	});

	const activityActive = $derived(
		lastEventTime > 0 && nowMs - lastEventTime < 500
	);

	// Drag-drop for `.mid` files moved to FilesPanel (PRD §7.6). This panel
	// is now a device-control surface only.

	const statusLabel = $derived.by(() => {
		switch (status) {
			case 'unsupported': return 'Not supported';
			case 'idle':        return 'Not connected';
			case 'pending':     return 'Requesting…';
			case 'denied':      return 'Permission denied';
			case 'unavailable': return 'Unavailable';
			case 'active':      return 'Active';
		}
	});

	const statusClass = $derived(`status status-${status}`);

	async function enableMidi() {
		await audioEngine.ensureMidiAccess();
	}

	function chooseDefaultDevice(name: string) {
		audioEngine.setDefaultMidiDevice(name);
	}
</script>

<div class="midi-input-panel">
	<div class="header">
		<span class="title">MIDI Input</span>
		<span class={statusClass}>{statusLabel}</span>
	</div>

	{#if error}
		<div class="error-line">{error}</div>
	{/if}

	{#if status === 'unsupported'}
		<p class="hint">
			This browser doesn't expose the Web MIDI API. Try Chrome or Edge.
		</p>
	{:else if status === 'idle' || status === 'pending'}
		<p class="hint">
			Allow MIDI access to play live devices into <code>midi()</code> blocks.
		</p>
		<button class="enable-btn" onclick={enableMidi} disabled={status === 'pending'}>
			{status === 'pending' ? 'Requesting…' : 'Enable MIDI'}
		</button>
	{:else if status === 'denied'}
		<p class="hint">
			Permission denied. Enable MIDI for this site in your browser's site
			settings, then click below to retry.
		</p>
		<button class="enable-btn" onclick={enableMidi}>Retry</button>
	{:else if status === 'unavailable'}
		<p class="hint">
			Web MIDI is available but no devices were granted. Plug a device in
			and reload, or click below to re-request access.
		</p>
		<button class="enable-btn" onclick={enableMidi}>Retry</button>
	{:else if status === 'active'}
		<div class="sub-block">
			<label class="setting-label" for="midi-device">
				Default device
				<span class="led" class:active={activityActive} aria-label="Activity"></span>
			</label>
			<select
				id="midi-device"
				value={defaultDeviceName}
				onchange={(e) => chooseDefaultDevice((e.target as HTMLSelectElement).value)}
			>
				<option value="">First available</option>
				{#each devices as dev (dev.id)}
					<option value={dev.name}>
						{dev.name}{dev.state === 'disconnected' ? ' (disconnected)' : ''}
					</option>
				{/each}
			</select>
			{#if devices.length === 0}
				<p class="hint">
					No MIDI input devices found. Plug one in — it'll appear here.
				</p>
			{/if}
		</div>
	{/if}

	<p class="hint">
		Drop <code>.mid</code> files in the <strong>Files</strong> tab to make
		them available to <code>midi(&lbrace;file: …&rbrace;)</code>.
	</p>
</div>

<style>
	.midi-input-panel {
		display: flex;
		flex-direction: column;
		gap: 0.5rem;
		padding: 0.5rem 0;
	}

	.header {
		display: flex;
		align-items: center;
		justify-content: space-between;
	}

	.title {
		font-weight: 600;
		font-size: var(--ui-font-size-sm, 0.85rem);
		color: var(--text-primary);
	}

	.status {
		font-size: 0.75rem;
		padding: 0.125rem 0.5rem;
		border-radius: 999px;
		background: var(--bg-secondary);
		color: var(--text-secondary);
		border: 1px solid var(--border);
	}

	.status-active {
		color: var(--accent, #4caf50);
		border-color: var(--accent, #4caf50);
	}

	.status-pending {
		color: var(--text-secondary);
	}

	.status-denied,
	.status-unavailable,
	.status-unsupported {
		color: var(--error, #e0625a);
		border-color: var(--error, #e0625a);
	}

	.error-line {
		font-size: 0.75rem;
		color: var(--error, #e0625a);
	}

	.sub-block {
		display: flex;
		flex-direction: column;
		gap: 0.25rem;
	}

	.setting-label {
		font-size: 0.75rem;
		color: var(--text-secondary);
		display: flex;
		align-items: center;
		gap: 0.4rem;
	}

	select {
		background: var(--bg-secondary);
		border: 1px solid var(--border);
		border-radius: 4px;
		color: var(--text-primary);
		padding: 0.25rem 0.5rem;
		font-size: 0.8rem;
	}

	.hint {
		font-size: 0.75rem;
		color: var(--text-secondary);
		margin: 0;
	}

	.hint code {
		font-family: var(--font-mono, monospace);
		background: var(--bg-secondary);
		padding: 0 0.25rem;
		border-radius: 2px;
	}

	.enable-btn {
		align-self: flex-start;
		background: var(--accent, #4caf50);
		color: var(--bg-primary, #111);
		border: none;
		border-radius: 4px;
		padding: 0.25rem 0.75rem;
		font-size: 0.8rem;
		cursor: pointer;
	}

	.enable-btn:disabled {
		opacity: 0.6;
		cursor: not-allowed;
	}

	.led {
		display: inline-block;
		width: 0.55rem;
		height: 0.55rem;
		border-radius: 50%;
		background: var(--bg-secondary);
		border: 1px solid var(--border);
		transition: background 80ms linear;
	}

	.led.active {
		background: var(--accent, #4caf50);
		border-color: var(--accent, #4caf50);
		box-shadow: 0 0 6px var(--accent, #4caf50);
	}

</style>
