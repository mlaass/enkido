/**
 * Pattern highlighting state store using Svelte 5 runes
 *
 * Manages pattern preview data and active step highlighting for the editor.
 */

import { audioEngine, type PatternInfo, type PatternEvent } from './audio.svelte';

interface PatternData {
	info: PatternInfo;
	events: PatternEvent[];
}

interface ActiveStep {
	offset: number;
	length: number;
}

function createPatternHighlightStore() {
	// Map of stateId -> pattern data (info + events)
	let patterns = $state<Map<number, PatternData>>(new Map());

	// Map of stateId -> active step source range
	let activeSteps = $state<Map<number, ActiveStep>>(new Map());

	// Incremented when patterns change (triggers decoration rebuild)
	let patternsVersion = $state(0);

	// Polling state
	let pollRafId = 0;
	let isPolling = false;

	/**
	 * Update pattern previews from compile result
	 * Called after successful compilation
	 */
	async function updatePreviews() {
		const infos = await audioEngine.getPatternInfo();
		const newPatterns = new Map<number, PatternData>();

		for (let i = 0; i < infos.length; i++) {
			const info = infos[i];
			// Query events for 4 cycles to handle alternating patterns
			const events = await audioEngine.queryPatternPreview(i, 0, info.cycleLength * 4);

			newPatterns.set(info.stateId, { info, events });
		}

		patterns = newPatterns;
		patternsVersion++;
	}

	/**
	 * Clear all pattern data
	 */
	function clear() {
		patterns = new Map();
		activeSteps = new Map();
		patternsVersion++;
	}

	/**
	 * Start polling for active steps at 60fps
	 */
	function startPolling() {
		if (isPolling) return;
		isPolling = true;

		const poll = async () => {
			if (!isPolling) return;

			// Get state IDs for all patterns
			const stateIds = Array.from(patterns.keys());
			if (stateIds.length > 0) {
				const steps = await audioEngine.getActiveSteps(stateIds);
				// Convert to Map
				const newActiveSteps = new Map<number, ActiveStep>();
				for (const [stateId, step] of Object.entries(steps)) {
					newActiveSteps.set(Number(stateId), step);
				}
				activeSteps = newActiveSteps;
			}

			pollRafId = requestAnimationFrame(poll);
		};

		poll();
	}

	/**
	 * Stop polling for active steps
	 */
	function stopPolling() {
		isPolling = false;
		if (pollRafId) {
			cancelAnimationFrame(pollRafId);
			pollRafId = 0;
		}
		activeSteps = new Map();
	}

	/**
	 * Get pattern data by state ID
	 */
	function getPattern(stateId: number): PatternData | undefined {
		return patterns.get(stateId);
	}

	/**
	 * Get all patterns as array
	 */
	function getAllPatterns(): PatternData[] {
		return Array.from(patterns.values());
	}

	/**
	 * Get active step for a pattern
	 */
	function getActiveStep(stateId: number): ActiveStep | undefined {
		return activeSteps.get(stateId);
	}

	return {
		get patterns() {
			return patterns;
		},
		get activeSteps() {
			return activeSteps;
		},
		get patternsVersion() {
			return patternsVersion;
		},
		updatePreviews,
		clear,
		startPolling,
		stopPolling,
		getPattern,
		getAllPatterns,
		getActiveStep
	};
}

export const patternHighlightStore = createPatternHighlightStore();
