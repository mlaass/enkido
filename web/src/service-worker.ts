/// <reference types="@sveltejs/kit" />
/// <reference no-default-lib="true" />
/// <reference lib="esnext" />
/// <reference lib="webworker" />

import { build, files, prerendered, version } from '$service-worker';

const sw = self as unknown as ServiceWorkerGlobalScope;

const CACHE = `nkido-cache-${version}`;

// App shell: hashed JS/CSS bundles, static assets (icons, manifest,
// WASM, AudioWorklet), prerendered routes, AND the SPA fallback at `/`.
// The root isn't in `prerendered` (no `export const prerender = true`),
// but adapter-static's `fallback: 'index.html'` means `/` returns the
// SPA shell — we need it explicitly so offline navigations resolve.
const PRECACHE: readonly string[] = [...build, ...files, ...prerendered, '/'];

sw.addEventListener('install', (event) => {
	event.waitUntil(
		(async () => {
			const cache = await caches.open(CACHE);
			// Add resources individually so one failure doesn't abort the
			// whole install (some assets may 404 on initial dev builds).
			await Promise.allSettled(PRECACHE.map((url) => cache.add(url)));
			await sw.skipWaiting();
		})()
	);
});

sw.addEventListener('activate', (event) => {
	event.waitUntil(
		(async () => {
			for (const key of await caches.keys()) {
				if (key !== CACHE) await caches.delete(key);
			}
			await sw.clients.claim();
		})()
	);
});

sw.addEventListener('fetch', (event) => {
	const { request } = event;

	// Only handle same-origin GETs. Cross-origin (share API, etc.) and
	// non-GET methods go through untouched.
	if (request.method !== 'GET') return;
	const url = new URL(request.url);
	if (url.origin !== self.location.origin) return;

	// Avoid intercepting Range requests — they'd break partial audio
	// fetches if a host ever streams samples.
	if (request.headers.has('range')) return;

	event.respondWith(handle(request, url));
});

async function handle(request: Request, url: URL): Promise<Response> {
	const cache = await caches.open(CACHE);

	// 1. Precached app shell → cache-first.
	if (PRECACHE.includes(url.pathname)) {
		const hit = await cache.match(request);
		if (hit) return hit;
	}

	// 2. Docs markdown (and other dynamic same-origin GETs) →
	//    network-first, cache the response, fall back to cache offline.
	try {
		const response = await fetch(request);
		if (response.ok && response.type === 'basic') {
			cache.put(request, response.clone()).catch(() => {});
		}
		return response;
	} catch {
		const hit = await cache.match(request);
		if (hit) return hit;
		// SPA navigation fallback: a client-routed URL (e.g. /p/abc) hits
		// the SW with mode "navigate" but isn't in our precache; serving
		// the cached `/` shell lets the router take over once online JS
		// boots, instead of dumping a hard network error.
		if (request.mode === 'navigate') {
			const shell = await cache.match('/');
			if (shell) return shell;
		}
		throw new Error(`offline and no cached response for ${url.pathname}`);
	}
}
