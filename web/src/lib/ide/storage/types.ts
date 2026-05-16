/**
 * StorageProvider interface — abstracts persistence of drafts and shares.
 *
 * Phase 1 ships `LocalDraftProvider` (drafts only; share methods throw).
 * Phase 2 adds `WorkerShareProvider` wrapping the local provider with a
 * remote share backend. The future SaaS PRD adds a third provider.
 *
 * PRD: docs/prd-shareable-patches.md §5.2
 */

export interface DraftSummary {
	/** UUID for named drafts; `inline:<hash>` / `phantom:p/<slug>` for phantoms. */
	id: string;
	/** User-editable label. */
	name: string;
	/** Epoch ms of the last edit. */
	updatedAt: number;
	/** True when this draft holds local edits to someone else's shared patch. */
	isPhantom: boolean;
	/** For slug-keyed phantoms (Phase 2+). Null for inline phantoms and named drafts. */
	phantomSlug: string | null;
}

export interface DraftFull extends DraftSummary {
	code: string;
}

export interface ShareInput {
	code: string;
	title?: string;
	description?: string;
	/** Set when forking from a slug share. */
	parentSlug?: string;
}

export interface ShareSummary {
	slug: string;
	title: string | null;
	description: string | null;
	parentSlug: string | null;
	createdAt: number;
}

export interface ShareFull extends ShareSummary {
	code: string;
}

export interface RecentlyVisited {
	slug: string;
	title: string | null;
	visitedAt: number;
}

export interface StorageProvider {
	saveDraft(draft: DraftFull): Promise<void>;
	loadDraft(id: string): Promise<DraftFull | null>;
	listDrafts(): Promise<DraftSummary[]>;
	deleteDraft(id: string): Promise<void>;
	renameDraft(id: string, newName: string): Promise<void>;

	share?(input: ShareInput): Promise<{ slug: string }>;
	fetchShare?(slug: string): Promise<ShareFull | null>;
	reportShare?(slug: string, reason?: string): Promise<void>;

	recordVisited?(slug: string, title: string | null): Promise<void>;
	listRecentlyVisited?(): Promise<RecentlyVisited[]>;
}
