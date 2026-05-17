import { defineConfig, devices } from '@playwright/test';

const PORT = 4173;
const BASE_URL = `http://localhost:${PORT}`;

export default defineConfig({
	testDir: './e2e',

	// Service-worker registration only happens against the built output,
	// so we always run against `vite preview` (never `vite dev`). The
	// build step is included so a stale `build/` doesn't silently mask
	// a regression.
	webServer: {
		command: `bun run build && bunx --bun vite preview --port ${PORT} --strictPort`,
		url: BASE_URL,
		reuseExistingServer: !process.env.CI,
		timeout: 180_000
	},

	use: {
		baseURL: BASE_URL,
		trace: 'on-first-retry',
		// Sensible desktop default — individual tests in responsive.spec.ts
		// override via `test.use({ viewport, hasTouch })`.
		viewport: { width: 1280, height: 800 }
	},

	// Chromium only — sufficient for the launch-readiness contract
	// (PWA install + service worker + responsive CSS). Cross-browser is
	// a v0.2 problem.
	projects: [
		{ name: 'chromium', use: { ...devices['Desktop Chrome'] } }
	],

	reporter: process.env.CI ? [['github'], ['list']] : 'list',
	forbidOnly: !!process.env.CI,
	retries: process.env.CI ? 1 : 0,
	workers: process.env.CI ? 1 : undefined
});
