// Small in-memory registry for drag-dropped `.mid` files. Mirrors the
// shape of the soundfont/sample-bank registries but does not need a
// manifest fetch — drag-drop hands us bytes directly, and we hold a
// blob URL keyed by the filename the akkado source references.
//
// The audio store's compile pipeline (prd-midi-input Phase 5) consults
// this registry first when resolving a `midi({file: "name.mid"})` URL.
// Drops the registry on full-reset paths.

class MidiBank {
	private blobUrls = new Map<string, string>();

	/** Register raw bytes under `name`, returning the resulting blob URL. */
	register(name: string, bytes: ArrayBuffer): string {
		this.revoke(name);
		const blob = new Blob([bytes], { type: 'audio/midi' });
		const url = URL.createObjectURL(blob);
		this.blobUrls.set(name, url);
		return url;
	}

	/** Look up a blob URL by name. Returns undefined if not dropped yet. */
	lookup(name: string): string | undefined {
		return this.blobUrls.get(name);
	}

	/** Names currently registered (used by the UI for the listing). */
	list(): string[] {
		return [...this.blobUrls.keys()];
	}

	/** Drop a single registered file, revoking its blob URL. */
	revoke(name: string): void {
		const url = this.blobUrls.get(name);
		if (url) {
			URL.revokeObjectURL(url);
			this.blobUrls.delete(name);
		}
	}

	/** Drop every registered file (called on engine reset). */
	clear(): void {
		for (const url of this.blobUrls.values()) URL.revokeObjectURL(url);
		this.blobUrls.clear();
	}
}

export const midiBank = new MidiBank();
