/**
 * Host-only UI entry point (prd-web-audio-backend.md §5.5).
 *
 * Everything under `web/src/lib/native/` ships only in the native bundle
 * (`bun run build:native`, `__IS_NATIVE__` = true) that the nkido-studio /
 * plugin ResourceProvider serves. Site code must never import this module
 * outside an `if (__IS_NATIVE__)` gate — `scripts/check-native-bundle.ts`
 * asserts the marker below is absent from the site build artifact.
 *
 * The actual host panels (studio device settings, bus mixer, plugin
 * preset browser) are owned by the nkido-studio / plugin PRDs and get
 * added here as they land; this file is the seam, not the panels.
 */

// Grep-able sentinel for the build-artifact assertion. Keep it referenced
// from runtime code so minification can't drop it from the native bundle.
export const NATIVE_BUNDLE_MARKER = 'NKIDO_NATIVE_BUNDLE_MARKER';

/**
 * Called once from the root layout when `__IS_NATIVE__` is set. Native
 * panels register themselves here as they are implemented.
 */
export function initNativeHost(): void {
	console.log('[native] host UI seam active:', NATIVE_BUNDLE_MARKER);
}
