/**
 * WorkerShareProvider — proxies share endpoints to a Cloudflare Worker
 * (typically share.nkido.cc) and delegates draft + recently-visited storage
 * to a wrapped LocalDraftProvider.
 *
 * PRD: docs/prd-shareable-patches.md §5.2
 */

import type {
	DraftFull,
	DraftSummary,
	RecentlyVisited,
	ShareFull,
	ShareInput,
	StorageProvider
} from './types';
import { LocalDraftProvider } from './local-draft';

export interface WorkerShareError {
	error: string;
	[key: string]: unknown;
}

export class WorkerShareApiError extends Error {
	readonly status: number;
	readonly code: string;
	readonly payload: unknown;
	constructor(status: number, code: string, payload: unknown, message?: string) {
		super(message ?? `${code} (HTTP ${status})`);
		this.name = 'WorkerShareApiError';
		this.status = status;
		this.code = code;
		this.payload = payload;
	}
}

export interface WorkerShareProviderOptions {
	apiBase: string;
	local?: LocalDraftProvider;
	fetchImpl?: typeof fetch;
}

export class WorkerShareProvider implements StorageProvider {
	private readonly apiBase: string;
	private readonly local: LocalDraftProvider;
	private readonly fetchImpl: typeof fetch;

	constructor(opts: WorkerShareProviderOptions) {
		this.apiBase = opts.apiBase.replace(/\/$/, '');
		this.local = opts.local ?? new LocalDraftProvider();
		this.fetchImpl = opts.fetchImpl ?? fetch;
	}

	/** Public URL (Worker-served OG HTML) for a slug. */
	shareUrl(slug: string): string {
		return `${this.apiBase}/p/${slug}`;
	}

	saveDraft(draft: DraftFull): Promise<void> { return this.local.saveDraft(draft); }
	loadDraft(id: string): Promise<DraftFull | null> { return this.local.loadDraft(id); }
	listDrafts(): Promise<DraftSummary[]> { return this.local.listDrafts(); }
	deleteDraft(id: string): Promise<void> { return this.local.deleteDraft(id); }
	renameDraft(id: string, name: string): Promise<void> { return this.local.renameDraft(id, name); }
	recordVisited(slug: string, title: string | null): Promise<void> {
		return this.local.recordVisited(slug, title);
	}
	listRecentlyVisited(): Promise<RecentlyVisited[]> {
		return this.local.listRecentlyVisited();
	}

	async share(input: ShareInput): Promise<{ slug: string }> {
		const body: Record<string, unknown> = { code: input.code };
		if (input.title) body.title = input.title;
		if (input.description) body.description = input.description;
		if (input.parentSlug) body.parent_slug = input.parentSlug;

		const res = await this.fetchImpl(`${this.apiBase}/share`, {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify(body)
		});
		const data = await safeJson(res);
		if (!res.ok) {
			throw new WorkerShareApiError(res.status, errorCode(data), data);
		}
		const slug = (data as { slug?: unknown })?.slug;
		if (typeof slug !== 'string' || slug.length === 0) {
			throw new WorkerShareApiError(res.status, 'invalid_response', data, 'Worker returned no slug');
		}
		return { slug };
	}

	async fetchShare(slug: string): Promise<ShareFull | null> {
		const res = await this.fetchImpl(`${this.apiBase}/api/p/${encodeURIComponent(slug)}`, {
			method: 'GET',
			headers: { accept: 'application/json' }
		});
		if (res.status === 404) return null;
		const data = await safeJson(res);
		if (!res.ok) {
			throw new WorkerShareApiError(res.status, errorCode(data), data);
		}
		return normalizeShare(data);
	}

	async reportShare(slug: string, reason?: string): Promise<void> {
		const res = await this.fetchImpl(`${this.apiBase}/report`, {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ slug, reason })
		});
		if (!res.ok) {
			const data = await safeJson(res);
			throw new WorkerShareApiError(res.status, errorCode(data), data);
		}
	}
}

async function safeJson(res: Response): Promise<unknown> {
	const text = await res.text();
	if (!text) return null;
	try {
		return JSON.parse(text);
	} catch {
		return { error: 'invalid_json_response', raw: text.slice(0, 200) };
	}
}

function errorCode(data: unknown): string {
	if (data && typeof data === 'object' && 'error' in data && typeof (data as WorkerShareError).error === 'string') {
		return (data as WorkerShareError).error;
	}
	return 'http_error';
}

function normalizeShare(data: unknown): ShareFull | null {
	if (!data || typeof data !== 'object') return null;
	const d = data as Record<string, unknown>;
	if (typeof d.slug !== 'string' || typeof d.code !== 'string') return null;
	return {
		slug: d.slug,
		code: d.code,
		title: typeof d.title === 'string' ? d.title : null,
		description: typeof d.description === 'string' ? d.description : null,
		parentSlug: typeof d.parent_slug === 'string' ? d.parent_slug : null,
		createdAt: typeof d.created_at === 'number' ? d.created_at : 0
	};
}
