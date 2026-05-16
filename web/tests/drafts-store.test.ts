import { describe, it, expect, beforeEach, vi } from 'vitest';

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
	(globalThis as any).localStorage = new MemoryStorage();
	vi.resetModules();
});

// Fresh import per test so init() runs against the per-test localStorage.
async function loadFreshStore() {
	const mod = await import('$lib/stores/drafts.svelte');
	return mod.draftsStore;
}

describe('drafts store — first-launch migration', () => {
	it('creates one Untitled draft on a fresh install', async () => {
		const store = await loadFreshStore();
		expect(store.drafts).toHaveLength(1);
		expect(store.drafts[0].name).toBe('Untitled');
		expect(store.activeTabId).toBe(store.drafts[0].id);
		expect(store.openTabIds).toEqual([store.drafts[0].id]);
		expect(localStorage.getItem('nkido-drafts-migrated-v1')).toBe('1');
	});

	it('migrates legacy nkido-editor-code into Untitled draft', async () => {
		localStorage.setItem('nkido-editor-code', 'legacy code here');
		const store = await loadFreshStore();
		expect(store.drafts).toHaveLength(1);
		expect(store.drafts[0].name).toBe('Untitled');
		expect(store.activeCode).toBe('legacy code here');
		// Legacy key is left untouched (one-release safety net).
		expect(localStorage.getItem('nkido-editor-code')).toBe('legacy code here');
		expect(localStorage.getItem('nkido-drafts-migrated-v1')).toBe('1');
	});

	it('both keys present → nkido-drafts wins, no re-migrate', async () => {
		localStorage.setItem('nkido-editor-code', 'legacy code');
		const existing = [{
			id: 'd-1', name: 'My Draft', code: 'existing code',
			updatedAt: 1000, isPhantom: false, phantomSlug: null
		}];
		localStorage.setItem('nkido-drafts', JSON.stringify(existing));

		const store = await loadFreshStore();
		expect(store.drafts).toHaveLength(1);
		expect(store.drafts[0].name).toBe('My Draft');
		expect(store.activeCode).toBe('existing code');
		// Legacy key stays put; not consumed.
		expect(localStorage.getItem('nkido-editor-code')).toBe('legacy code');
	});

	it('migration flag prevents re-import when user manually clears drafts', async () => {
		localStorage.setItem('nkido-editor-code', 'should not re-import');
		localStorage.setItem('nkido-drafts-migrated-v1', '1');
		const store = await loadFreshStore();
		// Legacy code is NOT imported; gets a fresh DEFAULT_CODE Untitled.
		expect(store.activeCode).not.toBe('should not re-import');
		expect(store.drafts).toHaveLength(1);
	});
});

describe('drafts store — CRUD and active draft', () => {
	it('createDraft appends and activates by default', async () => {
		const store = await loadFreshStore();
		const initialId = store.activeTabId;
		const newId = store.createDraft({ name: 'Second', code: 'sin(220)' });
		expect(store.drafts).toHaveLength(2);
		expect(store.activeTabId).toBe(newId);
		expect(store.activeCode).toBe('sin(220)');
		expect(store.openTabIds).toContain(initialId);
		expect(store.openTabIds).toContain(newId);
	});

	it('setActive switches activeCode in place', async () => {
		const store = await loadFreshStore();
		const id1 = store.activeTabId!;
		store.updateActiveCode('first code');
		const id2 = store.createDraft({ name: 'Second', code: 'second code' });
		expect(store.activeCode).toBe('second code');
		store.setActive(id1);
		expect(store.activeTabId).toBe(id1);
		expect(store.activeCode).toBe('first code');
	});

	it('rename persists the new name', async () => {
		const store = await loadFreshStore();
		const id = store.activeTabId!;
		store.renameDraft(id, 'Renamed');
		expect(store.drafts.find((d) => d.id === id)?.name).toBe('Renamed');
		const raw = JSON.parse(localStorage.getItem('nkido-drafts')!);
		expect(raw.find((d: any) => d.id === id)?.name).toBe('Renamed');
	});

	it('delete-active falls back to most-recent remaining', async () => {
		const store = await loadFreshStore();
		const id1 = store.activeTabId!;
		const id2 = store.createDraft({ name: 'B' });
		const id3 = store.createDraft({ name: 'C' });
		// active is id3 (last created)
		expect(store.activeTabId).toBe(id3);
		store.deleteDraft(id3);
		// fallback: the most-recent of the remaining open tabs (id2 since it was after id1)
		expect(store.drafts.find((d) => d.id === id3)).toBeUndefined();
		expect([id1, id2]).toContain(store.activeTabId);
	});

	it('deleting all drafts auto-creates a fresh Untitled', async () => {
		const store = await loadFreshStore();
		const id = store.activeTabId!;
		store.deleteDraft(id);
		expect(store.drafts).toHaveLength(1);
		expect(store.drafts[0].name).toBe('Untitled');
		expect(store.activeTabId).toBe(store.drafts[0].id);
	});

	it('updateActiveCode mutates the active draft and persists', async () => {
		const store = await loadFreshStore();
		store.updateActiveCode('typed code');
		expect(store.activeCode).toBe('typed code');
		store.flushPendingSave();
		const raw = JSON.parse(localStorage.getItem('nkido-drafts')!);
		expect(raw[0].code).toBe('typed code');
	});
});

