/**
 * Waterfall (Spectrogram) Visualization Renderer
 *
 * Uses an offscreen canvas with full FFT bin resolution on the frequency axis
 * and display-native resolution on the time axis. The offscreen always scrolls
 * left-to-right, then is drawn rotated onto the visible canvas for arbitrary
 * scroll angles. The browser's image scaling handles frequency downsampling
 * smoothly.
 */

import type { VisualizationRenderer } from './registry';
import { registerRenderer } from './registry';
import { VizType, type VizDecl } from '$lib/stores/audio.svelte';
import { audioEngine } from '$lib/stores/audio.svelte';
import { GRADIENT_PRESETS, DEFAULT_GRADIENT, type GradientLUT } from './gradients';

interface WaterfallState {
	canvas: HTMLCanvasElement;
	ctx: CanvasRenderingContext2D;
	// Offscreen: time axis (X) at display resolution, freq axis (Y) at full bin resolution
	offCanvas: HTMLCanvasElement;
	offCtx: CanvasRenderingContext2D;
	offImageData: ImageData;
	offWidth: number;   // time axis pixels (display-scaled)
	offHeight: number;  // = binCount (full FFT resolution)
	// Dimensions for drawing the rotated offscreen onto visible canvas
	drawWidth: number;  // time axis length in visible pixels
	drawHeight: number; // freq axis length in visible pixels
	rotation: number;
	gradientLUT: GradientLUT;
	lastFrameCounter: number;
	lastUpdateTime: number;
	scrollAccumulator: number;
	speed: number;
	minDb: number;
	maxDb: number;
	dpr: number;
	cacheKey: string;
	// Geometry inputs needed to recompute dimensions on container resize
	angle: number;
	binCount: number;
	resizeObserver: ResizeObserver | null;
}

const stateMap = new WeakMap<HTMLElement, WaterfallState>();

// Persist offscreen canvas across recompilations, keyed by viz name.
// drawImage handles resampling when dimensions change (FFT size, angle, etc.)
const offscreenCache = new Map<string, HTMLCanvasElement>();

const DEFAULT_WIDTH = 300;
const DEFAULT_HEIGHT = 150;
const LABEL_HEIGHT = 18;

/**
 * Compute the rotation-aware canvas and offscreen dimensions for a given
 * visible size. The offscreen's time axis must be long enough to cover the
 * visible area once rotated by `angle`; its freq axis is always `binCount`.
 */
function computeGeometry(
	width: number,
	canvasHeight: number,
	angle: number,
	binCount: number,
	dpr: number
) {
	const rad = angle * Math.PI / 180;
	const absCos = Math.abs(Math.cos(rad));
	const absSin = Math.abs(Math.sin(rad));
	const timeAxisPx = Math.ceil((width * absCos + canvasHeight * absSin) * dpr);
	const freqAxisPx = Math.ceil((width * absSin + canvasHeight * absCos) * dpr);
	return {
		canvasW: Math.round(width * dpr),
		canvasH: Math.round(canvasHeight * dpr),
		timeAxisPx,
		freqAxisPx,
		offWidth: timeAxisPx,
		offHeight: binCount
	};
}

/**
 * Resize a live waterfall to a new visible size, rebuilding the visible canvas
 * and the offscreen buffer. Existing spectral history is resampled via
 * drawImage so the display stays continuous across container resizes.
 */
function resizeWaterfall(state: WaterfallState, newWidth: number, newCanvasHeight: number): void {
	const geo = computeGeometry(newWidth, newCanvasHeight, state.angle, state.binCount, state.dpr);

	// Resize the visible canvas
	state.canvas.width = geo.canvasW;
	state.canvas.height = geo.canvasH;
	state.canvas.style.width = `${newWidth}px`;
	state.canvas.style.height = `${newCanvasHeight}px`;
	state.ctx.fillStyle = '#000';
	state.ctx.fillRect(0, 0, geo.canvasW, geo.canvasH);

	// Rebuild the offscreen at the new time-axis width, resampling history.
	const newOff = document.createElement('canvas');
	newOff.width = geo.offWidth;
	newOff.height = geo.offHeight;
	const newOffCtx = newOff.getContext('2d')!;
	newOffCtx.drawImage(state.offCanvas, 0, 0, geo.offWidth, geo.offHeight);

	state.offCanvas = newOff;
	state.offCtx = newOffCtx;
	state.offImageData = newOffCtx.getImageData(0, 0, geo.offWidth, geo.offHeight);
	state.offWidth = geo.offWidth;
	state.offHeight = geo.offHeight;
	state.drawWidth = geo.timeAxisPx;
	state.drawHeight = geo.freqAxisPx;
}

