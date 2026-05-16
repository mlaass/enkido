import { describe, expect, it } from 'vitest';
import { ALPHABET, isValidSlug, newSlug } from '../src/slug';

describe('newSlug', () => {
	it('produces 8-char strings from the ambiguity-stripped alphabet', () => {
		for (let i = 0; i < 1000; i++) {
			const s = newSlug();
			expect(s).toHaveLength(8);
			for (const c of s) {
				expect(ALPHABET).toContain(c);
			}
		}
	});

	it('alphabet excludes ambiguous characters', () => {
		for (const c of '01OIl') {
			expect(ALPHABET).not.toContain(c);
		}
		expect(ALPHABET).toHaveLength(55);
	});

	it('isValidSlug accepts well-formed slugs', () => {
		for (let i = 0; i < 100; i++) {
			expect(isValidSlug(newSlug())).toBe(true);
		}
	});

	it('isValidSlug rejects malformed slugs', () => {
		expect(isValidSlug('')).toBe(false);
		expect(isValidSlug('short')).toBe(false);
		expect(isValidSlug('toolongtoolong')).toBe(false);
		expect(isValidSlug('00000000')).toBe(false); // contains '0'
		expect(isValidSlug('aaaaaaaO')).toBe(false); // contains 'O'
		expect(isValidSlug('aaaa-aaa')).toBe(false); // contains '-'
		expect(isValidSlug('abcd efg')).toBe(false); // contains space
	});

	it('1000 generated slugs have no obvious collisions', () => {
		const seen = new Set<string>();
		for (let i = 0; i < 1000; i++) {
			seen.add(newSlug());
		}
		// Collision is theoretically possible at 1000/54^8 ~ 1.4e-11, so this
		// must always pass in practice.
		expect(seen.size).toBe(1000);
	});
});
