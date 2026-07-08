/**
 * Backend-selection test (prd-web-audio-backend §10): `window.__JUCE__`
 * present → JuceAudioBackend; absent → WasmAudioBackend; the Juce adapter
 * is never constructed on the plain-browser path.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';

// wasm-backend pulls in Svelte-runes stores the vitest config does not
// preprocess; stub the module (selection only needs identity).
vi.mock('$lib/audio/wasm-backend', () => ({
	createWasmAudioBackend: vi.fn(() => ({ __kind: 'wasm' }))
}));
vi.mock('$lib/audio/juce-backend', async (importOriginal) => {
	const original = await importOriginal<typeof import('../src/lib/audio/juce-backend')>();
	return {
		...original,
		createJuceAudioBackend: vi.fn(() => ({ __kind: 'juce' }))
	};
});

import { selectAudioBackend, hasJuceBridge } from '../src/lib/audio/backend-select';
import { createJuceAudioBackend } from '../src/lib/audio/juce-backend';
import { createWasmAudioBackend } from '../src/lib/audio/wasm-backend';
import type { AudioBackendHost } from '../src/lib/audio/audio-backend';

const host = {} as AudioBackendHost;

afterEach(() => {
	vi.unstubAllGlobals();
	vi.clearAllMocks();
});

describe('backend selection', () => {
	it('picks WasmAudioBackend when window.__JUCE__ is absent', () => {
		vi.stubGlobal('window', {});
		expect(hasJuceBridge()).toBe(false);
		const backend = selectAudioBackend(host) as unknown as { __kind: string };
		expect(backend.__kind).toBe('wasm');
		expect(createJuceAudioBackend).not.toHaveBeenCalled();
	});

	it('picks JuceAudioBackend when window.__JUCE__.backend exists', () => {
		vi.stubGlobal('window', { __JUCE__: { backend: {} } });
		expect(hasJuceBridge()).toBe(true);
		const backend = selectAudioBackend(host) as unknown as { __kind: string };
		expect(backend.__kind).toBe('juce');
		expect(createWasmAudioBackend).not.toHaveBeenCalled();
	});

	it('treats an empty __JUCE__ shim (no backend) as no bridge', () => {
		vi.stubGlobal('window', { __JUCE__: {} });
		expect(hasJuceBridge()).toBe(false);
	});
});
