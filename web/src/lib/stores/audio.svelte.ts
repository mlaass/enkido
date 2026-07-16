/**
 * Audio engine state store using Svelte 5 runes — the backend-agnostic
 * SHELL of prd-web-audio-backend.md.
 *
 * Holds the reactive `$state` the ~34 UI consumers read (transport flags,
 * params, viz decls, the Files-panel asset registries) and delegates all
 * transport / compile / asset / query work to the active `AudioBackend`
 * (`$lib/audio/backend-select`). The backend pushes engine truth back in
 * through the `AudioBackendHost` callbacks defined here — the shell never
 * touches an AudioContext, worklet, worker, or WASM directly.
 */

import { midiBank } from '$lib/audio/midi-bank';
import { inertMidiInputController } from '$lib/midi/midi-input.svelte';
import type { MidiMeta } from '$lib/audio/midi-meta';
import { settingsStore } from './settings.svelte';
import { bankRegistry } from '$lib/audio/bank-registry';
import { pathToFetchUri } from '$lib/io/path-to-uri';
import { forgetUpload } from '$lib/io/upload-manifest';
import {
	DEFAULT_INPUT_CONSTRAINTS,
	type InputConstraints,
	type InputSourceConfig,
	type InputSourceKind,
	type InputStatus
} from '$lib/audio/input-source';
import { selectAudioBackend } from '$lib/audio/backend-select';
import type {
	AudioBackendHost,
	CompileResult,
	DisassemblyInfo,
	ParamDecl,
	SoundFontInfo,
	VizDecl
} from '$lib/audio/audio-backend';

// Transport-facing types moved to the AudioBackend interface module
// (prd-web-audio-backend.md §4.1); re-exported here so existing consumers
// keep importing them from the store unchanged.
export { ParamType, VizType } from '$lib/audio/audio-backend';
export type {
	BuiltinInfo,
	BuiltinParam,
	BuiltinsData,
	DisassemblyInfo,
	DisassemblyInstruction,
	DisassemblySummary,
	FFTProbeData,
	MiniAstNode,
	OptionFieldSpec,
	ParamDecl,
	PatternDebugEvent,
	PatternDebugInfo,
	PatternDebugSequence,
	PatternEvent,
	PatternInfo,
	RequiredSampleExtended,
	SoundFontInfo,
	SoundFontPresetInfo,
	SourceLocation,
	StateInspection,
	UriKind,
	UriRequest,
	VizDecl
} from '$lib/audio/audio-backend';

/**
 * Reactive registry entry for a loaded sample. Mirrors the engine's
 * loaded-sample set so the Files panel can browse what's currently
 * registered. The companion `Set<string>` index inside the audio store
 * is kept in sync for O(1) `has()` lookups.
 */
export interface LoadedSample {
	name: string;
	origin: 'builtin' | 'user';
	// For built-ins this is the bundled URL the sample was fetched from;
	// for drag-drop uploads it's undefined (we only have the blob), for
	// URL-loader uploads it's the user-supplied URL.
	sourceUri?: string;
}

/**
 * Reactive registry entry for a loaded MIDI file. Mirrors `midiBank`
 * (which still owns the blob URLs) so the Files panel can list dropped
 * `.mid` files.
 */
export interface LoadedMidiFile {
	name: string;
	origin: 'user'; // no built-in MIDI today; field is here for symmetry.
	meta?: MidiMeta; // populated from midiBank when bytes are parseable
}

interface AudioState {
	isPlaying: boolean;
	bpm: number;
	volume: number;
	isInitialized: boolean;
	isLoading: boolean;
	visualizationsEnabled: boolean;
	currentBeat: number;
	currentBar: number;
	hasProgram: boolean;
	error: string | null;
	samplesLoaded: boolean;
	samplesLoading: boolean;
	isLoadingSamples: boolean;
	loadedSoundfonts: SoundFontInfo[];
	// PRD prd-unified-files-panel §4: reactive registries the Files panel
	// browses. `loadedSamples` is the canonical list; the parallel
	// `loadedSamplesIndex` Set is an O(1) lookup cache, not state.
	loadedSamples: LoadedSample[];
	loadedMidiFiles: LoadedMidiFile[];
	params: ParamDecl[];
	paramValues: Map<string, number>;
	vizDecls: VizDecl[];
	disassembly: DisassemblyInfo | null;
	activeSampleRate: number | null;
	// Audio input (audio-input PRD)
	inputKind: InputSourceKind;
	inputDeviceId: string | null;
	inputFileName: string | null;
	inputConstraints: InputConstraints;
	inputStatus: InputStatus;
	inputError: string | null;
}

