import { env, SELF } from 'cloudflare:test';
import { beforeEach, describe, expect, it } from 'vitest';

import worker from '../src/index';
import { LIMITS } from '../src/handlers';
import { isValidSlug, newSlug } from '../src/slug';

// Apply the schema fresh for each test so rows don't leak across cases.
async function resetDb() {
	await env.DB.exec('DROP TABLE IF EXISTS patches');
	const schema = `
		CREATE TABLE patches (
			slug          TEXT PRIMARY KEY,
			code          TEXT NOT NULL CHECK (length(code) BETWEEN 1 AND 16384),
			title         TEXT CHECK (title IS NULL OR length(title) <= 200),
			description   TEXT CHECK (description IS NULL OR length(description) <= 500),
			parent_slug   TEXT REFERENCES patches(slug) ON DELETE SET NULL,
			created_at    INTEGER NOT NULL DEFAULT (unixepoch() * 1000),
			ip_hash       TEXT,
			reported_at   INTEGER,
			report_count  INTEGER NOT NULL DEFAULT 0,
			deleted_at    INTEGER
		);
	`.replace(/\s+/g, ' ').trim();
	await env.DB.exec(schema);
}

const ORIGIN = 'http://localhost:5173';
const SELF_ORIGIN = 'https://share.example.test';

function callWorker(path: string, init: RequestInit = {}): Promise<Response> {
	const headers = new Headers(init.headers);
	if (!headers.has('Origin')) headers.set('Origin', ORIGIN);
	return worker.fetch(
		new Request(SELF_ORIGIN + path, { ...init, headers }),
		env as never
	);
}

async function postShare(body: unknown, extra: RequestInit = {}): Promise<Response> {
	return callWorker('/share', {
		method: 'POST',
		headers: { 'content-type': 'application/json', ...(extra.headers as Record<string, string> ?? {}) },
		body: JSON.stringify(body),
		...extra
	});
}

describe('POST /share', () => {
	beforeEach(resetDb);

	it('returns a valid slug for a valid body', async () => {
		const res = await postShare({ code: 'sin(440) |> out(@, @)' });
		expect(res.status).toBe(200);
		const data = (await res.json()) as { slug: string };
		expect(isValidSlug(data.slug)).toBe(true);

		const row = await env.DB.prepare('SELECT code FROM patches WHERE slug = ?').bind(data.slug).first();
		expect(row).toBeTruthy();
		expect((row as { code: string }).code).toBe('sin(440) |> out(@, @)');
	});

	it('rejects empty code', async () => {
		const res = await postShare({ code: '' });
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('code_empty');
	});

	it('rejects code over 16 KB', async () => {
		const big = 'x'.repeat(LIMITS.MAX_CODE_BYTES + 1);
		const res = await postShare({ code: big });
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('code_too_large');
	});

	it('accepts code at exactly 16 KB', async () => {
		const max = 'x'.repeat(LIMITS.MAX_CODE_BYTES);
		const res = await postShare({ code: max });
		expect(res.status).toBe(200);
	});

	it('trims title to 200 chars and strips newlines', async () => {
		const longTitle = 'A'.repeat(300) + '\nB';
		const res = await postShare({ code: 'x', title: longTitle });
		expect(res.status).toBe(200);
		const { slug } = (await res.json()) as { slug: string };
		const row = await env.DB.prepare('SELECT title FROM patches WHERE slug = ?').bind(slug).first<{ title: string }>();
		expect(row?.title).toHaveLength(LIMITS.MAX_TITLE_CHARS);
		expect(row?.title).not.toMatch(/[\r\n\t]/);
	});

	it('trims description to 500 chars', async () => {
		const longDesc = 'd'.repeat(800);
		const res = await postShare({ code: 'x', description: longDesc });
		expect(res.status).toBe(200);
		const { slug } = (await res.json()) as { slug: string };
		const row = await env.DB.prepare('SELECT description FROM patches WHERE slug = ?').bind(slug).first<{ description: string }>();
		expect(row?.description).toHaveLength(LIMITS.MAX_DESC_CHARS);
	});

	it('rejects malformed JSON body', async () => {
		const res = await callWorker('/share', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: '{not json'
		});
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('invalid_json');
	});

	it('preserves parent_slug when it references an existing share', async () => {
		const parentRes = await postShare({ code: 'parent' });
		const parent = (await parentRes.json()) as { slug: string };

		const childRes = await postShare({ code: 'child', parent_slug: parent.slug });
		expect(childRes.status).toBe(200);
		const child = (await childRes.json()) as { slug: string };

		const row = await env.DB.prepare('SELECT parent_slug FROM patches WHERE slug = ?').bind(child.slug).first<{ parent_slug: string | null }>();
		expect(row?.parent_slug).toBe(parent.slug);
	});

	it('nulls parent_slug when it references a non-existent share', async () => {
		const ghost = newSlug();
		const res = await postShare({ code: 'orphan', parent_slug: ghost });
		expect(res.status).toBe(200);
		const child = (await res.json()) as { slug: string };

		const row = await env.DB.prepare('SELECT parent_slug FROM patches WHERE slug = ?').bind(child.slug).first<{ parent_slug: string | null }>();
		expect(row?.parent_slug).toBeNull();
	});

	it('nulls parent_slug when it references a soft-deleted share', async () => {
		const parentRes = await postShare({ code: 'parent' });
		const parent = (await parentRes.json()) as { slug: string };
		await env.DB.prepare('UPDATE patches SET deleted_at = unixepoch()*1000 WHERE slug = ?').bind(parent.slug).run();

		const res = await postShare({ code: 'orphan', parent_slug: parent.slug });
		expect(res.status).toBe(200);
		const child = (await res.json()) as { slug: string };
		const row = await env.DB.prepare('SELECT parent_slug FROM patches WHERE slug = ?').bind(child.slug).first<{ parent_slug: string | null }>();
		expect(row?.parent_slug).toBeNull();
	});

	it('rejects malformed parent_slug', async () => {
		const res = await postShare({ code: 'x', parent_slug: 'not-a-slug!' });
		expect(res.status).toBe(400);
	});
});

