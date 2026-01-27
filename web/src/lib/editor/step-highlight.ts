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
				return buildActiveStepDecorations(effect.value);
			}
		}
		// Map decorations through document changes
		return decorations.map(tr.changes);
	},

	provide: (f) => EditorView.decorations.from(f)
});

/**
 * Build decorations for active steps
 */
function buildActiveStepDecorations(
	activeSteps: Map<number, { docOffset: number; sourceOffset: number; sourceLength: number }>
): DecorationSet {
	const ranges: Array<{ from: number; to: number }> = [];

	for (const [_stateId, step] of activeSteps) {
		if (step.sourceLength > 0) {
			const from = step.docOffset + step.sourceOffset;
			const to = from + step.sourceLength;
			ranges.push({ from, to });
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
 */
class StepHighlightPlugin {
	private view: EditorView;
	private rafId = 0;
	private isPolling = false;

	constructor(view: EditorView) {
		this.view = view;
	}

	update(_update: ViewUpdate) {
		// Start/stop polling based on playback state
		const shouldPoll = audioEngine.isPlaying;

		if (shouldPoll && !this.isPolling) {
			this.startPolling();
		} else if (!shouldPoll && this.isPolling) {
			this.stopPolling();
		}
	}

	destroy() {
		this.stopPolling();
	}

	private startPolling() {
		if (this.isPolling) return;
		this.isPolling = true;

		// Also start the store's polling for active steps
		patternHighlightStore.startPolling();

		const poll = () => {
			if (!this.isPolling) return;

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

			this.rafId = requestAnimationFrame(poll);
		};

		poll();
	}

	private stopPolling() {
		this.isPolling = false;
		if (this.rafId) {
			cancelAnimationFrame(this.rafId);
			this.rafId = 0;
		}

		// Stop the store's polling
		patternHighlightStore.stopPolling();

		// Clear active step decorations
		this.view.dispatch({
			effects: setActiveSteps.of(new Map())
		});
	}
}

const stepHighlightPlugin = ViewPlugin.fromClass(StepHighlightPlugin);

/**
 * Export the complete step highlight extension (field + plugin)
 */
export function stepHighlight() {
	return [stepHighlightField, stepHighlightPlugin];
}
