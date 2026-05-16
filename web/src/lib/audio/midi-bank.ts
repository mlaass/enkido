// Small in-memory registry for drag-dropped `.mid` files. Mirrors the
// shape of the soundfont/sample-bank registries but does not need a
// manifest fetch — drag-drop hands us bytes directly, and we hold a
// blob URL keyed by the filename the akkado source references.
//
// The audio store's compile pipeline (prd-midi-input Phase 5) consults
// this registry first when resolving a `midi({file: "name.mid"})` URL.
// Drops the registry on full-reset paths.
//
// Each entry also carries lightweight metadata (channel usage, note
// counts, program changes) so the Files panel can show what's inside a
// MIDI file without re-reading the bytes.

import { parseMidiMeta, type MidiMeta } from './midi-meta';

interface MidiEntry {
	url: string;
	meta: MidiMeta;
}

class MidiBank {
	private entries = new Map<string, MidiEntry>();

	/** Register raw bytes under `name`, returning the resulting blob URL. */
	register(name: string, bytes: ArrayBuffer): string {
		this.revoke(name);
		const meta = parseMidiMeta(bytes);
		const blob = new Blob([bytes], { type: 'audio/midi' });
		const url = URL.createObjectURL(blob);
		this.entries.set(name, { url, meta });
		return url;
	}

	/** Look up a blob URL by name. Returns undefined if not dropped yet. */
	lookup(name: string): string | undefined {
		return this.entries.get(name)?.url;
	}

	/** Look up parsed metadata for a registered file. */
	getMeta(name: string): MidiMeta | undefined {
		return this.entries.get(name)?.meta;
	}

	/** Names currently registered (used by the UI for the listing). */
	list(): string[] {
		return [...this.entries.keys()];
	}

	/** Drop a single registered file, revoking its blob URL. */
	revoke(name: string): void {
		const entry = this.entries.get(name);
		if (entry) {
			URL.revokeObjectURL(entry.url);
			this.entries.delete(name);
		}
	}

	/** Drop every registered file (called on engine reset). */
	clear(): void {
		for (const entry of this.entries.values()) URL.revokeObjectURL(entry.url);
		this.entries.clear();
	}
}

export const midiBank = new MidiBank();