const waterfallRenderer: VisualizationRenderer = {
	create(viz: VizDecl): HTMLElement {
		const opts = viz.options || {};
		const isRelativeWidth = typeof opts.width === 'string';
		const isRelativeHeight = typeof opts.height === 'string';
		const width = isRelativeWidth ? DEFAULT_WIDTH : ((opts.width as number) ?? DEFAULT_WIDTH);
		const height = isRelativeHeight ? DEFAULT_HEIGHT : ((opts.height as number) ?? DEFAULT_HEIGHT);
		const canvasHeight = height - LABEL_HEIGHT;

		const angle = (opts.angle as number) ?? 180;
		const speed = (opts.speed as number) ?? 40;
		const fftSize = (opts.fft as number) ?? 1024;
		const gradientName = (opts.gradient as string) ?? DEFAULT_GRADIENT;
		const minDb = (opts.minDb as number) ?? -90;
		const maxDb = (opts.maxDb as number) ?? 0;

		const container = document.createElement('div');
		container.className = 'viz-waterfall';
		container.style.width = isRelativeWidth ? '100%' : `${width}px`;
		container.style.height = isRelativeHeight ? '100%' : `${height}px`;

		const label = document.createElement('div');
		label.className = 'viz-label';
		label.textContent = viz.name;
		container.appendChild(label);

		// Visible canvas at device pixel ratio
		const dpr = window.devicePixelRatio || 1;
		const canvas = document.createElement('canvas');
		canvas.width = Math.round(width * dpr);
		canvas.height = Math.round(canvasHeight * dpr);
		canvas.style.width = `${width}px`;
		canvas.style.height = `${canvasHeight}px`;
		container.appendChild(canvas);

		const ctx = canvas.getContext('2d')!;
		ctx.fillStyle = '#000';
		ctx.fillRect(0, 0, canvas.width, canvas.height);

		// Compute the time/freq axis lengths needed to cover the visible area
		// after rotation. Offscreen: time axis at display resolution, freq axis
		// at full FFT bin count.
		const binCount = fftSize / 2 + 1;
		const { timeAxisPx, freqAxisPx, offWidth, offHeight } =
			computeGeometry(width, canvasHeight, angle, binCount, dpr);

		const offCanvas = document.createElement('canvas');
		offCanvas.width = offWidth;
		offCanvas.height = offHeight;
		const offCtx = offCanvas.getContext('2d')!;

		// Restore from cache (drawImage resamples if dimensions changed)
		const cacheKey = viz.name;
		const cached = offscreenCache.get(cacheKey);
		if (cached) {
			offCtx.drawImage(cached, 0, 0, offWidth, offHeight);
		} else {
			offCtx.fillStyle = '#000';
			offCtx.fillRect(0, 0, offWidth, offHeight);
		}
		const offImageData = offCtx.getImageData(0, 0, offWidth, offHeight);

		const gradientLUT = GRADIENT_PRESETS[gradientName] ?? GRADIENT_PRESETS[DEFAULT_GRADIENT];
		const rotation = -angle * Math.PI / 180;

		// Attach a ResizeObserver for relative ("100%") sizing so the canvas
		// and offscreen buffer track the container's actual size.
		let resizeObserver: ResizeObserver | null = null;
		if (isRelativeWidth || isRelativeHeight) {
			resizeObserver = new ResizeObserver(entries => {
				const st = stateMap.get(container);
				if (!st) return;
				for (const entry of entries) {
					const rect = entry.contentRect;
					const newWidth = isRelativeWidth ? rect.width : width;
					const newHeight = isRelativeHeight ? rect.height : height;
					const newCanvasHeight = newHeight - LABEL_HEIGHT;
					if (newWidth <= 0 || newCanvasHeight <= 0) return;
					resizeWaterfall(st, newWidth, newCanvasHeight);
				}
			});
			resizeObserver.observe(container);
		}

		stateMap.set(container, {
			canvas,
			ctx,
			offCanvas,
			offCtx,
			offImageData,
			offWidth,
			offHeight,
			drawWidth: timeAxisPx,
			drawHeight: freqAxisPx,
			rotation,
			gradientLUT,
			lastFrameCounter: 0,
			lastUpdateTime: 0,
			scrollAccumulator: 0,
			speed,
			minDb,
			maxDb,
			dpr,
			cacheKey,
			angle,
			binCount,
			resizeObserver
		});

		return container;
	},

	update(element: HTMLElement, viz: VizDecl, _beatPos: number, isPlaying: boolean): void {
		const state = stateMap.get(element);
		if (!state) return;

		const now = performance.now();
		if (now - state.lastUpdateTime < 33) return;

		if (!isPlaying || !viz.stateId) {
			state.lastUpdateTime = now;
			return;
		}

		const deltaTime = state.lastUpdateTime > 0
			? Math.min((now - state.lastUpdateTime) / 1000, 0.1)
			: 0;
		state.lastUpdateTime = now;

		audioEngine.getFFTProbeData(viz.stateId).then(data => {
			if (!data || !data.magnitudes) return;

			if (data.frameCounter === state.lastFrameCounter) return;
			state.lastFrameCounter = data.frameCounter;

			// Scroll in display pixels
			state.scrollAccumulator += state.speed * state.dpr * deltaTime;
			const pixelsToScroll = Math.floor(state.scrollAccumulator);
			if (pixelsToScroll <= 0) return;
			state.scrollAccumulator -= pixelsToScroll;

			const { offImageData, offWidth, offHeight, gradientLUT, minDb, maxDb } = state;
			const pixels = offImageData.data;
			const stride = offWidth * 4;
			const dbRange = maxDb - minDb;
			const binCount = data.binCount;

			// Shift offscreen left
			const shiftBytes = Math.min(pixelsToScroll, offWidth) * 4;
			for (let row = 0; row < offHeight; row++) {
				const rowStart = row * stride;
				pixels.copyWithin(rowStart, rowStart + shiftBytes, rowStart + stride);
			}

			// Draw new spectral columns on right edge
			// offHeight === binCount, so y maps 1:1 to bins
			const colStart = Math.max(0, offWidth - pixelsToScroll);
			for (let x = colStart; x < offWidth; x++) {
				for (let y = 0; y < offHeight; y++) {
					const binIdx = Math.min(y, binCount - 1);
					const db = data.magnitudes[binIdx];

					const normalized = Math.max(0, Math.min(1, (db - minDb) / dbRange));
					const lutIdx = Math.floor(normalized * 255) * 4;

					const offset = y * stride + x * 4;
					pixels[offset] = gradientLUT[lutIdx];
					pixels[offset + 1] = gradientLUT[lutIdx + 1];
					pixels[offset + 2] = gradientLUT[lutIdx + 2];
					pixels[offset + 3] = 255;
				}
			}

			state.offCtx.putImageData(offImageData, 0, 0);

			// Draw rotated offscreen onto visible canvas, scaling freq axis to display size
			const { ctx, canvas, rotation, drawWidth, drawHeight } = state;
			const cw = canvas.width;
			const ch = canvas.height;
			ctx.clearRect(0, 0, cw, ch);
			ctx.save();
			ctx.translate(cw / 2, ch / 2);
			ctx.rotate(rotation);
			// drawImage scales offscreen (offWidth x binCount) to (drawWidth x drawHeight)
			ctx.drawImage(state.offCanvas,
				-drawWidth / 2, -drawHeight / 2,
				drawWidth, drawHeight);
			ctx.restore();
		});
	},

	destroy(element: HTMLElement): void {
		const state = stateMap.get(element);
		if (state) {
			state.resizeObserver?.disconnect();
			offscreenCache.set(state.cacheKey, state.offCanvas);
			stateMap.delete(element);
		}
	}
};

registerRenderer(VizType.Waterfall, waterfallRenderer);

export { waterfallRenderer };
