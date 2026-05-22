/**
 * Sample Bank Registry
 *
 * Manages named collections of samples (banks) with lazy loading.
 * Compatible with Strudel's strudel.json manifest format.
 */

import { audioEngine } from '$lib/stores/audio.svelte';
import { loadFile } from '$lib/io/file-loader';
import { githubToHttps } from '$lib/io/handlers/github-handler';

/**
 * Strudel-compatible manifest format
 */
export interface StrudelManifest {
	_base?: string; // URL prefix for all sample paths
	_name?: string; // Display name for UI
	[sampleName: string]: string | string[] | undefined; // Sample paths or arrays of variant paths
}

/**
 * Parsed bank manifest
 */
export interface BankManifest {
	name: string;
	baseUrl: string;
	samples: Map<string, string[]>; // name → variant URLs (always arrays for consistency)
	loaded: Set<string>; // Which sample variants have been loaded (e.g., "bd:0", "bd:1")
}

/**
 * Sample reference with bank context
 */
export interface SampleReference {
	bank: string; // Bank name (empty = default)
	name: string; // Sample name
	variant: number; // Variant index
}

/**
 * Registry for managing sample banks
 */
class BankRegistryClass {
	private banks = new Map<string, BankManifest>();
	private loadingPromises = new Map<string, Promise<BankManifest>>(); // Prevent duplicate loads

	/**
	 * Check if a bank is registered
	 */
	hasBank(name: string): boolean {
		return this.banks.has(name);
	}

	/**
	 * Get a registered bank
	 */
	getBank(name: string): BankManifest | undefined {
		return this.banks.get(name);
	}

	/**
	 * Get all registered bank names
	 */
	getBankNames(): string[] {
		return Array.from(this.banks.keys());
	}

	/**
	 * Load a bank from a URL (strudel.json manifest) or File
	 */
	async loadBank(source: string | File, name?: string): Promise<BankManifest> {
		// Check if already loading
		const sourceKey = typeof source === 'string' ? source : source.name;
		const existingPromise = this.loadingPromises.get(sourceKey);
		if (existingPromise) {
			return existingPromise;
		}

		const loadPromise = this._loadBankInternal(source, name);
		this.loadingPromises.set(sourceKey, loadPromise);

		try {
			const manifest = await loadPromise;
			return manifest;
		} finally {
			this.loadingPromises.delete(sourceKey);
		}
	}

	private async _loadBankInternal(source: string | File, name?: string): Promise<BankManifest> {
		let manifestData: StrudelManifest;
		let baseUrl: string;

		if (typeof source === 'string') {
			// URI - fetch manifest via the resolver (with caching). github: URIs
			// are resolved to the corresponding raw.githubusercontent.com URL by
			// the github handler; we mirror that transform here so we can derive
			// the asset base URL for relative sample paths in the manifest.
			const httpUri = source.startsWith('github:') ? githubToHttps(source) : source;
			const result = await loadFile(source, { cache: true });
			const text = new TextDecoder().decode(result.data);
			manifestData = JSON.parse(text);

			// Determine base URL: use _base if provided, otherwise derive from manifest URL
			if (manifestData._base) {
				baseUrl = manifestData._base;
			} else if (/^[a-z][a-z0-9+.-]*:/i.test(httpUri)) {
				// Absolute URL (http(s)://, file://, etc.) — parse to extract directory.
				const url = new URL(httpUri);
				baseUrl = url.href.substring(0, url.href.lastIndexOf('/') + 1);
			} else {
				// Bare path — string-wise extract directory.
				const lastSlash = httpUri.lastIndexOf('/');
				baseUrl = lastSlash >= 0 ? httpUri.substring(0, lastSlash + 1) : '';
			}
		} else {
			// File - read as JSON
			const text = await source.text();
			manifestData = JSON.parse(text);
			baseUrl = manifestData._base || '';
		}

		// Determine bank name
		const bankName = name || manifestData._name || this.extractBankName(typeof source === 'string' ? source : source.name);

		// Check if bank already exists
		if (this.banks.has(bankName)) {
			return this.banks.get(bankName)!;
		}

		// Parse samples from manifest
		const samples = new Map<string, string[]>();
		for (const [key, value] of Object.entries(manifestData)) {
			// Skip metadata fields
			if (key.startsWith('_')) continue;

			if (typeof value === 'string') {
				// Single sample
				samples.set(key, [value]);
			} else if (Array.isArray(value)) {
				// Multiple variants
				samples.set(key, value as string[]);
			}
		}

		const manifest: BankManifest = {
			name: bankName,
			baseUrl,
			samples,
			loaded: new Set()
		};

		this.banks.set(bankName, manifest);
		console.log(`[BankRegistry] Loaded bank "${bankName}" with ${samples.size} samples`);

		return manifest;
	}

