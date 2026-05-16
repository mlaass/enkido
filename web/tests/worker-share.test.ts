/**
 * WorkerShareProvider — share/fetchShare/reportShare against a stubbed fetch.
 * PRD §11.2.
 */

import { beforeEach, describe, expect, it, vi } from 'vitest';
import { WorkerShareProvider, WorkerShareApiError } from '../src/lib/ide/storage/worker-share';

function jsonRes(status: number, body: unknown): Response {
	return new Response(JSON.stringify(body), {
		status,
		headers: { 'content-type': 'application/json' }
	});
}

describe('WorkerShareProvider.share', () => {
	beforeEach(() => {
		// Provide a minimal localStorage shim because LocalDraftProvider may be
		// touched by the composed instance during construction in some envs.
		const store = new Map<string, string>();
		globalThis.localStorage = {
			getItem: (k: string) => store.get(k) ?? null,
			setItem: (k: string, v: string) => void store.set(k, v),
			removeItem: (k: string) => void store.delete(k),
			clear: () => store.clear(),
			length: 0,
			key: () => null
		} as unknown as Storage;
	});

	it('POSTs JSON to <apiBase>/share and returns the slug', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(200, { slug: 'k7gp2x99' }));
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });

		const result = await provider.share({ code: 'sin(440)', title: 'moody' });
		expect(result.slug).toBe('k7gp2x99');

		expect(fetchImpl).toHaveBeenCalledTimes(1);
		const [url, init] = fetchImpl.mock.calls[0];
		expect(url).toBe('http://localhost:8787/share');
		expect(init.method).toBe('POST');
		expect(init.headers['content-type']).toBe('application/json');
		expect(JSON.parse(init.body)).toEqual({ code: 'sin(440)', title: 'moody' });
	});

	it('includes parent_slug when forking', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(200, { slug: 'aaaaaaaa' }));
		const provider = new WorkerShareProvider({ apiBase: 'https://share.test', fetchImpl });

		await provider.share({ code: 'x', parentSlug: 'bbbbbbbb' });
		const body = JSON.parse(fetchImpl.mock.calls[0][1].body);
		expect(body.parent_slug).toBe('bbbbbbbb');
	});

	it('trims trailing slash from apiBase', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(200, { slug: 'cccccccc' }));
		const provider = new WorkerShareProvider({ apiBase: 'https://share.test/', fetchImpl });
		await provider.share({ code: 'x' });
		expect(fetchImpl.mock.calls[0][0]).toBe('https://share.test/share');
	});

	it('throws WorkerShareApiError on non-2xx with payload', async () => {
		const fetchImpl = vi.fn().mockImplementation(() =>
			Promise.resolve(jsonRes(400, { error: 'code_too_large', limit: 16384 }))
		);
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });

		let caught: unknown = null;
		try {
			await provider.share({ code: 'x' });
		} catch (e) {
			caught = e;
		}
		expect(caught).toBeInstanceOf(WorkerShareApiError);
		const err = caught as WorkerShareApiError;
		expect(err.status).toBe(400);
		expect(err.code).toBe('code_too_large');
		expect((err.payload as { limit: number }).limit).toBe(16384);
	});

	it('throws when Worker returns 200 but no slug', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(200, {}));
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });
		await expect(provider.share({ code: 'x' })).rejects.toThrow(WorkerShareApiError);
	});

	it('propagates network errors', async () => {
		const fetchImpl = vi.fn().mockRejectedValue(new TypeError('Failed to fetch'));
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });
		await expect(provider.share({ code: 'x' })).rejects.toThrow('Failed to fetch');
	});
});

describe('WorkerShareProvider.fetchShare', () => {
	it('parses JSON and normalizes the snake_case payload', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(200, {
			slug: 'k7gp2x99',
			code: 'sin(440)',
			title: 'moody',
			description: 'tweak @',
			parent_slug: 'abcdefgh',
			created_at: 1234567890000
		}));
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });

		const share = await provider.fetchShare('k7gp2x99');
		expect(share).toEqual({
			slug: 'k7gp2x99',
			code: 'sin(440)',
			title: 'moody',
			description: 'tweak @',
			parentSlug: 'abcdefgh',
			createdAt: 1234567890000
		});

		expect(fetchImpl.mock.calls[0][0]).toBe('http://localhost:8787/api/p/k7gp2x99');
	});

	it('returns null on 404', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(404, { error: 'not_found' }));
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });
		expect(await provider.fetchShare('zzzzzzzz')).toBeNull();
	});

	it('throws on non-2xx, non-404 error', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(500, { error: 'insert_failed' }));
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });
		await expect(provider.fetchShare('aaaaaaaa')).rejects.toThrow(WorkerShareApiError);
	});
});

describe('WorkerShareProvider.reportShare', () => {
	it('POSTs slug + reason to /report', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(200, { ok: true }));
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });

		await provider.reportShare('aaaaaaaa', 'spam');
		const [url, init] = fetchImpl.mock.calls[0];
		expect(url).toBe('http://localhost:8787/report');
		expect(init.method).toBe('POST');
		expect(JSON.parse(init.body)).toEqual({ slug: 'aaaaaaaa', reason: 'spam' });
	});

	it('throws on error response', async () => {
		const fetchImpl = vi.fn().mockResolvedValue(jsonRes(501, { error: 'not_implemented' }));
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787', fetchImpl });
		await expect(provider.reportShare('aaaaaaaa')).rejects.toThrow(WorkerShareApiError);
	});
});

describe('WorkerShareProvider draft delegation', () => {
	it('delegates draft CRUD to the wrapped local provider', async () => {
		const provider = new WorkerShareProvider({ apiBase: 'http://localhost:8787' });
		await provider.saveDraft({
			id: 'test-id',
			name: 'test',
			code: 'sin(440)',
			updatedAt: Date.now(),
			isPhantom: false,
			phantomSlug: null
		});
		const loaded = await provider.loadDraft('test-id');
		expect(loaded?.code).toBe('sin(440)');
		const list = await provider.listDrafts();
		expect(list.find((d) => d.id === 'test-id')).toBeTruthy();
		await provider.deleteDraft('test-id');
		expect(await provider.loadDraft('test-id')).toBeNull();
	});
});
