<script lang="ts">
	import { audioEngine } from '$lib/stores/audio.svelte';
	import type {
		PatternInfo,
		PatternDebugInfo,
		PatternDebugSequence,
		PatternDebugEvent,
		MiniAstNode
	} from '$lib/stores/audio.svelte';

	let patterns = $state<PatternInfo[]>([]);
	let selectedPatternIndex = $state<number | null>(null);
	let debugInfo = $state<PatternDebugInfo | null>(null);
	let activeTab = $state<'events' | 'sequences' | 'ast'>('events');
	let isLoading = $state(false);
	let expandedSequences = $state<Set<number>>(new Set([0]));
	let expandedAstNodes = $state<Set<string>>(new Set(['root']));

	// Fetch pattern list when disassembly changes
	$effect(() => {
		const dis = audioEngine.disassembly;
		if (dis) {
			loadPatternInfo();
		} else {
			patterns = [];
			selectedPatternIndex = null;
			debugInfo = null;
		}
	});

	async function loadPatternInfo() {
		const info = await audioEngine.getPatternInfo();
		patterns = info;
	}

	async function selectPattern(index: number) {
		if (index === selectedPatternIndex) return;
		selectedPatternIndex = index;
		isLoading = true;
		debugInfo = await audioEngine.getPatternDebug(index);
		isLoading = false;
		// Reset expanded state for new pattern
		expandedSequences = new Set([0]);
		expandedAstNodes = new Set(['root']);
	}

	function toggleSequence(id: number) {
		const newSet = new Set(expandedSequences);
		if (newSet.has(id)) {
			newSet.delete(id);
		} else {
			newSet.add(id);
		}
		expandedSequences = newSet;
	}

	function toggleAstNode(path: string) {
		const newSet = new Set(expandedAstNodes);
		if (newSet.has(path)) {
			newSet.delete(path);
		} else {
			newSet.add(path);
		}
		expandedAstNodes = newSet;
	}

	function formatValue(val: number): string {
		if (Number.isInteger(val)) return val.toString();
		return val.toFixed(3);
	}

	function getModeClass(mode: string): string {
		switch (mode) {
			case 'NORMAL':
				return 'mode-normal';
			case 'ALTERNATE':
				return 'mode-alternate';
			case 'RANDOM':
				return 'mode-random';
			default:
				return '';
		}
	}

	function getNodeTypeClass(type: string): string {
		if (type.startsWith('MiniAtom')) return 'node-atom';
		if (type.startsWith('MiniGroup')) return 'node-group';
		if (type.startsWith('MiniSequence')) return 'node-sequence';
		if (type.startsWith('MiniChoice')) return 'node-choice';
		if (type.startsWith('MiniEuclidean')) return 'node-euclidean';
		if (type.startsWith('MiniModified')) return 'node-modified';
		if (type.startsWith('MiniPolymeter') || type.startsWith('MiniPolyrhythm'))
			return 'node-poly';
		return 'node-default';
	}

	function handleSourceClick(offset: number, length: number) {
		window.dispatchEvent(
			new CustomEvent('nkido:instruction-highlight', {
				detail: { source: { offset, length, line: 0, column: 0 } }
			})
		);
	}

	// Flatten events from all sequences for the Events tab
	function getFlattenedEvents(): Array<PatternDebugEvent & { seqId: number }> {
		if (!debugInfo) return [];
		const events: Array<PatternDebugEvent & { seqId: number }> = [];
		for (const seq of debugInfo.sequences) {
			for (const evt of seq.events) {
				if (evt.type === 'DATA') {
					events.push({ ...evt, seqId: seq.id });
				}
			}
		}
		return events.sort((a, b) => a.time - b.time);
	}
</script>

