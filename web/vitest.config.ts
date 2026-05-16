import { defineConfig } from 'vitest/config';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { resolve } from 'path';

export default defineConfig({
	plugins: [svelte({ hot: false })],
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
