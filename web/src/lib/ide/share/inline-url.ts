/**
 * Inline URL sharing — packs Akkado source into a hash fragment.
 *
 * Format:  https://live.nkido.cc/p#code=<lz-base64url>
 *
 * PRD: docs/prd-shareable-patches.md §5.7
 */

import LZString from 'lz-string';

const HASH_KEY = 'code';

export interface InlineUrl {
	/** Full URL, e.g. https://live.nkido.cc/p#code=... */
	url: string;
	/** Byte length of the full URL (for the share-dialog cap warning). */
	byteLength: number;
	/**
	 * Cap-warning tier. < 4000 = fits anywhere; 4000–7999 = Discord/Slack OK
	 * but some clients may truncate; ≥ 8000 = recommend Worker share instead.
	 */
	status: 'green' | 'yellow' | 'red';
}

/**
 * Encode source code into an inline-link URL.
 *
 * `base` defaults to `window.location.origin` at call time. In tests / SSR
 * pass an explicit base.
 */
export function encodeInlineUrl(code: string, base?: string): InlineUrl {
	const origin = base ?? (typeof window !== 'undefined' ? window.location.origin : '');
	const encoded = LZString.compressToEncodedURIComponent(code);
	const url = `${origin}/p#${HASH_KEY}=${encoded}`;
	const byteLength = new TextEncoder().encode(url).length;
	const status: InlineUrl['status'] =
		byteLength < 4000 ? 'green' : byteLength < 8000 ? 'yellow' : 'red';
	return { url, byteLength, status };
}

/**
 * Decode a `#code=...` hash fragment back to source. Returns null on any
 * failure (malformed lz-string, missing key, empty payload). Never throws.
 *
 * `hash` may include the leading `#` (as `location.hash` does) or not.
 */
export function decodeInlineHash(hash: string): string | null {
	if (!hash) return null;
	const trimmed = hash.startsWith('#') ? hash.slice(1) : hash;
	if (!trimmed) return null;
	const params = new URLSearchParams(trimmed);
	const encoded = params.get(HASH_KEY);
	if (!encoded) return null;
	try {
		const code = LZString.decompressFromEncodedURIComponent(encoded);
		return code && code.length > 0 ? code : null;
	} catch {
		return null;
	}
}

/**
 * Derive a stable 12-hex-char key from source code, used to key inline
 * phantom drafts. Same source → same key → revisiting an inline URL reuses
 * the existing phantom (with the visitor's local edits) rather than
 * overwriting them.
 *
 * SHA-256 truncated to 12 hex chars is collision-safe at OSS scale.
 */
export async function inlineHashKey(code: string): Promise<string> {
	const bytes = new TextEncoder().encode(code);
	const digest = await crypto.subtle.digest('SHA-256', bytes);
	const hex = Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, '0')).join('');
	return hex.slice(0, 12);
}
