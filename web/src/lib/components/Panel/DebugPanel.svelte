<script lang="ts">
	import { audioEngine } from '$lib/stores/audio.svelte';
	import type { DisassemblyInstruction } from '$lib/stores/audio.svelte';

	let disassembly = $derived(audioEngine.disassembly);
	let showAllInstructions = $state(false);
	let filterStateful = $state(false);
	let searchQuery = $state('');

	// Filter instructions based on search and stateful filter
	let filteredInstructions = $derived.by(() => {
		if (!disassembly) return [];
		let instructions = disassembly.instructions;

		if (filterStateful) {
			instructions = instructions.filter((i) => i.stateful);
		}

		if (searchQuery.trim()) {
			const query = searchQuery.toLowerCase();
			instructions = instructions.filter(
				(i) =>
					i.opcode.toLowerCase().includes(query) ||
					i.stateId.toString().includes(query) ||
					i.index.toString() === query
			);
		}

		return instructions;
	});

	// Group instructions by opcode for summary view
	let opcodeGroups = $derived.by(() => {
		if (!disassembly) return new Map<string, number>();
		const groups = new Map<string, number>();
		for (const inst of disassembly.instructions) {
			groups.set(inst.opcode, (groups.get(inst.opcode) || 0) + 1);
		}
		return groups;
	});

	function formatStateId(id: number): string {
		if (id === 0) return '-';
		return `0x${id.toString(16).toUpperCase().padStart(8, '0')}`;
	}

	function formatInputs(inputs: number[]): string {
		return inputs
			.map((i) => (i === 65535 ? '-' : `b${i}`))
			.filter((_, idx) => inputs[idx] !== 65535 || idx === 0)
			.join(', ');
	}

	// Opcode pairs that intentionally share state (cooperative opcodes)
	const COOPERATIVE_OPCODE_PAIRS = new Set([
		'SEQPAT_QUERY,SEQPAT_STEP', // Query populates state, Step reads from it
		'SEQPAT_STEP,SEQPAT_QUERY'
	]);

	function isCooperativePair(opcodes: string[]): boolean {
		if (opcodes.length !== 2) return false;
		const key = opcodes.sort().join(',');
		return COOPERATIVE_OPCODE_PAIRS.has(key);
	}

	// Check for state ID collisions (same ID used by different opcodes)
	// Excludes cooperative opcode pairs that intentionally share state
	let stateCollisions = $derived.by(() => {
		if (!disassembly) return [];
		const stateIdOpcodes = new Map<number, Set<string>>();

		for (const inst of disassembly.instructions) {
			if (inst.stateful && inst.stateId !== 0) {
				if (!stateIdOpcodes.has(inst.stateId)) {
					stateIdOpcodes.set(inst.stateId, new Set());
				}
				stateIdOpcodes.get(inst.stateId)!.add(inst.opcode);
			}
		}

		const collisions: Array<{ stateId: number; opcodes: string[] }> = [];
		for (const [stateId, opcodes] of stateIdOpcodes) {
			const opcodeList = Array.from(opcodes);
			// Only flag as collision if multiple opcodes AND not a cooperative pair
			if (opcodes.size > 1 && !isCooperativePair(opcodeList)) {
				collisions.push({ stateId, opcodes: opcodeList });
			}
		}
		return collisions;
	});
</script>

