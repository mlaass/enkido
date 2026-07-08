import { defineConfig } from 'vitest/config';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { resolve } from 'path';

export default defineConfig({
	plugins: [svelte({ hot: false })],
	define: {
		// Mirror vite.config.ts — tests run as the site build.
		__IS_NATIVE__: 'false'
	},
	test: {
		include: ['tests/**/*.test.ts'],
		globals: true,
		testTimeout: 30000 // WASM loading can take a while
	},
	resolve: {
		alias: {
			$lib: resolve('./src/lib')
		}
	}
});
