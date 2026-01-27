/**
 * CodeMirror extension for inline piano roll pattern previews
 *
 * Displays a scrolling mini piano roll after each pattern string,
 * showing pattern events with a playhead during playback.
 */

import {
	EditorView,
	Decoration,
	WidgetType,
	ViewPlugin
} from '@codemirror/view';
import type { DecorationSet, ViewUpdate } from '@codemirror/view';
import { RangeSet } from '@codemirror/state';
import { patternHighlightStore } from '$lib/stores/pattern-highlight.svelte';
import { audioEngine } from '$lib/stores/audio.svelte';
import type { PatternEvent } from '$lib/stores/audio.svelte';

/**
 * Piano roll widget that displays pattern events
 */
class PianoRollWidget extends WidgetType {
	private events: PatternEvent[];
	private cycleLength: number;
	private stateId: number;
	private canvas: HTMLCanvasElement | null = null;
	private rafId = 0;

	constructor(events: PatternEvent[], cycleLength: number, stateId: number) {
		super();
		this.events = events;
		this.cycleLength = cycleLength;
		this.stateId = stateId;
	}

	eq(other: PianoRollWidget): boolean {
		return (
			this.stateId === other.stateId &&
			this.cycleLength === other.cycleLength &&
			this.events.length === other.events.length
		);
	}

	toDOM(): HTMLElement {
		const container = document.createElement('span');
		container.className = 'pattern-preview';
		container.style.cssText = `
			display: inline-block;
			margin-left: 8px;
			vertical-align: middle;
			border-radius: 4px;
			overflow: hidden;
			background: var(--bg-secondary, #1a1a1a);
			border: 1px solid var(--border-primary, #333);
		`;

		const canvas = document.createElement('canvas');
		canvas.width = 200;
		canvas.height = 32;
		canvas.style.cssText = 'display: block;';
		container.appendChild(canvas);

		this.canvas = canvas;
		this.startRenderLoop();

		return container;
	}

	destroy(): void {
		if (this.rafId) {
			cancelAnimationFrame(this.rafId);
			this.rafId = 0;
		}
		this.canvas = null;
	}

	private startRenderLoop(): void {
		const render = async () => {
			if (!this.canvas) return;

			const isPlaying = audioEngine.isPlaying;
			const beatPos = isPlaying ? await audioEngine.getCurrentBeatPosition() : 0;

			this.renderPianoRoll(beatPos, isPlaying);

			if (isPlaying) {
				this.rafId = requestAnimationFrame(render);
			}
		};

		render();
	}

	/**
	 * Convert frequency to Y position using MIDI-based pitch mapping.
	 * Maps MIDI range 36-84 (C2-C6) to canvas height.
	 */
	private freqToY(freq: number, height: number): number {
		// Convert frequency to MIDI note number (A4 = 440Hz = MIDI 69)
		const midi = 12 * Math.log2(freq / 440) + 69;
		// Map MIDI 36-84 (C2-C6, 4 octaves) to canvas height
		const normalized = Math.max(0, Math.min(1, (midi - 36) / 48));
		return height - 4 - normalized * (height - 8);
	}

