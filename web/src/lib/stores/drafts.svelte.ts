/**
 * Drafts store — owns the list of named drafts and phantom drafts, the open
 * tab set, and the active tab.
 *
 * Persistence keys:
 *   - `nkido-drafts`            JSON array of DraftFull (via LocalDraftProvider)
 *   - `nkido-open-tabs`         JSON array of draft ids in tab order
 *   - `nkido-active-tab`        single id string
 *   - `nkido-drafts-migrated-v1` flag — set after first-launch migration
 *   - `nkido-editor-code`       legacy single-buffer key (read once for migration)
 *
 * PRD: docs/prd-shareable-patches.md §4.4, §5.2, §8 Phase 1
 */

import { LocalDraftProvider } from '$lib/ide/storage/local-draft';
import type { DraftFull, DraftSummary } from '$lib/ide/storage/types';

const OPEN_TABS_KEY = 'nkido-open-tabs';
const ACTIVE_TAB_KEY = 'nkido-active-tab';
const MIGRATED_KEY = 'nkido-drafts-migrated-v1';
const LEGACY_CODE_KEY = 'nkido-editor-code';

export const DEFAULT_CODE = `// Welcome to NKIDO!
// Press Ctrl+Enter to evaluate

bpm = 120

// Simple sine wave
sin(440) |> out(@, @)
`;

const SAVE_DEBOUNCE_MS = 500;

function readOpenTabs(): string[] {
	if (typeof localStorage === 'undefined') return [];
	try {
		const raw = localStorage.getItem(OPEN_TABS_KEY);
		if (!raw) return [];
		const parsed = JSON.parse(raw);
		return Array.isArray(parsed) ? parsed.filter((x) => typeof x === 'string') : [];
	} catch {
		return [];
	}
}

function writeOpenTabs(ids: string[]): void {
	if (typeof localStorage === 'undefined') return;
	try {
		localStorage.setItem(OPEN_TABS_KEY, JSON.stringify(ids));
	} catch (e) {
		console.warn('[drafts] failed to write open tabs:', e);
	}
}

function readActiveTab(): string | null {
	if (typeof localStorage === 'undefined') return null;
	return localStorage.getItem(ACTIVE_TAB_KEY);
}

function writeActiveTab(id: string | null): void {
	if (typeof localStorage === 'undefined') return;
	try {
		if (id == null) localStorage.removeItem(ACTIVE_TAB_KEY);
		else localStorage.setItem(ACTIVE_TAB_KEY, id);
	} catch (e) {
		console.warn('[drafts] failed to write active tab:', e);
	}
}

function newUuid(): string {
	if (typeof crypto !== 'undefined' && 'randomUUID' in crypto) return crypto.randomUUID();
	return 'd-' + Math.random().toString(36).slice(2) + Date.now().toString(36);
}

function freshDraft(over: Partial<DraftFull> = {}): DraftFull {
	return {
		id: newUuid(),
		name: 'Untitled',
		code: DEFAULT_CODE,
		updatedAt: Date.now(),
		isPhantom: false,
		phantomSlug: null,
		...over
	};
}

interface DraftsState {
	drafts: DraftSummary[];
	openTabIds: string[];
	activeTabId: string | null;
	/** The active draft's full content (mirrored for fast editor access). */
	activeCode: string;
}

