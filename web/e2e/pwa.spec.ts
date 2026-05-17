import { expect, test } from '@playwright/test';

// Verifies the PWA structural and runtime contract shipped in commit
// 947d107: manifest fields, app.html meta tags, service-worker
// registration, precache contents, and offline fallback.

test.describe('PWA manifest', () => {
	test('manifest.json is served with the expected fields', async ({ request }) => {
		const res = await request.get('/manifest.json');
		expect(res.status()).toBe(200);

		const manifest = await res.json();
		expect(manifest.name).toBe('NKIDO — Live Coding Synthesis');
		expect(manifest.short_name).toBe('NKIDO');
		expect(manifest.display).toBe('standalone');
		expect(manifest.scope).toBe('/');
		expect(manifest.start_url).toBe('/');
		expect(manifest.theme_color).toBe('#0a0a0a');
		expect(manifest.background_color).toBe('#0a0a0a');

		expect(Array.isArray(manifest.icons)).toBe(true);
		expect(manifest.icons.length).toBeGreaterThanOrEqual(3);

		const sizes = manifest.icons.map((i: { sizes: string }) => i.sizes);
		expect(sizes).toEqual(expect.arrayContaining(['192x192', '512x512']));

		// At least one icon must be `maskable` for Android adaptive icons.
		const purposes = manifest.icons.flatMap(
			(i: { purpose?: string }) => (i.purpose ?? 'any').split(/\s+/)
		);
		expect(purposes).toContain('maskable');
	});

	test('icon files are reachable', async ({ request }) => {
		for (const path of [
			'/img/icon-192.png',
			'/img/icon-512.png',
			'/img/icon-maskable-512.png'
		]) {
			const res = await request.get(path);
			expect(res.status(), `${path} should be served`).toBe(200);
		}
	});

	test('app.html includes manifest link and theme-color meta', async ({ page }) => {
		await page.goto('/');

		// `<link rel="manifest">` exists and resolves to manifest.json.
		const manifestHref = await page
			.locator('link[rel="manifest"]')
			.getAttribute('href');
		expect(manifestHref).toBeTruthy();
		expect(manifestHref).toMatch(/manifest\.json$/);

		// theme-color drives the address bar / system UI colour.
		const themeColor = await page
			.locator('meta[name="theme-color"]')
			.getAttribute('content');
		expect(themeColor).toBe('#0a0a0a');

		// apple-touch-icon hooks iOS home-screen install.
		const appleIcon = await page
			.locator('link[rel="apple-touch-icon"]')
			.getAttribute('href');
		expect(appleIcon).toMatch(/icon-192\.png$/);
	});
});

test.describe('Service worker', () => {
	test('registers and becomes active within 10s', async ({ page }) => {
		await page.goto('/');

		// `navigator.serviceWorker.ready` resolves once the SW reaches
		// the activated state. 10s is the SvelteKit boot + install
		// budget on cold cache; tests run against fresh contexts so
		// every run pays this cost.
		const active = await page.evaluate(
			() =>
				new Promise<boolean>((resolve) => {
					const timer = setTimeout(() => resolve(false), 10_000);
					navigator.serviceWorker.ready
						.then((reg) => {
							clearTimeout(timer);
							resolve(!!reg.active);
						})
						.catch(() => {
							clearTimeout(timer);
							resolve(false);
						});
				})
		);
		expect(active).toBe(true);
	});

	test('precaches the app shell (manifest, wasm, worklet, icon)', async ({ page }) => {
		await page.goto('/');
		await page.evaluate(() => navigator.serviceWorker.ready);

		const cachedUrls = await page.evaluate(async () => {
			const keys = await caches.keys();
			const nkidoCache = keys.find((k) => k.startsWith('nkido-cache-'));
			if (!nkidoCache) return null;
			const cache = await caches.open(nkidoCache);
			const requests = await cache.keys();
			return requests.map((r) => new URL(r.url).pathname);
		});

		expect(cachedUrls, 'nkido-cache-<version> must exist').not.toBeNull();
		const urls = cachedUrls!;
		// Spot-check critical app shell assets — full precache list comes
		// from $service-worker so we don't restate it; we just confirm the
		// load-bearing pieces are in there.
		expect(urls).toContain('/manifest.json');
		expect(urls).toContain('/wasm/nkido.wasm');
		expect(urls).toContain('/worklet/cedar-processor.js');
		expect(urls).toContain('/img/icon-512.png');
	});

	test('app shell loads while offline', async ({ page, context }) => {
		await page.goto('/');
		await page.evaluate(() => navigator.serviceWorker.ready);

		// Drop the network, then reload from cache only.
		await context.setOffline(true);
		await page.reload();

		// Title + Logo prove the SvelteKit shell rendered from cache.
		await expect(page).toHaveTitle('NKIDO');
		await expect(page.locator('svg[aria-label="NKIDO"]').first()).toBeVisible();
	});
});
