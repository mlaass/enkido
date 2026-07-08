/**
 * Build-artifact assertion (prd-web-audio-backend §10): the site bundle
 * must contain no `$lib/native` code; the native bundle must contain it.
 *
 * Usage:
 *   bun scripts/check-native-bundle.ts site     # after `vite build` → build/
 *   bun scripts/check-native-bundle.ts native   # after build:native → build-native/
 *
 * Detection is by the grep-able sentinel exported from
 * `src/lib/native/index.ts` and kept alive by a runtime reference.
 */
import { readdirSync, readFileSync, statSync } from 'fs';
import { join } from 'path';

const MARKER = 'NKIDO_NATIVE_BUNDLE_MARKER';

const mode = process.argv[2];
if (mode !== 'site' && mode !== 'native') {
	console.error('usage: check-native-bundle.ts <site|native>');
	process.exit(2);
}
const dir = mode === 'site' ? 'build' : 'build-native';

function* walk(d: string): Generator<string> {
	for (const entry of readdirSync(d)) {
		const p = join(d, entry);
		if (statSync(p).isDirectory()) yield* walk(p);
		else yield p;
	}
}

const hits: string[] = [];
for (const file of walk(dir)) {
	if (!/\.(js|mjs|css|html|json)$/.test(file)) continue;
	if (readFileSync(file, 'utf8').includes(MARKER)) hits.push(file);
}

if (mode === 'site' && hits.length > 0) {
	console.error(`✗ site bundle contains native-only code (${MARKER}):\n  ` + hits.join('\n  '));
	process.exit(1);
}
if (mode === 'native' && hits.length === 0) {
	console.error(`✗ native bundle is missing ${MARKER} — IS_NATIVE gating broke`);
	process.exit(1);
}
console.log(
	mode === 'site'
		? `✓ site bundle clean — no native-only code in ${dir}/`
		: `✓ native bundle carries the native seam (${hits.length} chunk(s)) in ${dir}/`
);