	private renderPianoRoll(beatPos: number, isPlaying: boolean): void {
		const canvas = this.canvas;
		if (!canvas) return;

		const ctx = canvas.getContext('2d');
		if (!ctx) return;

		const w = canvas.width;
		const h = canvas.height;

		// Calculate visible window (1+ cycle centered on playhead when playing)
		const windowBeats = Math.max(4, this.cycleLength);
		const windowStart = isPlaying ? Math.max(0, beatPos - windowBeats * 0.25) : 0;
		const windowEnd = windowStart + windowBeats;

		// Clear with background
		ctx.fillStyle = getComputedStyle(canvas).getPropertyValue('--bg-secondary') || '#1a1a1a';
		ctx.fillRect(0, 0, w, h);

		// Draw beat grid
		ctx.strokeStyle = getComputedStyle(canvas).getPropertyValue('--border-primary') || '#333';
		ctx.lineWidth = 1;
		for (let beat = Math.ceil(windowStart); beat < windowEnd; beat++) {
			const x = ((beat - windowStart) / (windowEnd - windowStart)) * w;
			ctx.beginPath();
			ctx.moveTo(x, 0);
			ctx.lineTo(x, h);
			ctx.stroke();
		}

		// Draw events
		const accentColor = getComputedStyle(canvas).getPropertyValue('--accent-primary') || '#4ade80';
		const dimColor = getComputedStyle(canvas).getPropertyValue('--text-tertiary') || '#666';

		for (const evt of this.events) {
			// Handle looping - draw events at cycle multiples
			const cycleTime = evt.time % this.cycleLength;
			const maxCycle = Math.ceil(windowEnd / this.cycleLength) + 1;

			for (let cycle = -1; cycle <= maxCycle; cycle++) {
				const evtTime = cycleTime + cycle * this.cycleLength;
				if (evtTime >= windowStart && evtTime < windowEnd) {
					const x = ((evtTime - windowStart) / (windowEnd - windowStart)) * w;
					const evtW = Math.max((evt.duration / (windowEnd - windowStart)) * w, 3);

					// Map frequency to y position using MIDI-based pitch mapping
					const y = this.freqToY(evt.value, h);

					// Highlight past events differently
					ctx.fillStyle = evtTime < beatPos ? dimColor : accentColor;
					ctx.fillRect(x, y, evtW, 4);
				}
			}
		}

		// Draw playhead when playing
		if (isPlaying) {
			const playheadBeat = beatPos % this.cycleLength + Math.floor(windowStart / this.cycleLength) * this.cycleLength;
			// Find closest playhead position in view
			for (let offset = -this.cycleLength; offset <= this.cycleLength; offset += this.cycleLength) {
				const ph = playheadBeat + offset;
				if (ph >= windowStart && ph < windowEnd) {
					const x = ((ph - windowStart) / (windowEnd - windowStart)) * w;
					ctx.strokeStyle = getComputedStyle(canvas).getPropertyValue('--accent-secondary') || '#fb923c';
					ctx.lineWidth = 2;
					ctx.beginPath();
					ctx.moveTo(x, 0);
					ctx.lineTo(x, h);
					ctx.stroke();
					break;
				}
			}
		}
	}
}

/**
 * Track last patterns version to detect changes
 */
let lastPatternsVersion = -1;

/**
 * Build decorations for pattern previews
 */
function buildDecorations(view: EditorView): DecorationSet {
	const widgets: Array<{ from: number; to: number; decoration: Decoration }> = [];
	const patterns = patternHighlightStore.getAllPatterns();

	for (const patternData of patterns) {
		const { info, events } = patternData;
		// Place widget at end of pattern string
		const pos = Math.min(info.docOffset + info.docLength, view.state.doc.length);

		if (pos > 0 && pos <= view.state.doc.length) {
			widgets.push({
				from: pos,
				to: pos,
				decoration: Decoration.widget({
					widget: new PianoRollWidget(events, info.cycleLength, info.stateId),
					side: 1
				})
			});
		}
	}

	// Sort by position and create RangeSet
	widgets.sort((a, b) => a.from - b.from);
	return RangeSet.of(
		widgets.map((w) => w.decoration.range(w.from))
	);
}

/**
 * ViewPlugin that manages pattern preview widgets
 */
export const patternPreviewPlugin = ViewPlugin.fromClass(
	class {
		decorations: DecorationSet;

		constructor(view: EditorView) {
			this.decorations = buildDecorations(view);
			lastPatternsVersion = patternHighlightStore.patternsVersion;
		}

		update(update: ViewUpdate) {
			// Rebuild if document changed or patterns version changed
			if (
				update.docChanged ||
				patternHighlightStore.patternsVersion !== lastPatternsVersion
			) {
				this.decorations = buildDecorations(update.view);
				lastPatternsVersion = patternHighlightStore.patternsVersion;
			}
		}
	},
	{
		decorations: (v) => v.decorations
	}
);

/**
 * Export the extension
 */
export function patternPreview() {
	return patternPreviewPlugin;
}
