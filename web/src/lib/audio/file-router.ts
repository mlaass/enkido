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
				const ok = await audioEngine.loadSampleFromFile(name, file);
				return { name, kind, ok };
			}
			case 'soundfont': {
				const data = await file.arrayBuffer();
				const info = await audioEngine.loadSoundFont(name, data);
				return { name, kind, ok: info !== null };
			}
			case 'midi': {
				// Register with the in-memory midiBank so a later
				// `midi({file: "..."})` reference resolves through the blob
				// URL without an HTTP fetch; then parse into cedar so the
				// sequence is ready when the next compile runs.
				const data = await file.arrayBuffer();
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