describe('drafts store — open-tab persistence', () => {
	it('persists openTabIds and activeTab across reload', async () => {
		const store = await loadFreshStore();
		const id1 = store.activeTabId!;
		const id2 = store.createDraft({ name: 'B' });
		const id3 = store.createDraft({ name: 'C' });
		store.setActive(id2);

		const open = JSON.parse(localStorage.getItem('nkido-open-tabs')!);
		expect(open).toEqual([id1, id2, id3]);
		expect(localStorage.getItem('nkido-active-tab')).toBe(id2);

		// Simulate reload by re-importing.
		const reloaded = await loadFreshStore();
		expect(reloaded.openTabIds).toEqual([id1, id2, id3]);
		expect(reloaded.activeTabId).toBe(id2);
	});

	it('drops stale tab ids on reload', async () => {
		// Pre-seed drafts with one id, but persist a stale open-tab list.
		localStorage.setItem('nkido-drafts', JSON.stringify([
			{ id: 'real', name: 'X', code: '', updatedAt: 1, isPhantom: false, phantomSlug: null }
		]));
		localStorage.setItem('nkido-open-tabs', JSON.stringify(['real', 'ghost-1', 'ghost-2']));
		localStorage.setItem('nkido-active-tab', 'ghost-1');

		const store = await loadFreshStore();
		expect(store.openTabIds).toEqual(['real']);
		expect(store.activeTabId).toBe('real');
	});

	it('closeTab does not delete the underlying draft', async () => {
		const store = await loadFreshStore();
		const id = store.createDraft({ name: 'Closable' });
		store.closeTab(id);
		expect(store.openTabIds).not.toContain(id);
		expect(store.drafts.find((d) => d.id === id)).toBeDefined();
	});

	it('closing the last tab auto-creates Untitled', async () => {
		const store = await loadFreshStore();
		const id = store.activeTabId!;
		store.closeTab(id);
		expect(store.openTabIds.length).toBe(1);
		expect(store.activeTabId).not.toBeNull();
	});
});

describe('drafts store — inline phantom lifecycle', () => {
	it('openInlinePhantomTab does not persist until first edit', async () => {
		const store = await loadFreshStore();
		const before = JSON.parse(localStorage.getItem('nkido-drafts')!).length;
		store.openInlinePhantomTab({ key: 'inline:abc123', code: 'phantom code' });
		// Tab opens, active set, but localStorage not yet updated.
		expect(store.activeTabId).toBe('inline:abc123');
		expect(store.activeCode).toBe('phantom code');
		const after = JSON.parse(localStorage.getItem('nkido-drafts')!).length;
		expect(after).toBe(before);
	});

	it('first edit materializes the phantom in localStorage', async () => {
		const store = await loadFreshStore();
		store.openInlinePhantomTab({ key: 'inline:abc123', code: 'phantom code' });
		store.updateActiveCode('phantom code edited');
		store.flushPendingSave();
		const raw = JSON.parse(localStorage.getItem('nkido-drafts')!);
		const phantom = raw.find((d: any) => d.id === 'inline:abc123');
		expect(phantom).toBeDefined();
		expect(phantom.isPhantom).toBe(true);
		expect(phantom.code).toBe('phantom code edited');
	});

	it('reopening an existing phantom reuses it (does not overwrite edits)', async () => {
		const store = await loadFreshStore();
		store.openInlinePhantomTab({ key: 'inline:abc', code: 'original' });
		store.updateActiveCode('edited');
		store.flushPendingSave();
		// Re-open with the original code; should NOT overwrite edits.
		store.openInlinePhantomTab({ key: 'inline:abc', code: 'original' });
		expect(store.activeCode).toBe('edited');
	});

	it('Keep promotes phantom to a named draft (new id, isPhantom=false)', async () => {
		const store = await loadFreshStore();
		store.openInlinePhantomTab({ key: 'inline:xyz', code: 'phantom' });
		store.updateActiveCode('phantom-edit');
		store.flushPendingSave();

		const newId = store.keepActiveAsNamed('My Patch');
		expect(newId).toBeTruthy();
		expect(newId).not.toBe('inline:xyz');
		expect(store.activeTabId).toBe(newId);
		const promoted = store.drafts.find((d) => d.id === newId);
		expect(promoted?.isPhantom).toBe(false);
		expect(promoted?.name).toBe('My Patch');
		// old phantom id is gone
		expect(store.drafts.find((d) => d.id === 'inline:xyz')).toBeUndefined();
		// localStorage reflects the swap
		const raw = JSON.parse(localStorage.getItem('nkido-drafts')!);
		expect(raw.find((d: any) => d.id === 'inline:xyz')).toBeUndefined();
		expect(raw.find((d: any) => d.id === newId)?.name).toBe('My Patch');
	});
});