function createAudioEngine() {
	let state = $state<AudioState>({
		isPlaying: false,
		bpm: 120,
		volume: 0.8,
		isInitialized: false,
		isLoading: false,
		visualizationsEnabled: true,
		currentBeat: 0,
		currentBar: 0,
		hasProgram: false,
		error: null,
		samplesLoaded: false,
		samplesLoading: false,
		isLoadingSamples: false,
		loadedSoundfonts: [],
		loadedSamples: [],
		loadedMidiFiles: [],
		params: [],
		paramValues: new Map(),
		vizDecls: [],
		disassembly: null,
		activeSampleRate: null,
		inputKind: 'none',
		inputDeviceId: null,
		inputFileName: null,
		inputConstraints: { ...DEFAULT_INPUT_CONSTRAINTS },
		inputStatus: 'idle',
		inputError: null
	});

	// O(1) lookup index mirroring `state.loadedSamples`. Kept in sync via
	// `addLoadedSample` / `removeLoadedSample` so existence checks don't
	// have to scan the reactive array.
	const loadedSamplesIndex = new Set<string>();
	// O(1) lookup index mirroring `state.loadedMidiFiles`.
	const loadedMidiFilesIndex = new Set<string>();

	// PRD prd-unified-files-panel §4: single helpers that keep the
	// reactive registry and the O(1) index in sync. Every load path
	// goes through these instead of touching state arrays directly.
	function addLoadedSample(
		name: string,
		origin: 'builtin' | 'user',
		sourceUri?: string
	): void {
		if (loadedSamplesIndex.has(name)) {
			// Already registered — promote `origin` from `builtin` to `user`
			// if the user manually drops a name that collided with a built-in
			// (last-writer-wins for the UI label).
			state.loadedSamples = state.loadedSamples.map((s) =>
				s.name === name ? { name, origin, sourceUri } : s
			);
			return;
		}
		loadedSamplesIndex.add(name);
		state.loadedSamples = [...state.loadedSamples, { name, origin, sourceUri }];
	}
	function removeLoadedSample(name: string): void {
		if (!loadedSamplesIndex.has(name)) return;
		loadedSamplesIndex.delete(name);
		state.loadedSamples = state.loadedSamples.filter((s) => s.name !== name);
	}
	function addLoadedMidiFile(name: string): void {
		if (loadedMidiFilesIndex.has(name)) return;
		loadedMidiFilesIndex.add(name);
		const meta = midiBank.getMeta(name);
		state.loadedMidiFiles = [...state.loadedMidiFiles, { name, origin: 'user', meta }];
	}
	function removeLoadedMidiFile(name: string): void {
		if (!loadedMidiFilesIndex.has(name)) return;
		loadedMidiFilesIndex.delete(name);
		state.loadedMidiFiles = state.loadedMidiFiles.filter((m) => m.name !== name);
	}

	/**
	 * The push side of "backend-pushed truth" (PRD §4): every callback
	 * lands in the reactive `$state` above. The backend is the only caller.
	 */
	const host: AudioBackendHost = {
		onTransport(t) {
			if (t.isPlaying !== undefined) state.isPlaying = t.isPlaying;
			if (t.bpm !== undefined) state.bpm = t.bpm;
			if (t.currentBeat !== undefined) state.currentBeat = t.currentBeat;
			if (t.currentBar !== undefined) state.currentBar = t.currentBar;
		},
		onParam(name, value) {
			// Host automation write-back (native backends). Last write wins;
			// the backend echoes host truth, not UI intent, so no feedback loop.
			state.paramValues.set(name, value);
		},
		onError(message) {
			state.error = message;
		},
		onStatus(s) {
			Object.assign(state, s);
		},
		onAssetLoaded(ev) {
			if (ev.kind === 'sample') {
				addLoadedSample(ev.name, ev.origin, ev.sourceUri);
			} else if (ev.kind === 'soundfont') {
				// Avoid duplicates by sfId
				if (!state.loadedSoundfonts.some((s) => s.sfId === ev.info.sfId)) {
					state.loadedSoundfonts = [...state.loadedSoundfonts, ev.info];
				}
			} else {
				addLoadedMidiFile(ev.name);
			}
		},
		onAssetsCleared(kind) {
			if (kind === 'sample') {
				loadedSamplesIndex.clear();
				state.loadedSamples = [];
			} else {
				loadedMidiFilesIndex.clear();
				state.loadedMidiFiles = [];
			}
		},
		onCompileResult(result: CompileResult) {
			if (!result.success) {
				state.disassembly = null;
				return;
			}
			if (result.paramDecls) updateParamDecls(result.paramDecls);
			state.vizDecls = result.vizDecls ?? [];
			state.disassembly = result.disassembly ?? null;
		},
		onEngineReset() {
			// Fired by backend.restart() between teardown and re-initialize:
			// reset per-engine reactive state so the fresh boot (and its
			// IndexedDB restore acks) repopulates it. Same field set the
			// pre-extraction restartAudio() reset.
			state.isInitialized = false;
			state.isLoading = false;
			state.hasProgram = false;
			state.samplesLoaded = false;
			state.samplesLoading = false;
			state.loadedSoundfonts = [];
			state.activeSampleRate = null;
			state.params = [];
			state.paramValues = new Map();
			state.disassembly = null;
			state.inputKind = 'none';
			state.inputDeviceId = null;
			state.inputFileName = null;
			state.inputStatus = 'idle';
			state.inputError = null;
			loadedSamplesIndex.clear();
			state.loadedSamples = [];
		}
	};

	const backend = selectAudioBackend(host);

	async function stop() {
		await backend.stop();
	}

	/**
	 * Restart the audio engine with updated settings (e.g., new sample rate)
	 */
	async function restartAudio() {
		// Stop playback first (the shell owns `isPlaying`).
		if (state.isPlaying) {
			await stop();
		}
		await backend.restart();
	}

	function setBpm(bpm: number) {
		state.bpm = Math.max(20, Math.min(999, bpm));
		backend.setBpm(state.bpm);
	}

	function setVolume(volume: number) {
		state.volume = Math.max(0, Math.min(1, volume));
		backend.setVolume(state.volume);
	}

	function toggleVisualizations() {
		state.visualizationsEnabled = !state.visualizationsEnabled;
	}

	/**
	 * Load a sample from a WAV file
	 * @param name Sample name (e.g., "kick", "snare")
	 * @param file File object or Blob containing WAV data
	 */
	async function loadSampleFromFile(
		name: string,
		file: File | Blob,
		origin: 'builtin' | 'user' = 'user'
	): Promise<boolean> {
		try {
			const arrayBuffer = await file.arrayBuffer();
			return await backend.loadSampleFromBytes(name, arrayBuffer, origin);
		} catch (err) {
			console.error('[AudioEngine] Failed to read sample file:', err);
			return false;
		}
	}

	/**
	 * Load multiple samples from URLs (e.g., a drum kit)
	 * @param samples Array of {name, url} objects
	 */
	async function loadSamplePack(samples: Array<{ name: string; url: string }>): Promise<number> {
		let loaded = 0;
		for (const sample of samples) {
			const success = await backend.loadAsset(pathToFetchUri(sample.url), 'sample', sample.name);
			if (success) loaded++;
		}
		console.log('[AudioEngine] Loaded', loaded, 'of', samples.length, 'samples');
		return loaded;
	}

	/**
	 * Drop a `.mid` registration from the main-thread bank (revokes the
	 * blob URL) and from the reactive Files-panel list. The engine's
	 * sequence stays attached for any currently-running program; the next
	 * compile that references this name will fail to resolve and surface a
	 * "missing asset" error, which is the same failure mode an unloaded
	 * file already produces.
	 */
	function unregisterMidiFile(name: string): void {
		midiBank.revoke(name);
		removeLoadedMidiFile(name);
		backend.forgetMidiFile(name);
		// Drop the persistence manifest entry so the row doesn't
		// reappear on the next reload (Phase B). Fire-and-forget —
		// the file-cache layer swallows errors silently.
		void forgetUpload('midi', name);
	}

	/**
	 * UI-only "forget" for a user-uploaded sample. Removes the row from
	 * the reactive registry and drops the persistence manifest entry.
	 * The engine keeps the sample loaded until the next reload —
	 * `cedar_remove_sample` doesn't exist yet (PRD Phase B notes a
	 * follow-up that would touch cedar C++).
	 */
	async function forgetSample(name: string): Promise<void> {
		removeLoadedSample(name);
		backend.forgetSample(name);
		await forgetUpload('sample', name);
	}

	/**
	 * UI-only "forget" for a user-uploaded SoundFont. Same semantics as
	 * `forgetSample`: the engine's `SoundFontRegistry` entry survives
	 * until reload, but the UI row and the manifest row are gone.
	 */
	async function forgetSoundFont(sfId: number): Promise<void> {
		const entry = state.loadedSoundfonts.find((s) => s.sfId === sfId);
		if (!entry) return;
		state.loadedSoundfonts = state.loadedSoundfonts.filter((s) => s.sfId !== sfId);
		backend.forgetSoundFont(entry.name);
		await forgetUpload('soundfont', entry.name);
	}

	// =========================================================================
	// Sample Bank API (main-thread registry — backend-agnostic bookkeeping)
	// =========================================================================

	/**
	 * Get all loaded bank names
	 */
	function getBankNames(): string[] {
		return bankRegistry.getBankNames();
	}

	/**
	 * Get sample names in a bank
	 */
	function getBankSampleNames(bankName: string): string[] {
		return bankRegistry.getSampleNames(bankName);
	}

	/**
	 * Get variant count for a sample in a bank
	 */
	function getBankSampleVariantCount(bankName: string, sampleName: string): number {
		return bankRegistry.getVariantCount(bankName, sampleName);
	}

	/**
	 * Check if a bank is loaded
	 */
	function hasBank(name: string): boolean {
		return bankRegistry.hasBank(name);
	}

	// =========================================================================
	// Parameter Exposure API
	// =========================================================================

	/**
	 * Update parameter declarations after successful compile.
	 * Preserves values for params that still exist.
	 */
	function updateParamDecls(newParams: ParamDecl[]) {
		const oldValues = new Map(state.paramValues);
		const newValues = new Map<string, number>();

		for (const param of newParams) {
			// Preserve existing value if param still exists, otherwise use default
			const existingValue = oldValues.get(param.name);
			if (existingValue !== undefined) {
				newValues.set(param.name, existingValue);
			} else {
				newValues.set(param.name, param.defaultValue);
				// Send initial value to the engine
				backend.setParam(param.name, param.defaultValue);
			}
		}

		state.params = newParams;
		state.paramValues = newValues;
	}

	function clearParams() {
		state.params = [];
		state.paramValues = new Map();
	}

	/**
	 * Set a parameter value (for sliders/continuous params)
	 */
	function setParamValue(name: string, value: number, slewMs?: number) {
		state.paramValues.set(name, value);
		backend.setParam(name, value, slewMs);
	}

	/**
	 * Get current value of a parameter
	 */
	function getParamValue(name: string): number {
		return state.paramValues.get(name) ?? 0;
	}

	/**
	 * Press a button (set to 1)
	 */
	function pressButton(name: string) {
		state.paramValues.set(name, 1);
		backend.setParam(name, 1, 0);
	}

	/**
	 * Release a button (set to 0)
	 */
	function releaseButton(name: string) {
		state.paramValues.set(name, 0);
		backend.setParam(name, 0, 0);
	}

	/**
	 * Toggle a boolean parameter
	 */
	function toggleParam(name: string) {
		const current = state.paramValues.get(name) ?? 0;
		const newValue = current > 0.5 ? 0 : 1;
		state.paramValues.set(name, newValue);
		backend.setParam(name, newValue);
	}

	/**
	 * Reset a parameter to its default value
	 */
	function resetParam(name: string) {
		const param = state.params.find((p) => p.name === name);
		if (param) {
			state.paramValues.set(name, param.defaultValue);
			backend.setParam(name, param.defaultValue);
		}
	}

	return {
		get isPlaying() { return state.isPlaying; },
		get bpm() { return state.bpm; },
		get volume() { return state.volume; },
		get isInitialized() { return state.isInitialized; },
		get isLoading() { return state.isLoading; },
		get visualizationsEnabled() { return state.visualizationsEnabled; },
		get currentBeat() { return state.currentBeat; },
		get currentBar() { return state.currentBar; },
		get hasProgram() { return state.hasProgram; },
		get error() { return state.error; },
		get samplesLoaded() { return state.samplesLoaded; },
		get samplesLoading() { return state.samplesLoading; },
		get isLoadingSamples() { return state.isLoadingSamples; },
		// Reactive registries the Files panel browses (PRD prd-unified-files-panel).
		// Origin-tagged so the panel can distinguish built-in defaults from
		// user drag-drop / URL uploads.
		get loadedSamples() { return state.loadedSamples; },
		get loadedSoundfonts() { return state.loadedSoundfonts; },
		get loadedMidiFiles() { return state.loadedMidiFiles; },
		// Parameter exposure
		get params() { return state.params; },
		get paramValues() { return state.paramValues; },
		// Visualization declarations
		get vizDecls() { return state.vizDecls; },
		// Debug info
		get disassembly() { return state.disassembly; },
		// Audio config
		get activeSampleRate() { return state.activeSampleRate; },

		// Audio input (audio-input PRD)
		get inputKind() { return state.inputKind; },
		get inputDeviceId() { return state.inputDeviceId; },
		get inputFileName() { return state.inputFileName; },
		get inputConstraints() { return state.inputConstraints; },
		get inputStatus() { return state.inputStatus; },
		get inputError() { return state.inputError; },
		setInputSource: (config: InputSourceConfig) => backend.setInputSource(config),
		setInputConstraints: (c: Partial<InputConstraints>) => backend.setInputConstraints(c),
		listInputDevices: () => backend.listInputDevices(),
		registerInputFile: (name: string, data: ArrayBuffer) =>
			backend.registerInputFile(name, data),
		unregisterInputFile: (name: string) => backend.unregisterInputFile(name),
		getInputFileNames: () => backend.getInputFileNames(),

		// MIDI input (prd-midi-input). The controller is web-only; the native
		// backend returns null (the host owns MIDI devices). The MIDI panel IS
		// still mounted there — its layout slot is persisted — so hand it an
		// inert 'unsupported' controller: dereferencing null aborted the whole
		// route mount (the native black-screen bug).
		get midi() { return backend.getMidiController() ?? inertMidiInputController; },
		setDefaultMidiDevice(name: string) {
			settingsStore.setDefaultMidiDevice(name);
			backend.setDefaultMidiDevice(name);
		},
		async ensureMidiAccess() {
			return backend.ensureMidiAccess();
		},

		initialize: () => backend.initialize(),
		play: () => backend.play(),
		pause: () => backend.pause(),
		stop,
		restartAudio,
		setBpm,
		setVolume,
		toggleVisualizations,
		compile: (source: string) => backend.compile(source),
		terminateCompileWorker: () => backend.terminateCompileWorker(),
		setParam: (name: string, value: number, slewMs?: number) =>
			backend.setParam(name, value, slewMs),
		getAnalyserNode: () => backend.getAnalyserNode(),
		getAudioContext: () => backend.getAudioContext(),
		getTimeDomainData: () => backend.getTimeDomainData(),
		getFrequencyData: () => backend.getFrequencyData(),
		loadSample: (name: string, audioData: Float32Array, channels: number, sampleRate: number) =>
			backend.loadSample(name, audioData, channels, sampleRate),
		loadSampleFromFile,
		loadSampleFromBytes: (name: string, data: ArrayBuffer, origin?: 'builtin' | 'user') =>
			backend.loadSampleFromBytes(name, data, origin),
		loadSamplePack,
		clearSamples: () => backend.clearSamples(),
		forgetSample,
		// Sample bank API
		loadBank: (url: string, name?: string) => backend.loadBank(url, name),
		getBankNames,
		// SoundFont API
		loadSoundFont: (name: string, data: ArrayBuffer, origin?: 'builtin' | 'user') =>
			backend.loadSoundFont(name, data, origin),
		forgetSoundFont,
		// MIDI file API (prd-midi-input Phase 5)
		loadMidiFile: (name: string, data: ArrayBuffer) => backend.loadMidiFile(name, data),
		unregisterMidiFile,
		midiBank,
		// Wavetable API (Smooch)
		loadWavetable: (name: string, data: ArrayBuffer) => backend.loadWavetable(name, data),
		clearWavetables: () => backend.clearWavetables(),
		// Unified URI-keyed asset loader
		loadAsset: backend.loadAsset,
		getBankSampleNames,
		getBankSampleVariantCount,
		hasBank,
		getBuiltins: () => backend.getBuiltins(),
		getShapeIndex: (source: string, cursorOffset: number) =>
			backend.getShapeIndex(source, cursorOffset),
		// Parameter exposure API
		setParamValue,
		getParamValue,
		pressButton,
		releaseButton,
		toggleParam,
		resetParam,
		clearParams,
		// Pattern highlighting API
		getPatternInfo: () => backend.getPatternInfo(),
		queryPatternPreview: (patternIndex: number, startBeat: number, endBeat: number) =>
			backend.queryPatternPreview(patternIndex, startBeat, endBeat),
		getCurrentBeatPosition: () => backend.getCurrentBeatPosition(),
		getActiveSteps: (stateIds: number[]) => backend.getActiveSteps(stateIds),
		// State inspection API
		inspectState: (stateId: number) => backend.inspectState(stateId),
		// Pattern debug API
		getPatternDebug: (patternIndex: number) => backend.getPatternDebug(patternIndex),
		// Visualization probe data
		getProbeData: (stateId: number) => backend.getProbeData(stateId),
		getFFTProbeData: (stateId: number) => backend.getFFTProbeData(stateId)
	};
}

export const audioEngine = createAudioEngine();
