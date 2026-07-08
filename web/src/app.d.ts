/**
 * Build-time constant injected by Vite `define` (see vite.config.ts).
 * `true` only in the native bundle (`bun run build:native`) consumed by
 * the nkido-studio / plugin WebView shells; statically `false` on the
 * site so `if (__IS_NATIVE__)` blocks — and everything they import from
 * `$lib/native/` — tree-shake out of the site bundle entirely
 * (prd-web-audio-backend.md §5.5).
 */
declare global {
	const __IS_NATIVE__: boolean;
}

export {};
