// CORS header builder. In production, only the configured `ALLOW_ORIGIN`
// (typically https://live.nkido.cc) is permitted. In dev, the worker may
// additionally be bound with `ALLOW_ORIGIN_DEV_PATTERN` (a regex applied to
// the request's `Origin` header) to accept any localhost port Vite picks.
// PRD §5.6.

export interface CorsEnv {
	ALLOW_ORIGIN: string;
	ALLOW_ORIGIN_DEV_PATTERN?: string;
}

export function corsHeaders(env: CorsEnv, req: Request): Record<string, string> {
	const reqOrigin = req.headers.get('Origin') ?? '';
	let allowOrigin = '';

	if (reqOrigin && env.ALLOW_ORIGIN_DEV_PATTERN) {
		try {
			if (new RegExp(env.ALLOW_ORIGIN_DEV_PATTERN).test(reqOrigin)) {
				allowOrigin = reqOrigin;
			}
		} catch {
			// invalid regex — ignore the dev pattern, fall through to prod origin
		}
	}

	if (!allowOrigin && reqOrigin && reqOrigin === env.ALLOW_ORIGIN) {
		allowOrigin = env.ALLOW_ORIGIN;
	}

	// Default to the configured production origin if no match — keeps the
	// header present so preflight responses are well-formed even for non-CORS
	// callers (e.g. curl).
	if (!allowOrigin) allowOrigin = env.ALLOW_ORIGIN;

	return {
		'Access-Control-Allow-Origin': allowOrigin,
		'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
		'Access-Control-Allow-Headers': 'content-type',
		'Access-Control-Max-Age': '86400',
		Vary: 'Origin'
	};
}
