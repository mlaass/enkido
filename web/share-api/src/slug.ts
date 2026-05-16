// 8-char base62 slugs over an ambiguity-stripped 55-char alphabet
// (no 0/O/1/I/l). 55^8 ≈ 8.4e13 — collision odds < 1e-5 at 1M shares.
// PRD §5.5 (the PRD's "54 chars" comment was off by one; the literal string
// matches the PRD verbatim, just with the count corrected).

export const ALPHABET = '23456789abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ';

export function newSlug(): string {
	const buf = crypto.getRandomValues(new Uint8Array(8));
	let s = '';
	for (let i = 0; i < buf.length; i++) {
		s += ALPHABET[buf[i] % ALPHABET.length];
	}
	return s;
}

export function isValidSlug(s: string): boolean {
	if (s.length !== 8) return false;
	for (const c of s) if (!ALPHABET.includes(c)) return false;
	return true;
}