<div class="pattern-debug-panel">
	{#if patterns.length === 0}
		<div class="empty-state">
			<p>No patterns in compiled code</p>
			<p class="hint">Use pat("...") to create patterns</p>
		</div>
	{:else}
		<!-- Pattern selector -->
		<div class="pattern-selector">
			<label for="pattern-select">Pattern:</label>
			<select
				id="pattern-select"
				onchange={(e) => selectPattern(parseInt((e.target as HTMLSelectElement).value))}
			>
				<option value="" disabled selected={selectedPatternIndex === null}>
					Select pattern...
				</option>
				{#each patterns as pat, i (pat.stateId)}
					<option value={i} selected={selectedPatternIndex === i}>
						Pattern {i} (cycle: {pat.cycleLength} beats)
					</option>
				{/each}
			</select>
		</div>

		{#if selectedPatternIndex !== null}
			<!-- Tab bar -->
			<div class="tab-bar">
				<button
					class="tab"
					class:active={activeTab === 'events'}
					onclick={() => (activeTab = 'events')}
				>
					Events
				</button>
				<button
					class="tab"
					class:active={activeTab === 'sequences'}
					onclick={() => (activeTab = 'sequences')}
				>
					Sequences
				</button>
				<button class="tab" class:active={activeTab === 'ast'} onclick={() => (activeTab = 'ast')}>
					AST
				</button>
			</div>

			{#if isLoading}
				<div class="loading">Loading...</div>
			{:else if debugInfo}
				<div class="tab-content">
					{#if activeTab === 'events'}
						<!-- Flattened events table -->
						<div class="events-table">
							<div class="table-header">
								<span class="col-time">Time</span>
								<span class="col-duration">Duration</span>
								<span class="col-value">Value</span>
								<span class="col-chance">Chance</span>
								<span class="col-source">Source</span>
							</div>
							<div class="table-body">
								{#each getFlattenedEvents() as evt, i (i)}
									<button
										class="table-row"
										class:clickable={evt.sourceLength > 0}
										onclick={() =>
											evt.sourceLength > 0 &&
											handleSourceClick(evt.sourceOffset, evt.sourceLength)}
									>
										<span class="col-time">{formatValue(evt.time)}</span>
										<span class="col-duration">{formatValue(evt.duration)}</span>
										<span class="col-value">
											{#if evt.values && evt.numValues}
												{evt.values.slice(0, evt.numValues).map(formatValue).join(', ')}
											{:else}
												-
											{/if}
										</span>
										<span class="col-chance">{formatValue(evt.chance)}</span>
										<span class="col-source">
											{#if evt.sourceLength > 0}
												{evt.sourceOffset}:{evt.sourceLength}
											{:else}
												-
											{/if}
										</span>
									</button>
								{/each}
							</div>
						</div>
					{:else if activeTab === 'sequences'}
						<!-- Sequence hierarchy -->
						<div class="sequences-list">
							{#each debugInfo.sequences as seq (seq.id)}
								<div class="sequence-item">
									<button class="sequence-header" onclick={() => toggleSequence(seq.id)}>
										<span class="expand-icon">
											{expandedSequences.has(seq.id) ? '▼' : '▶'}
										</span>
										<span class="seq-id">Seq {seq.id}</span>
										<span class="mode-badge {getModeClass(seq.mode)}">{seq.mode}</span>
										<span class="seq-info">
											{seq.events.length} events, dur: {formatValue(seq.duration)}
										</span>
									</button>

									{#if expandedSequences.has(seq.id)}
										<div class="sequence-events">
											{#each seq.events as evt, i (i)}
												<button
													class="event-item"
													class:sub-seq={evt.type === 'SUB_SEQ'}
													class:clickable={evt.sourceLength > 0}
													onclick={() =>
														evt.sourceLength > 0 &&
														handleSourceClick(evt.sourceOffset, evt.sourceLength)}
												>
													<span class="event-type">{evt.type}</span>
													<span class="event-time">t:{formatValue(evt.time)}</span>
													<span class="event-dur">d:{formatValue(evt.duration)}</span>
													{#if evt.type === 'DATA'}
														<span class="event-val">
															{evt.values?.slice(0, evt.numValues).map(formatValue).join(', ')}
														</span>
													{:else}
														<span class="event-ref">→ Seq {evt.seqId}</span>
													{/if}
													{#if evt.chance < 1}
														<span class="event-chance">?{formatValue(evt.chance)}</span>
													{/if}
												</button>
											{/each}
										</div>
									{/if}
								</div>
							{/each}
						</div>
					{:else if activeTab === 'ast'}
						<!-- AST tree view -->
						{#if debugInfo.ast}
							<div class="ast-tree">
								{#snippet renderAstNode(node: MiniAstNode, path: string, depth: number)}
									<div class="ast-node" style="--depth: {depth}">
										<button
											class="node-header"
											class:clickable={node.location && node.location.length > 0}
											onclick={() => {
												if (node.children && node.children.length > 0) {
													toggleAstNode(path);
												}
												if (node.location && node.location.length > 0) {
													handleSourceClick(node.location.offset, node.location.length);
												}
											}}
										>
											{#if node.children && node.children.length > 0}
												<span class="expand-icon">
													{expandedAstNodes.has(path) ? '▼' : '▶'}
												</span>
											{:else}
												<span class="expand-icon leaf">•</span>
											{/if}
											<span class="node-type {getNodeTypeClass(node.type)}">{node.type}</span>

											<!-- Node-specific info -->
											{#if node.kind}
												<span class="node-detail">{node.kind}</span>
											{/if}
											{#if node.midi !== undefined}
												<span class="node-value">MIDI:{node.midi}</span>
											{/if}
											{#if node.sampleName}
												<span class="node-value">{node.sampleName}</span>
											{/if}
											{#if node.modifier}
												<span class="node-detail">{node.modifier}={node.value}</span>
											{/if}
											{#if node.hits !== undefined}
												<span class="node-detail">
													({node.hits},{node.steps},{node.rotation})
												</span>
											{/if}
										</button>

										{#if node.children && node.children.length > 0 && expandedAstNodes.has(path)}
											<div class="ast-children">
												{#each node.children as child, i (i)}
													{@render renderAstNode(child, `${path}/${i}`, depth + 1)}
												{/each}
											</div>
										{/if}
									</div>
								{/snippet}
								{@render renderAstNode(debugInfo.ast, 'root', 0)}
							</div>
						{:else}
							<div class="no-ast">AST not available</div>
						{/if}
					{/if}
				</div>

				<!-- Summary -->
				<div class="summary">
					<span>Cycle: {debugInfo.cycleLength} beats</span>
					<span>{debugInfo.isSamplePattern ? 'Sample' : 'Pitch'} pattern</span>
					<span>{debugInfo.sequences.length} sequences</span>
				</div>
			{/if}
		{/if}
	{/if}
</div>

<style>
	.pattern-debug-panel {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-sm);
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

	.pattern-selector {
		display: flex;
		align-items: center;
		gap: var(--spacing-sm);
	}

	.pattern-selector label {
		color: var(--text-secondary);
		font-size: 11px;
	}

	.pattern-selector select {
		flex: 1;
		padding: 4px 8px;
		font-size: 11px;
		border-radius: 4px;
		background: var(--bg-secondary);
		border: 1px solid var(--border-muted);
		color: var(--text-primary);
	}

	.tab-bar {
		display: flex;
		gap: 2px;
		background: var(--bg-tertiary);
		border-radius: 6px;
		padding: 2px;
	}

	.tab {
		flex: 1;
		padding: 6px 12px;
		border: none;
		background: transparent;
		color: var(--text-secondary);
		font-size: 11px;
		font-weight: 500;
		border-radius: 4px;
		cursor: pointer;
		transition:
			background 0.15s,
			color 0.15s;
	}

	.tab:hover {
		color: var(--text-primary);
	}

	.tab.active {
		background: var(--bg-secondary);
		color: var(--text-primary);
	}

	.loading {
		text-align: center;
		padding: var(--spacing-md);
		color: var(--text-muted);
	}

	.tab-content {
		flex: 1;
		min-height: 0;
		overflow: auto;
	}

	/* Events table */
	.events-table {
		display: flex;
		flex-direction: column;
		border: 1px solid var(--border-muted);
		border-radius: 6px;
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
		overflow-y: auto;
	}

	.table-row {
		display: flex;
		width: 100%;
		padding: var(--spacing-xs) var(--spacing-sm);
		border: none;
		border-top: 1px solid var(--border-muted);
		background: transparent;
		font-family: var(--font-mono);
		font-size: 11px;
		text-align: left;
		cursor: default;
	}

	.table-row.clickable {
		cursor: pointer;
	}

	.table-row:hover {
		background: var(--bg-hover);
	}

	.col-time {
		width: 60px;
		flex-shrink: 0;
	}
	.col-duration {
		width: 60px;
		flex-shrink: 0;
	}
	.col-value {
		flex: 1;
		min-width: 0;
		overflow: hidden;
		text-overflow: ellipsis;
		white-space: nowrap;
	}
	.col-chance {
		width: 50px;
		flex-shrink: 0;
		text-align: right;
	}
	.col-source {
		width: 70px;
		flex-shrink: 0;
		text-align: right;
		color: var(--text-muted);
	}

	/* Sequences list */
	.sequences-list {
		display: flex;
		flex-direction: column;
		gap: var(--spacing-xs);
	}

	.sequence-item {
		border: 1px solid var(--border-muted);
		border-radius: 6px;
		overflow: hidden;
	}

	.sequence-header {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		width: 100%;
		padding: var(--spacing-xs) var(--spacing-sm);
		border: none;
		background: var(--bg-tertiary);
		color: var(--text-primary);
		font-size: 11px;
		text-align: left;
		cursor: pointer;
	}

	.sequence-header:hover {
		background: var(--bg-hover);
	}

	.expand-icon {
		width: 12px;
		font-size: 8px;
		color: var(--text-muted);
	}

	.expand-icon.leaf {
		color: var(--text-muted);
	}

	.seq-id {
		font-weight: 600;
		font-family: var(--font-mono);
	}

	.mode-badge {
		padding: 1px 4px;
		border-radius: 3px;
		font-size: 9px;
		font-weight: 600;
		text-transform: uppercase;
	}

	.mode-normal {
		background: rgba(100, 200, 100, 0.2);
		color: var(--success-fg);
	}
	.mode-alternate {
		background: rgba(100, 150, 255, 0.2);
		color: var(--accent-primary);
	}
	.mode-random {
		background: rgba(255, 150, 100, 0.2);
		color: var(--warning-fg);
	}

	.seq-info {
		color: var(--text-muted);
		font-size: 10px;
		margin-left: auto;
	}

	.sequence-events {
		display: flex;
		flex-direction: column;
	}

	.event-item {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		padding: 4px var(--spacing-sm);
		padding-left: 24px;
		border: none;
		border-top: 1px solid var(--border-muted);
		background: transparent;
		font-family: var(--font-mono);
		font-size: 10px;
		text-align: left;
		cursor: default;
	}

	.event-item.clickable {
		cursor: pointer;
	}

	.event-item:hover {
		background: var(--bg-hover);
	}

	.event-item.sub-seq {
		background: rgba(100, 150, 255, 0.05);
	}

	.event-type {
		width: 48px;
		color: var(--text-secondary);
	}

	.event-time,
	.event-dur {
		width: 50px;
		color: var(--syntax-number);
	}

	.event-val {
		flex: 1;
		color: var(--syntax-string);
	}

	.event-ref {
		color: var(--accent-primary);
	}

	.event-chance {
		color: var(--warning-fg);
	}

	/* AST tree */
	.ast-tree {
		font-family: var(--font-mono);
		font-size: 11px;
	}

	.ast-node {
		padding-left: calc(var(--depth, 0) * 12px);
	}

	.node-header {
		display: flex;
		align-items: center;
		gap: var(--spacing-xs);
		width: 100%;
		padding: 3px 4px;
		border: none;
		background: transparent;
		text-align: left;
		cursor: default;
		border-radius: 3px;
	}

	.node-header.clickable {
		cursor: pointer;
	}

	.node-header:hover {
		background: var(--bg-hover);
	}

	.node-type {
		padding: 1px 4px;
		border-radius: 3px;
		font-size: 10px;
		font-weight: 600;
	}

	.node-atom {
		background: rgba(100, 200, 100, 0.2);
		color: var(--success-fg);
	}
	.node-group {
		background: rgba(200, 200, 100, 0.2);
		color: var(--warning-fg);
	}
	.node-sequence {
		background: rgba(100, 150, 255, 0.2);
		color: var(--accent-primary);
	}
	.node-choice {
		background: rgba(255, 100, 100, 0.2);
		color: var(--error-fg);
	}
	.node-euclidean {
		background: rgba(200, 100, 255, 0.2);
		color: var(--syntax-function);
	}
	.node-modified {
		background: rgba(100, 200, 255, 0.2);
		color: var(--syntax-variable);
	}
	.node-poly {
		background: rgba(255, 200, 100, 0.2);
		color: var(--syntax-number);
	}
	.node-default {
		background: var(--bg-tertiary);
		color: var(--text-secondary);
	}

	.node-detail {
		color: var(--text-secondary);
		font-size: 10px;
	}

	.node-value {
		color: var(--syntax-number);
	}

	.ast-children {
		border-left: 1px solid var(--border-muted);
		margin-left: 6px;
	}

	.no-ast {
		text-align: center;
		padding: var(--spacing-md);
		color: var(--text-muted);
	}

	.summary {
		display: flex;
		gap: var(--spacing-md);
		padding: var(--spacing-xs) var(--spacing-sm);
		background: var(--bg-tertiary);
		border-radius: 4px;
		font-size: 10px;
		color: var(--text-muted);
	}
</style>
