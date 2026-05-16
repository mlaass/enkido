/**
 * Slug-keyed phantom lifecycle in the drafts store. Mirrors the inline
 * phantom tests in `drafts-store.test.ts` but for `slug:<slug>`-keyed tabs
 * created by visiting /p/<slug>.
 *
 * PRD §11.2 (last bullet) + §4.3 phantom lifecycle.
 */

import { beforeEach, describe, expect, it, vi } from 'vitest';

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
	(globalThis as unknown as { localStorage: Storage }).localStorage = new MemoryStorage();
	vi.resetModules();
});

async function loadFreshStore() {
	const mod = await import('$lib/stores/drafts.svelte');
	return mod.draftsStore;
}

describe('openSlugPhantomTab', () => {
	it('opens a phantom tab with id slug:<slug> and phantomSlug populated', async () => {
		const store = await loadFreshStore();
		const id = store.openSlugPhantomTab({
			slug: 'abcdefgh',
			code: 'sin(440)',
			title: 'moody bass'
		});

		expect(id).toBe('slug:abcdefgh');
		expect(store.activeTabId).toBe(id);
		expect(store.activeCode).toBe('sin(440)');

		const tab = store.drafts.find((d) => d.id === id);
		expect(tab).toBeTruthy();
		expect(tab?.isPhantom).toBe(true);
		expect(tab?.phantomSlug).toBe('abcdefgh');
		expect(tab?.name).toBe('moody bass');
	});

	it('uses a fallback title when none is provided', async () => {
		const store = await loadFreshStore();
		store.openSlugPhantomTab({ slug: 'aaaaaaaa', code: 'x', title: null });
		const tab = store.drafts.find((d) => d.id === 'slug:aaaaaaaa');
		expect(tab?.name).toBe('Patch aaaaaaaa');
	});

	it('does NOT persist the phantom until first updateActiveCode (lazy)', async () => {
		const store = await loadFreshStore();
		store.openSlugPhantomTab({ slug: 'lazyslug', code: 'sin(220)' });

		// Initial persisted state has only the migration's Untitled — the
		// phantom is in memory but not flushed.
		const persisted = JSON.parse(localStorage.getItem('nkido-drafts') ?? '[]');
		expect(persisted.find((d: { id: string }) => d.id === 'slug:lazyslug')).toBeUndefined();
	});

	it('first updateActiveCode materializes the phantom into localStorage', async () => {
		const store = await loadFreshStore();
		store.openSlugPhantomTab({ slug: 'matslug1', code: 'orig' });
		store.updateActiveCode('edited');

		// debounce flushes via beforeunload; force it
		store.flushPendingSave();
		const persisted = JSON.parse(localStorage.getItem('nkido-drafts') ?? '[]');
		const phantom = persisted.find((d: { id: string }) => d.id === 'slug:matslug1');
		expect(phantom).toBeTruthy();
		expect(phantom.code).toBe('edited');
		expect(phantom.isPhantom).toBe(true);
		expect(phantom.phantomSlug).toBe('matslug1');
	});

	it('revisit reuses the existing phantom (does NOT overwrite local edits)', async () => {
		const store = await loadFreshStore();
		store.openSlugPhantomTab({ slug: 'reusedme', code: 'original' });
		store.updateActiveCode('user edits');
		store.flushPendingSave();

		// Simulate viewer re-visit. Phantom should be reused, NOT overwritten.
		store.openSlugPhantomTab({ slug: 'reusedme', code: 'original-again' });
		expect(store.activeCode).toBe('user edits');
	});
});

describe('findSlugPhantom', () => {
	it('returns null when no phantom exists', async () => {
		const store = await loadFreshStore();
		expect(store.findSlugPhantom('zzzzzzzz')).toBeNull();
	});

	it('returns the phantom after openSlugPhantomTab', async () => {
		const store = await loadFreshStore();
		store.openSlugPhantomTab({ slug: 'findable', code: 'x' });
		const p = store.findSlugPhantom('findable');
		expect(p).toBeTruthy();
		expect(p?.phantomSlug).toBe('findable');
	});

	it('returns null after deleteSlugPhantom', async () => {
		const store = await loadFreshStore();
		store.openSlugPhantomTab({ slug: 'tempslug', code: 'x' });
		store.updateActiveCode('y');
		store.flushPendingSave();
		store.deleteSlugPhantom('tempslug');
		expect(store.findSlugPhantom('tempslug')).toBeNull();
	});
});

describe('deleteSlugPhantom', () => {
	it('removes the phantom from storage and closes the tab', async () => {
		const store = await loadFreshStore();
		store.openSlugPhantomTab({ slug: 'gonenow1', code: 'x' });
		store.updateActiveCode('edits');
		store.flushPendingSave();

		expect(store.openTabIds).toContain('slug:gonenow1');

		store.deleteSlugPhantom('gonenow1');

		expect(store.openTabIds).not.toContain('slug:gonenow1');
		expect(store.findSlugPhantom('gonenow1')).toBeNull();
		const persisted = JSON.parse(localStorage.getItem('nkido-drafts') ?? '[]');
		expect(persisted.find((d: { id: string }) => d.id === 'slug:gonenow1')).toBeUndefined();
	});

	it('is a no-op when the phantom does not exist', async () => {
		const store = await loadFreshStore();
		const before = store.drafts.length;
		store.deleteSlugPhantom('neverhere');
		expect(store.drafts.length).toBe(before);
	});
});

describe('recentlyVisited reactivity', () => {
	it('starts empty, populates after recordVisited, prunes after forgetVisited', async () => {
		const store = await loadFreshStore();
		// initial: empty (the hydration promise resolves on the microtask queue;
		// give it a tick by awaiting any pending microtask)
		await Promise.resolve();
		expect(store.recentlyVisited).toEqual([]);

		await store.recordVisited('aaaaaaaa', 'first');
		await store.recordVisited('bbbbbbbb', 'second');

		expect(store.recentlyVisited).toHaveLength(2);
		expect(store.recentlyVisited[0].slug).toBe('bbbbbbbb');

		await store.forgetVisited('aaaaaaaa');
		expect(store.recentlyVisited).toHaveLength(1);
		expect(store.recentlyVisited[0].slug).toBe('bbbbbbbb');
	});
});

describe('keepActiveAsNamed on a slug phantom', () => {
	it('promotes to a named draft and clears the phantom-slug binding', async () => {
		const store = await loadFreshStore();
		store.openSlugPhantomTab({ slug: 'keep1234', code: 'x', title: 'moody bass' });
		store.updateActiveCode('edits to keep');
		store.flushPendingSave();

		const newId = store.keepActiveAsNamed('My moody bass fork');

		expect(newId).toBeTruthy();
		expect(newId).not.toBe('slug:keep1234');
		const tab = store.drafts.find((d) => d.id === newId);
		expect(tab).toBeTruthy();
		expect(tab?.isPhantom).toBe(false);
		expect(tab?.phantomSlug).toBeNull();
		expect(tab?.name).toBe('My moody bass fork');
		expect(store.activeCode).toBe('edits to keep');
	});
});
