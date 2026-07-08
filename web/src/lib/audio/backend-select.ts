/**
 * Backend selection (prd-web-audio-backend.md §5): chosen once at boot.
 * `window.__JUCE__` present → native `JuceAudioBackend` (studio/plugin
 * WebView); absent → `WasmAudioBackend` (plain browser). The Juce adapter
 * is only constructed when the bridge exists, so it never enters the
 * site's runtime path.
 */

import type { AudioBackend, AudioBackendHost } from './audio-backend';
import { createWasmAudioBackend } from './wasm-backend';
import { createJuceAudioBackend } from './juce-backend';

export function hasJuceBridge(): boolean {
	return typeof window !== 'undefined' && window.__JUCE__?.backend != null;
}

export function selectAudioBackend(host: AudioBackendHost): AudioBackend {
	return hasJuceBridge() ? createJuceAudioBackend(host) : createWasmAudioBackend(host);
}
