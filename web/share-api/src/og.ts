// Server-rendered OG HTML for /p/:slug. Body is what social cards unfurl and
// what JS-disabled visitors see; meta-refresh hops to the live SPA so users
// land in the editor automatically.
// PRD §6.5.

export interface OgInput {
	slug: string;
	title: string | null;
	description: string | null;
	code: string;
	liveOrigin: string;
	selfOrigin: string;
}

function escape(s: string): string {
	return s
		.replace(/&/g, '&amp;')
		.replace(/</g, '&lt;')
		.replace(/>/g, '&gt;')
		.replace(/"/g, '&quot;')
		.replace(/'/g, '&#39;');
}

export function renderOgHtml(input: OgInput): string {
	const titleText = (input.title ?? `Patch ${input.slug}`).trim() || `Patch ${input.slug}`;
	const descText = (input.description ?? 'A patch shared from the nkido live coding IDE.').trim() ||
		'A patch shared from the nkido live coding IDE.';
	const liveUrl = `${input.liveOrigin}/p/${input.slug}`;
	const selfUrl = `${input.selfOrigin}/p/${input.slug}`;
	// Cap the inline code preview so the OG body stays reasonable. Full code
	// is fetched by the SPA at /api/p/:slug.
	const preview = input.code.length > 2000 ? input.code.slice(0, 2000) + '\n…' : input.code;

	return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>${escape(titleText)} — nkido</title>
<meta name="description" content="${escape(descText)}">
<meta property="og:title" content="${escape(titleText)}">
<meta property="og:description" content="${escape(descText)}">
<meta property="og:url" content="${escape(selfUrl)}">
<meta property="og:type" content="website">
<meta property="og:site_name" content="nkido">
<meta http-equiv="refresh" content="0; url=${escape(liveUrl)}">
<link rel="canonical" href="${escape(liveUrl)}">
<style>
body { font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif; max-width: 720px; margin: 2rem auto; padding: 0 1rem; color: #222; }
h1 { margin: 0 0 .25rem; font-size: 1.25rem; }
p.desc { color: #555; margin: 0 0 1rem; }
pre { background: #f6f8fa; padding: 1rem; border-radius: 6px; overflow-x: auto; font-size: 13px; line-height: 1.5; }
a.cta { display: inline-block; margin-top: 1rem; padding: .5rem 1rem; background: #2c5cdb; color: #fff; text-decoration: none; border-radius: 6px; }
</style>
</head>
<body>
<main>
<h1>${escape(titleText)}</h1>
<p class="desc">${escape(descText)}</p>
<pre><code>${escape(preview)}</code></pre>
<a class="cta" href="${escape(liveUrl)}">Open in editor →</a>
</main>
</body>
</html>`;
}

export function renderNotFoundHtml(slug: string, liveOrigin: string): string {
	return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Patch not found — nkido</title>
<meta name="robots" content="noindex">
<link rel="canonical" href="${escape(liveOrigin)}/">
<style>
body { font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif; max-width: 480px; margin: 4rem auto; padding: 0 1rem; color: #222; text-align: center; }
h1 { font-size: 1.25rem; }
a { color: #2c5cdb; }
</style>
</head>
<body>
<h1>Patch not found</h1>
<p>The patch <code>/p/${escape(slug)}</code> does not exist or was removed.</p>
<p><a href="${escape(liveOrigin)}/">Back to the editor →</a></p>
</body>
</html>`;
}
