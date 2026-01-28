/**
 * CodeMirror extension for instruction-to-source highlighting
 *
 * Highlights the source code corresponding to a selected bytecode instruction
 * in the debug panel. Uses underline decoration to show source location.
 */

import { EditorView, Decoration } from '@codemirror/view';
import type { DecorationSet } from '@codemirror/view';
import { StateField, StateEffect, RangeSet } from '@codemirror/state';
import type { SourceLocation } from '$lib/stores/audio.svelte';

// Decoration style for highlighted instruction source
const instructionHighlightMark = Decoration.mark({
	class: 'cm-instruction-highlight'
});

/**
 * State effect to set instruction highlight
 * Pass null to clear the highlight
 */
export const setInstructionHighlight = StateEffect.define<SourceLocation | null>();

/**
 * State field that manages instruction highlight decoration
 */
const instructionHighlightField = StateField.define<DecorationSet>({
	create() {
		return Decoration.none;
	},

	update(decorations, tr) {
		for (const effect of tr.effects) {
			if (effect.is(setInstructionHighlight)) {
				if (effect.value === null) {
					return Decoration.none;
				}

				const loc = effect.value;
				const docLength = tr.state.doc.length;

				// Calculate positions from source location
				const from = loc.offset;
				const to = loc.offset + loc.length;

				// Bounds check
				if (from < 0 || to > docLength || from >= to) {
					return Decoration.none;
				}

				return RangeSet.of([instructionHighlightMark.range(from, to)]);
			}
		}
		// Map decorations through document changes
		return decorations.map(tr.changes);
	},

	provide: (f) => EditorView.decorations.from(f)
});

/**
 * Export the instruction highlight extension
 */
export function instructionHighlight() {
	return [instructionHighlightField];
}

/**
 * Helper to dispatch instruction highlight to an EditorView
 */
export function highlightInstruction(view: EditorView, source: SourceLocation | null) {
	view.dispatch({
		effects: setInstructionHighlight.of(source)
	});

	// If highlighting, also scroll the source into view
	if (source && source.offset >= 0 && source.length > 0) {
		const from = source.offset;
		const to = source.offset + source.length;
		view.dispatch({
			effects: EditorView.scrollIntoView(from, {
				y: 'center'
			})
		});
	}
}
