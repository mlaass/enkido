/**
 * Unified file routing (PRD prd-midi-input §7.6 — FilesPanel consolidation).
 *
 * One drop zone → many asset types. `routeFile` inspects each File's
 * extension, chooses the right `audioEngine` API, and reports the result so
 * the panel can render a kind-aware status line. Keeps the per-type load
 * surface stable; the panel only knows about `routeFile`.
 *
 * Live audio (`.wav`, `.mp3`, `.ogg`, `.flac`, `.aiff`, `.aif`) goes
 * through `audioEngine.loadSampleFromFile`. SoundFonts (`.sf2`, `.sf3`)
 * go through `audioEngine.loadSoundFont`. MIDI files (`.mid`, `.midi`)
 * are registered with the midiBank (so `midi({file: ...})` lookups hit
 * the blob URL) and parsed into the cedar registry.
 */
import { audioEngine } from '$lib/stores/audio.svelte';
import { persistUpload } from '$lib/io/upload-manifest';

export type FileKind = 'sample' | 'soundfont' | 'midi' | 'unknown';

export interface RoutedFile {
	name: string;
	kind: FileKind;
	ok: boolean;
	error?: string;
}

const SAMPLE_EXTS = ['.wav', '.mp3', '.ogg', '.flac', '.aiff', '.aif'];
const SOUNDFONT_EXTS = ['.sf2', '.sf3'];
const MIDI_EXTS = ['.mid', '.midi'];

function basename(file: File): string {
	return file.name.replace(/\\/g, '/').split('/').pop() ?? file.name;
}

function endsWithAny(s: string, exts: string[]): boolean {
	const lower = s.toLowerCase();
	return exts.some((e) => lower.endsWith(e));
}

export function inferFromExtension(file: File): FileKind {
	const name = file.name;
	if (endsWithAny(name, SAMPLE_EXTS)) return 'sample';
	if (endsWithAny(name, SOUNDFONT_EXTS)) return 'soundfont';
	if (endsWithAny(name, MIDI_EXTS)) return 'midi';
	return 'unknown';
}

/**
 * URL-equivalent of `inferFromExtension`. Strips query/fragment and
 * the path before deciding. Used by the FilesPanel's generalized URL
 * loader (PRD prd-unified-files-panel §3) so one input can route a
 * `.sf2`, a `.wav`, or a `.mid` URL.
 */
export function inferFromUrl(url: string): FileKind {
	// Drop query string / fragment, then take the trailing path segment.
	const clean = url.split(/[?#]/)[0];
	const tail = clean.split('/').pop() ?? clean;
	if (endsWithAny(tail, SAMPLE_EXTS)) return 'sample';
	if (endsWithAny(tail, SOUNDFONT_EXTS)) return 'soundfont';
	if (endsWithAny(tail, MIDI_EXTS)) return 'midi';
	return 'unknown';
}

/**
 * Strip the path off a URL and return the basename — the akkado source
 * references the basename, not the full URL.
 */
export function basenameFromUrl(url: string): string {
	const clean = url.split(/[?#]/)[0];
	return clean.split('/').pop() ?? clean;
}

/**
 * Route one file to the correct audioEngine API. The returned promise
 * resolves once the worklet acknowledges the load (or rejects with `ok:
 * false` and a human-readable `error`). The basename — not the original
 * path — is what akkado source references, so we strip any directory
 * fragments the browser leaks through (some drag sources include them).
 */
export async function routeFile(file: File): Promise<RoutedFile> {
	const name = basename(file);
	const kind = inferFromExtension(file);

	try {
		switch (kind) {
			case 'sample': {
				// Persist the bytes (Phase B) so the upload survives a
				// reload. Read once and pass through `loadSampleFromBytes`
				// to avoid double-reading the File.
				const data = await file.arrayBuffer();
				const ok = await audioEngine.loadSampleFromBytes(name, data, 'user');
				if (ok) await persistUpload('sample', name, data);
				return { name, kind, ok };
			}
			case 'soundfont': {
				const data = await file.arrayBuffer();
				// loadSoundFont transfers `data` to the worklet (detaches
				// it). Clone first so the persist write has live bytes.
				const persistCopy = data.slice(0);
				const info = await audioEngine.loadSoundFont(name, data);
				const ok = info !== null;
				if (ok) await persistUpload('soundfont', name, persistCopy);
				return { name, kind, ok };
			}
			case 'midi': {
				// Register with the in-memory midiBank so a later
				// `midi({file: "..."})` reference resolves through the blob
				// URL without an HTTP fetch; then parse into cedar so the
				// sequence is ready when the next compile runs.
				const data = await file.arrayBuffer();
				// loadMidiFile transfers `data` to the worklet. Clone for
				// the persist write before the transfer detaches it.
				const persistCopy = data.slice(0);
				audioEngine.midiBank.register(name, data.slice(0));
				const ok = await audioEngine.loadMidiFile(name, data);
				if (ok) await persistUpload('midi', name, persistCopy);
				else audioEngine.midiBank.revoke(name);
				return { name, kind, ok };
			}
			default:
				return {
					name,
					kind: 'unknown',
					ok: false,
					error: 'Unrecognized extension'
				};
		}
	} catch (err) {
		const message = err instanceof Error ? err.message : String(err);
		return { name, kind, ok: false, error: message };
	}
}

/**
 * Route a whole FileList in parallel. Useful for the drag-drop handler.
 * Returns a per-file result array in input order.
 */
export async function routeFiles(files: FileList | File[]): Promise<RoutedFile[]> {
	const arr = Array.from(files);
	return Promise.all(arr.map(routeFile));
}

/**
 * Route a URL through the same per-kind audioEngine APIs the drag-drop
 * path uses. PRD prd-unified-files-panel §3 — the Files panel's URL
 * loader is just `inferFromUrl` + this dispatch. For `.mid` URLs we
 * fetch the bytes and register with `midiBank` so subsequent
 * `midi({file: name})` lookups resolve through the blob URL exactly
 * like a drag-drop. For `.wav` / `.sf2` we delegate to
 * `loadAsset(uri, kind, name)` which handles fetching + decoding.
 */
export async function routeUrl(url: string): Promise<RoutedFile> {
	const name = basenameFromUrl(url);
	const kind = inferFromUrl(url);
	try {
		switch (kind) {
			case 'sample': {
				const ok = await audioEngine.loadAsset(url, 'sample', name);
				return { name, kind, ok };
			}
			case 'soundfont': {
				const info = await audioEngine.loadAsset(url, 'soundfont', name);
				return { name, kind, ok: info !== null };
			}
			case 'midi': {
				// Fetch the bytes once so we can both populate the
				// midiBank (for `midi({file: name})` lookups) and hand
				// them to the worklet through the existing loader.
				const resp = await fetch(url);
				if (!resp.ok) {
					return { name, kind, ok: false, error: `HTTP ${resp.status}` };
				}
				const data = await resp.arrayBuffer();
				audioEngine.midiBank.register(name, data.slice(0));
				const ok = await audioEngine.loadMidiFile(name, data);
				if (!ok) audioEngine.midiBank.revoke(name);
				return { name, kind, ok };
			}
			default:
				return {
					name,
					kind: 'unknown',
					ok: false,
					error: 'Unrecognized URL extension'
				};
		}
	} catch (err) {
		const message = err instanceof Error ? err.message : String(err);
		return { name, kind, ok: false, error: message };
	}
}
