/**
 * Best-effort client-side dedup for the Report button.
 *
 * The Worker increments `report_count` on every POST /report regardless, so
 * this is a UX nicety — once a visitor has reported a slug from this device,
 * the UI shows "Already reported" instead of opening the dialog again.
 *
 * PRD: docs/prd-shareable-patches.md §10.8
 */

const KEY = 'nkido-reported-slugs';

function read(): Set<string> {
	if (typeof localStorage === 'undefined') return new Set();
	try {
		const raw = localStorage.getItem(KEY);
		if (!raw) return new Set();
		const arr = JSON.parse(raw);
		if (!Array.isArray(arr)) return new Set();
		return new Set(arr.filter((s): s is string => typeof s === 'string'));
	} catch {
		return new Set();
	}
}

function write(set: Set<string>): void {
	if (typeof localStorage === 'undefined') return;
	try {
		localStorage.setItem(KEY, JSON.stringify([...set]));
	} catch {
		// Quota exceeded or storage disabled — silent best-effort.
	}
}

export function hasReported(slug: string): boolean {
	return read().has(slug);
}

export function markReported(slug: string): void {
	const set = read();
	set.add(slug);
	write(set);
}

/** Test-only escape hatch — do NOT call from app code. */
export function _clearReported(): void {
	write(new Set());
}
