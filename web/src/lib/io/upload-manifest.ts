/**
 * Upload manifest — PRD prd-unified-files-panel Phase B.
 *
 * Thin wrapper over `fileCache` that hides the `upload:<kind>:<name>`
 * key format from callers. The "manifest" is virtual: rows in the
 * existing `files` object store with `pinned: true` and a key that
 * matches the upload prefix. This avoids cross-store transactions and
 * lets the existing LRU eviction logic stay untouched (pinned rows
 * are simply skipped).
 *
 * `routeFile` calls `persistUpload` after a successful drop. On the
 * next `audioEngine.initialize()`, the restore step walks
 * `listUploads()` serially and re-runs each kind's loader. UI "forget"
 * actions call `forgetUpload` to remove the manifest entry.
 */

import { fileCache } from './file-cache';

export type UploadKind = 'sample' | 'soundfont' | 'midi';

export interface UploadEntry {
	kind: UploadKind;
	name: string;
	data: ArrayBuffer;
}

function keyFor(kind: UploadKind, name: string): string {
	return `upload:${kind}:${name}`;
}

const KEY_PATTERN = /^upload:(sample|soundfont|midi):(.+)$/;

export async function persistUpload(
	kind: UploadKind,
	name: string,
	data: ArrayBuffer
): Promise<void> {
	await fileCache.set(keyFor(kind, name), data, { pinned: true });
}

export async function forgetUpload(kind: UploadKind, name: string): Promise<void> {
	await fileCache.delete(keyFor(kind, name));
}

export async function listUploads(): Promise<UploadEntry[]> {
	const pinned = await fileCache.getAllPinned();
	const out: UploadEntry[] = [];
	for (const entry of pinned) {
		const match = KEY_PATTERN.exec(entry.key);
		if (!match) continue;
		out.push({
			kind: match[1] as UploadKind,
			name: match[2],
			data: entry.data
		});
	}
	return out;
}
