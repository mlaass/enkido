/**
 * CodeMirror extension for real-time step highlighting
 *
 * Highlights the currently playing step in each pattern during playback.
 */

import {
	EditorView,
	Decoration,
	ViewPlugin
} from '@codemirror/view';
import type { DecorationSet, ViewUpdate } from '@codemirror/view';
import { StateField, StateEffect, RangeSet } from '@codemirror/state';
import { patternHighlightStore } from '$lib/stores/pattern-highlight.svelte';
import { audioEngine } from '$lib/stores/audio.svelte';

// Decoration style for active steps
const activeStepMark = Decoration.mark({
	class: 'cm-active-step'
});

/**
 * State effect to update active step decorations
 */
const setActiveSteps = StateEffect.define<Map<number, { docOffset: number; sourceOffset: number; sourceLength: number }>>();

/**
 * State field that manages active step decorations
 */
const stepHighlightField = StateField.define<DecorationSet>({
	create() {
		return Decoration.none;
	},

	update(decorations, tr) {
		for (const effect of tr.effects) {
			if (effect.is(setActiveSteps)) {
				return buildActiveStepDecorations(effect.value, tr.state.doc.length);
			}
		}
		// Map decorations through document changes
		return decorations.map(tr.changes);
	},

	provide: (f) => EditorView.decorations.from(f)
});

/**
 * Build decorations for active steps
 * @param activeSteps Map of state ID to step position info
 * @param docLength Current document length for bounds checking
 */
function buildActiveStepDecorations(
	activeSteps: Map<number, { docOffset: number; sourceOffset: number; sourceLength: number }>,
	docLength: number
): DecorationSet {
	const ranges: Array<{ from: number; to: number }> = [];

	for (const [_stateId, step] of activeSteps) {
		if (step.sourceLength > 0) {
			const from = step.docOffset + step.sourceOffset;
			const to = from + step.sourceLength;
			// Bounds check - skip invalid ranges
			if (from >= 0 && to <= docLength) {
				ranges.push({ from, to });
			}
		}
	}

	// Sort by position
	ranges.sort((a, b) => a.from - b.from);

	return RangeSet.of(
		ranges.map((r) => activeStepMark.range(r.from, r.to))
	);
}

/**
 * ViewPlugin that polls for active steps during playback
 *
 * Always polls, but only updates decorations when audio is playing.
 * This is necessary because ViewPlugin.update() only fires on editor changes,
 * not when external state (like audioEngine.isPlaying) changes.
 */
class StepHighlightPlugin {
	private view: EditorView;
	private rafId = 0;
	private isDestroyed = false;

	constructor(view: EditorView) {
		this.view = view;
		// Start polling immediately - we check isPlaying inside the loop
		this.startPolling();
	}

	// Note: update() is NOT used for polling control because it only fires
	// on editor changes, not when audioEngine.isPlaying changes

	destroy() {
		this.isDestroyed = true;
		if (this.rafId) {
			cancelAnimationFrame(this.rafId);
			this.rafId = 0;
		}
		patternHighlightStore.stopPolling();
	}

	private startPolling() {
		let wasPlaying = false;

		const poll = () => {
			if (this.isDestroyed) return;

			const isPlaying = audioEngine.isPlaying;

			// Start/stop the store's polling based on playback
			if (isPlaying && !wasPlaying) {
				patternHighlightStore.startPolling();
			} else if (!isPlaying && wasPlaying) {
				patternHighlightStore.stopPolling();
				// Clear decorations when stopping
				this.view.dispatch({
					effects: setActiveSteps.of(new Map())
				});
			}
			wasPlaying = isPlaying;

			// Only update decorations while playing
			if (isPlaying) {
				// Get active steps from store and build decorations
				const patterns = patternHighlightStore.getAllPatterns();
				const activeSteps = new Map<number, { docOffset: number; sourceOffset: number; sourceLength: number }>();

				for (const patternData of patterns) {
					const step = patternHighlightStore.getActiveStep(patternData.info.stateId);
					if (step && step.length > 0) {
						activeSteps.set(patternData.info.stateId, {
							docOffset: patternData.info.docOffset,
							sourceOffset: step.offset,
							sourceLength: step.length
						});
					}
				}

				// Dispatch effect to update decorations
				this.view.dispatch({
					effects: setActiveSteps.of(activeSteps)
				});
			}

			this.rafId = requestAnimationFrame(poll);
		};

		poll();
	}
}

const stepHighlightPlugin = ViewPlugin.fromClass(StepHighlightPlugin);

/**
 * Export the complete step highlight extension (field + plugin)
 */
export function stepHighlight() {
	return [stepHighlightField, stepHighlightPlugin];
}
