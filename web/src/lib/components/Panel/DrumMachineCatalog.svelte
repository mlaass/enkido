<script lang="ts">
	// PRD prd-tidal-drum-machines — browsable catalog of the geikha/
	// tidal-drum-machines drum machines. Samples stream from GitHub on
	// demand; "Load" prefetches a whole machine into the pinned IndexedDB
	// cache so the kit plays offline. Browsing reads only the committed
	// catalog.json — no network until the user plays or loads a machine.
	import { onMount } from 'svelte';
	import { ChevronDown, ChevronRight } from 'lucide-svelte';
	import {
		ensureCatalogLoaded,
		getCatalogMachines,
		prefetchCatalogMachine,
		type CatalogMachine
	} from '$lib/audio/bank-catalog';

	let open = $state(false);
	let status = $state<'loading' | 'ready' | 'error'>('loading');
	let machines = $state<CatalogMachine[]>([]);
	let search = $state('');

	type LoadState = { state: 'idle' | 'loading' | 'loaded' | 'error'; done: number; total: number };
	let loadStates = $state<Record<string, LoadState>>({});

	onMount(async () => {
		const ok = await ensureCatalogLoaded();
		if (ok) {
			machines = getCatalogMachines();
			status = 'ready';
		} else {
			status = 'error';
		}
	});

	const filtered = $derived(
		search.trim()
			? machines.filter((m) => m.name.toLowerCase().includes(search.trim().toLowerCase()))
			: machines
	);

	// "bd·8 sd·4 hh·3 …" — sound codes with their variant counts.
	function soundSummary(machine: CatalogMachine): string {
		return Array.from(machine.sounds.entries())
			.map(([code, variants]) => `${code}·${variants.length}`)
			.join('  ');
	}

	async function loadMachine(name: string) {
		const current = loadStates[name];
		if (current && (current.state === 'loading' || current.state === 'loaded')) return;
		loadStates = { ...loadStates, [name]: { state: 'loading', done: 0, total: 0 } };
		const result = await prefetchCatalogMachine(name, (done, total) => {
			loadStates = { ...loadStates, [name]: { state: 'loading', done, total } };
		});
		loadStates = {
			...loadStates,
			[name]: {
				state: result.failed > 0 && result.ok === 0 ? 'error' : 'loaded',
				done: result.ok,
				total: result.ok + result.failed
			}
		};
	}

	function loadLabel(name: string): string {
		const s = loadStates[name];
		if (!s || s.state === 'idle') return 'Load';
		if (s.state === 'loading') return s.total ? `${s.done}/${s.total}` : '…';
		if (s.state === 'loaded') return 'Loaded';
		return 'Retry';
	}
</script>

<section class="kind-section">
	<button class="section-header" onclick={() => (open = !open)}>
		{#if open}
			<ChevronDown size={14} />
		{:else}
			<ChevronRight size={14} />
		{/if}
		<span class="section-title">Drum Machines</span>
		<span class="section-meta">
			{status === 'ready' ? machines.length : status === 'loading' ? '…' : '—'}
		</span>
	</button>

	{#if open}
		<div class="section-body">
			{#if status === 'loading'}
				<div class="empty">Loading catalog…</div>
			{:else if status === 'error'}
				<div class="empty">Catalog unavailable — drum machines cannot be browsed.</div>
			{:else}
				<input
					class="search"
					type="text"
					placeholder="Filter machines… (e.g. tr8)"
					bind:value={search}
				/>
				{#if filtered.length === 0}
					<div class="empty">No machine matches “{search}”.</div>
				{:else}
					<ul class="row-list">
						{#each filtered as machine (machine.name)}
							{@const ls = loadStates[machine.name]}
							<li class="machine">
								<div class="machine-row">
									<span class="machine-name">{machine.name}</span>
									<button
										class="load-btn"
										class:loaded={ls?.state === 'loaded'}
										class:error={ls?.state === 'error'}
										disabled={ls?.state === 'loading'}
										onclick={() => loadMachine(machine.name)}
										title="Prefetch every sample of {machine.name} for offline use"
									>
										{loadLabel(machine.name)}
									</button>
								</div>
								<div class="machine-sounds">{soundSummary(machine)}</div>
							</li>
						{/each}
					</ul>
				{/if}
			{/if}
		</div>
	{/if}
</section>

<style>
	.kind-section {
		display: flex;
		flex-direction: column;
		border-top: 1px solid var(--border-muted);
		padding-top: var(--spacing-xs);
	}
	.section-header {
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
	.section-header:hover {
		color: var(--accent-primary, #4caf50);
	}
	.section-header :global(svg) {
		color: var(--text-muted);
		flex-shrink: 0;
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
	.section-body {
		display: flex;
		flex-direction: column;
		gap: 4px;
		padding-left: 14px;
		padding-bottom: var(--spacing-xs);
	}
	.empty {
		font-size: 11px;
		color: var(--text-muted);
		padding: 4px 0;
	}
	.search {
		padding: 4px 8px;
		font-size: 12px;
		min-width: 0;
		width: 100%;
	}
	.row-list {
		display: flex;
		flex-direction: column;
		gap: 0;
		list-style: none;
		margin: 0;
		padding: 0;
		max-height: 280px;
		overflow-y: auto;
	}
	.machine {
		display: flex;
		flex-direction: column;
		padding: 3px 4px;
		border-radius: 2px;
	}
	.machine:hover {
		background: var(--bg-hover);
	}
	.machine-row {
		display: flex;
		align-items: center;
		gap: 0.5rem;
	}
	.machine-name {
		flex: 1;
		font-size: 12px;
		font-family: var(--font-mono, monospace);
		color: var(--text-secondary);
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}
	.machine-sounds {
		font-size: 10px;
		color: var(--text-muted);
		font-family: var(--font-mono, monospace);
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}
	.load-btn {
		font-size: 11px;
		font-weight: 500;
		padding: 2px 8px;
		color: var(--text-secondary);
		background-color: var(--bg-tertiary);
		border-radius: 4px;
		cursor: pointer;
		flex-shrink: 0;
		min-width: 48px;
		font-variant-numeric: tabular-nums;
		transition: all var(--transition-fast);
	}
	.load-btn:hover:not(:disabled) {
		background-color: var(--bg-hover);
		color: var(--text-primary);
	}
	.load-btn:disabled {
		opacity: 0.6;
		cursor: default;
	}
	.load-btn.loaded {
		color: var(--accent-primary, #4caf50);
	}
	.load-btn.error {
		color: var(--error, #e0625a);
	}
</style>
