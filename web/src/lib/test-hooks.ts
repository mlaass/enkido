/**
 * Test hooks for e2e (Playwright) — exposes a stable surface on `window`
 * so tests can drive compile/playback and read audio level without
 * scraping component internals.
 *
 * Loaded unconditionally so `preview` builds (which Playwright runs
 * against) have the hook. The exposed surface is read-only debug data
 * plus the audio engine's existing public methods — no security risk.
 *
 * Tests address `window.__nkidoTest`.
 */

import { audioEngine } from '$stores/audio.svelte';
import { editorStore } from '$stores/editor.svelte';

export interface NkidoTestHooks {
	audioEngine: typeof audioEngine;
	editor: typeof editorStore;

	/**
	 * Read N samples of time-domain audio (analyser node, 8-bit, 128 max
	 * per call from a 2048-bin analyser). Returns the centred float
	 * samples in [-1, 1]; an empty array if the audio engine isn't
	 * running yet.
	 */
	readTimeDomain(): Float32Array;

	/** Compute RMS over a single time-domain read. */
	readRms(): number;

	/**
	 * Sample audio at ~`samples` rate over `durationMs` and return the
	 * per-sample RMS trace. Resolves when the requested duration has
	 * elapsed. Useful for "does a gap appear during this window?" tests.
	 */
	captureRmsTrace(durationMs: number, sampleIntervalMs?: number): Promise<{
		rms: number[];
		sampleIntervalMs: number;
	}>;
}

declare global {
	interface Window {
		__nkidoTest?: NkidoTestHooks;
	}
}

export function installTestHooks() {
	if (typeof window === 'undefined') return;

	const readTimeDomain = (): Float32Array => {
		const u8 = audioEngine.getTimeDomainData();
		if (u8.length === 0) return new Float32Array(0);
		const out = new Float32Array(u8.length);
		for (let i = 0; i < u8.length; i++) {
			out[i] = (u8[i] - 128) / 128;
		}
		return out;
	};

	const readRms = (): number => {
		const samples = readTimeDomain();
		if (samples.length === 0) return 0;
		let acc = 0;
		for (let i = 0; i < samples.length; i++) {
			acc += samples[i] * samples[i];
		}
		return Math.sqrt(acc / samples.length);
	};

	const captureRmsTrace = async (
		durationMs: number,
		sampleIntervalMs: number = 20
	): Promise<{ rms: number[]; sampleIntervalMs: number }> => {
		const rms: number[] = [];
		const start = performance.now();
		while (performance.now() - start < durationMs) {
			rms.push(readRms());
			await new Promise((r) => setTimeout(r, sampleIntervalMs));
		}
		return { rms, sampleIntervalMs };
	};

	window.__nkidoTest = {
		audioEngine,
		editor: editorStore,
		readTimeDomain,
		readRms,
		captureRmsTrace
	};
}