describe('GET /api/p/:slug', () => {
	beforeEach(resetDb);

	it('returns 200 + JSON for an existing share', async () => {
		const created = await postShare({ code: 'hello world', title: 't', description: 'd' });
		const { slug } = (await created.json()) as { slug: string };

		const res = await callWorker(`/api/p/${slug}`);
		expect(res.status).toBe(200);
		expect(res.headers.get('cache-control')).toBe('public, max-age=31536000, immutable');
		const data = (await res.json()) as { slug: string; code: string; title: string; description: string };
		expect(data.slug).toBe(slug);
		expect(data.code).toBe('hello world');
		expect(data.title).toBe('t');
		expect(data.description).toBe('d');
	});

	it('returns 404 for a non-existent slug', async () => {
		const res = await callWorker(`/api/p/${newSlug()}`);
		expect(res.status).toBe(404);
	});

	it('returns 404 for a soft-deleted slug', async () => {
		const created = await postShare({ code: 'doomed' });
		const { slug } = (await created.json()) as { slug: string };
		await env.DB.prepare('UPDATE patches SET deleted_at = unixepoch()*1000 WHERE slug = ?').bind(slug).run();
		const res = await callWorker(`/api/p/${slug}`);
		expect(res.status).toBe(404);
	});

	it('returns 400 for an invalid slug shape', async () => {
		const res = await callWorker('/api/p/not-a-slug');
		expect(res.status).toBe(400);
	});
});

