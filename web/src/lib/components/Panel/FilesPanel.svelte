<script lang="ts">
	// PRD prd-midi-input §7.6: unified drop zone for every asset type the
	// engine understands. Routes through `file-router` (which knows the
	// per-kind audioEngine API). Replaces the per-type drop zones that lived
	// on SampleBrowser (soundfonts) and MidiInputPanel (.mid files), and
	// adds the new sample drop zone the engine didn't expose before.
	import { audioEngine } from '$lib/stores/audio.svelte';
	import {
		routeFiles,
		inferFromExtension,
		type FileKind,
		type RoutedFile
	} from '$lib/audio/file-router';

	let dropping = $state(false);
	let recent = $state<RoutedFile[]>([]);
	let errors = $state<string[]>([]);

	// Counts pulled from the engine — these survive across panel mounts and
	// reflect what's actually loaded, not just what was dropped this session.
	const sampleCount = $derived(audioEngine.loadedSampleCount);
	const soundfontCount = $derived(audioEngine.loadedSoundfonts.length);
	const midiCount = $derived(audioEngine.midiBank.list().length);

	async function ingest(files: FileList | File[] | null) {
		if (!files || (files instanceof FileList && files.length === 0)) return;
		errors = [];

		const results = await routeFiles(files);

		// Collect human-readable errors per-file; surface them above the
		// drop zone so the user can correct typos / wrong extensions.
		const errorList: string[] = [];
		for (const r of results) {
			if (r.ok) continue;
			const reason = r.error
				? `${r.name}: ${r.error}`
				: r.kind === 'unknown'
				  ? `${r.name}: unrecognized extension`
				  : `${r.name}: load failed`;
			errorList.push(reason);
		}
		errors = errorList;

		// Keep the last 8 successful drops as a confirmation list.
		const okResults = results.filter((r) => r.ok);
		const merged = [...okResults, ...recent.filter((r) => !okResults.some((o) => o.name === r.name))];
		recent = merged.slice(0, 8);
	}

	function onDragOver(e: DragEvent) {
		e.preventDefault();
		dropping = true;
	}
	function onDragLeave() { dropping = false; }
	function onDrop(e: DragEvent) {
		e.preventDefault();
		dropping = false;
		ingest(e.dataTransfer?.files ?? null);
	}
	function onFileInput(e: Event) {
		const input = e.target as HTMLInputElement;
		ingest(input.files);
		input.value = '';
	}

	function kindLabel(k: FileKind): string {
		switch (k) {
			case 'sample':    return 'sample';
			case 'soundfont': return 'soundfont';
			case 'midi':      return 'MIDI';
			default:          return 'unknown';
		}
	}

	// Hover preview: tell the user what kind a dragged file will land as
	// before they drop. Pulls the first item's name out of dataTransfer.items
	// (which is the only metadata available during dragover — not files).
	let previewKind = $state<FileKind | null>(null);
	function onDragEnter(e: DragEvent) {
		const item = e.dataTransfer?.items?.[0];
		if (item && item.kind === 'file') {
			// dataTransfer doesn't expose filename here; we can only act on
			// the drop. Keep the hint generic during hover.
			previewKind = null;
		}
	}
</script>

<div class="files-panel">
	<div class="header">
		<span class="title">Files</span>
	</div>

	<div class="kind-summary">
		<span class="kind"><strong>{sampleCount}</strong> sample{sampleCount === 1 ? '' : 's'}</span>
		<span class="kind"><strong>{soundfontCount}</strong> soundfont{soundfontCount === 1 ? '' : 's'}</span>
		<span class="kind"><strong>{midiCount}</strong> MIDI</span>
	</div>

	<div
		class="drop-zone"
		class:dropping
		ondragover={onDragOver}
		ondragenter={onDragEnter}
		ondragleave={onDragLeave}
		ondrop={onDrop}
		role="region"
		aria-label="Drop files here"
	>
		<p class="drop-text">
			Drop <code>.wav</code> / <code>.sf2</code> / <code>.mid</code> files here
		</p>
		<label class="drop-pick">
			<input
				type="file"
				accept=".wav,.mp3,.ogg,.flac,.aiff,.aif,.sf2,.sf3,.mid,.midi"
				multiple
				onchange={onFileInput}
			/>
			<span>or browse…</span>
		</label>
	</div>

	{#if errors.length > 0}
		<ul class="error-list">
			{#each errors as err}
				<li>{err}</li>
			{/each}
		</ul>
	{/if}

	{#if recent.length > 0}
		<div class="recent">
			<span class="recent-label">Recently loaded:</span>
			<ul>
				{#each recent as r}
					<li>
						<span class="recent-kind kind-{r.kind}">{kindLabel(r.kind)}</span>
						<span class="recent-name">{r.name}</span>
					</li>
				{/each}
			</ul>
		</div>
	{/if}
</div>

<style>
	.files-panel {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
		padding: var(--spacing-sm);
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

	.kind-summary {
		display: flex;
		gap: var(--spacing-md);
		font-size: 11px;
		color: var(--text-secondary);
	}

	.kind strong {
		color: var(--text-primary);
		font-variant-numeric: tabular-nums;
		margin-right: 2px;
	}

	.drop-zone {
		display: flex;
		flex-direction: column;
		align-items: center;
		gap: 0.35rem;
		padding: var(--spacing-md);
		border: 1px dashed var(--border-default);
		border-radius: 4px;
		background: var(--bg-secondary);
		transition: border-color 80ms linear, background 80ms linear;
	}

	.drop-zone.dropping {
		border-color: var(--accent-primary, #4caf50);
		border-style: solid;
		background: color-mix(in srgb, var(--accent-primary, #4caf50) 12%, var(--bg-secondary));
	}

	.drop-text {
		font-size: 0.78rem;
		color: var(--text-secondary);
		margin: 0;
		text-align: center;
	}

	.drop-text code {
		font-family: var(--font-mono, monospace);
		padding: 0 0.25rem;
		background: var(--bg-tertiary);
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

	.error-list {
		margin: 0;
		padding-left: 1.25rem;
		font-size: 0.75rem;
		color: var(--error, #e0625a);
	}

	.recent {
		display: flex;
		flex-direction: column;
		gap: 0.25rem;
	}

	.recent-label {
		font-size: 0.7rem;
		text-transform: uppercase;
		letter-spacing: 0.05em;
		color: var(--text-muted);
	}

	.recent ul {
		display: flex;
		flex-direction: column;
		gap: 2px;
		margin: 0;
		padding: 0;
		list-style: none;
	}

	.recent li {
		display: flex;
		align-items: center;
		gap: 0.5rem;
		font-size: 0.75rem;
		color: var(--text-secondary);
	}

	.recent-kind {
		font-size: 0.65rem;
		text-transform: uppercase;
		letter-spacing: 0.04em;
		padding: 1px 6px;
		border-radius: 999px;
		color: var(--text-primary);
		background: var(--bg-tertiary);
		flex-shrink: 0;
	}

	.recent-kind.kind-sample    { color: var(--accent-primary, #4caf50); }
	.recent-kind.kind-soundfont { color: var(--accent-secondary, #6cb6ff); }
	.recent-kind.kind-midi      { color: var(--accent, #d6a06b); }

	.recent-name {
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}
</style>
