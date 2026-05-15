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

	// Drag-drop for `.mid` files (prd-midi-input Phase 5). The user drops a
	// file onto the panel; we register the bytes under its basename so a
	// later `midi({file: "name.mid"})` reference resolves through the
	// midiBank instead of being fetched over HTTP.
	let dropping = $state(false);
	let lastDropped = $state<string[]>([]);
	let dropError = $state<string | null>(null);

	function basename(file: File): string {
		return file.name.replace(/\\/g, '/').split('/').pop() ?? file.name;
	}

	async function ingestFiles(list: FileList | File[] | null) {
		if (!list) return;
		dropError = null;
		const files = Array.from(list).filter((f) =>
			f.name.toLowerCase().endsWith('.mid') ||
			f.name.toLowerCase().endsWith('.midi')
		);
		if (files.length === 0) {
			dropError = 'Drop one or more .mid / .midi files';
			return;
		}
		for (const file of files) {
			const name = basename(file);
			try {
				const bytes = await file.arrayBuffer();
				audioEngine.midiBank.register(name, bytes.slice(0));
				const ok = await audioEngine.loadMidiFile(name, bytes);
				if (!ok) {
					dropError = `Parse failed: ${name}`;
					audioEngine.midiBank.revoke(name);
					continue;
				}
				lastDropped = [name, ...lastDropped.filter((n) => n !== name)].slice(0, 5);
			} catch (err) {
				console.error('[MidiInputPanel] Failed to load dropped file', err);
				dropError = `Error reading ${name}`;
			}
		}
	}

	function onDragOver(e: DragEvent) {
		e.preventDefault();
		dropping = true;
	}
	function onDragLeave() { dropping = false; }
	function onDrop(e: DragEvent) {
		e.preventDefault();
		dropping = false;
		ingestFiles(e.dataTransfer?.files ?? null);
	}
	function onFileInput(e: Event) {
		const input = e.target as HTMLInputElement;
		ingestFiles(input.files);
		input.value = '';
	}

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

	<div
		class="drop-zone"
		class:dropping
		ondragover={onDragOver}
		ondragleave={onDragLeave}
		ondrop={onDrop}
		role="region"
		aria-label="Drop .mid files here"
	>
		<p class="drop-text">
			Drop <code>.mid</code> files here for <code>midi(&lbrace;file: …&rbrace;)</code>
		</p>
		<label class="drop-pick">
			<input type="file" accept=".mid,.midi" multiple onchange={onFileInput} />
			<span>or browse…</span>
		</label>
		{#if lastDropped.length > 0}
			<p class="hint loaded-list">
				Loaded: {lastDropped.join(', ')}
			</p>
		{/if}
		{#if dropError}
			<p class="error-line">{dropError}</p>
		{/if}
	</div>
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

	.drop-zone {
		display: flex;
		flex-direction: column;
		align-items: center;
		gap: 0.25rem;
		padding: 0.6rem;
		border: 1px dashed var(--border);
		border-radius: 4px;
		background: var(--bg-secondary);
		transition: border-color 80ms linear, background 80ms linear;
	}

	.drop-zone.dropping {
		border-color: var(--accent, #4caf50);
		border-style: solid;
		background: color-mix(in srgb, var(--accent, #4caf50) 12%, var(--bg-secondary));
	}

	.drop-text {
		font-size: 0.75rem;
		color: var(--text-secondary);
		margin: 0;
		text-align: center;
	}

	.drop-text code {
		font-family: var(--font-mono, monospace);
		padding: 0 0.2rem;
		background: var(--bg-primary);
		border-radius: 2px;
	}

	.drop-pick {
		font-size: 0.75rem;
		color: var(--text-secondary);
		cursor: pointer;
	}

	.drop-pick input[type='file'] {
		display: none;
	}

	.drop-pick span {
		text-decoration: underline dotted;
	}

	.loaded-list {
		font-size: 0.7rem;
		color: var(--accent, #4caf50);
	}
</style>