function createDraftsStore() {
	const provider = new LocalDraftProvider();

	// In-memory mirror of all drafts (kept in sync with localStorage).
	let allDrafts: DraftFull[] = [];

	let state = $state<DraftsState>({
		drafts: [],
		openTabIds: [],
		activeTabId: null,
		activeCode: DEFAULT_CODE
	});

	let saveTimer: ReturnType<typeof setTimeout> | null = null;

	function syncSummariesFromAllDrafts() {
		state.drafts = allDrafts.map((d) => ({
			id: d.id,
			name: d.name,
			updatedAt: d.updatedAt,
			isPhantom: d.isPhantom,
			phantomSlug: d.phantomSlug
		}));
	}

	function persistDraft(draft: DraftFull) {
		// async fire-and-forget; provider catches its own errors
		provider.saveDraft(draft).catch((e) => console.warn('[drafts] saveDraft failed:', e));
	}

	function persistAll(reason = 'persist') {
		if (typeof localStorage === 'undefined') return;
		try {
			localStorage.setItem('nkido-drafts', JSON.stringify(allDrafts));
		} catch (e) {
			console.warn('[drafts] failed to persist all drafts:', e, reason);
		}
	}

	function flushPendingSave() {
		if (saveTimer) {
			clearTimeout(saveTimer);
			saveTimer = null;
			const active = allDrafts.find((d) => d.id === state.activeTabId);
			if (active) persistDraft(active);
		}
	}

	/**
	 * First-launch migration. Returns the seeded draft list.
	 */
	function migrate(): DraftFull[] {
		if (typeof localStorage === 'undefined') return [freshDraft()];

		const existingRaw = localStorage.getItem('nkido-drafts');
		// If nkido-drafts already exists, it always wins — never re-migrate.
		if (existingRaw) {
			try {
				const parsed = JSON.parse(existingRaw);
				if (Array.isArray(parsed) && parsed.length > 0) {
					localStorage.setItem(MIGRATED_KEY, '1');
					return parsed as DraftFull[];
				}
			} catch {
				// fall through to migration
			}
		}

		// Skip migration if flag is set (user manually cleared drafts).
		const migrated = localStorage.getItem(MIGRATED_KEY);
		if (migrated) {
			return [freshDraft()];
		}

		const legacy = localStorage.getItem(LEGACY_CODE_KEY);
		if (legacy && legacy.length > 0) {
			const seeded = freshDraft({ name: 'Untitled', code: legacy });
			localStorage.setItem(MIGRATED_KEY, '1');
			// Do not delete LEGACY_CODE_KEY — one-release safety net.
			return [seeded];
		}

		localStorage.setItem(MIGRATED_KEY, '1');
		return [freshDraft()];
	}

	function restoreOpenTabs(draftIds: Set<string>): { open: string[]; active: string | null } {
		const persistedOpen = readOpenTabs().filter((id) => draftIds.has(id));
		const persistedActive = readActiveTab();

		if (persistedOpen.length === 0) {
			// First launch or no persisted tabs: open the most-recently-updated draft.
			const mru = allDrafts.slice().sort((a, b) => b.updatedAt - a.updatedAt)[0];
			return { open: mru ? [mru.id] : [], active: mru ? mru.id : null };
		}

		const active = persistedActive && persistedOpen.includes(persistedActive)
			? persistedActive
			: persistedOpen[0];
		return { open: persistedOpen, active };
	}

	function init() {
		allDrafts = migrate();
		// Ensure persisted (in case we just migrated)
		persistAll('init-migrate');

		const idSet = new Set(allDrafts.map((d) => d.id));
		const { open, active } = restoreOpenTabs(idSet);

		state.openTabIds = open;
		state.activeTabId = active;
		state.activeCode = active ? (allDrafts.find((d) => d.id === active)?.code ?? DEFAULT_CODE) : DEFAULT_CODE;
		writeOpenTabs(open);
		writeActiveTab(active);
		syncSummariesFromAllDrafts();
	}

	// Browser- and test-only init: skip when localStorage is unavailable
	// (SSR), since the store would emit a randomly-generated draft id that
	// mismatches the client hydration. The client re-imports this module
	// where localStorage exists, init runs, and Svelte's runes drive the
	// first paint from the persisted draft list.
	if (typeof localStorage !== 'undefined') {
		if (typeof window !== 'undefined') {
			window.addEventListener('beforeunload', () => flushPendingSave());
		}
		init();
	}

	function ensureOpen(id: string): void {
		if (!state.openTabIds.includes(id)) {
			state.openTabIds = [...state.openTabIds, id];
			writeOpenTabs(state.openTabIds);
		}
	}

	function setActive(id: string): void {
		if (id === state.activeTabId) return;
		// flush any pending save for the *previous* active draft before switching
		flushPendingSave();
		const draft = allDrafts.find((d) => d.id === id);
		if (!draft) return;
		state.activeTabId = id;
		state.activeCode = draft.code;
		ensureOpen(id);
		writeActiveTab(id);
	}

	function openTab(id: string): void {
		ensureOpen(id);
		setActive(id);
	}

	function closeTab(id: string): void {
		const idx = state.openTabIds.indexOf(id);
		if (idx < 0) return;
		const next = state.openTabIds.filter((x) => x !== id);
		state.openTabIds = next;
		writeOpenTabs(next);

		if (state.activeTabId === id) {
			// pick neighbor: try prev, else first, else null
			const fallback = next[idx - 1] ?? next[0] ?? null;
			if (fallback) {
				setActive(fallback);
			} else {
				// no tabs left; auto-create Untitled
				const fresh = freshDraft();
				allDrafts.push(fresh);
				persistAll('autocreate-after-close');
				syncSummariesFromAllDrafts();
				state.openTabIds = [fresh.id];
				writeOpenTabs(state.openTabIds);
				state.activeTabId = fresh.id;
				state.activeCode = fresh.code;
				writeActiveTab(fresh.id);
			}
		}
	}

	function createDraft(opts: { name?: string; code?: string; activate?: boolean } = {}): string {
		const draft = freshDraft({
			name: opts.name ?? `Untitled ${allDrafts.filter((d) => !d.isPhantom).length + 1}`,
			code: opts.code ?? DEFAULT_CODE
		});
		allDrafts.push(draft);
		persistAll('create');
		syncSummariesFromAllDrafts();
		if (opts.activate !== false) openTab(draft.id);
		return draft.id;
	}

	function renameDraft(id: string, newName: string): void {
		const d = allDrafts.find((x) => x.id === id);
		if (!d) return;
		d.name = newName;
		d.updatedAt = Date.now();
		persistAll('rename');
		syncSummariesFromAllDrafts();
	}

	function deleteDraft(id: string): void {
		const i = allDrafts.findIndex((d) => d.id === id);
		if (i < 0) return;
		allDrafts.splice(i, 1);
		persistAll('delete');
		syncSummariesFromAllDrafts();
		if (state.openTabIds.includes(id)) closeTab(id);
		if (allDrafts.length === 0) {
			const fresh = freshDraft();
			allDrafts.push(fresh);
			persistAll('autocreate-after-delete');
			syncSummariesFromAllDrafts();
			openTab(fresh.id);
		}
	}

	/**
	 * Called by editorStore.setCode on every keystroke when persistence is on.
	 * Mutates the active draft's `code` and debounce-saves.
	 */
	function updateActiveCode(code: string): void {
		const draft = allDrafts.find((d) => d.id === state.activeTabId);
		if (!draft) return;
		if (draft.code === code) return;
		draft.code = code;
		draft.updatedAt = Date.now();
		state.activeCode = code;
		// also refresh the summary's updatedAt so sidebar sort stays current
		syncSummariesFromAllDrafts();

		if (saveTimer) clearTimeout(saveTimer);
		saveTimer = setTimeout(() => {
			persistAll('debounce');
			saveTimer = null;
		}, SAVE_DEBOUNCE_MS);
	}

	/**
	 * Open or focus an inline-URL phantom tab. Does not write to localStorage
	 * until the user types (materialization on first keystroke).
	 */
	function openInlinePhantomTab(opts: { key: string; code: string; title?: string }): string {
		const id = opts.key; // 'inline:<hash>'
		const existing = allDrafts.find((d) => d.id === id);
		if (existing) {
			// reuse existing phantom — load its (potentially edited) code
			openTab(id);
			return id;
		}
		// Add to in-memory list but DO NOT persist yet.
		const phantom: DraftFull = {
			id,
			name: opts.title ?? 'From inline link',
			code: opts.code,
			updatedAt: Date.now(),
			isPhantom: true,
			phantomSlug: null
		};
		allDrafts.push(phantom);
		syncSummariesFromAllDrafts();
		// Open + activate; first keystroke (updateActiveCode) will persist.
		ensureOpen(id);
		state.activeTabId = id;
		state.activeCode = phantom.code;
		writeActiveTab(id);
		return id;
	}

	/**
	 * Open or focus a slug-keyed phantom tab. Mirrors `openInlinePhantomTab`
	 * but the tab id is `slug:<slug>` and `phantomSlug` is set so the
	 * Share dialog / PatchesPanel can recognize the parent share.
	 *
	 * Like inline phantoms, the entry is added to the in-memory list
	 * immediately but only persisted to localStorage on the first
	 * `updateActiveCode` call (lazy materialization).
	 */
	function openSlugPhantomTab(opts: { slug: string; code: string; title?: string | null }): string {
		const id = 'slug:' + opts.slug;
		const existing = allDrafts.find((d) => d.id === id);
		if (existing) {
			openTab(id);
			return id;
		}
		const phantom: DraftFull = {
			id,
			name: (opts.title && opts.title.trim()) || `Patch ${opts.slug}`,
			code: opts.code,
			updatedAt: Date.now(),
			isPhantom: true,
			phantomSlug: opts.slug
		};
		allDrafts.push(phantom);
		syncSummariesFromAllDrafts();
		ensureOpen(id);
		state.activeTabId = id;
		state.activeCode = phantom.code;
		writeActiveTab(id);
		return id;
	}

	/** True if a slug-phantom (with local edits or not) already exists. */
	function findSlugPhantom(slug: string): DraftFull | null {
		return allDrafts.find((d) => d.id === 'slug:' + slug && d.isPhantom) ?? null;
	}

	/**
	 * Remove a slug-phantom from storage and close its tab if open.
	 * Used by PatchesPanel's "Forget" action and when discarding local
	 * edits to revisit the original share.
	 */
	function deleteSlugPhantom(slug: string): void {
		const id = 'slug:' + slug;
		const i = allDrafts.findIndex((d) => d.id === id);
		if (i >= 0) {
			allDrafts.splice(i, 1);
			persistAll('delete-slug-phantom');
			syncSummariesFromAllDrafts();
		}
		if (state.openTabIds.includes(id)) closeTab(id);
	}

	/**
	 * Promote the active phantom draft to a regular named draft.
	 * Replaces the phantom id with a fresh UUID, sets isPhantom=false.
	 */
	function keepActiveAsNamed(name: string): string | null {
		const active = allDrafts.find((d) => d.id === state.activeTabId);
		if (!active || !active.isPhantom) return null;

		const oldId = active.id;
		const newId = newUuid();
		active.id = newId;
		active.name = name;
		active.isPhantom = false;
		active.phantomSlug = null;
		active.updatedAt = Date.now();
		persistAll('keep');
		syncSummariesFromAllDrafts();

		// swap id in openTabIds
		state.openTabIds = state.openTabIds.map((x) => (x === oldId ? newId : x));
		writeOpenTabs(state.openTabIds);
		state.activeTabId = newId;
		writeActiveTab(newId);
		return newId;
	}

	function getActive(): DraftFull | null {
		return allDrafts.find((d) => d.id === state.activeTabId) ?? null;
	}

	return {
		get drafts() { return state.drafts; },
		get openTabIds() { return state.openTabIds; },
		get activeTabId() { return state.activeTabId; },
		get activeCode() { return state.activeCode; },
		get activeDraft() { return getActive(); },

		setActive,
		openTab,
		closeTab,
		createDraft,
		renameDraft,
		deleteDraft,
		updateActiveCode,
		openInlinePhantomTab,
		openSlugPhantomTab,
		findSlugPhantom,
		deleteSlugPhantom,
		keepActiveAsNamed,
		flushPendingSave
	};
}

export const draftsStore = createDraftsStore();
export type DraftsStore = ReturnType<typeof createDraftsStore>;
