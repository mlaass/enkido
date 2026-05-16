<script lang="ts">
	// PRD prd-unified-files-panel — single Files tab that is both the
	// drop / URL ingest point and the browser for every asset the engine
	// knows about. Replaces the per-asset-type panels (the old
	// SampleBrowser tab is gone). Built-ins are visible-but-collapsed so
	// they don't drown out user uploads.
	import { audioEngine } from '$lib/stores/audio.svelte';
	import {
		routeFiles,
		routeUrl,
		inferFromUrl,
		type FileKind,
		type RoutedFile
	} from '$lib/audio/file-router';

	let dropping = $state(false);
	let errors = $state<string[]>([]);
	let urlInput = $state('');
	let urlLoading = $state(false);

	// Section open/closed state. Built-ins start collapsed; user-loaded
	// sections start open so a fresh upload is immediately visible.
	let samplesOpen = $state(true);
	let builtinSamplesOpen = $state(false);
	let soundfontsOpen = $state(true);
	let midiOpen = $state(true);
	// Which soundfont row is expanded (for the preset list). Shape lifted
	// from the old SampleBrowser; only one expanded at a time keeps the
	// panel readable.
	let expandedSf = $state<number | null>(null);

	const allSamples = $derived(audioEngine.loadedSamples);
	const userSamples = $derived(allSamples.filter((s) => s.origin === 'user'));
	const builtinSamples = $derived(allSamples.filter((s) => s.origin === 'builtin'));
	const soundfonts = $derived(audioEngine.loadedSoundfonts);
	const midiFiles = $derived(audioEngine.loadedMidiFiles);

	async function ingest(files: FileList | File[] | null) {
		if (!files || (files instanceof FileList && files.length === 0)) return;
		errors = [];

		const results = await routeFiles(files);
		errors = collectErrors(results);
	}

	async function loadUrl() {
		const url = urlInput.trim();
		if (!url) return;
		errors = [];
		urlLoading = true;
		try {
			const result = await routeUrl(url);
			errors = collectErrors([result]);
			if (result.ok) urlInput = '';
		} finally {
			urlLoading = false;
		}
	}

	function collectErrors(results: RoutedFile[]): string[] {
		const out: string[] = [];
		for (const r of results) {
			if (r.ok) continue;
			const reason = r.error
				? `${r.name}: ${r.error}`
				: r.kind === 'unknown'
				  ? `${r.name}: unrecognized extension`
				  : `${r.name}: load failed`;
			out.push(reason);
		}
		return out;
	}

	function onDragOver(e: DragEvent) {
		e.preventDefault();
		dropping = true;
	}
	function onDragLeave() {
		dropping = false;
	}
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

	function toggleSf(sfId: number) {
		expandedSf = expandedSf === sfId ? null : sfId;
	}

	function removeMidi(name: string) {
		audioEngine.unregisterMidiFile(name);
	}

	// Live preview of what kind the URL input will resolve to (or
	// "unknown" if the extension isn't recognized). Helps users notice
	// typos before they hit Load.
	const urlKindPreview = $derived<FileKind | null>(
		urlInput.trim() ? inferFromUrl(urlInput.trim()) : null
	);
</script>

