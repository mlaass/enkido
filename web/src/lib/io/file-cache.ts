/**
 * IndexedDB-based LRU file cache for audio samples
 */

const DB_NAME = 'nkido-file-cache';
const STORE_NAME = 'files';
const DB_VERSION = 2;
const MAX_CACHE_SIZE = 500 * 1024 * 1024; // 500MB

export interface CacheEntry {
	key: string;
	data: ArrayBuffer;
	size: number;
	lastAccess: number;
	// PRD prd-unified-files-panel Phase B: pinned rows are excluded from
	// LRU eviction so user-dropped uploads survive a heavy HTTP-cache
	// session. Defaults to false (HTTP/idb-handler writes stay evictable).
	pinned?: boolean;
}

function openDB(): Promise<IDBDatabase> {
	return new Promise((resolve, reject) => {
		const request = indexedDB.open(DB_NAME, DB_VERSION);

		request.onupgradeneeded = (event) => {
			const db = request.result;
			const tx = request.transaction;
			if (!db.objectStoreNames.contains(STORE_NAME)) {
				const store = db.createObjectStore(STORE_NAME, { keyPath: 'key' });
				store.createIndex('lastAccess', 'lastAccess');
				return;
			}
			// Upgrading an existing DB. v1 → v2 back-fills `pinned: false`
			// on every row so old HTTP-cached entries keep participating
			// in LRU eviction unchanged.
			const oldVersion = (event as IDBVersionChangeEvent).oldVersion;
			if (oldVersion < 2 && tx) {
				const store = tx.objectStore(STORE_NAME);
				const cursorReq = store.openCursor();
				cursorReq.onsuccess = () => {
					const cursor = cursorReq.result;
					if (!cursor) return;
					const entry = cursor.value as CacheEntry;
					if (entry.pinned === undefined) {
						entry.pinned = false;
						cursor.update(entry);
					}
					cursor.continue();
				};
			}
		};

		request.onsuccess = () => resolve(request.result);
		request.onerror = () => reject(request.error);
	});
}

export interface SetOptions {
	/** When true, the entry is exempt from LRU eviction. */
	pinned?: boolean;
}

export class FileCache {
	private dbPromise: Promise<IDBDatabase> | null = null;

	private getDB(): Promise<IDBDatabase> {
		if (!this.dbPromise) {
			this.dbPromise = openDB().catch((err) => {
				this.dbPromise = null;
				throw err;
			});
		}
		return this.dbPromise;
	}

	async get(key: string): Promise<ArrayBuffer | null> {
		try {
			const db = await this.getDB();
			return new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readwrite');
				const store = tx.objectStore(STORE_NAME);
				const request = store.get(key);

				request.onsuccess = () => {
					const entry = request.result as CacheEntry | undefined;
					if (!entry) {
						resolve(null);
						return;
					}

					// Update last access time
					entry.lastAccess = Date.now();
					store.put(entry);

					resolve(entry.data);
				};

				request.onerror = () => reject(request.error);
			});
		} catch {
			return null;
		}
	}

	async set(key: string, data: ArrayBuffer, options?: SetOptions): Promise<void> {
		try {
			const db = await this.getDB();
			const pinned = options?.pinned === true;

			// Evict if needed before inserting. Pinned writes still
			// trigger eviction of *unpinned* rows; pinned rows are never
			// evicted to make room (see evictIfNeeded).
			await this.evictIfNeeded(db, data.byteLength);

			return new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readwrite');
				const store = tx.objectStore(STORE_NAME);

				const entry: CacheEntry = {
					key,
					data,
					size: data.byteLength,
					lastAccess: Date.now(),
					pinned
				};

				const request = store.put(entry);
				request.onsuccess = () => resolve();
				request.onerror = () => reject(request.error);
			});
		} catch {
			// Silently fail - cache is optional
		}
	}

	async delete(key: string): Promise<void> {
		try {
			const db = await this.getDB();
			return new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readwrite');
				const store = tx.objectStore(STORE_NAME);
				const request = store.delete(key);

				request.onsuccess = () => resolve();
				request.onerror = () => reject(request.error);
			});
		} catch {
			// Silently fail
		}
	}

	async clear(): Promise<void> {
		try {
			const db = await this.getDB();
			return new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readwrite');
				const store = tx.objectStore(STORE_NAME);
				const request = store.clear();

				request.onsuccess = () => resolve();
				request.onerror = () => reject(request.error);
			});
		} catch {
			// Silently fail
		}
	}

	async getAllPinned(): Promise<CacheEntry[]> {
		try {
			const db = await this.getDB();
			return await new Promise((resolve, reject) => {
				const tx = db.transaction(STORE_NAME, 'readonly');
				const store = tx.objectStore(STORE_NAME);
				const req = store.getAll();
				req.onsuccess = () => {
					const all = (req.result as CacheEntry[]) ?? [];
					resolve(all.filter((e) => e.pinned === true));
				};
				req.onerror = () => reject(req.error);
			});
		} catch {
			return [];
		}
	}

	private async evictIfNeeded(db: IDBDatabase, incomingSize: number): Promise<void> {
		return new Promise((resolve, reject) => {
			const tx = db.transaction(STORE_NAME, 'readwrite');
			const store = tx.objectStore(STORE_NAME);

			// Calculate total size
			const allRequest = store.getAll();

			allRequest.onsuccess = () => {
				const entries = allRequest.result as CacheEntry[];
				let totalSize = entries.reduce((sum, e) => sum + e.size, 0);

				if (totalSize + incomingSize <= MAX_CACHE_SIZE) {
					resolve();
					return;
				}

				// Sort by lastAccess ascending (oldest first) for LRU
				// eviction, and exclude pinned rows entirely — pinned
				// uploads must survive a full cache fill.
				const candidates = entries
					.filter((e) => e.pinned !== true)
					.sort((a, b) => a.lastAccess - b.lastAccess);

				// Evict oldest unpinned entries until we have room. If
				// the unpinned set can't satisfy the request, the loop
				// exits and the new write proceeds anyway — pinned rows
				// dominate the cap, which is bounded by the user's own
				// drops and the browser's quota.
				const evictTx = db.transaction(STORE_NAME, 'readwrite');
				const evictStore = evictTx.objectStore(STORE_NAME);

				for (const entry of candidates) {
					if (totalSize + incomingSize <= MAX_CACHE_SIZE) break;
					evictStore.delete(entry.key);
					totalSize -= entry.size;
				}

				evictTx.oncomplete = () => resolve();
				evictTx.onerror = () => reject(evictTx.error);
			};

			allRequest.onerror = () => reject(allRequest.error);
		});
	}
}

/// Generate a cache key from a URL
export function cacheKeyFromUrl(url: string): string {
	return `url:${url}`;
}

/// Generate a cache key from a File
export function cacheKeyFromFile(file: File): string {
	return `file:${file.name}:${file.size}:${file.lastModified}`;
}

/// Singleton cache instance
export const fileCache = new FileCache();
