export type {
	DraftSummary,
	DraftFull,
	ShareInput,
	ShareSummary,
	ShareFull,
	RecentlyVisited,
	StorageProvider
} from './types';
export { LocalDraftProvider, DRAFTS_KEY, RECENTLY_VISITED_KEY, RECENTLY_VISITED_CAP } from './local-draft';
export {
	WorkerShareProvider,
	WorkerShareApiError,
	type WorkerShareProviderOptions
} from './worker-share';

import { LocalDraftProvider } from './local-draft';
import { WorkerShareProvider } from './worker-share';
import type { StorageProvider } from './types';

let cachedProvider: StorageProvider | null = null;
let cachedApiBase = '';

/**
 * Returns the active storage provider for this session. Reads
 * `PUBLIC_SHARE_API_BASE` from Vite env. Empty / unset → bare
 * LocalDraftProvider (drafts only; share methods undefined).
 * Set → WorkerShareProvider composed over a single shared local provider.
 *
 * Memoized so all callers share one instance per env-base value (re-reads if
 * the env changes between calls, which is only meaningful in tests).
 */
export function getProvider(): StorageProvider {
	const apiBase = readApiBase();
	if (cachedProvider && cachedApiBase === apiBase) return cachedProvider;
	cachedApiBase = apiBase;
	const local = new LocalDraftProvider();
	cachedProvider = apiBase
		? new WorkerShareProvider({ apiBase, local })
		: local;
	return cachedProvider;
}

export function resetProviderCacheForTests(): void {
	cachedProvider = null;
	cachedApiBase = '';
}

function readApiBase(): string {
	try {
		// Vite-injected at build time. Empty string when unset.
		const raw = (import.meta as { env?: Record<string, string | undefined> }).env
			?.PUBLIC_SHARE_API_BASE;
		return typeof raw === 'string' ? raw.trim() : '';
	} catch {
		return '';
	}
}
