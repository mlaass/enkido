/**
 * LocalDraftProvider — drafts and recently-visited slugs in localStorage.
 *
 * Storage keys:
 *   - `nkido-drafts` — JSON array of DraftFull
 *   - `nkido-recently-visited` — JSON array of RecentlyVisited (capped)
 *
 * The drafts store mediates access; components should not call this directly.
 *
 * PRD: docs/prd-shareable-patches.md §5.2
 */

import type {
	DraftFull,
	DraftSummary,
	RecentlyVisited,
	StorageProvider
} from './types';

export const DRAFTS_KEY = 'nkido-drafts';
export const RECENTLY_VISITED_KEY = 'nkido-recently-visited';
export const RECENTLY_VISITED_CAP = 20;

function readDrafts(): DraftFull[] {
	if (typeof localStorage === 'undefined') return [];
	try {
		const raw = localStorage.getItem(DRAFTS_KEY);
		if (!raw) return [];
		const parsed = JSON.parse(raw);
		if (!Array.isArray(parsed)) return [];
		return parsed.filter(isDraftFull);
	} catch {
		return [];
	}
}

function writeDrafts(drafts: DraftFull[]): void {
	if (typeof localStorage === 'undefined') return;
	try {
		localStorage.setItem(DRAFTS_KEY, JSON.stringify(drafts));
	} catch (e) {
		console.warn('[drafts] failed to write drafts:', e);
	}
}

function isDraftFull(x: unknown): x is DraftFull {
	if (!x || typeof x !== 'object') return false;
	const d = x as Record<string, unknown>;
	return (
		typeof d.id === 'string' &&
		typeof d.name === 'string' &&
		typeof d.code === 'string' &&
		typeof d.updatedAt === 'number' &&
		typeof d.isPhantom === 'boolean'
	);
}

function readVisited(): RecentlyVisited[] {
	if (typeof localStorage === 'undefined') return [];
	try {
		const raw = localStorage.getItem(RECENTLY_VISITED_KEY);
		if (!raw) return [];
		const parsed = JSON.parse(raw);
		if (!Array.isArray(parsed)) return [];
		return parsed.filter(
			(v): v is RecentlyVisited =>
				v &&
				typeof v === 'object' &&
				typeof v.slug === 'string' &&
				typeof v.visitedAt === 'number'
		);
	} catch {
		return [];
	}
}

function writeVisited(list: RecentlyVisited[]): void {
	if (typeof localStorage === 'undefined') return;
	try {
		localStorage.setItem(RECENTLY_VISITED_KEY, JSON.stringify(list));
	} catch (e) {
		console.warn('[drafts] failed to write recently-visited:', e);
	}
}

function toSummary(d: DraftFull): DraftSummary {
	return {
		id: d.id,
		name: d.name,
		updatedAt: d.updatedAt,
		isPhantom: d.isPhantom,
		phantomSlug: d.phantomSlug
	};
}

export class LocalDraftProvider implements StorageProvider {
	async saveDraft(draft: DraftFull): Promise<void> {
		const drafts = readDrafts();
		const i = drafts.findIndex((d) => d.id === draft.id);
		if (i >= 0) drafts[i] = draft;
		else drafts.push(draft);
		writeDrafts(drafts);
	}

	async loadDraft(id: string): Promise<DraftFull | null> {
		return readDrafts().find((d) => d.id === id) ?? null;
	}

	async listDrafts(): Promise<DraftSummary[]> {
		return readDrafts().map(toSummary);
	}

	async deleteDraft(id: string): Promise<void> {
		writeDrafts(readDrafts().filter((d) => d.id !== id));
	}

	async renameDraft(id: string, newName: string): Promise<void> {
		const drafts = readDrafts();
		const d = drafts.find((x) => x.id === id);
		if (!d) return;
		d.name = newName;
		d.updatedAt = Date.now();
		writeDrafts(drafts);
	}

	async recordVisited(slug: string, title: string | null): Promise<void> {
		const list = readVisited().filter((v) => v.slug !== slug);
		list.unshift({ slug, title, visitedAt: Date.now() });
		writeVisited(list.slice(0, RECENTLY_VISITED_CAP));
	}

	async listRecentlyVisited(): Promise<RecentlyVisited[]> {
		return readVisited();
	}
}