<div class="files-panel">
	<div class="header">
		<span class="title">Files</span>
	</div>

	<div
		class="drop-zone"
		class:dropping
		ondragover={onDragOver}
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

	<div class="url-loader">
		<input
			type="text"
			placeholder="URL: https://… (.wav / .sf2 / .mid)"
			bind:value={urlInput}
			onkeydown={(e) => e.key === 'Enter' && loadUrl()}
			disabled={urlLoading}
		/>
		<button onclick={loadUrl} disabled={urlLoading || !urlInput.trim()}>
			{urlLoading ? '…' : 'Load'}
		</button>
	</div>
	{#if urlKindPreview === 'unknown'}
		<div class="url-hint warn">URL extension not recognized</div>
	{:else if urlKindPreview}
		<div class="url-hint">Will load as <strong>{urlKindPreview}</strong></div>
	{/if}

	{#if errors.length > 0}
		<ul class="error-list">
			{#each errors as err}
				<li>{err}</li>
			{/each}
		</ul>
	{/if}

	<!-- Samples section -->
	<section class="kind-section">
		<button
			class="section-header"
			onclick={() => (samplesOpen = !samplesOpen)}
		>
			<span class="chev" class:open={samplesOpen}>▶</span>
			<span class="section-title">Samples</span>
			<span class="section-meta">
				{userSamples.length} user, {builtinSamples.length} built-in
			</span>
		</button>
		{#if samplesOpen}
			<div class="section-body">
				{#if userSamples.length === 0}
					<div class="empty">No user samples — drop a <code>.wav</code> above.</div>
				{:else}
					<ul class="row-list">
						{#each userSamples as s (s.name)}
							<li class="row">
								<span class="row-name">{s.name}</span>
							</li>
						{/each}
					</ul>
				{/if}

				{#if builtinSamples.length > 0}
					<button
						class="subsection-header"
						onclick={() => (builtinSamplesOpen = !builtinSamplesOpen)}
					>
						<span class="chev" class:open={builtinSamplesOpen}>▶</span>
						<span class="subsection-title">
							Built-in: 808 drum kit ({builtinSamples.length})
						</span>
					</button>
					{#if builtinSamplesOpen}
						<ul class="row-list builtin">
							{#each builtinSamples as s (s.name)}
								<li class="row">
									<span class="row-name">{s.name}</span>
								</li>
							{/each}
						</ul>
					{/if}
				{/if}
			</div>
		{/if}
	</section>

	<!-- SoundFonts section -->
	<section class="kind-section">
		<button
			class="section-header"
			onclick={() => (soundfontsOpen = !soundfontsOpen)}
		>
			<span class="chev" class:open={soundfontsOpen}>▶</span>
			<span class="section-title">SoundFonts</span>
			<span class="section-meta">{soundfonts.length}</span>
		</button>
		{#if soundfontsOpen}
			<div class="section-body">
				{#if soundfonts.length === 0}
					<div class="empty">No SoundFonts — drop a <code>.sf2</code> above.</div>
				{:else}
					<ul class="row-list">
						{#each soundfonts as sf (sf.sfId)}
							<li class="sf-card">
								<button
									class="row sf-row"
									onclick={() => toggleSf(sf.sfId)}
								>
									<span class="chev" class:open={expandedSf === sf.sfId}>▶</span>
									<span class="row-name">{sf.name}</span>
									<span class="row-meta">{sf.presetCount} presets</span>
									{#if sf.origin === 'builtin'}
										<span class="badge">built-in</span>
									{/if}
								</button>
								{#if expandedSf === sf.sfId}
									<div class="sf-presets">
										{#each sf.presets as preset, i (i)}
											<div class="preset-row">
												<span class="preset-index">{i}</span>
												<span class="preset-name">{preset.name}</span>
												<span class="preset-bank">{preset.bank}:{preset.program}</span>
											</div>
										{/each}
									</div>
								{/if}
							</li>
						{/each}
					</ul>
				{/if}
			</div>
		{/if}
	</section>

	<!-- MIDI section -->
	<section class="kind-section">
		<button
			class="section-header"
			onclick={() => (midiOpen = !midiOpen)}
		>
			<span class="chev" class:open={midiOpen}>▶</span>
			<span class="section-title">MIDI files</span>
			<span class="section-meta">{midiFiles.length}</span>
		</button>
		{#if midiOpen}
			<div class="section-body">
				{#if midiFiles.length === 0}
					<div class="empty">No MIDI files — drop a <code>.mid</code> above.</div>
				{:else}
					<ul class="row-list">
						{#each midiFiles as m (m.name)}
							<li class="row">
								<span class="row-name">{m.name}</span>
								<button
									class="row-action"
									title="Remove {m.name}"
									aria-label="Remove {m.name}"
									onclick={() => removeMidi(m.name)}
								>×</button>
							</li>
						{/each}
					</ul>
				{/if}
			</div>
		{/if}
	</section>
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

	.url-loader {
		display: flex;
		gap: 4px;
	}
	.url-loader input {
		flex: 1;
		padding: 4px 8px;
		font-size: 12px;
		min-width: 0;
	}
	.url-loader button {
		padding: 4px 10px;
		font-size: 12px;
		font-weight: 500;
		color: var(--text-secondary);
		background-color: var(--bg-tertiary);
		border-radius: 4px;
		transition: all var(--transition-fast);
		flex-shrink: 0;
	}
	.url-loader button:hover:not(:disabled) {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}
	.url-loader button:disabled {
		opacity: 0.5;
	}
	.url-hint {
		font-size: 11px;
		color: var(--text-muted);
	}
	.url-hint.warn {
		color: var(--error, #e0625a);
	}

	.error-list {
		margin: 0;
		padding-left: 1.25rem;
		font-size: 0.75rem;
		color: var(--error, #e0625a);
	}

	.kind-section {
		display: flex;
		flex-direction: column;
		border-top: 1px solid var(--border-muted);
		padding-top: var(--spacing-xs);
	}
	.section-header,
	.subsection-header {
		display: flex;
		align-items: center;
		gap: 0.4rem;
		padding: 4px 2px;
		font-size: 12px;
		color: var(--text-primary);
		text-align: left;
		background: transparent;
		cursor: pointer;
	}
	.section-header:hover,
	.subsection-header:hover {
		color: var(--accent-primary, #4caf50);
	}
	.section-title {
		font-weight: 600;
		flex: 1;
	}
	.section-meta {
		font-size: 11px;
		color: var(--text-muted);
		font-variant-numeric: tabular-nums;
	}
	.subsection-title {
		font-size: 11px;
		color: var(--text-secondary);
		font-style: italic;
	}
	.chev {
		display: inline-block;
		width: 10px;
		text-align: center;
		font-size: 10px;
		color: var(--text-muted);
		transition: transform var(--transition-fast);
	}
	.chev.open {
		transform: rotate(90deg);
	}

	.section-body {
		display: flex;
		flex-direction: column;
		gap: 2px;
		padding-left: 14px;
		padding-bottom: var(--spacing-xs);
	}
	.empty {
		font-size: 11px;
		color: var(--text-muted);
		padding: 4px 0;
	}
	.empty code {
		font-family: var(--font-mono, monospace);
		padding: 0 0.2rem;
		background: var(--bg-tertiary);
		border-radius: 2px;
	}

	.row-list {
		display: flex;
		flex-direction: column;
		gap: 0;
		list-style: none;
		margin: 0;
		padding: 0;
	}
	.row-list.builtin {
		max-height: 200px;
		overflow-y: auto;
		border-left: 1px solid var(--border-muted);
		padding-left: 6px;
		margin-top: 2px;
	}
	.row {
		display: flex;
		align-items: center;
		gap: 0.5rem;
		padding: 2px 4px;
		font-size: 12px;
		color: var(--text-secondary);
		background: transparent;
		text-align: left;
		border-radius: 2px;
	}
	.row:hover {
		background: var(--bg-hover);
	}
	.row-name {
		flex: 1;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
		font-family: var(--font-mono, monospace);
	}
	.row-meta {
		font-size: 11px;
		color: var(--text-muted);
		font-variant-numeric: tabular-nums;
		flex-shrink: 0;
	}
	.row-action {
		font-size: 13px;
		line-height: 1;
		color: var(--text-muted);
		background: transparent;
		padding: 0 4px;
		border-radius: 2px;
		cursor: pointer;
	}
	.row-action:hover {
		color: var(--error, #e0625a);
		background: var(--bg-tertiary);
	}

	.sf-card {
		display: flex;
		flex-direction: column;
	}
	.sf-row {
		width: 100%;
		cursor: pointer;
	}
	.sf-presets {
		border-top: 1px solid var(--border-muted);
		max-height: 200px;
		overflow-y: auto;
		margin: 2px 0 4px 14px;
		padding: 2px 0;
	}
	.preset-row {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		padding: 1px 4px;
		font-size: 11px;
		color: var(--text-secondary);
	}
	.preset-index {
		width: 24px;
		text-align: right;
		color: var(--text-muted);
		font-variant-numeric: tabular-nums;
	}
	.preset-name {
		flex: 1;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}
	.preset-bank {
		color: var(--text-muted);
		font-variant-numeric: tabular-nums;
	}

	.badge {
		font-size: 9px;
		text-transform: uppercase;
		letter-spacing: 0.04em;
		padding: 1px 5px;
		border-radius: 999px;
		background: var(--bg-tertiary);
		color: var(--text-muted);
		flex-shrink: 0;
	}
</style>
