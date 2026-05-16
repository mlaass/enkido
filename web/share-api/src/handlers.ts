// Request handlers for the share Worker.
// PRD §5.3.

import { corsHeaders, type CorsEnv } from './cors';
import { isValidSlug, newSlug } from './slug';
import { renderNotFoundHtml, renderOgHtml } from './og';

export interface Env extends CorsEnv {
	DB: D1Database;
	ALLOW_ORIGIN: string;
	ALLOW_ORIGIN_DEV_PATTERN?: string;
	LIVE_ORIGIN: string;
}

const MAX_CODE_BYTES = 16384;
const MAX_TITLE_CHARS = 200;
const MAX_DESC_CHARS = 500;
const SLUG_RETRIES = 3;

function jsonResponse(body: unknown, init: ResponseInit & { headers: Record<string, string> }): Response {
	return new Response(JSON.stringify(body), {
		...init,
		headers: { 'content-type': 'application/json; charset=utf-8', ...init.headers }
	});
}

function sanitizeTitle(raw: unknown): string | null {
	if (typeof raw !== 'string') return null;
	const cleaned = raw.replace(/[\r\n\t]/g, ' ').trim();
	if (!cleaned) return null;
	return cleaned.slice(0, MAX_TITLE_CHARS);
}

function sanitizeDescription(raw: unknown): string | null {
	if (typeof raw !== 'string') return null;
	const trimmed = raw.trim();
	if (!trimmed) return null;
	return trimmed.slice(0, MAX_DESC_CHARS);
}

function codeByteLength(code: string): number {
	return new TextEncoder().encode(code).length;
}

export async function handleShare(req: Request, env: Env, cors: Record<string, string>): Promise<Response> {
	let body: unknown;
	try {
		body = await req.json();
	} catch {
		return jsonResponse({ error: 'invalid_json' }, { status: 400, headers: cors });
	}
	if (!body || typeof body !== 'object') {
		return jsonResponse({ error: 'invalid_body' }, { status: 400, headers: cors });
	}
	const b = body as Record<string, unknown>;

	const code = typeof b.code === 'string' ? b.code : '';
	const codeLen = codeByteLength(code);
	if (codeLen < 1) {
		return jsonResponse({ error: 'code_empty' }, { status: 400, headers: cors });
	}
	if (codeLen > MAX_CODE_BYTES) {
		return jsonResponse(
			{ error: 'code_too_large', limit: MAX_CODE_BYTES, actual: codeLen },
			{ status: 400, headers: cors }
		);
	}

	const title = sanitizeTitle(b.title);
	const description = sanitizeDescription(b.description);

	let parentSlug: string | null = null;
	if (typeof b.parent_slug === 'string' && b.parent_slug.length > 0) {
		if (!isValidSlug(b.parent_slug)) {
			return jsonResponse({ error: 'invalid_parent_slug' }, { status: 400, headers: cors });
		}
		// Best-effort existence check — set NULL if parent missing or deleted,
		// log the anomaly so the operator can spot misbehaviour.
		const parentRow = await env.DB.prepare(
			'SELECT slug FROM patches WHERE slug = ? AND deleted_at IS NULL'
		).bind(b.parent_slug).first<{ slug: string }>();
		if (parentRow) {
			parentSlug = b.parent_slug;
		} else {
			console.warn('[share] parent_slug not found or deleted:', b.parent_slug);
			parentSlug = null;
		}
	}

	const ipHash = await hashClientIp(req);

	for (let attempt = 0; attempt < SLUG_RETRIES; attempt++) {
		const slug = newSlug();
		try {
			await env.DB.prepare(
				`INSERT INTO patches (slug, code, title, description, parent_slug, ip_hash)
				 VALUES (?, ?, ?, ?, ?, ?)`
			).bind(slug, code, title, description, parentSlug, ipHash).run();
			return jsonResponse({ slug }, { status: 200, headers: cors });
		} catch (e) {
			const msg = (e as Error)?.message ?? String(e);
			// D1 surfaces unique-constraint violations via the message; retry.
			if (!/UNIQUE/i.test(msg)) {
				console.error('[share] insert failed:', msg);
				return jsonResponse({ error: 'insert_failed' }, { status: 500, headers: cors });
			}
			console.warn('[share] slug collision, retrying:', slug);
		}
	}

	return jsonResponse({ error: 'slug_collision_giveup' }, { status: 500, headers: cors });
}