describe('GET /p/:slug (OG HTML)', () => {
	beforeEach(resetDb);

	it('renders OG tags + meta-refresh for an existing share', async () => {
		const created = await postShare({ code: 'sin(440) |> out(@, @)', title: 'moody bass', description: 'tweak @' });
		const { slug } = (await created.json()) as { slug: string };

		const res = await callWorker(`/p/${slug}`);
		expect(res.status).toBe(200);
		expect(res.headers.get('content-type')).toMatch(/text\/html/);
		const html = await res.text();
		expect(html).toContain('og:title');
		expect(html).toContain('moody bass');
		expect(html).toContain('tweak @');
		expect(html).toMatch(/<meta http-equiv="refresh"[^>]*url=http:\/\/localhost:5173\/p\//);
		expect(html).toContain(`/p/${slug}`);
	});

	it('returns 404 HTML for a missing slug', async () => {
		const res = await callWorker(`/p/${newSlug()}`);
		expect(res.status).toBe(404);
		expect(res.headers.get('content-type')).toMatch(/text\/html/);
		const html = await res.text();
		expect(html).toContain('Patch not found');
	});

	it('escapes HTML in title and description', async () => {
		const created = await postShare({
			code: 'x',
			title: '<script>alert(1)</script>',
			description: '<img onerror=alert(2)>'
		});
		const { slug } = (await created.json()) as { slug: string };

		const res = await callWorker(`/p/${slug}`);
		const html = await res.text();
		expect(html).not.toContain('<script>alert(1)</script>');
		expect(html).toContain('&lt;script&gt;');
		expect(html).not.toContain('<img onerror=alert(2)>');
	});
});

describe('OPTIONS preflight', () => {
	it('returns CORS headers', async () => {
		const res = await callWorker('/share', { method: 'OPTIONS' });
		expect(res.status).toBe(204);
		expect(res.headers.get('Access-Control-Allow-Origin')).toBe(ORIGIN);
		expect(res.headers.get('Access-Control-Allow-Methods')).toContain('POST');
	});

	it('matches the dev origin pattern for arbitrary localhost ports', async () => {
		const res = await callWorker('/share', {
			method: 'OPTIONS',
			headers: { Origin: 'http://localhost:65432' }
		});
		expect(res.headers.get('Access-Control-Allow-Origin')).toBe('http://localhost:65432');
	});
});

async function postReport(body: unknown, extra: RequestInit = {}): Promise<Response> {
	return callWorker('/report', {
		method: 'POST',
		headers: { 'content-type': 'application/json', ...(extra.headers as Record<string, string> ?? {}) },
		body: JSON.stringify(body),
		...extra
	});
}

async function createPatch(code = 'sin(440) |> out(@, @)'): Promise<string> {
	const res = await postShare({ code });
	const { slug } = (await res.json()) as { slug: string };
	return slug;
}

describe('POST /report', () => {
	beforeEach(resetDb);

	it('returns 200 + { ok: true } for an existing slug and flags the row', async () => {
		const slug = await createPatch();

		const res = await postReport({ slug });
		expect(res.status).toBe(200);
		const data = (await res.json()) as { ok: boolean };
		expect(data.ok).toBe(true);

		const row = await env.DB
			.prepare('SELECT reported_at, report_count FROM patches WHERE slug = ?')
			.bind(slug)
			.first<{ reported_at: number | null; report_count: number }>();
		expect(row?.reported_at).toBeTypeOf('number');
		expect(row?.reported_at).toBeGreaterThan(0);
		expect(row?.report_count).toBe(1);
	});

	it('preserves the first report timestamp across repeated reports', async () => {
		const slug = await createPatch();

		await postReport({ slug });
		const after1 = await env.DB
			.prepare('SELECT reported_at, report_count FROM patches WHERE slug = ?')
			.bind(slug)
			.first<{ reported_at: number; report_count: number }>();
		expect(after1?.report_count).toBe(1);

		// Wait long enough that unixepoch() * 1000 would tick if reported_at
		// were recomputed.
		await new Promise((r) => setTimeout(r, 15));

		await postReport({ slug });
		const after2 = await env.DB
			.prepare('SELECT reported_at, report_count FROM patches WHERE slug = ?')
			.bind(slug)
			.first<{ reported_at: number; report_count: number }>();
		expect(after2?.report_count).toBe(2);
		expect(after2?.reported_at).toBe(after1?.reported_at);
	});

	it('returns 404 for an unknown slug', async () => {
		const res = await postReport({ slug: newSlug() });
		expect(res.status).toBe(404);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('not_found');
	});

	it('returns 404 for a soft-deleted slug and does not mutate the row', async () => {
		const slug = await createPatch();
		await env.DB
			.prepare('UPDATE patches SET deleted_at = unixepoch()*1000 WHERE slug = ?')
			.bind(slug)
			.run();

		const res = await postReport({ slug });
		expect(res.status).toBe(404);

		const row = await env.DB
			.prepare('SELECT reported_at, report_count FROM patches WHERE slug = ?')
			.bind(slug)
			.first<{ reported_at: number | null; report_count: number }>();
		expect(row?.reported_at).toBeNull();
		expect(row?.report_count).toBe(0);
	});

	it('rejects a malformed slug', async () => {
		const res = await postReport({ slug: 'not-a-slug!' });
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('invalid_slug');
	});

	it('rejects a missing slug field', async () => {
		const res = await postReport({});
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('invalid_slug');
	});

	it('rejects a non-JSON body', async () => {
		const res = await callWorker('/report', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: '{not json'
		});
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('invalid_json');
	});

	it('rejects a JSON array body', async () => {
		const res = await callWorker('/report', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify([])
		});
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('invalid_body');
	});

	it('accepts a reason at exactly 500 chars', async () => {
		const slug = await createPatch();
		const reason = 'r'.repeat(LIMITS.MAX_REASON_CHARS);
		const res = await postReport({ slug, reason });
		expect(res.status).toBe(200);
	});

	it('rejects a reason over 500 chars after trim', async () => {
		const slug = await createPatch();
		const reason = '  ' + 'r'.repeat(LIMITS.MAX_REASON_CHARS + 1) + '  ';
		const res = await postReport({ slug, reason });
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string; limit: number };
		expect(data.error).toBe('reason_too_long');
		expect(data.limit).toBe(LIMITS.MAX_REASON_CHARS);
	});

	it('accepts a missing or null reason', async () => {
		const slug = await createPatch();
		const res1 = await postReport({ slug });
		expect(res1.status).toBe(200);
		const res2 = await postReport({ slug, reason: null });
		expect(res2.status).toBe(200);
	});

	it('rejects a reason of the wrong type', async () => {
		const slug = await createPatch();
		const res = await postReport({ slug, reason: 42 });
		expect(res.status).toBe(400);
		const data = (await res.json()) as { error: string };
		expect(data.error).toBe('reason_too_long');
	});
});

describe('routing', () => {
	it('returns 404 for unknown paths', async () => {
		const res = await callWorker('/nope');
		expect(res.status).toBe(404);
	});
});

// Keep `SELF` import alive — vitest-pool-workers requires it as part of the
// runtime even if unused directly.
void SELF;
