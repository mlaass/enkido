import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';

export default defineConfig({
	plugins: [sveltekit()],
	server: {
		headers: {
			// Required for SharedArrayBuffer (needed for AudioWorklet communication)
			'Cross-Origin-Opener-Policy': 'same-origin',
			'Cross-Origin-Embedder-Policy': 'require-corp'
		}
	},
	optimizeDeps: {
		exclude: ['codemirror', '@codemirror/state', '@codemirror/view']
	},
	ssr: {
		noExternal: ['lucide-svelte']
	}
});
