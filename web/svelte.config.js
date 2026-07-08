import adapter from '@sveltejs/adapter-static';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

// Native (studio/plugin) bundle builds to its own directory so a site
// build never clobbers it and the artifact assertion can check both —
// prd-web-audio-backend §5.5. Selected via `bun run build:native`.
const outDir = process.env.NKIDO_NATIVE === '1' ? 'build-native' : 'build';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	preprocess: vitePreprocess(),
	kit: {
		adapter: adapter({
			pages: outDir,
			assets: outDir,
			fallback: 'index.html',
			precompress: false,
			strict: true
		}),
		alias: {
			$lib: './src/lib',
			$components: './src/lib/components',
			$audio: './src/lib/audio',
			$stores: './src/lib/stores',
			$compiler: './src/lib/compiler'
		}
	}
};

export default config;