<div class="debug-panel">
	{#if !disassembly}
		<div class="empty-state">
			<p>No compiled program</p>
			<p class="hint">Compile code with Ctrl+Enter to see bytecode</p>
		</div>
	{:else}
		<!-- Summary Section -->
		<section class="summary-section">
			<h3>Summary</h3>
			<div class="summary-grid">
				<div class="summary-item">
					<span class="label">Instructions</span>
					<span class="value">{disassembly.summary.totalInstructions}</span>
				</div>
				<div class="summary-item">
					<span class="label">Stateful</span>
					<span class="value">{disassembly.summary.statefulCount}</span>
				</div>
				<div class="summary-item">
					<span class="label">Unique States</span>
					<span class="value">{disassembly.summary.uniqueStateIds}</span>
				</div>
				<div class="summary-item">
					<span class="label">Bytecode</span>
					<span class="value">{disassembly.summary.totalInstructions * 20} bytes</span>
				</div>
			</div>
		</section>

		<!-- Collision Warning -->
		{#if stateCollisions.length > 0}
			<section class="warning-section">
				<h3>State ID Collisions</h3>
				<p class="warning-text">
					Different opcode types are sharing the same state ID. This may cause unexpected behavior.
				</p>
				{#each stateCollisions as collision (collision.stateId)}
					<div class="collision-item">
						<code>{formatStateId(collision.stateId)}</code>
						<span class="collision-opcodes">{collision.opcodes.join(', ')}</span>
					</div>
				{/each}
			</section>
		{/if}

		<!-- Opcode Distribution -->
		<section class="opcodes-section">
			<h3>Opcode Distribution</h3>
			<div class="opcode-list">
				{#each Array.from(opcodeGroups).sort((a, b) => b[1] - a[1]) as [opcode, count] (opcode)}
					<div class="opcode-item">
						<span class="opcode-name">{opcode}</span>
						<span class="opcode-count">{count}</span>
					</div>
				{/each}
			</div>
		</section>

		<!-- Instructions Table -->
		<section class="instructions-section">
			<div class="instructions-header">
				<h3>Instructions</h3>
				<div class="controls">
					<label class="filter-checkbox">
						<input type="checkbox" bind:checked={filterStateful} />
						Stateful only
					</label>
					<input
						type="text"
						placeholder="Search..."
						bind:value={searchQuery}
						class="search-input"
					/>
				</div>
			</div>

			<div class="instructions-table">
				<div class="table-header">
					<span class="col-index">#</span>
					<span class="col-opcode">Opcode</span>
					<span class="col-out">Out</span>
					<span class="col-inputs">Inputs</span>
					<span class="col-state">State ID</span>
				</div>

				<div class="table-body">
					{#each filteredInstructions as inst (inst.index)}
						<div class="table-row" class:stateful={inst.stateful}>
							<span class="col-index">{inst.index}</span>
							<span class="col-opcode">{inst.opcode}</span>
							<span class="col-out">b{inst.out}</span>
							<span class="col-inputs">{formatInputs(inst.inputs)}</span>
							<span class="col-state" class:has-state={inst.stateId !== 0}>
								{formatStateId(inst.stateId)}
							</span>
						</div>
					{/each}
				</div>
			</div>

			{#if filteredInstructions.length === 0 && disassembly.instructions.length > 0}
				<p class="no-results">No matching instructions</p>
			{/if}
		</section>
	{/if}
</div>

<style>
	.debug-panel {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-md);
		padding: var(--spacing-sm);
		font-size: 12px;
		height: 100%;
		min-height: 0;
		overflow: hidden;
	}

	.empty-state {
		display: flex;
		flex-direction: column;
		align-items: center;
		justify-content: center;
		padding: var(--spacing-xl);
		color: var(--text-muted);
		text-align: center;
	}

	.empty-state .hint {
		font-size: 11px;
		margin-top: var(--spacing-xs);
	}

	section {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
	}

	h3 {
		font-size: 11px;
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.05em;
		color: var(--text-secondary);
		margin: 0;
	}

	/* Summary Section */
	.summary-grid {
		display: grid;
		grid-template-columns: repeat(2, 1fr);
		gap: var(--spacing-xs);
	}

	.summary-item {
		display: flex;
		justify-content: space-between;
		padding: var(--spacing-xs) var(--spacing-sm);
		background: var(--bg-tertiary);
		border-radius: 4px;
	}

	.summary-item .label {
		color: var(--text-secondary);
	}

	.summary-item .value {
		font-weight: 600;
		color: var(--text-primary);
		font-family: var(--font-mono);
	}

	/* Warning Section */
	.warning-section {
		background: rgba(255, 100, 100, 0.1);
		border: 1px solid rgba(255, 100, 100, 0.3);
		border-radius: 6px;
		padding: var(--spacing-sm);
	}

	.warning-section h3 {
		color: var(--error-fg);
	}

	.warning-text {
		color: var(--text-secondary);
		font-size: 11px;
		margin: 0;
	}

	.collision-item {
		display: flex;
		gap: var(--spacing-sm);
		align-items: center;
		padding: var(--spacing-xs) 0;
	}

	.collision-item code {
		font-family: var(--font-mono);
		font-size: 10px;
		background: var(--bg-secondary);
		padding: 2px 4px;
		border-radius: 3px;
	}

	.collision-opcodes {
		color: var(--error-fg);
	}

	/* Opcode Distribution */
	.opcode-list {
		display: flex;
		flex-wrap: wrap;
		gap: var(--spacing-xs);
	}

	.opcode-item {
		display: flex;
		gap: var(--spacing-xs);
		padding: 2px 6px;
		background: var(--bg-tertiary);
		border-radius: 4px;
		font-family: var(--font-mono);
		font-size: 10px;
	}

	.opcode-name {
		color: var(--syntax-function);
	}

	.opcode-count {
		color: var(--text-muted);
	}

	/* Instructions Section */
	.instructions-section {
		flex: 1;
		min-height: 0;
	}

	.instructions-header {
		display: flex;
		justify-content: space-between;
		align-items: center;
	}

	.controls {
		display: flex;
		gap: var(--spacing-sm);
		align-items: center;
	}

	.filter-checkbox {
		display: flex;
		gap: var(--spacing-xs);
		align-items: center;
		font-size: 11px;
		color: var(--text-secondary);
		cursor: pointer;
	}

	.filter-checkbox input {
		margin: 0;
	}

	.search-input {
		padding: 4px 8px;
		font-size: 11px;
		border-radius: 4px;
		width: 100px;
	}

	/* Instructions Table */
	.instructions-table {
		display: flex;
		flex-direction: column;
		flex: 1;
		min-height: 0;
		border: 1px solid var(--border-muted);
		border-radius: 6px;
		overflow: hidden;
	}

	.table-header {
		display: flex;
		background: var(--bg-tertiary);
		padding: var(--spacing-xs) var(--spacing-sm);
		font-weight: 600;
		color: var(--text-secondary);
		font-size: 10px;
		text-transform: uppercase;
	}

	.table-body {
		flex: 1;
		min-height: 0;
		overflow-y: auto;
	}

	.table-row {
		display: flex;
		padding: var(--spacing-xs) var(--spacing-sm);
		border-top: 1px solid var(--border-muted);
		font-family: var(--font-mono);
		font-size: 11px;
	}

	.table-row:hover {
		background: var(--bg-hover);
	}

	.table-row.stateful {
		background: rgba(var(--accent-primary-rgb, 100, 149, 237), 0.05);
	}

	.col-index {
		width: 32px;
		color: var(--text-muted);
	}

	.col-opcode {
		flex: 1;
		color: var(--syntax-function);
	}

	.col-out {
		width: 40px;
		color: var(--syntax-variable);
	}

	.col-inputs {
		width: 100px;
		color: var(--text-secondary);
	}

	.col-state {
		width: 90px;
		color: var(--text-muted);
		font-size: 10px;
	}

	.col-state.has-state {
		color: var(--syntax-number);
	}

	.no-results {
		text-align: center;
		color: var(--text-muted);
		padding: var(--spacing-md);
		font-size: 11px;
	}
</style>
