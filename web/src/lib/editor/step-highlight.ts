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
import { StateField, StateEffect, RangeSet, type Text } from '@codemirror/state';
import { patternHighlightStore } from '$lib/stores/pattern-highlight.svelte';
import { audioEngine } from '$lib/stores/audio.svelte';
import { charOffsetFromUtf8Byte } from '$lib/editor/akkado-shape-index';

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
				return buildActiveStepDecorations(effect.value, tr.state.doc);
			}
		}
		// Map decorations through document changes
		return decorations.map(tr.changes);
	},

	provide: (f) => EditorView.decorations.from(f)
});

/**
 * Build decorations for active steps.
 *
 * The compiler reports `docOffset` / `sourceOffset` / `sourceLength` as UTF-8
 * byte offsets, but CodeMirror positions are UTF-16 character offsets — they
 * are converted here against the current document text. Each entry in
 * `activeSteps` is one running sequence state, so several instances of the
 * same pattern literal (e.g. `.slow()`/`.fast()` copies, or a literal inside a
 * function called more than once) each contribute their own highlight.
 *
 * @param activeSteps Map of state ID to step position info (byte offsets)
 * @param doc Current document for byte→char conversion and bounds checking
 */
function buildActiveStepDecorations(
	activeSteps: Map<number, { docOffset: number; sourceOffset: number; sourceLength: number }>,
	doc: Text
): DecorationSet {
	if (activeSteps.size === 0) return Decoration.none;

	const text = doc.toString();
	const docLength = doc.length;
	const ranges: Array<{ from: number; to: number }> = [];

	for (const [_stateId, step] of activeSteps) {
		if (step.sourceLength <= 0) continue;
		const fromByte = step.docOffset + step.sourceOffset;
		const from = charOffsetFromUtf8Byte(text, fromByte);
		const to = charOffsetFromUtf8Byte(text, fromByte + step.sourceLength);
		// Bounds check - skip invalid ranges
		if (from >= 0 && to <= docLength && to > from) {
			ranges.push({ from, to });
		}
	}

	// Total ordering (from, then to) so RangeSet.of accepts the set, then drop
	// exact duplicates — two instances of one literal landing on the same step
	// would otherwise stack identical decorations.
	ranges.sort((a, b) => a.from - b.from || a.to - b.to);
	const deduped: Array<{ from: number; to: number }> = [];
	for (const r of ranges) {
		const last = deduped[deduped.length - 1];
		if (last && last.from === r.from && last.to === r.to) continue;
		deduped.push(r);
	}

	return RangeSet.of(deduped.map((r) => activeStepMark.range(r.from, r.to)));
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

	update(update: ViewUpdate) {
		if (update.docChanged) {
			const changedRanges: Array<[number, number]> = [];
			let firstChangeStartB = Infinity;
			update.changes.iterChangedRanges(
				(fromA: number, toA: number, fromB: number) => {
					changedRanges.push([fromA, toA]);
					if (fromB < firstChangeStartB) firstChangeStartB = fromB;
				}
			);
			patternHighlightStore.mapThroughChanges(
				(pos, assoc) => update.changes.mapPos(pos, assoc),
				changedRanges
			);
			if (firstChangeStartB !== Infinity) {
				const lineStart = update.state.doc.lineAt(firstChangeStartB).from;
				patternHighlightStore.freezeFrom(lineStart);
			}
		}
	}

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
				const frozenOffset = patternHighlightStore.frozenOffset;
				const activeSteps = new Map<number, { docOffset: number; sourceOffset: number; sourceLength: number }>();

				for (const patternData of patterns) {
					if (frozenOffset !== null && patternData.info.docOffset >= frozenOffset) continue;
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
