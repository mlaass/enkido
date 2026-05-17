import { devices, expect, test } from '@playwright/test';

// Verifies the responsive CSS shipped in commit 947d107:
// - ≤768px reflows the panel below the editor
// - ≤480px hides volume control and BPM label
// - (pointer: coarse) bumps play/stop hit targets
//
// These are purely structural assertions — no audio, no settings store
// mutation, no draft persistence.

const HEADER_HEIGHT_TOLERANCE = 12; // alignment slop for y comparisons

test.describe('Desktop viewport (1280×800)', () => {
	test.use({ viewport: { width: 1280, height: 800 } });

	test('panel sits to the side of the editor', async ({ page }) => {
		await page.goto('/');
		await page.locator('aside.panel').waitFor();
		await page.locator('.editor-container').waitFor();

		const panel = await page.locator('aside.panel').boundingBox();
		const editor = await page.locator('.editor-container').boundingBox();
		expect(panel).not.toBeNull();
		expect(editor).not.toBeNull();

		// Side-by-side: tops align (within tolerance) and x ranges don't
		// overlap (one is fully left of the other).
		expect(Math.abs(panel!.y - editor!.y)).toBeLessThan(HEADER_HEIGHT_TOLERANCE);
		const panelRight = panel!.x + panel!.width;
		const editorRight = editor!.x + editor!.width;
		const sideBySide =
			panelRight <= editor!.x + 1 || editorRight <= panel!.x + 1;
		expect(sideBySide).toBe(true);
	});
});

test.describe('Tablet viewport (768×1024)', () => {
	test.use({ viewport: { width: 768, height: 1024 } });

	test('panel reflows below the editor (panel.y > editor midpoint)', async ({
		page
	}) => {
		await page.goto('/');
		await page.locator('aside.panel').waitFor();
		await page.locator('.editor-container').waitFor();

		const panel = await page.locator('aside.panel').boundingBox();
		const editor = await page.locator('.editor-container').boundingBox();
		expect(panel).not.toBeNull();
		expect(editor).not.toBeNull();

		// Bottom layout: panel's top is below editor's vertical centre,
		// and both span ~full viewport width.
		expect(panel!.y).toBeGreaterThan(editor!.y + editor!.height / 2);
		expect(panel!.width).toBeGreaterThan(700);
		expect(editor!.width).toBeGreaterThan(700);
	});
});

test.describe('Phone viewport (375×667)', () => {
	test.use({ viewport: { width: 375, height: 667 } });

	test('volume control is hidden and BPM label is hidden', async ({ page }) => {
		await page.goto('/');
		await page.locator('.transport').waitFor();

		// `display: none` from the @media (max-width: 480px) rules.
		await expect(page.locator('.volume-control')).toBeHidden();
		await expect(page.locator('.bpm-control label')).toBeHidden();

		// Play button stays visible — primary tap target.
		await expect(page.locator('.play-button')).toBeVisible();
	});

	test('header tightens: Keep/Share labels collapse to icon-only', async ({
		page
	}) => {
		await page.goto('/');
		await page.locator('.header').waitFor();

		// The Share/Fork button stays present, but its label span hides.
		// (Inline phantoms get a Keep+Fork button cluster; loaded
		// patches get a Share button — both follow the same rule.)
		const labelSpans = page.locator(
			'.share-button span, .keep-button span'
		);
		const count = await labelSpans.count();
		for (let i = 0; i < count; i++) {
			await expect(labelSpans.nth(i)).toBeHidden();
		}
	});
});

test.describe('Touch device (Pixel 5 — coarse pointer)', () => {
	// Spread Pixel 5 but strip `defaultBrowserType` — Playwright forbids
	// switching browsers inside a describe block; we want the viewport +
	// hasTouch + deviceScaleFactor, not the browser swap.
	const { defaultBrowserType: _drop, ...pixel5 } = devices['Pixel 5'];
	test.use(pixel5);

	test('play/stop hit targets meet touch guideline (≥44 / ≥40 px)', async ({
		page
	}) => {
		await page.goto('/');
		await page.locator('.transport').waitFor();

		const play = await page.locator('.play-button').boundingBox();
		const stop = await page.locator('.stop-button').boundingBox();
		expect(play).not.toBeNull();
		expect(stop).not.toBeNull();

		// Apple HIG: ≥44pt for primary, ≥40pt acceptable for secondary.
		// The `@media (pointer: coarse)` block in Transport.svelte
		// promotes both above their desktop defaults of 36/32.
		expect(play!.height).toBeGreaterThanOrEqual(44);
		expect(play!.width).toBeGreaterThanOrEqual(44);
		expect(stop!.height).toBeGreaterThanOrEqual(40);
		expect(stop!.width).toBeGreaterThanOrEqual(40);
	});
});
