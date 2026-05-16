import { describe, it, expect, beforeEach } from 'vitest';
import {
	LocalDraftProvider,
	DRAFTS_KEY,
	RECENTLY_VISITED_KEY,
	RECENTLY_VISITED_CAP
} from '$lib/ide/storage/local-draft';
import type { DraftFull } from '$lib/ide/storage/types';

class MemoryStorage implements Storage {
	private map = new Map<string, string>();
	get length() { return this.map.size; }
	clear() { this.map.clear(); }
	getItem(k: string) { return this.map.get(k) ?? null; }
	setItem(k: string, v: string) { this.map.set(k, v); }
	removeItem(k: string) { this.map.delete(k); }
	key(i: number) { return Array.from(this.map.keys())[i] ?? null; }
}

function makeDraft(over: Partial<DraftFull> = {}): DraftFull {
	return {
		id: 'd-1',
		name: 'Untitled',
		code: 'sin(440)',
		updatedAt: 1000,
		isPhantom: false,
		phantomSlug: null,
		...over
	};
}

beforeEach(() => {
	(globalThis as any).localStorage = new MemoryStorage();
});

describe('LocalDraftProvider — drafts CRUD', () => {
	it('saves and loads a draft', async () => {
		const p = new LocalDraftProvider();
		await p.saveDraft(makeDraft({ id: 'a', code: 'A' }));
		const loaded = await p.loadDraft('a');
		expect(loaded?.code).toBe('A');
	});

	it('updates an existing draft in place (no duplicate)', async () => {
		const p = new LocalDraftProvider();
		await p.saveDraft(makeDraft({ id: 'a', code: 'A' }));
		await p.saveDraft(makeDraft({ id: 'a', code: 'A-edited' }));
		const list = await p.listDrafts();
		expect(list).toHaveLength(1);
		expect((await p.loadDraft('a'))?.code).toBe('A-edited');
	});

	it('lists summaries (no code field)', async () => {
		const p = new LocalDraftProvider();
		await p.saveDraft(makeDraft({ id: 'a', code: 'A' }));
		await p.saveDraft(makeDraft({ id: 'b', code: 'B' }));
		const summaries = await p.listDrafts();
		expect(summaries).toHaveLength(2);
		expect((summaries[0] as any).code).toBeUndefined();
	});

	it('deletes a draft', async () => {
		const p = new LocalDraftProvider();
		await p.saveDraft(makeDraft({ id: 'a' }));
		await p.deleteDraft('a');
		expect(await p.loadDraft('a')).toBeNull();
		expect(await p.listDrafts()).toHaveLength(0);
	});

	it('renames a draft (and bumps updatedAt)', async () => {
		const p = new LocalDraftProvider();
		await p.saveDraft(makeDraft({ id: 'a', name: 'old', updatedAt: 100 }));
		await p.renameDraft('a', 'new');
		const d = await p.loadDraft('a');
		expect(d?.name).toBe('new');
		expect(d?.updatedAt).toBeGreaterThan(100);
	});

	it('returns empty list when storage is empty', async () => {
		const p = new LocalDraftProvider();
		expect(await p.listDrafts()).toEqual([]);
	});

	it('returns empty list on corrupted JSON', async () => {
		localStorage.setItem(DRAFTS_KEY, '{not valid json');
		const p = new LocalDraftProvider();
		expect(await p.listDrafts()).toEqual([]);
	});

	it('filters out malformed entries', async () => {
		localStorage.setItem(DRAFTS_KEY, JSON.stringify([
			makeDraft({ id: 'good' }),
			{ id: 'bad' }, // missing fields
			null,
			'not an object'
		]));
		const p = new LocalDraftProvider();
		const list = await p.listDrafts();
		expect(list).toHaveLength(1);
		expect(list[0].id).toBe('good');
	});
});

describe('LocalDraftProvider — recently visited', () => {
	it('records and lists in MRU order', async () => {
		const p = new LocalDraftProvider();
		await p.recordVisited('aaa', 'first');
		await p.recordVisited('bbb', 'second');
		const list = await p.listRecentlyVisited();
		expect(list[0].slug).toBe('bbb');
		expect(list[1].slug).toBe('aaa');
	});

	it('deduplicates and re-pins on revisit', async () => {
		const p = new LocalDraftProvider();
		await p.recordVisited('aaa', 'first');
		await p.recordVisited('bbb', 'second');
		await p.recordVisited('aaa', 'first');
		const list = await p.listRecentlyVisited();
		expect(list).toHaveLength(2);
		expect(list[0].slug).toBe('aaa');
	});

	it(`caps at ${RECENTLY_VISITED_CAP} entries`, async () => {
		const p = new LocalDraftProvider();
		for (let i = 0; i < RECENTLY_VISITED_CAP + 5; i++) {
			await p.recordVisited(`s-${i}`, null);
		}
		const list = await p.listRecentlyVisited();
		expect(list).toHaveLength(RECENTLY_VISITED_CAP);
		// Newest first; oldest evicted.
		expect(list[0].slug).toBe(`s-${RECENTLY_VISITED_CAP + 4}`);
		expect(list.find((v) => v.slug === 's-0')).toBeUndefined();
	});

	it('returns empty list on corrupted JSON', async () => {
		localStorage.setItem(RECENTLY_VISITED_KEY, '{garbage');
		const p = new LocalDraftProvider();
		expect(await p.listRecentlyVisited()).toEqual([]);
	});

	it('forgetVisited removes the specific slug', async () => {
		const p = new LocalDraftProvider();
		await p.recordVisited('aaa', 'a');
		await p.recordVisited('bbb', 'b');
		await p.recordVisited('ccc', 'c');
		await p.forgetVisited('bbb');
		const list = await p.listRecentlyVisited();
		expect(list).toHaveLength(2);
		expect(list.find((v) => v.slug === 'bbb')).toBeUndefined();
	});

	it('forgetVisited is a no-op when slug not present', async () => {
		const p = new LocalDraftProvider();
		await p.recordVisited('aaa', 'a');
		await p.forgetVisited('zzz');
		expect(await p.listRecentlyVisited()).toHaveLength(1);
	});
});