export async function handleApiGet(slug: string, env: Env, cors: Record<string, string>): Promise<Response> {
	if (!isValidSlug(slug)) {
		return jsonResponse({ error: 'invalid_slug' }, { status: 400, headers: cors });
	}
	const row = await env.DB.prepare(
		`SELECT slug, code, title, description, parent_slug, created_at
		 FROM patches WHERE slug = ? AND deleted_at IS NULL`
	).bind(slug).first<{
		slug: string;
		code: string;
		title: string | null;
		description: string | null;
		parent_slug: string | null;
		created_at: number;
	}>();

	if (!row) {
		return jsonResponse({ error: 'not_found' }, { status: 404, headers: cors });
	}

	return new Response(JSON.stringify(row), {
		status: 200,
		headers: {
			'content-type': 'application/json; charset=utf-8',
			'cache-control': 'public, max-age=31536000, immutable',
			...cors
		}
	});
}

export async function handleHtmlGet(slug: string, env: Env, selfOrigin: string): Promise<Response> {
	if (!isValidSlug(slug)) {
		return new Response(renderNotFoundHtml(slug, env.LIVE_ORIGIN), {
			status: 404,
			headers: { 'content-type': 'text/html; charset=utf-8' }
		});
	}
	const row = await env.DB.prepare(
		`SELECT slug, code, title, description FROM patches
		 WHERE slug = ? AND deleted_at IS NULL`
	).bind(slug).first<{
		slug: string;
		code: string;
		title: string | null;
		description: string | null;
	}>();

	if (!row) {
		return new Response(renderNotFoundHtml(slug, env.LIVE_ORIGIN), {
			status: 404,
			headers: { 'content-type': 'text/html; charset=utf-8' }
		});
	}

	const html = renderOgHtml({
		slug: row.slug,
		title: row.title,
		description: row.description,
		code: row.code,
		liveOrigin: env.LIVE_ORIGIN,
		selfOrigin
	});

	return new Response(html, {
		status: 200,
		headers: {
			'content-type': 'text/html; charset=utf-8',
			'cache-control': 'public, max-age=300'
		}
	});
}

export async function handleReport(_req: Request, _env: Env, cors: Record<string, string>): Promise<Response> {
	// Stub for Phase 3. Routing is wired so the endpoint exists; the body of
	// the handler is intentionally a 501 until Phase 3 lands.
	return jsonResponse({ error: 'not_implemented' }, { status: 501, headers: cors });
}

async function hashClientIp(req: Request): Promise<string | null> {
	const ip = req.headers.get('cf-connecting-ip') ?? req.headers.get('x-forwarded-for') ?? '';
	if (!ip) return null;
	// Daily-rotating salt would make this a non-trivial cron — for v1 we use a
	// per-day deterministic salt derived from the UTC date so the hash rotates
	// without operator intervention. See PRD §12.
	const today = new Date().toISOString().slice(0, 10);
	const data = new TextEncoder().encode(`${ip}|${today}`);
	const digest = await crypto.subtle.digest('SHA-256', data);
	return Array.from(new Uint8Array(digest)).slice(0, 16).map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Re-export limits so tests can assert against the same values.
export const LIMITS = { MAX_CODE_BYTES, MAX_TITLE_CHARS, MAX_DESC_CHARS, SLUG_RETRIES } as const;

// Re-export for tests
export { corsHeaders };