	/**
	 * Resolve a sample reference to a Cedar sample ID.
	 * Lazy-loads the sample if not yet loaded.
	 */
	async resolveSample(ref: SampleReference): Promise<number> {
		const { bank, name, variant } = ref;

		// Use default bank if not specified
		const bankName = bank || 'default';

		// Get or create the qualified name for Cedar
		const qualifiedName = this.getQualifiedName(bankName, name, variant);

		// Check if already loaded in Cedar
		// @ts-expect-error - cedar_get_sample_id is a WASM export
		const existingId = await audioEngine.getSampleId?.(qualifiedName);
		if (existingId && existingId > 0) {
			return existingId;
		}

		// Get bank manifest
		const manifest = this.banks.get(bankName);
		if (!manifest) {
			// Try loading from default if this is a default bank sample
			if (bankName === 'default') {
				// Default bank samples are handled by the audio engine
				return 0;
			}
			throw new Error(`Bank "${bankName}" not found`);
		}

		// Get sample variants
		const variants = manifest.samples.get(name);
		if (!variants || variants.length === 0) {
			throw new Error(`Sample "${name}" not found in bank "${bankName}"`);
		}

		// Wrap variant index if out of range (Strudel behavior)
		const actualVariant = variant % variants.length;
		const samplePath = variants[actualVariant];

		// Check if already loaded
		const loadKey = `${name}:${actualVariant}`;
		if (manifest.loaded.has(loadKey)) {
			// Should already be in Cedar
			return 0; // Caller will need to look up by qualified name
		}

		// Construct full URL
		const fullUrl = this.resolveUrl(manifest.baseUrl, samplePath);

		// Fetch and load the sample via audio engine (which uses FileLoader with caching)
		console.log(`[BankRegistry] Loading sample ${qualifiedName} from ${fullUrl}`);
		const success = await audioEngine.loadAsset(fullUrl, 'sample', qualifiedName);
		if (!success) {
			throw new Error(`Failed to load sample ${qualifiedName}`);
		}

		// Mark as loaded
		manifest.loaded.add(loadKey);

		// Get the assigned sample ID
		// @ts-expect-error - This method may be added
		return audioEngine.getSampleId?.(qualifiedName) || 0;
	}

	/**
	 * Get sample names available in a bank
	 */
	getSampleNames(bankName: string): string[] {
		const manifest = this.banks.get(bankName);
		if (!manifest) return [];
		return Array.from(manifest.samples.keys());
	}

	/**
	 * Get variant count for a sample in a bank
	 */
	getVariantCount(bankName: string, sampleName: string): number {
		const manifest = this.banks.get(bankName);
		if (!manifest) return 0;
		const variants = manifest.samples.get(sampleName);
		return variants?.length || 0;
	}

	/**
	 * Get the qualified name for a sample (bank_name_variant)
	 */
	getQualifiedName(bank: string, name: string, variant: number): string {
		if (bank === 'default' || !bank) {
			// Default bank uses simple names for backwards compatibility
			return variant > 0 ? `${name}:${variant}` : name;
		}
		return `${bank}_${name}_${variant}`;
	}

	/**
	 * Register a bank with pre-loaded samples (for default/built-in banks)
	 */
	registerBuiltinBank(name: string, samples: Map<string, string[]>): void {
		const manifest: BankManifest = {
			name,
			baseUrl: '',
			samples,
			loaded: new Set()
		};
		this.banks.set(name, manifest);
	}

	/**
	 * Register a known/lazy catalog bank (e.g. a Tidal Drum Machine). Only
	 * metadata is stored — `samples` holds variant *paths* relative to
	 * `baseUrl`, and individual samples are fetched on demand by
	 * `ensureBankSampleLoaded`. A bank already present is left untouched so
	 * a user-loaded bank of the same name keeps priority.
	 */
	registerCatalogBank(name: string, baseUrl: string, samples: Map<string, string[]>): void {
		if (this.banks.has(name)) return;
		this.banks.set(name, { name, baseUrl, samples, loaded: new Set() });
	}

	/**
	 * Clear a specific bank
	 */
	clearBank(name: string): void {
		this.banks.delete(name);
	}

	/**
	 * Clear all banks
	 */
	clearAll(): void {
		this.banks.clear();
	}

	// Helper: Extract bank name from URL or filename
	private extractBankName(source: string): string {
		// Try to extract from path
		const parts = source.split('/');
		const filename = parts[parts.length - 1];

		// Remove extension
		const nameWithoutExt = filename.replace(/\.(json|strudel\.json)$/i, '');

		// If it's just "strudel", use parent directory name
		if (nameWithoutExt.toLowerCase() === 'strudel' && parts.length > 1) {
			return parts[parts.length - 2];
		}

		return nameWithoutExt || 'unnamed';
	}

	// Helper: Resolve relative URL against base
	private resolveUrl(baseUrl: string, path: string): string {
		// If path is absolute, return as-is
		if (path.startsWith('http://') || path.startsWith('https://') || path.startsWith('/')) {
			return path;
		}

		// Ensure base URL ends with /
		const base = baseUrl.endsWith('/') ? baseUrl : baseUrl + '/';

		return base + path;
	}
}

// Export singleton instance
export const bankRegistry = new BankRegistryClass();

// Export types for convenience
export type { BankRegistryClass };
