import { describe, it, expect, beforeEach } from 'vitest';
import { hasReported, markReported, _clearReported } from '$lib/ide/share/reported-slugs';

const KEY = 'nkido-reported-slugs';

class MemoryStorage implements Storage {
	private map = new Map<string, string>();
	get length() { return this.map.size; }
	clear() { this.map.clear(); }
	getItem(k: string) { return this.map.get(k) ?? null; }
	setItem(k: string, v: string) { this.map.set(k, v); }
	removeItem(k: string) { this.map.delete(k); }
	key(i: number) { return Array.from(this.map.keys())[i] ?? null; }
}

beforeEach(() => {
	(globalThis as { localStorage?: Storage }).localStorage = new MemoryStorage();
});

describe('reported-slugs', () => {
	it('hasReported is false initially', () => {
		expect(hasReported('aaaaaaaa')).toBe(false);
	});

	it('markReported then hasReported returns true for that slug only', () => {
		markReported('aaaaaaaa');
		expect(hasReported('aaaaaaaa')).toBe(true);
		expect(hasReported('bbbbbbbb')).toBe(false);
	});

	it('repeated markReported leaves the set at size 1', () => {
		markReported('aaaaaaaa');
		markReported('aaaaaaaa');
		markReported('aaaaaaaa');
		const raw = localStorage.getItem(KEY);
		const parsed = raw ? JSON.parse(raw) : [];
		expect(Array.isArray(parsed)).toBe(true);
		expect(parsed).toHaveLength(1);
		expect(parsed[0]).toBe('aaaaaaaa');
	});

	it('survives a corrupted localStorage value without throwing', () => {
		localStorage.setItem(KEY, '{not json');
		expect(hasReported('aaaaaaaa')).toBe(false);
		expect(() => markReported('aaaaaaaa')).not.toThrow();
		// After markReported, storage should be a valid JSON array again.
		expect(hasReported('aaaaaaaa')).toBe(true);
	});

	it('survives a non-array JSON value without throwing', () => {
		localStorage.setItem(KEY, JSON.stringify({ not: 'an array' }));
		expect(hasReported('aaaaaaaa')).toBe(false);
		expect(() => markReported('aaaaaaaa')).not.toThrow();
	});

	it('survives a missing localStorage shim', () => {
		delete (globalThis as { localStorage?: Storage }).localStorage;
		expect(() => hasReported('aaaaaaaa')).not.toThrow();
		expect(hasReported('aaaaaaaa')).toBe(false);
		expect(() => markReported('aaaaaaaa')).not.toThrow();
	});

	it('is silent on quota errors from setItem', () => {
		const storage = localStorage as Storage;
		const original = storage.setItem.bind(storage);
		storage.setItem = () => {
			throw new DOMException('quota exceeded', 'QuotaExceededError');
		};
		expect(() => markReported('aaaaaaaa')).not.toThrow();
		storage.setItem = original;
	});

	it('_clearReported empties the set', () => {
		markReported('aaaaaaaa');
		markReported('bbbbbbbb');
		_clearReported();
		expect(hasReported('aaaaaaaa')).toBe(false);
		expect(hasReported('bbbbbbbb')).toBe(false);
	});
});
