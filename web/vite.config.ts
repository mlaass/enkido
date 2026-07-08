import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';

export default defineConfig({
	plugins: [sveltekit()],
	define: {
		// Native (studio/plugin WebView) bundle flag — prd-web-audio-backend
		// §5.5. Statically false on the site so native-only code tree-shakes.
		__IS_NATIVE__: JSON.stringify(process.env.NKIDO_NATIVE === '1')
	},
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
