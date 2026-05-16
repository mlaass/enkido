// nkido share-api — Cloudflare Worker entry point.
// PRD: docs/prd-shareable-patches.md §5.3
//
// Routes:
//   POST /share         publish a patch  (rate-limited via Cloudflare RL Rule)
//   GET  /api/p/:slug   JSON read for the SPA
//   GET  /p/:slug       OG-tagged HTML + meta-refresh to live.nkido.cc/p/:slug
//   POST /report        flag a patch for operator review (Phase 3 — stub)

import { corsHeaders } from './cors';
import {
	handleApiGet,
	handleHtmlGet,
	handleReport,
	handleShare,
	type Env
} from './handlers';
import { isValidSlug } from './slug';

const API_PATH = /^\/api\/p\/([^/]+)\/?$/;
const HTML_PATH = /^\/p\/([^/]+)\/?$/;

export default {
	async fetch(req: Request, env: Env): Promise<Response> {
		const url = new URL(req.url);
		const cors = corsHeaders(env, req);

		if (req.method === 'OPTIONS') {
			return new Response(null, { status: 204, headers: cors });
		}

		if (req.method === 'POST' && url.pathname === '/share') {
			return handleShare(req, env, cors);
		}

		const apiMatch = url.pathname.match(API_PATH);
		if (req.method === 'GET' && apiMatch) {
			const slug = apiMatch[1];
			if (!isValidSlug(slug)) {
				return new Response(JSON.stringify({ error: 'invalid_slug' }), {
					status: 400,
					headers: { 'content-type': 'application/json; charset=utf-8', ...cors }
				});
			}
			return handleApiGet(slug, env, cors);
		}

		const htmlMatch = url.pathname.match(HTML_PATH);
		if (req.method === 'GET' && htmlMatch) {
			return handleHtmlGet(htmlMatch[1], env, url.origin);
		}

		if (req.method === 'POST' && url.pathname === '/report') {
			return handleReport(req, env, cors);
		}

		return new Response(JSON.stringify({ error: 'not_found' }), {
			status: 404,
			headers: { 'content-type': 'application/json; charset=utf-8', ...cors }
		});
	}
};
