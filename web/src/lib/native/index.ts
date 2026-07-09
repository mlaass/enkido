/**
 * Host-only UI entry point (prd-web-audio-backend.md §5.5).
 *
 * Everything under `web/src/lib/native/` ships only in the native bundle
 * (`bun run build:native`, `__IS_NATIVE__` = true) that the nkido-studio /
 * plugin ResourceProvider serves. Site code must never import this module
 * outside an `if (__IS_NATIVE__)` gate — `scripts/check-native-bundle.ts`
 * asserts the marker below is absent from the site build artifact.
 *
 * Panels live here: the studio daw-core dock (transport + capture indicator,
 * bus mixer, takes browser, render dialog) — prd-studio-daw-core §5.3–§5.6.
 * The plugin's preset browser will register alongside it.
 */
import { mount } from 'svelte';

import StudioPanels from './StudioPanels.svelte';
import { studio } from './studio-bridge.svelte';

// Grep-able sentinel for the build-artifact assertion. Keep it referenced
// from runtime code so minification can't drop it from the native bundle.
export const NATIVE_BUNDLE_MARKER = 'NKIDO_NATIVE_BUNDLE_MARKER';

let mounted = false;

/**
 * Called once from the root layout when `__IS_NATIVE__` is set.
 *
 * The panels only render once the native bridge answers (`studio.available`),
 * so a native bundle loaded without a host — the dev server, a plain browser —
 * shows the plain IDE rather than dead controls.
 */
export function initNativeHost(): void {
	console.log('[native] host UI seam active:', NATIVE_BUNDLE_MARKER);
	if (mounted) return;
	mounted = true;

	studio.start();

	const host = document.createElement('div');
	host.id = 'nkido-studio-panels';
	document.body.appendChild(host);
	mount(StudioPanels, { target: host });
}
