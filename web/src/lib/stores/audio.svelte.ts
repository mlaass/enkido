/**
 * Audio engine state store using Svelte 5 runes
 *
 * Wraps the Cedar AudioWorklet-based engine with reactive state.
 */

import { DEFAULT_DRUM_KIT } from '$lib/audio/default-samples';
import { DEFAULT_SOUNDFONTS, resolveDefaultSoundFontUrls } from '$lib/audio/default-soundfonts';
import { midiBank } from '$lib/audio/midi-bank';
import type { MidiMeta } from '$lib/audio/midi-meta';
import { settingsStore } from './settings.svelte';
import { bankRegistry, type SampleReference } from '$lib/audio/bank-registry';
import { loadFile } from '$lib/io/file-loader';
import { pathToFetchUri } from '$lib/io/path-to-uri';
import { listUploads, forgetUpload } from '$lib/io/upload-manifest';
import {
	acquireMicSource,
	acquireTabSource,
	acquireFileSource,
	enumerateInputDevices,
	DEFAULT_INPUT_CONSTRAINTS,
	type ActiveInputSource,
	type InputConstraints,
	type InputSourceConfig,
	type InputSourceKind,
	type InputStatus
} from '$lib/audio/input-source';
import {
	createMidiInputController,
	type RequiredMidiSource,
	type RequiredMidiCcRoute
} from '$lib/midi/midi-input.svelte';
import type { ShapeIndex as ShapeIndexData } from '$lib/editor/akkado-shape-index';

interface Diagnostic {
	severity: number;
	message: string;
	line: number;
	column: number;
}

// Parameter declaration types
export enum ParamType {
	Continuous = 0,
	Button = 1,
	Toggle = 2,
	Select = 3
}

export interface ParamDecl {
	name: string;
	type: ParamType;
	defaultValue: number;
	min: number;
	max: number;
	options: string[];
	sourceOffset: number;
	sourceLength: number;
}

// Visualization declaration types
export enum VizType {
	PianoRoll = 0,
	Oscilloscope = 1,
	Waveform = 2,
	Spectrum = 3,
	Waterfall = 4
}

export interface FFTProbeData {
	magnitudes: Float32Array;
	binCount: number;
	frameCounter: number;
}

export interface VizDecl {
	name: string;
	type: VizType;
	stateId: number;
	sourceOffset: number;
	sourceLength: number;
	patternIndex: number; // Index into stateInits for piano roll, or -1
	options: Record<string, unknown> | null;
}

// Source location for bytecode-to-source mapping
export interface SourceLocation {
	line: number;
	column: number;
	offset: number;
	length: number;
}

// Disassembly info for debug panel
interface DisassemblyInstruction {
	index: number;
	opcode: string;
	opcodeNum: number;
	out: number;
	inputs: number[];
	stateId: number;
	rate: number;
	stateful: boolean;
	source?: SourceLocation;
}

interface DisassemblySummary {
	totalInstructions: number;
	statefulCount: number;
	uniqueStateIds: number;
	stateIds: number[];
}

interface DisassemblyInfo {
	instructions: DisassemblyInstruction[];
	summary: DisassemblySummary;
}

export type { DisassemblyInfo, DisassemblyInstruction, DisassemblySummary };

/**
 * Extended sample requirement with bank context
 */
export interface RequiredSampleExtended {
	bank: string | null; // Bank name (null = default)
	name: string; // Sample name
	variant: number; // Variant index
	qualifiedName: string; // Full name for Cedar lookup (e.g., "TR808_bd_0")
}

/**
 * SoundFont preset info returned from WASM
 */
export interface SoundFontPresetInfo {
	name: string;
	bank: number;
	program: number;
	zoneCount: number;
}

/**
 * Result of loading a SoundFont
 */
export interface SoundFontInfo {
	sfId: number;
	name: string;
	presetCount: number;
	presets: SoundFontPresetInfo[];
	// `builtin` = auto-loaded from DEFAULT_SOUNDFONTS; `user` = drag-drop
	// or URL loader. The Files panel uses this to gate the remove button
	// and to group built-in defaults into a separate sub-section.
	origin: 'builtin' | 'user';
}

/**
 * Reactive registry entry for a loaded sample. Mirrors the worklet's
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
	origin: 'user';  // no built-in MIDI today; field is here for symmetry.
	meta?: MidiMeta; // populated from midiBank when bytes are parseable
}

interface RequiredSoundFont {
	filename: string;
	preset: number;
}

interface RequiredWavetable {
	name: string;  // bank name (matches the smooch("...") string arg)
	path: string;  // path/URL to fetch the WAV from
	id: number;    // sequential slot ID assigned by the compiler
}

// URI kinds emitted by the akkado compiler. Mirrors `akkado::UriKind`.
// 0 = SampleBank manifest (strudel.json), 1 = SoundFont (.sf2),
// 2 = Wavetable (.wav), 3 = standalone sample.
export type UriKind = 0 | 1 | 2 | 3;

export interface UriRequest {
	uri: string;
	kind: UriKind;
}

interface CompileResult {
	success: boolean;
	bytecodeSize?: number;
	diagnostics?: Diagnostic[];
	requiredSamples?: string[]; // Legacy: simple sample names
	requiredSamplesExtended?: RequiredSampleExtended[]; // Extended: with bank/variant info
	requiredSoundfonts?: RequiredSoundFont[]; // SF2 files needed
	requiredWavetables?: RequiredWavetable[]; // Wavetable banks needed (Smooch)
	// URIs declared via top-level directives (samples("..."), etc.). Hosts iterate
	// these in source order, dispatch each by `kind` to the appropriate registry,
	// and block bytecode swap until every URI resolves.
	requiredUris?: UriRequest[];
	// Source strings collected from in() calls (one per call, "" = UI default).
	// Populated by audio-input PRD §4.4. Empty array when in() is not used.
	requiredInputSources?: string[];
	// midi() call sites from the compile result (prd-midi-input §4.7). The
	// host walks this list to build the live-device route table.
	requiredMidiSources?: RequiredMidiSource[];
	// midi_cc() routes from the compile result (prd-midi-input §4.8). The
	// controller turns these into a per-device CC route table.
	requiredMidiCcRoutes?: RequiredMidiCcRoute[];
	paramDecls?: ParamDecl[];
	vizDecls?: VizDecl[];
	disassembly?: DisassemblyInfo;
}

// Builtins metadata from the compiler
interface OptionFieldSpec {
	name: string;
	type: 'number' | 'string' | 'bool' | 'enum';
	default?: string;
	description?: string;
	values?: string;
}

interface BuiltinParam {
	name: string;
	required: boolean;
	default?: number;
	type?: string;
	optionFields?: OptionFieldSpec[];
	acceptsSpread?: boolean;
	extended?: boolean;
}

interface BuiltinInfo {
	params: BuiltinParam[];
	description: string;
}

interface BuiltinsData {
	functions: Record<string, BuiltinInfo>;
	aliases: Record<string, string>;
	keywords: string[];
}

export type { BuiltinsData, BuiltinInfo, BuiltinParam, OptionFieldSpec };

// Pattern highlighting types
interface PatternInfo {
	stateId: number;
	docOffset: number;
	docLength: number;
	cycleLength: number;
}

interface PatternEvent {
	time: number;
	duration: number;
	value: number;
	sourceOffset: number;
	sourceLength: number;
}

export type { PatternInfo, PatternEvent };

// Pattern debug types (detailed debugging info)
export interface PatternDebugEvent {
	type: 'DATA' | 'SUB_SEQ';
	time: number;
	duration: number;
	chance: number;
	values?: number[];
	numValues?: number;
	seqId?: number;
	sourceOffset: number;
	sourceLength: number;
}

export interface PatternDebugSequence {
	id: number;
	mode: 'NORMAL' | 'ALTERNATE' | 'RANDOM';
	duration: number;
	step?: number;
	events: PatternDebugEvent[];
}

export interface MiniAstNode {
	type: string; // MiniPattern, MiniGroup, MiniAtom, etc.
	location?: { offset: number; length: number };
	children?: MiniAstNode[];
	// MiniAtom-specific
	kind?: string; // Pitch, Sample, Rest
	midi?: number;
	sampleName?: string;
	variant?: number;
	// MiniModified-specific
	modifier?: string; // Speed, Slow, Duration, Weight, Repeat, Chance
	value?: number;
	// MiniEuclidean-specific
	hits?: number;
	steps?: number;
	rotation?: number;
	// MiniPolymeter-specific
	stepCount?: number;
}

export interface PatternDebugInfo {
	ast: MiniAstNode | null;
	sequences: PatternDebugSequence[];
	cycleLength: number;
	isSamplePattern: boolean;
}

// State inspection types
export interface StateInspection {
	type: string;
	[key: string]: unknown;
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

	let audioContext: AudioContext | null = null;
	let workletNode: AudioWorkletNode | null = null;
	let gainNode: GainNode | null = null;
	let analyserNode: AnalyserNode | null = null;
	let wasmJsCode: string | null = null;
	let wasmBinary: ArrayBuffer | null = null;
	// Memoizes the in-flight bootstrap so concurrent callers (eager
	// onMount init + a racing file drop) await the same promise instead
	// of seeing workletNode still null and bailing out.
	let initPromise: Promise<void> | null = null;

	// Currently connected live-input source (audio-input PRD §4.5).
	// Null = no input attached; in() then returns silence.
	let activeInput: ActiveInputSource | null = null;

	// Web MIDI input controller (prd-midi-input §4.10). Constructed eagerly
	// so the panel can render a "request permission" button; the actual
	// requestMIDIAccess() call is deferred until the user interacts.
	const midiInput = createMidiInputController();
	// Seed the persisted default device choice from settings.
	midiInput.setDefaultDeviceName(settingsStore.defaultMidiDevice);
	// Stash the most recent set of required midi() sources so the panel
	// (and re-acquired Web MIDI access) can rebuild routes idempotently.
	let lastRequiredMidiSources: RequiredMidiSource[] = [];
	let lastRequiredMidiCcRoutes: RequiredMidiCcRoute[] = [];

	// Uploaded input files keyed by display name. Map kept on the main thread
	// because file sources need raw ArrayBuffers for ctx.decodeAudioData().
	const inputFileBuffers = new Map<string, ArrayBuffer>();

	// Compile result callback (resolved when worklet responds)
	let compileResolve: ((result: CompileResult) => void) | null = null;

	// Monotonic ID for load requests; lets the worklet tag responses so
	// overlapping refreshes resolve their own promises.
	let loadRefreshCounter = 0;

	// Builtins metadata cache
	let builtinsCache: BuiltinsData | null = null;
	let builtinsResolve: ((data: BuiltinsData | null) => void) | null = null;

	// Phase 2 records-system-unification: shape-index resolver. Single
	// resolver pattern is fine — the editor coalesces calls by source hash
	// and only emits one outstanding request at a time.
	let shapeIndexResolve: ((data: ShapeIndexData | null) => void) | null = null;

	// Pattern highlighting resolve functions
	let patternInfoResolve: ((patterns: PatternInfo[]) => void) | null = null;
	let patternPreviewResolve: ((events: PatternEvent[]) => void) | null = null;
	// Array of pending beat position resolvers - all get resolved with same value
	let beatPositionResolvers: Array<(position: number) => void> = [];
	let activeStepsResolve: ((steps: Record<number, { offset: number; length: number }>) => void) | null = null;
	let stateInspectionResolve: ((data: StateInspection | null) => void) | null = null;
	// Map from stateId to resolve callback - supports multiple concurrent probe requests
	const probeDataResolvers = new Map<number, (samples: Float32Array | null) => void>();
	const fftProbeDataResolvers = new Map<number, (data: FFTProbeData | null) => void>();
	let patternDebugResolve: ((data: PatternDebugInfo | null) => void) | null = null;

	// Track sample loading state: 'pending' | 'loading' | 'loaded' | 'error'
	const sampleLoadState = new Map<string, 'pending' | 'loading' | 'loaded' | 'error'>();
	// O(1) lookup index mirroring `state.loadedSamples`. Kept in sync via
	// `addLoadedSample` / `removeLoadedSample` so existence checks don't
	// have to scan the reactive array.
	const loadedSamplesIndex = new Set<string>();
	// Pending sample load promises. Origin / sourceUri are stashed here so
	// the `sampleLoaded` worklet ack can attach them to the reactive entry
	// without the handler knowing which path called it.
	const pendingSampleLoads = new Map<string, {
		resolve: (success: boolean) => void;
		origin: 'builtin' | 'user';
		sourceUri?: string;
	}>();
	// Pending SoundFont load promises (origin threaded for the Files panel).
	const pendingSoundFontLoads = new Map<string, {
		resolve: (info: SoundFontInfo | null) => void;
		origin: 'builtin' | 'user';
	}>();
	// Pending wavetable bank load promises (resolves to assigned bank ID, -1 on failure)
	const pendingWavetableLoads = new Map<string, { resolve: (bankId: number) => void }>();
	// Track which wavetable bank IDs are currently registered in the worklet
	// (kept in sync via clearWavetables / loadWavetable). Map: name → bankId.
	const loadedWavetables = new Map<string, number>();
	// Pending .mid file load promises (prd-midi-input Phase 5).
	const pendingMidiFileLoads = new Map<string, { resolve: (success: boolean) => void }>();
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

	async function initialize(): Promise<void> {
		if (state.isInitialized) return;
		if (initPromise) return initPromise;

		state.isLoading = true;
		state.error = null;

		initPromise = (async () => {
			// Create AudioContext with sample rate from settings.
			// On eager onMount init this starts in 'suspended' state until
			// play() calls resume() — that is the expected browser pattern
			// and allows pre-play sample/SoundFont/MIDI loads to reach the
			// worklet without the autoplay policy blocking us.
			audioContext = new AudioContext({
				sampleRate: settingsStore.sampleRate,
				latencyHint: 'interactive'
			});
			state.activeSampleRate = audioContext.sampleRate;

			gainNode = audioContext.createGain();
			gainNode.gain.value = state.volume;

			analyserNode = audioContext.createAnalyser();
			analyserNode.fftSize = 2048;
			analyserNode.smoothingTimeConstant = 0.8;

			console.log('[AudioEngine] Fetching WASM module...');
			const [jsResponse, wasmResponse] = await Promise.all([
				fetch('/wasm/nkido.js'),
				fetch('/wasm/nkido.wasm')
			]);
			wasmJsCode = await jsResponse.text();
			wasmBinary = await wasmResponse.arrayBuffer();
			console.log('[AudioEngine] WASM fetched:', wasmJsCode.length, 'bytes JS,', wasmBinary.byteLength, 'bytes WASM');

			await audioContext.audioWorklet.addModule('/worklet/cedar-processor.js');

			// numberOfInputs=1 lets the audio-input PRD's in() opcode receive
			// live audio (mic / tab / uploaded file) — see setInputSource below.
			workletNode = new AudioWorkletNode(audioContext, 'cedar-processor', {
				numberOfInputs: 1,
				numberOfOutputs: 1,
				outputChannelCount: [2]
			});

			workletNode.port.onmessage = (event) => {
				handleWorkletMessage(event.data);
			};

			// Wire the MIDI controller's outbound port. Events the user
			// plays on a Web MIDI device will be posted here; the worklet's
			// 'midi' handler forwards them to cedar_push_midi_event.
			midiInput.setWorkletPort(workletNode.port);

			workletNode.connect(gainNode);
			gainNode.connect(analyserNode);
			analyserNode.connect(audioContext.destination);

			state.isInitialized = true;
			console.log('[AudioEngine] Initialized with AudioWorklet');

			// PRD prd-unified-files-panel Phase B: restore user uploads
			// from IndexedDB before initialize() resolves, so the first
			// compile (which awaits initialize()) sees every restored
			// asset and never surfaces a transient "missing asset" error.
			// Serial loop keeps ack ordering simple and avoids spamming
			// the worklet on cold start.
			try {
				const uploads = await listUploads();
				if (uploads.length > 0) {
					console.log('[AudioEngine] Restoring', uploads.length, 'user upload(s) from IndexedDB');
				}
				for (const entry of uploads) {
					try {
						if (entry.kind === 'sample') {
							await loadSampleFromBytes(entry.name, entry.data, 'user');
						} else if (entry.kind === 'soundfont') {
							await loadSoundFont(entry.name, entry.data, 'user');
						} else {
							// MIDI: re-register with midiBank so compile-time
							// lookups (and stop() re-upload) find a blob URL,
							// then push bytes to the worklet sequence registry.
							midiBank.register(entry.name, entry.data.slice(0));
							const ok = await loadMidiFile(entry.name, entry.data);
							if (!ok) midiBank.revoke(entry.name);
						}
					} catch (err) {
						// Best-effort: a broken row shouldn't bury init.
						console.warn(
							'[AudioEngine] Restore failed for',
							entry.kind,
							entry.name,
							err
						);
					}
				}
			} catch (err) {
				console.warn('[AudioEngine] Upload manifest read failed:', err);
			}
		})();

		try {
			await initPromise;
		} catch (err) {
			console.error('[AudioEngine] Failed to initialize:', err);
			state.error = err instanceof Error ? err.message : String(err);
			// Clear so a later caller (e.g. user clicking Play) can retry.
			initPromise = null;
		} finally {
			state.isLoading = false;
		}
	}

	function handleWorkletMessage(msg: { type: string; [key: string]: unknown }) {
		switch (msg.type) {
			case 'requestInit':
				// Worklet is requesting the WASM module
				sendWasmToWorklet();
				break;
			case 'initialized':
				console.log('[AudioEngine] Worklet WASM initialized');
				// Set initial BPM after worklet is ready
				workletNode?.port.postMessage({ type: 'setBpm', bpm: state.bpm });
				// Default samples load lazily when compile() needs them
				break;
			case 'compiled': {
				// Compilation result from worklet
				const result: CompileResult = {
					success: msg.success as boolean,
					bytecodeSize: msg.bytecodeSize as number | undefined,
					diagnostics: msg.diagnostics as Diagnostic[] | undefined,
					requiredSamples: msg.requiredSamples as string[] | undefined,
					requiredSamplesExtended: msg.requiredSamplesExtended as RequiredSampleExtended[] | undefined,
					requiredSoundfonts: msg.requiredSoundfonts as RequiredSoundFont[] | undefined,
					requiredWavetables: msg.requiredWavetables as RequiredWavetable[] | undefined,
					requiredUris: msg.requiredUris as UriRequest[] | undefined,
					requiredInputSources: msg.requiredInputSources as string[] | undefined,
					requiredMidiSources: msg.requiredMidiSources as RequiredMidiSource[] | undefined,
					requiredMidiCcRoutes: msg.requiredMidiCcRoutes as RequiredMidiCcRoute[] | undefined,
					paramDecls: msg.paramDecls as ParamDecl[] | undefined,
					vizDecls: msg.vizDecls as VizDecl[] | undefined,
					disassembly: msg.disassembly as DisassemblyInfo | undefined
				};
				if (result.success) {
					console.log(
						'[AudioEngine] Compiled successfully, bytecode size:',
						result.bytecodeSize,
						'required samples:',
						result.requiredSamples,
						'param decls:',
						result.paramDecls?.length ?? 0,
						'unique states:',
						result.disassembly?.summary?.uniqueStateIds ?? 'N/A'
					);
					// Update param declarations and preserve values for existing params
					if (result.paramDecls) {
						updateParamDecls(result.paramDecls);
					}
					// Update visualization declarations
					state.vizDecls = result.vizDecls ?? [];
					// Apply builtin variable overrides (e.g., bpm from code)
					const overrides = msg.builtinVarOverrides as
						Array<{ name: string; value: number }> | undefined;
					if (overrides) {
						for (const override of overrides) {
							if (override.name === 'bpm') {
								state.bpm = override.value;
							}
						}
					}
					// Store disassembly for debug panel
					state.disassembly = result.disassembly ?? null;
				} else {
					console.error('[AudioEngine] Compilation failed:', result.diagnostics);
					state.disassembly = null;
				}
				// Update MIDI input routes from the new compile's
				// midi() call sites. This is idempotent and safe even
				// when access has not yet been granted — the controller
				// late-resolves on permission grant / device plug-in.
				if (result.success) {
					lastRequiredMidiSources = result.requiredMidiSources ?? [];
					lastRequiredMidiCcRoutes = result.requiredMidiCcRoutes ?? [];
					midiInput.setRoutes(lastRequiredMidiSources, lastRequiredMidiCcRoutes);

					// PRD prd-midi-input §7.4: publish the file-CC dispatch
					// plan to the worklet. File-kind midi sources have no
					// physical device, so their CC events are drained
					// inside _cedar_process_block and routed through the
					// same CC table the live-device path uses.
					if (workletNode) {
						const fileMidiStateIds = lastRequiredMidiSources
							.filter((s) => s.kind === 2)
							.map((s) => s.stateId);
						workletNode.port.postMessage({
							type: 'setFileCcPlan',
							fileMidiStateIds,
							fileCcRoutes: lastRequiredMidiCcRoutes
						});
					}
				}
				// Resolve pending compile promise
				if (compileResolve) {
					compileResolve(result);
					compileResolve = null;
				}
				break;
			}
			case 'programLoaded':
				state.hasProgram = true;
				console.log('[AudioEngine] Program loaded');
				break;
			case 'sampleLoaded': {
				const name = msg.name as string;
				sampleLoadState.set(name, 'loaded');
				// Origin/uri come from the pending entry the caller created.
				// If there's no pending entry (legacy raw `loadSampleFromFile`
				// posts that don't wait for ack — currently none, but the
				// fallback keeps the registry consistent if one is added),
				// default to 'user' since that's the only path that wouldn't
				// have set up a pending entry.
				const pending = pendingSampleLoads.get(name);
				addLoadedSample(name, pending?.origin ?? 'user', pending?.sourceUri);
				console.log('[AudioEngine] Sample loaded:', name);
				if (pending) {
					pending.resolve(true);
					pendingSampleLoads.delete(name);
				}
				break;
			}
			case 'soundFontLoaded': {
				const sfName = msg.name as string;
				if (msg.success) {
					console.log('[AudioEngine] SoundFont loaded:', sfName, 'id:', msg.sfId, 'presets:', msg.presetCount);
					const pendingSf = pendingSoundFontLoads.get(sfName);
					const sfInfo: SoundFontInfo = {
						sfId: msg.sfId as number,
						name: sfName,
						presetCount: msg.presetCount as number,
						presets: (msg.presets as SoundFontPresetInfo[]) || [],
						origin: pendingSf?.origin ?? 'user'
					};
					// Add to reactive state (avoid duplicates by sfId)
					if (!state.loadedSoundfonts.some(s => s.sfId === sfInfo.sfId)) {
						state.loadedSoundfonts = [...state.loadedSoundfonts, sfInfo];
					}
					if (pendingSf) {
						pendingSf.resolve(sfInfo);
						pendingSoundFontLoads.delete(sfName);
					}
				} else {
					console.error('[AudioEngine] SoundFont load failed:', sfName, msg.error);
					const pendingSf = pendingSoundFontLoads.get(sfName);
					if (pendingSf) {
						pendingSf.resolve(null);
						pendingSoundFontLoads.delete(sfName);
					}
				}
				break;
			}
			case 'midiFileLoaded': {
				const midiName = msg.name as string;
				if (msg.success) {
					console.log('[AudioEngine] MIDI file loaded:', midiName);
					addLoadedMidiFile(midiName);
					const pendingMidi = pendingMidiFileLoads.get(midiName);
					if (pendingMidi) {
						pendingMidi.resolve(true);
						pendingMidiFileLoads.delete(midiName);
					}
				} else {
					console.error('[AudioEngine] MIDI file load failed:', midiName, msg.error);
					const pendingMidi = pendingMidiFileLoads.get(midiName);
					if (pendingMidi) {
						pendingMidi.resolve(false);
						pendingMidiFileLoads.delete(midiName);
					}
				}
				break;
			}
			case 'wavetableLoaded': {
				const wtName = msg.name as string;
				if (msg.success) {
					const bankId = msg.bankId as number;
					console.log('[AudioEngine] Wavetable loaded:', wtName, 'bankId:', bankId);
					loadedWavetables.set(wtName, bankId);
					const pendingWt = pendingWavetableLoads.get(wtName);
					if (pendingWt) {
						pendingWt.resolve(bankId);
						pendingWavetableLoads.delete(wtName);
					}
				} else {
					console.error('[AudioEngine] Wavetable load failed:', wtName, msg.error);
					const pendingWt = pendingWavetableLoads.get(wtName);
					if (pendingWt) {
						pendingWt.resolve(-1);
						pendingWavetableLoads.delete(wtName);
					}
				}
				break;
			}
			case 'error': {
				const errorMsg = String(msg.message);
				state.error = errorMsg;
				console.error('[AudioEngine] Worklet error:', errorMsg);
				// Check if this is a sample load error and resolve pending promise
				const sampleMatch = errorMsg.match(/Failed to load.*sample:\s*(\w+)/i);
				if (sampleMatch) {
					const sampleName = sampleMatch[1];
					const pending = pendingSampleLoads.get(sampleName);
					if (pending) {
						pending.resolve(false);
						pendingSampleLoads.delete(sampleName);
					}
					sampleLoadState.set(sampleName, 'error');
				}
				break;
			}
			case 'builtins': {
				if (msg.success && msg.data) {
					builtinsCache = msg.data as BuiltinsData;
					console.log('[AudioEngine] Received builtins metadata');
				}
				if (builtinsResolve) {
					builtinsResolve(builtinsCache);
					builtinsResolve = null;
				}
				break;
			}
			case 'shapeIndex': {
				if (shapeIndexResolve) {
					shapeIndexResolve(msg.success && msg.data ? (msg.data as ShapeIndexData) : null);
					shapeIndexResolve = null;
				}
				break;
			}
			case 'patternInfo': {
				if (patternInfoResolve) {
					patternInfoResolve(msg.success ? (msg.patterns as PatternInfo[]) : []);
					patternInfoResolve = null;
				}
				break;
			}
			case 'patternPreview': {
				if (patternPreviewResolve) {
					patternPreviewResolve(msg.success ? (msg.events as PatternEvent[]) : []);
					patternPreviewResolve = null;
				}
				break;
			}
			case 'beatPosition': {
				const position = msg.position as number;
				// Resolve ALL pending beat position requests with the same value
				for (const resolve of beatPositionResolvers) {
					resolve(position);
				}
				beatPositionResolvers = [];
				break;
			}
			case 'activeSteps': {
				if (activeStepsResolve) {
					activeStepsResolve(msg.steps as Record<number, { offset: number; length: number }>);
					activeStepsResolve = null;
				}
				break;
			}
			case 'stateInspection': {
				if (stateInspectionResolve) {
					stateInspectionResolve(msg.data as StateInspection | null);
					stateInspectionResolve = null;
				}
				break;
			}
			case 'patternDebug': {
				if (patternDebugResolve) {
					patternDebugResolve(msg.success ? (msg.data as PatternDebugInfo) : null);
					patternDebugResolve = null;
				}
				break;
			}
			case 'probeData': {
				const stateId = msg.stateId as number;
				const samples = msg.samples as number[] | null;
				const resolver = probeDataResolvers.get(stateId);
				if (resolver) {
					resolver(samples ? new Float32Array(samples) : null);
					probeDataResolvers.delete(stateId);
				}
				break;
			}
			case 'fftProbeData': {
				const stateId = msg.stateId as number;
				const magnitudes = msg.magnitudes as number[] | null;
				const binCount = msg.binCount as number;
				const frameCounter = msg.frameCounter as number;
				const resolver = fftProbeDataResolvers.get(stateId);
				if (resolver) {
					resolver(magnitudes ? {
						magnitudes: new Float32Array(magnitudes),
						binCount,
						frameCounter
					} : null);
					fftProbeDataResolvers.delete(stateId);
				}
				break;
			}
		}
	}

	function sendWasmToWorklet() {
		if (!workletNode || !wasmJsCode || !wasmBinary) {
			console.error('[AudioEngine] Cannot send WASM - not ready');
			return;
		}

		console.log('[AudioEngine] Sending WASM to worklet...');

		// Send the JS code and binary to the worklet
		// Clone the binary since we want to keep a copy
		workletNode.port.postMessage({
			type: 'init',
			jsCode: wasmJsCode,
			wasmBinary: wasmBinary.slice(0)
		});
	}

	async function play() {
		if (!state.isInitialized) {
			await initialize();
		}

		if (audioContext?.state === 'suspended') {
			await audioContext.resume();
		}

		state.isPlaying = true;
	}

	async function pause() {
		if (audioContext?.state === 'running') {
			await audioContext.suspend();
		}
		state.isPlaying = false;
	}

	async function stop() {
		workletNode?.port.postMessage({ type: 'reset' });
		await pause();
		state.currentBeat = 0;
		state.currentBar = 0;

		// _cedar_reset() resets audio_arena_, which owns every parsed
		// MidiSequence (prd-midi-input Phase 5). SampleBank /
		// SoundFontRegistry / WavetableRegistry use heap storage and
		// survive reset, so only MIDI needs to be re-uploaded. The bytes
		// still live behind midiBank's native blob URLs from the original
		// drop/fetch — re-fetch and re-push to the worklet so the next
		// compile can resolve the file from the worklet's registry. (The
		// compile-time loadAsset path can't reuse these URLs because the
		// uri-resolver blob handler only recognises blob:nkido: URIs.)
		const names = state.loadedMidiFiles.map((m) => m.name);
		loadedMidiFilesIndex.clear();
		state.loadedMidiFiles = [];
		for (const name of names) {
			const blobUrl = midiBank.lookup(name);
			if (!blobUrl) continue;
			try {
				const resp = await fetch(blobUrl);
				if (!resp.ok) continue;
				const data = await resp.arrayBuffer();
				await loadMidiFile(name, data);
			} catch (err) {
				console.warn('[AudioEngine] Failed to re-upload MIDI after stop:', name, err);
			}
		}
	}

	/**
	 * Restart the audio engine with updated settings (e.g., new sample rate)
	 */
	async function restartAudio() {
		console.log('[AudioEngine] Restarting audio with new settings...');

		// Stop playback
		if (state.isPlaying) {
			await stop();
		}

		// Tear down any active live-input source — the audio context is about
		// to close, so its source nodes will be invalid anyway.
		disconnectActiveInput();

		// Close existing audio context
		if (audioContext) {
			await audioContext.close();
			audioContext = null;
		}

		// Clear references
		workletNode = null;
		gainNode = null;
		analyserNode = null;
		// Drop the memoized bootstrap so initialize() actually re-runs
		// instead of resolving instantly off the prior (now-stale) promise.
		initPromise = null;

		// Reset state
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

		// Clear sample tracking
		sampleLoadState.clear();
		loadedSamplesIndex.clear();
		state.loadedSamples = [];
		pendingSampleLoads.clear();
		pendingSoundFontLoads.clear();

		// Reinitialize
		await initialize();
	}

	function setBpm(bpm: number) {
		state.bpm = Math.max(20, Math.min(999, bpm));
		workletNode?.port.postMessage({ type: 'setBpm', bpm: state.bpm });
	}

	function setVolume(volume: number) {
		state.volume = Math.max(0, Math.min(1, volume));
		if (gainNode && audioContext) {
			gainNode.gain.setTargetAtTime(state.volume, audioContext.currentTime, 0.01);
		}
	}

	function toggleVisualizations() {
		state.visualizationsEnabled = !state.visualizationsEnabled;
	}

	/**
	 * Ensure a sample is loaded (waits if loading, loads if pending/unknown)
	 * @returns true if sample is loaded, false if loading failed or unknown
	 */
	async function ensureSampleLoaded(name: string): Promise<boolean> {
		// Already loaded?
		if (loadedSamplesIndex.has(name)) {
			return true;
		}

		const currentState = sampleLoadState.get(name);

		// Already loaded (check state too)
		if (currentState === 'loaded') {
			return true;
		}

		// Failed previously
		if (currentState === 'error') {
			return false;
		}

		// Currently loading - wait for it
		if (currentState === 'loading') {
			return new Promise((resolve) => {
				const check = setInterval(() => {
					const s = sampleLoadState.get(name);
					if (s === 'loaded') {
						clearInterval(check);
						resolve(true);
					}
					if (s === 'error') {
						clearInterval(check);
						resolve(false);
					}
				}, 50);
				// Timeout after 30 seconds
				setTimeout(() => {
					clearInterval(check);
					resolve(false);
				}, 30000);
			});
		}

		// Try to find in default kit
		const defaultSample = DEFAULT_DRUM_KIT.find((s) => s.name === name);
		if (defaultSample) {
			sampleLoadState.set(name, 'loading');
			try {
				const success = await loadAsset(pathToFetchUri(defaultSample.url), 'sample', name, 'builtin');
				// Note: loadAsset waits for worklet confirmation
				// The 'sampleLoaded' handler will set state to 'loaded'
				if (!success) {
					sampleLoadState.set(name, 'error');
				}
				return success;
			} catch {
				sampleLoadState.set(name, 'error');
				return false;
			}
		}

		// Unknown sample - not in default kit
		return false;
	}

	/**
	 * Ensure an extended sample (with bank context) is loaded
	 * @returns true if sample is loaded, false if loading failed
	 */
	async function ensureBankSampleLoaded(sample: RequiredSampleExtended): Promise<boolean> {
		const { bank, name, variant, qualifiedName } = sample;

		// Already loaded?
		if (loadedSamplesIndex.has(qualifiedName)) {
			return true;
		}

		const currentState = sampleLoadState.get(qualifiedName);

		// Already loaded (check state too)
		if (currentState === 'loaded') {
			return true;
		}

		// Failed previously
		if (currentState === 'error') {
			return false;
		}

		// Currently loading - wait for it
		if (currentState === 'loading') {
			return new Promise((resolve) => {
				const check = setInterval(() => {
					const s = sampleLoadState.get(qualifiedName);
					if (s === 'loaded') {
						clearInterval(check);
						resolve(true);
					}
					if (s === 'error') {
						clearInterval(check);
						resolve(false);
					}
				}, 50);
				// Timeout after 30 seconds
				setTimeout(() => {
					clearInterval(check);
					resolve(false);
				}, 30000);
			});
		}

		// Default bank - try to load from default kit
		if (!bank || bank === 'default') {
			// For default bank, name with variant suffix (e.g., "bd:1") or simple name
			const simpleName = variant > 0 ? `${name}:${variant}` : name;
			const baseName = name; // Try without variant first

			// Try variant-specific name first
			const variantSample = DEFAULT_DRUM_KIT.find((s) => s.name === simpleName);
			if (variantSample) {
				sampleLoadState.set(qualifiedName, 'loading');
				try {
					const success = await loadAsset(pathToFetchUri(variantSample.url), 'sample', qualifiedName, 'builtin');
					if (!success) {
						sampleLoadState.set(qualifiedName, 'error');
					}
					return success;
				} catch {
					sampleLoadState.set(qualifiedName, 'error');
					return false;
				}
			}

			// Try base name (variant 0)
			const baseSample = DEFAULT_DRUM_KIT.find((s) => s.name === baseName);
			if (baseSample) {
				sampleLoadState.set(qualifiedName, 'loading');
				try {
					const success = await loadAsset(pathToFetchUri(baseSample.url), 'sample', qualifiedName, 'builtin');
					if (!success) {
						sampleLoadState.set(qualifiedName, 'error');
					}
					return success;
				} catch {
					sampleLoadState.set(qualifiedName, 'error');
					return false;
				}
			}

			return false;
		}

		// Custom bank - try to load via BankRegistry
		if (!bankRegistry.hasBank(bank)) {
			console.warn(`[AudioEngine] Bank "${bank}" not loaded`);
			return false;
		}

		const manifest = bankRegistry.getBank(bank);
		if (!manifest) {
			return false;
		}

		const variants = manifest.samples.get(name);
		if (!variants || variants.length === 0) {
			console.warn(`[AudioEngine] Sample "${name}" not found in bank "${bank}"`);
			return false;
		}

		// Wrap variant index if out of range (Strudel behavior)
		const actualVariant = variant % variants.length;
		const samplePath = variants[actualVariant];

		// Construct full URL
		const baseUrl = manifest.baseUrl.endsWith('/') ? manifest.baseUrl : manifest.baseUrl + '/';
		const rawUrl = samplePath.startsWith('http') || samplePath.startsWith('/') ? samplePath : baseUrl + samplePath;
		const fullUrl = pathToFetchUri(rawUrl);

		// Load the sample with qualified name
		sampleLoadState.set(qualifiedName, 'loading');
		try {
			console.log(`[AudioEngine] Loading bank sample ${qualifiedName} from ${fullUrl}`);
			const success = await loadAsset(fullUrl, 'sample', qualifiedName);
			if (!success) {
				sampleLoadState.set(qualifiedName, 'error');
			} else {
				// Mark as loaded in bank manifest
				manifest.loaded.add(`${name}:${actualVariant}`);
			}
			return success;
		} catch (err) {
			console.error(`[AudioEngine] Failed to load bank sample ${qualifiedName}:`, err);
			sampleLoadState.set(qualifiedName, 'error');
			return false;
		}
	}

	/**
	 * Compile source code in the worklet and load into Cedar VM
	 * This handles the full compile -> load samples -> load program flow
	 */
	async function compile(source: string): Promise<CompileResult> {
		if (!workletNode) {
			return {
				success: false,
				diagnostics: [{ severity: 2, message: 'Worklet not initialized', line: 1, column: 1 }]
			};
		}

		console.log('[AudioEngine] Sending source for compilation, length:', source.length);

		// Step 1: Compile (fast, no sample loading)
		const compilePromise = new Promise<CompileResult>((resolve) => {
			compileResolve = resolve;
			// Timeout after 5 seconds to prevent main thread hang if worklet crashes
			setTimeout(() => {
				if (compileResolve === resolve) {
					compileResolve = null;
					resolve({
						success: false,
						diagnostics: [{ severity: 2, message: 'Compilation timeout - worklet may have crashed', line: 1, column: 1 }]
					});
				}
			}, 5000);
		});

		workletNode.port.postMessage({ type: 'compile', source });
		const compileResult = await compilePromise;

		if (!compileResult.success) {
			return compileResult;
		}

		// Step 2: Load any required samples and soundfonts (await ALL before proceeding)
		state.isLoadingSamples = true;
		try {
			// Drain required URIs first: samples() and friends register bank
			// manifests in their respective registries, which the per-sample
			// loader below relies on to resolve `.bank("Name")` references.
			const requiredUris = compileResult.requiredUris || [];
			for (const req of requiredUris) {
				try {
					if (req.kind === 0) {
						// SampleBank manifest (e.g. github:user/repo)
						const ok = await loadAsset(req.uri, 'sample_bank');
						if (!ok) {
							return {
								success: false,
								diagnostics: [
									{
										severity: 2,
										message: `Sample bank '${req.uri}' failed to load`,
										line: 1,
										column: 1
									}
								]
							};
						}
					} else {
						// Other kinds are reserved for future use; warn and skip
						// so a stray declaration doesn't silently sink playback.
						console.warn(
							`[AudioEngine] Unsupported URI kind ${req.kind} for '${req.uri}', skipping`
						);
					}
				} catch (err) {
					console.error(`[AudioEngine] URI '${req.uri}' failed to load:`, err);
					return {
						success: false,
						diagnostics: [
							{
								severity: 2,
								message: `URI '${req.uri}' failed to load: ${(err as Error).message}`,
								line: 1,
								column: 1
							}
						]
					};
				}
			}

			// requiredSamplesExtended is the canonical sample list — every
			// Pattern producer in the compiler publishes its sample_refs into
			// it (see CodeGenerator::publish_sample_refs). Bank-less samples
			// arrive with empty `bank` and round-trip through the bank-aware
			// loader unchanged. The legacy `requiredSamples` array stays
			// exposed for non-web consumers but the web loader does not
			// branch on it anymore — the previous extended/legacy fork
			// caused cross-chain coupling where one chain's missing
			// extended entry redirected another chain's loader path.
			const extendedSamples = compileResult.requiredSamplesExtended || [];
			const missingSamples: string[] = [];
			for (const sample of extendedSamples) {
				const loaded = await ensureBankSampleLoaded(sample);
				if (!loaded) {
					const displayName = sample.bank
						? `${sample.bank}/${sample.name}:${sample.variant}`
						: sample.name;
					missingSamples.push(displayName);
				}
			}

			// If any samples couldn't be loaded, report as error
			if (missingSamples.length > 0) {
				return {
					success: false,
					diagnostics: missingSamples.map((name) => ({
						severity: 2,
						message: `Sample '${name}' not found or failed to load`,
						line: 1,
						column: 1
					}))
				};
			}

			// Load any required wavetable banks (Smooch).
			// Order matters: the compiler assigns sequential slot IDs in
			// source order, so we clear the runtime registry first and
			// then load each bank in compile order — the runtime IDs match
			// inst.rate values embedded in the bytecode.
			const requiredWavetables = compileResult.requiredWavetables || [];
			if (requiredWavetables.length > 0) {
				clearWavetables();
				for (const wt of requiredWavetables) {
					// The URI resolver requires an explicit scheme; bare
					// paths from `wt_load("name", "wavetables/x.wav")` get
					// resolved against the document origin so the http
					// handler can fetch them.
					const url = pathToFetchUri(wt.path);
					const bankId = await loadAsset(url, 'wavetable', wt.name);
					if (bankId < 0) {
						return {
							success: false,
							diagnostics: [
								{
									severity: 2,
									message: `Wavetable bank '${wt.name}' (${wt.path}) failed to load`,
									line: 1,
									column: 1
								}
							]
						};
					}
					if (bankId !== wt.id) {
						console.warn(
							`[AudioEngine] Wavetable '${wt.name}' assigned bank ID ${bankId}` +
							` but compiler expected ${wt.id} — bytecode may reference the wrong bank`
						);
					}
				}
			}

			// Load any required SoundFonts
			const requiredSoundfonts = compileResult.requiredSoundfonts || [];
			for (const sf of requiredSoundfonts) {
				// Skip if already loaded (by name)
				if (state.loadedSoundfonts.some((s) => s.name === sf.filename)) continue;

				// Resolve short names (e.g., "gm") to default soundfont URLs
				const defaultUrls = resolveDefaultSoundFontUrls(sf.filename);
				const urls = defaultUrls.length > 0 ? defaultUrls : [sf.filename];

				let loaded = false;
				for (const rawUrl of urls) {
					const url = pathToFetchUri(rawUrl);
					const info = await loadAsset(url, 'soundfont', sf.filename);
					if (info) {
						loaded = true;
						break;
					}
				}
				if (!loaded) {
					const e = new Error(`All URLs failed for '${sf.filename}'`);
					console.warn(`[AudioEngine] Failed to load SoundFont '${sf.filename}':`, e);
					return {
						success: false,
						diagnostics: [
							{
								severity: 2,
								message: `SoundFont '${sf.filename}' failed to load`,
								line: 1,
								column: 1
							}
						]
					};
				}
			}

			// Load any required .mid files (prd-midi-input Phase 5). For
			// each File-kind RequiredMidiSource, resolve the name to a URL
			// (drag-drop blob via midi-bank registry → fallback to treating
			// the name as a fetchable URL) and push bytes into the worklet.
			// Mirrors the soundfont gating: bail out with a compile-style
			// diagnostic if any required file fails to load, so the user
			// gets a clear error rather than silent playback.
			const requiredMidi = compileResult.requiredMidiSources ?? [];
			for (const src of requiredMidi) {
				// MidiSourceKind.File = 2 (see akkado/include/akkado/codegen.hpp)
				if (src.kind !== 2 || !src.name) continue;
				if (loadedMidiFilesIndex.has(src.name)) continue;
				const blobUrl = midiBank.lookup(src.name);
				const url = blobUrl ?? pathToFetchUri(src.name);
				const ok = await loadAsset(url, 'midi', src.name);
				if (!ok) {
					return {
						success: false,
						diagnostics: [
							{
								severity: 2,
								message: `MIDI file '${src.name}' failed to load — drop the file into the MIDI panel or check the URL`,
								line: 1,
								column: 1
							}
						]
					};
				}
			}
		} finally {
			state.isLoadingSamples = false;
		}

		// Step 3: Load the compiled program. The worklet retries SlotBusy from
		// process() each block, so we just wait for the tagged response.
		const node = workletNode; // Capture for closure (TypeScript null-check)
		const refreshId = ++loadRefreshCounter;

		const loadResult = await new Promise<{ success: boolean; error?: string }>((resolve) => {
			let timeout: ReturnType<typeof setTimeout>;
			const handler = (event: MessageEvent) => {
				const data = event.data;
				if (data.refreshId !== refreshId) return;
				if (data.type === 'programLoaded') {
					clearTimeout(timeout);
					node.port.removeEventListener('message', handler);
					resolve({ success: true });
				} else if (data.type === 'error') {
					clearTimeout(timeout);
					node.port.removeEventListener('message', handler);
					resolve({ success: false, error: data.message });
				}
			};
			timeout = setTimeout(() => {
				node.port.removeEventListener('message', handler);
				resolve({ success: false, error: 'Audio engine unresponsive (timeout)' });
			}, 5000);
			node.port.addEventListener('message', handler);
			node.port.postMessage({ type: 'loadCompiledProgram', refreshId });
		});

		if (loadResult.success) {
			return compileResult;
		}

		console.error('[AudioEngine] Load failed:', loadResult.error);
		return {
			success: false,
			diagnostics: [{ severity: 2, message: loadResult.error || 'Load failed', line: 1, column: 1 }]
		};
	}

	/**
	 * Load bytecode into the Cedar VM (legacy - prefer compile())
	 */
	function loadProgram(bytecode: Uint8Array) {
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load program - worklet not initialized');
			return;
		}

		console.log('[AudioEngine] Loading program, bytecode size:', bytecode.length);

		// Clone the bytecode since we're transferring the buffer
		const bytecodeClone = bytecode.slice();

		// Transfer bytecode to worklet
		workletNode.port.postMessage(
			{ type: 'loadProgram', bytecode: bytecodeClone.buffer },
			[bytecodeClone.buffer]
		);
	}

	/**
	 * Set an external parameter
	 */
	function setParam(name: string, value: number, slewMs?: number) {
		workletNode?.port.postMessage({ type: 'setParam', name, value, slewMs });
	}

	function getAnalyserNode() {
		return analyserNode;
	}

	function getAudioContext() {
		return audioContext;
	}

	/**
	 * Get time domain data for visualization
	 */
	function getTimeDomainData(): Uint8Array {
		if (!analyserNode) return new Uint8Array(0);
		const data = new Uint8Array(analyserNode.fftSize);
		analyserNode.getByteTimeDomainData(data);
		return data;
	}

	/**
	 * Get frequency data for visualization
	 */
	function getFrequencyData(): Uint8Array {
		if (!analyserNode) return new Uint8Array(0);
		const data = new Uint8Array(analyserNode.frequencyBinCount);
		analyserNode.getByteFrequencyData(data);
		return data;
	}

	/**
	 * Load a sample from float audio data
	 * @param name Sample name (e.g., "kick", "snare")
	 * @param audioData Float32Array of interleaved audio samples
	 * @param channels Number of channels (1=mono, 2=stereo)
	 * @param sampleRate Sample rate in Hz
	 */
	async function loadSample(name: string, audioData: Float32Array, channels: number, sampleRate: number) {
		await initialize();
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load sample - worklet bootstrap failed');
			return;
		}

		console.log('[AudioEngine] Loading sample:', name, 'samples:', audioData.length, 'channels:', channels);

		// Send audio data to worklet
		workletNode.port.postMessage({
			type: 'loadSample',
			name,
			audioData,
			channels,
			sampleRate
		});
	}

	/**
	 * Load a sample from raw decoded-or-encoded bytes. The worklet
	 * C++/WASM side decodes WAV/MP3/FLAC/etc., so all this does is
	 * post the bytes and await the `sampleLoaded` ack.
	 *
	 * Used by `loadSampleFromFile` (drag-drop path) and by the
	 * Phase B restore-on-init step (`upload-manifest` re-hydration).
	 */
	async function loadSampleFromBytes(
		name: string,
		data: ArrayBuffer,
		origin: 'builtin' | 'user' = 'user'
	): Promise<boolean> {
		await initialize();
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load sample - worklet bootstrap failed');
			return false;
		}

		try {
			console.log('[AudioEngine] Loading audio sample:', name, 'size:', data.byteLength);

			// Wait for the worklet ack so the reactive registry gets the
			// origin tag (file drops weren't waited on historically; the
			// promise is cheap and keeps the Files panel honest).
			const loadPromise = new Promise<boolean>((resolve) => {
				pendingSampleLoads.set(name, { resolve, origin });
				setTimeout(() => {
					if (pendingSampleLoads.has(name)) {
						console.error('[AudioEngine] Sample load timeout:', name);
						pendingSampleLoads.delete(name);
						resolve(false);
					}
				}, 10000);
			});

			workletNode.port.postMessage({
				type: 'loadSampleAudio',
				name,
				audioData: data
			});

			return await loadPromise;
		} catch (err) {
			console.error('[AudioEngine] Failed to load sample from bytes:', err);
			return false;
		}
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
			return await loadSampleFromBytes(name, arrayBuffer, origin);
		} catch (err) {
			console.error('[AudioEngine] Failed to read sample file:', err);
			return false;
		}
	}

	/**
	 * Load a sample from a URI (any scheme: file://, https://, github:, blob:, ...).
	 * Internal helper called by `loadAsset(uri, 'sample', name)`.
	 */
	async function loadSampleFromUri(
		name: string,
		uri: string,
		origin: 'builtin' | 'user' = 'user'
	): Promise<boolean> {
		await initialize();
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load sample - worklet bootstrap failed');
			return false;
		}

		try {
			console.log('[AudioEngine] Fetching sample from URI:', uri);
			const result = await loadFile(uri, { cache: true });
			const arrayBuffer = result.data;
			console.log('[AudioEngine] Loaded sample from URI:', name, 'size:', arrayBuffer.byteLength);

			// Create a promise that will be resolved when worklet confirms load
			const loadPromise = new Promise<boolean>((resolve) => {
				pendingSampleLoads.set(name, { resolve, origin, sourceUri: uri });
				// Timeout after 10 seconds
				setTimeout(() => {
					if (pendingSampleLoads.has(name)) {
						console.error('[AudioEngine] Sample load timeout:', name);
						pendingSampleLoads.delete(name);
						resolve(false);
					}
				}, 10000);
			});

			// Send raw bytes to worklet — C++/WASM decodes all formats
			workletNode.port.postMessage({
				type: 'loadSampleAudio',
				name,
				audioData: arrayBuffer
			});

			// Wait for worklet to confirm sample is loaded
			return await loadPromise;
		} catch (err) {
			console.error('[AudioEngine] Failed to load sample from URI:', err);
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
			const success = await loadAsset(pathToFetchUri(sample.url), 'sample', sample.name);
			if (success) loaded++;
		}
		console.log('[AudioEngine] Loaded', loaded, 'of', samples.length, 'samples');
		return loaded;
	}

	/**
	 * Start background preloading of default samples (non-blocking)
	 * Called automatically when the audio engine initializes
	 * Samples will be loaded lazily - compile() will wait for required samples
	 */
	function loadDefaultSamples() {
		if (state.samplesLoaded || state.samplesLoading) return;

		state.samplesLoading = true;
		console.log('[AudioEngine] Starting background sample preload...');

		// Mark all samples as pending
		for (const sample of DEFAULT_DRUM_KIT) {
			if (!sampleLoadState.has(sample.name)) {
				sampleLoadState.set(sample.name, 'pending');
			}
		}

		// Load samples one at a time in background (non-blocking)
		(async () => {
			let loaded = 0;
			for (const sample of DEFAULT_DRUM_KIT) {
				if (sampleLoadState.get(sample.name) === 'pending') {
					sampleLoadState.set(sample.name, 'loading');
					try {
						const success = await loadAsset(pathToFetchUri(sample.url), 'sample', sample.name, 'builtin');
						if (success) {
							sampleLoadState.set(sample.name, 'loaded');
							// `addLoadedSample` already ran via the worklet ack —
							// the explicit add here was redundant. Keep the counter only.
							loaded++;
						} else {
							sampleLoadState.set(sample.name, 'error');
						}
					} catch {
						sampleLoadState.set(sample.name, 'error');
					}
				} else if (sampleLoadState.get(sample.name) === 'loaded') {
					loaded++;
				}
			}
			state.samplesLoaded = true;
			state.samplesLoading = false;
			console.log('[AudioEngine] Background preload complete:', loaded, 'samples');
		})();
	}

	/**
	 * Background preload of default SoundFonts (non-blocking)
	 * Called automatically when the audio engine initializes
	 */
	function loadDefaultSoundFonts() {
		console.log('[AudioEngine] Starting background SoundFont preload...');

		(async () => {
			for (const sf of DEFAULT_SOUNDFONTS) {
				if (!sf.preload) continue;
				// Skip if already loaded
				if (state.loadedSoundfonts.some((s) => s.name === sf.name)) continue;

				let loaded = false;
				for (const rawUrl of sf.urls) {
					try {
						const info = await loadAsset(pathToFetchUri(rawUrl), 'soundfont', sf.name, 'builtin');
						if (info) {
							console.log(`[AudioEngine] Default SoundFont '${sf.name}' loaded: ${info.presetCount} presets`);
							loaded = true;
							break;
						}
					} catch {
						// Try next URL
					}
				}
				if (!loaded) {
					console.warn(`[AudioEngine] Default SoundFont '${sf.name}' failed to load from all URLs`);
				}
			}
		})();
	}

	/**
	 * Clear all loaded samples
	 */
	function clearSamples() {
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot clear samples - worklet not initialized');
			return;
		}

		workletNode.port.postMessage({ type: 'clearSamples' });
		console.log('[AudioEngine] Cleared all samples');
	}

	// =========================================================================
	// Sample Bank API
	// =========================================================================

	/**
	 * Load a sample bank from a URL (strudel.json manifest)
	 * @param url URL to the strudel.json manifest
	 * @param name Optional name override for the bank
	 */
	async function loadBank(url: string, name?: string): Promise<boolean> {
		try {
			await bankRegistry.loadBank(url, name);
			return true;
		} catch (err) {
			console.error('[AudioEngine] Failed to load bank:', err);
			return false;
		}
	}

	// =========================================================================
	// SoundFont API
	// =========================================================================

	/**
	 * Load a SoundFont (SF2) file from a URL or ArrayBuffer
	 * @param name Display name for the SoundFont
	 * @param data SF2 file data as ArrayBuffer
	 * @returns SoundFont info with preset list, or null on failure
	 */
	async function loadSoundFont(
		name: string,
		data: ArrayBuffer,
		origin: 'builtin' | 'user' = 'user'
	): Promise<SoundFontInfo | null> {
		await initialize();
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load SoundFont - worklet bootstrap failed');
			return null;
		}

		// Create a promise that will be resolved when worklet confirms load
		const loadPromise = new Promise<SoundFontInfo | null>((resolve) => {
			pendingSoundFontLoads.set(name, { resolve, origin });
			// Timeout after 30 seconds (large SF2 files can take time)
			setTimeout(() => {
				if (pendingSoundFontLoads.has(name)) {
					console.error('[AudioEngine] SoundFont load timeout:', name);
					pendingSoundFontLoads.delete(name);
					resolve(null);
				}
			}, 30000);
		});

		workletNode.port.postMessage({
			type: 'loadSoundFont',
			name,
			data
		}, [data]);

		return await loadPromise;
	}

	/**
	 * Load a SoundFont from a URI. Internal helper called by
	 * `loadAsset(uri, 'soundfont', name)`.
	 */
	async function loadSoundFontFromUri(
		name: string,
		uri: string,
		origin: 'builtin' | 'user' = 'user'
	): Promise<SoundFontInfo | null> {
		try {
			const result = await loadFile(uri, { cache: true });
			return await loadSoundFont(name, result.data, origin);
		} catch (err) {
			console.error('[AudioEngine] Failed to fetch SoundFont:', err);
			return null;
		}
	}

	/**
	 * Send a clear-wavetables message to the worklet. Use this before
	 * loading a new program's required_wavetables to reset the runtime
	 * registry's slot IDs so they match the compiler's source-order
	 * assignments.
	 */
	function clearWavetables() {
		if (!workletNode) return;
		workletNode.port.postMessage({ type: 'clearWavetables' });
		loadedWavetables.clear();
	}

	/**
	 * Load a wavetable bank from raw WAV bytes into the worklet. Resolves
	 * to the assigned bank ID, or -1 on failure.
	 */
	async function loadWavetable(name: string, data: ArrayBuffer): Promise<number> {
		await initialize();
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load wavetable - worklet bootstrap failed');
			return -1;
		}
		const loadPromise = new Promise<number>((resolve) => {
			pendingWavetableLoads.set(name, { resolve });
			setTimeout(() => {
				if (pendingWavetableLoads.has(name)) {
					console.error('[AudioEngine] Wavetable load timeout:', name);
					pendingWavetableLoads.delete(name);
					resolve(-1);
				}
			}, 30000);
		});
		workletNode.port.postMessage(
			{ type: 'loadWavetable', name, data },
			[data]
		);
		return await loadPromise;
	}

	/**
	 * Load a `.mid` file (prd-midi-input Phase 5) into the worklet's
	 * name-keyed registry. Subsequent `loadCompiledProgram` calls
	 * `cedar_apply_midi_sources` which attaches the parsed sequence to
	 * each MidiQueueState whose RequiredMidiSource.name matches `name`.
	 */
	async function loadMidiFile(name: string, data: ArrayBuffer): Promise<boolean> {
		await initialize();
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load MIDI file - worklet bootstrap failed');
			return false;
		}

		const loadPromise = new Promise<boolean>((resolve) => {
			pendingMidiFileLoads.set(name, { resolve });
			setTimeout(() => {
				if (pendingMidiFileLoads.has(name)) {
					console.error('[AudioEngine] MIDI file load timeout:', name);
					pendingMidiFileLoads.delete(name);
					resolve(false);
				}
			}, 10000);
		});

		workletNode.port.postMessage(
			{ type: 'loadMidiFile', name, data },
			[data]
		);
		return await loadPromise;
	}

	/**
	 * Fetch a `.mid` from a URI and load it. Internal helper called by
	 * `loadAsset(uri, 'midi', name)`.
	 */
	async function loadMidiFileFromUri(name: string, uri: string): Promise<boolean> {
		try {
			const result = await loadFile(uri, { cache: true });
			return await loadMidiFile(name, result.data);
		} catch (err) {
			console.error('[AudioEngine] Failed to fetch MIDI file:', err);
			return false;
		}
	}

	/**
	 * Drop a `.mid` registration from the main-thread bank (revokes the
	 * blob URL) and from the reactive Files-panel list. The worklet's
	 * sequence stays in `cedar_apply_midi_sources`-attached state for any
	 * currently-running program; the next compile that references this
	 * name will fail to resolve and surface a "missing asset" error,
	 * which is the same failure mode an unloaded file already produces.
	 */
	function unregisterMidiFile(name: string): void {
		midiBank.revoke(name);
		removeLoadedMidiFile(name);
		// Drop the persistence manifest entry so the row doesn't
		// reappear on the next reload (Phase B). Fire-and-forget —
		// the file-cache layer swallows errors silently.
		void forgetUpload('midi', name);
	}

	/**
	 * UI-only "forget" for a user-uploaded sample. Removes the row from
	 * the reactive registry and drops the persistence manifest entry.
	 * The worklet keeps the sample in WASM heap until the next reload —
	 * `cedar_remove_sample` doesn't exist yet (PRD Phase B notes a
	 * follow-up that would touch cedar C++).
	 */
	async function forgetSample(name: string): Promise<void> {
		removeLoadedSample(name);
		await forgetUpload('sample', name);
	}

	/**
	 * UI-only "forget" for a user-uploaded SoundFont. Same semantics as
	 * `forgetSample`: the worklet's `SoundFontRegistry` entry survives
	 * until reload, but the UI row and the manifest row are gone.
	 */
	async function forgetSoundFont(sfId: number): Promise<void> {
		const entry = state.loadedSoundfonts.find((s) => s.sfId === sfId);
		if (!entry) return;
		state.loadedSoundfonts = state.loadedSoundfonts.filter((s) => s.sfId !== sfId);
		await forgetUpload('soundfont', entry.name);
	}

	/**
	 * Fetch a wavetable WAV from a URI and load it into the worklet.
	 * Internal helper called by `loadAsset(uri, 'wavetable', name)`.
	 */
	async function loadWavetableFromUri(name: string, uri: string): Promise<number> {
		try {
			const result = await loadFile(uri, { cache: true });
			return await loadWavetable(name, result.data);
		} catch (err) {
			console.error('[AudioEngine] Failed to fetch wavetable:', err);
			return -1;
		}
	}

	/**
	 * Unified asset loader keyed by URI. Dispatches to the right registry
	 * based on `kind`. The URI is resolved via the singleton `uriResolver`,
	 * so any scheme it knows about (file://, https://, github:, blob:, ...)
	 * works uniformly.
	 *
	 * Return type is overloaded by `kind`:
	 * - 'sample'      → boolean (loaded successfully)
	 * - 'soundfont'   → SoundFontInfo | null (preset list / null on failure)
	 * - 'wavetable'   → number (assigned bank ID, or -1 on failure)
	 * - 'sample_bank' → boolean (manifest fetched + parsed successfully)
	 */
	function loadAsset(uri: string, kind: 'sample', name: string, origin?: 'builtin' | 'user'): Promise<boolean>;
	function loadAsset(uri: string, kind: 'soundfont', name: string, origin?: 'builtin' | 'user'): Promise<SoundFontInfo | null>;
	function loadAsset(uri: string, kind: 'wavetable', name: string): Promise<number>;
	function loadAsset(uri: string, kind: 'sample_bank', name?: string): Promise<boolean>;
	function loadAsset(uri: string, kind: 'midi', name: string): Promise<boolean>;
	function loadAsset(
		uri: string,
		kind: 'sample' | 'soundfont' | 'wavetable' | 'sample_bank' | 'midi',
		name?: string,
		origin: 'builtin' | 'user' = 'user'
	): Promise<boolean | SoundFontInfo | null | number> {
		switch (kind) {
			case 'sample':
				return loadSampleFromUri(name!, uri, origin);
			case 'soundfont':
				return loadSoundFontFromUri(name!, uri, origin);
			case 'wavetable':
				return loadWavetableFromUri(name!, uri);
			case 'sample_bank':
				return loadBank(uri, name);
			case 'midi':
				return loadMidiFileFromUri(name!, uri);
		}
	}

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

	/**
	 * Get builtin function metadata for autocomplete
	 * Returns cached data if available, otherwise fetches from worklet
	 */
	async function getBuiltins(): Promise<BuiltinsData | null> {
		// Return cache if available
		if (builtinsCache) {
			return builtinsCache;
		}

		// Need to initialize first
		if (!state.isInitialized) {
			await initialize();
		}

		if (!workletNode) {
			console.warn('[AudioEngine] Cannot get builtins - worklet not initialized');
			return null;
		}

		// Request builtins from worklet
		return new Promise((resolve) => {
			builtinsResolve = resolve;
			// Timeout after 2 seconds
			setTimeout(() => {
				if (builtinsResolve === resolve) {
					builtinsResolve = null;
					resolve(null);
				}
			}, 2000);
			workletNode!.port.postMessage({ type: 'getBuiltins' });
		});
	}

	/**
	 * Get the analyzer-driven shape index for editor autocomplete.
	 * Phase 2 of the records-system-unification PRD.
	 *
	 * @param source - editor buffer
	 * @param cursorOffset - UTF-8 byte offset of the caret; pass `0xFFFFFFFF`
	 *                      to skip patternHole resolution
	 * @returns parsed shape index, or `null` on error / when the worklet
	 *          isn't initialized yet.
	 */
	async function getShapeIndex(
		source: string,
		cursorOffset: number
	): Promise<ShapeIndexData | null> {
		if (!state.isInitialized) {
			await initialize();
		}
		if (!workletNode) {
			return null;
		}

		// If a previous request is still pending, resolve it with null so
		// its caller doesn't hang. Only the latest request gets a real
		// answer — debouncing on the editor side keeps this simple.
		if (shapeIndexResolve) {
			shapeIndexResolve(null);
			shapeIndexResolve = null;
		}

		return new Promise((resolve) => {
			shapeIndexResolve = resolve;
			setTimeout(() => {
				if (shapeIndexResolve === resolve) {
					shapeIndexResolve = null;
					resolve(null);
				}
			}, 2000);
			workletNode!.port.postMessage({
				type: 'getShapeIndex',
				source,
				cursor: cursorOffset
			});
		});
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
				// Send initial value to worklet
				workletNode?.port.postMessage({
					type: 'setParam',
					name: param.name,
					value: param.defaultValue
				});
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
		workletNode?.port.postMessage({ type: 'setParam', name, value, slewMs });
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
		workletNode?.port.postMessage({ type: 'setParam', name, value: 1, slewMs: 0 });
	}

	/**
	 * Release a button (set to 0)
	 */
	function releaseButton(name: string) {
		state.paramValues.set(name, 0);
		workletNode?.port.postMessage({ type: 'setParam', name, value: 0, slewMs: 0 });
	}

	/**
	 * Toggle a boolean parameter
	 */
	function toggleParam(name: string) {
		const current = state.paramValues.get(name) ?? 0;
		const newValue = current > 0.5 ? 0 : 1;
		state.paramValues.set(name, newValue);
		workletNode?.port.postMessage({ type: 'setParam', name, value: newValue });
	}

	/**
	 * Reset a parameter to its default value
	 */
	function resetParam(name: string) {
		const param = state.params.find(p => p.name === name);
		if (param) {
			state.paramValues.set(name, param.defaultValue);
			workletNode?.port.postMessage({ type: 'setParam', name, value: param.defaultValue });
		}
	}

	// =========================================================================
	// Pattern Highlighting API
	// =========================================================================

	/**
	 * Get pattern info for all patterns in the current compile result
	 */
	async function getPatternInfo(): Promise<PatternInfo[]> {
		if (!workletNode) {
			return [];
		}

		return new Promise((resolve) => {
			patternInfoResolve = resolve;
			setTimeout(() => {
				if (patternInfoResolve === resolve) {
					patternInfoResolve = null;
					resolve([]);
				}
			}, 1000);
			workletNode!.port.postMessage({ type: 'getPatternInfo' });
		});
	}

	/**
	 * Query pattern for preview events
	 */
	async function queryPatternPreview(patternIndex: number, startBeat: number, endBeat: number): Promise<PatternEvent[]> {
		if (!workletNode) {
			return [];
		}

		return new Promise((resolve) => {
			patternPreviewResolve = resolve;
			setTimeout(() => {
				if (patternPreviewResolve === resolve) {
					patternPreviewResolve = null;
					resolve([]);
				}
			}, 1000);
			workletNode!.port.postMessage({ type: 'queryPatternPreview', patternIndex, startBeat, endBeat });
		});
	}

	/**
	 * Get current beat position from VM
	 */
	async function getCurrentBeatPosition(): Promise<number> {
		if (!workletNode) {
			return 0;
		}

		return new Promise((resolve) => {
			beatPositionResolvers.push(resolve);
			// Only send message if this is the first pending request
			if (beatPositionResolvers.length === 1) {
				workletNode!.port.postMessage({ type: 'getCurrentBeatPosition' });
			}
			// Timeout for this specific resolver
			setTimeout(() => {
				const idx = beatPositionResolvers.indexOf(resolve);
				if (idx !== -1) {
					beatPositionResolvers.splice(idx, 1);
					resolve(0);
				}
			}, 100);
		});
	}

	/**
	 * Get active step source ranges for patterns
	 */
	async function getActiveSteps(stateIds: number[]): Promise<Record<number, { offset: number; length: number }>> {
		if (!workletNode) {
			return {};
		}

		return new Promise((resolve) => {
			activeStepsResolve = resolve;
			setTimeout(() => {
				if (activeStepsResolve === resolve) {
					activeStepsResolve = null;
					resolve({});
				}
			}, 100);
			workletNode!.port.postMessage({ type: 'getActiveSteps', stateIds });
		});
	}

	/**
	 * Inspect state by ID, returning JSON representation of state fields
	 * @param stateId State ID (32-bit FNV-1a hash)
	 * @returns State inspection data or null if not found
	 */
	async function inspectState(stateId: number): Promise<StateInspection | null> {
		if (!workletNode) {
			return null;
		}

		return new Promise((resolve) => {
			stateInspectionResolve = resolve;
			setTimeout(() => {
				if (stateInspectionResolve === resolve) {
					stateInspectionResolve = null;
					resolve(null);
				}
			}, 100);
			workletNode!.port.postMessage({ type: 'inspectState', stateId });
		});
	}

	/**
	 * Get detailed pattern debug info (AST, sequences, events)
	 * @param patternIndex Pattern index (0 to pattern_count-1)
	 * @returns Pattern debug info or null if not found
	 */
	async function getPatternDebug(patternIndex: number): Promise<PatternDebugInfo | null> {
		if (!workletNode) {
			return null;
		}

		return new Promise((resolve) => {
			patternDebugResolve = resolve;
			setTimeout(() => {
				if (patternDebugResolve === resolve) {
					patternDebugResolve = null;
					resolve(null);
				}
			}, 1000);
			workletNode!.port.postMessage({ type: 'getPatternDebug', patternIndex });
		});
	}

	/**
	 * Get probe data (ring buffer samples) for a visualization
	 * @param stateId The probe's state_id from viz decl
	 * @returns Float32Array of samples (oldest to newest) or null if not available
	 */
	async function getProbeData(stateId: number): Promise<Float32Array | null> {
		if (!workletNode) {
			return null;
		}

		return new Promise((resolve) => {
			// Store resolver keyed by stateId for concurrent requests
			probeDataResolvers.set(stateId, resolve);
			setTimeout(() => {
				// Timeout: resolve with null if still pending
				if (probeDataResolvers.get(stateId) === resolve) {
					probeDataResolvers.delete(stateId);
					resolve(null);
				}
			}, 100);
			workletNode!.port.postMessage({ type: 'getProbeData', stateId });
		});
	}

	/**
	 * Get FFT probe data (magnitude spectrum) for a visualization
	 * @param stateId The FFT probe's state_id from viz decl
	 * @returns FFTProbeData with magnitudes in dB, or null if not available
	 */
	async function getFFTProbeData(stateId: number): Promise<FFTProbeData | null> {
		if (!workletNode) {
			return null;
		}

		return new Promise((resolve) => {
			fftProbeDataResolvers.set(stateId, resolve);
			setTimeout(() => {
				if (fftProbeDataResolvers.get(stateId) === resolve) {
					fftProbeDataResolvers.delete(stateId);
					resolve(null);
				}
			}, 100);
			workletNode!.port.postMessage({ type: 'getFFTProbeData', stateId });
		});
	}

	// =========================================================================
	// Audio input (audio-input PRD)
	// =========================================================================

	function disconnectActiveInput() {
		if (activeInput) {
			try { activeInput.stop(); } catch (e) {
				console.warn('[AudioEngine] disconnectActiveInput error', e);
			}
			activeInput = null;
		}
	}

	/**
	 * Switch the live audio input source. Pass {kind:'none'} to disconnect.
	 * UI surfaces granted/denied/etc via state.inputStatus.
	 *
	 * For 'file' sources, fileName must reference a sample registered with
	 * bankRegistry (the upload flow / drag-drop pipeline). The existing
	 * sample-loading registry is reused per the PRD §4.5.
	 */
	async function setInputSource(config: InputSourceConfig): Promise<void> {
		// Lazy-initialize the audio engine. The Audio Input panel lives in
		// Settings, so users typically click Mic/Tab/File before pressing Play
		// — the click counts as a user gesture, so AudioContext creation is
		// allowed here. Without this, the panel silently shows "Audio not
		// initialized" with no console output.
		if (!audioContext || !workletNode) {
			await initialize();
		}
		if (!audioContext || !workletNode) {
			state.inputError = state.error ?? 'Audio not initialized';
			state.inputStatus = 'error';
			return;
		}

		// Always tear down the previous source first to avoid summing two streams.
		disconnectActiveInput();

		state.inputKind = config.kind;
		state.inputDeviceId = config.deviceId ?? null;
		state.inputFileName = config.fileName ?? null;
		if (config.constraints) state.inputConstraints = { ...config.constraints };
		state.inputError = null;

		if (config.kind === 'none') {
			state.inputStatus = 'idle';
			return;
		}

		state.inputStatus = 'connecting';
		try {
			let acquired: ActiveInputSource;
			if (config.kind === 'mic') {
				acquired = await acquireMicSource(
					audioContext,
					config.deviceId,
					config.constraints ?? state.inputConstraints
				);
			} else if (config.kind === 'tab') {
				acquired = await acquireTabSource(audioContext);
			} else if (config.kind === 'file') {
				if (!config.fileName) throw new Error('file source requires fileName');
				const data = inputFileBuffers.get(config.fileName);
				if (!data) {
					throw new Error(`Input file "${config.fileName}" has not been uploaded`);
				}
				acquired = await acquireFileSource(audioContext, config.fileName, data);
			} else {
				throw new Error(`Unknown input source kind: ${config.kind}`);
			}

			acquired.node.connect(workletNode);
			activeInput = acquired;
			state.inputStatus = 'active';

			// Forward source string to WASM for any compile-time consumers
			// (currently informational; future per-call overrides will use it).
			const sourceStr = config.kind === 'mic' ? 'mic'
				: config.kind === 'tab' ? 'tab'
				: config.kind === 'file' ? `file:${config.fileName ?? ''}`
				: '';
			workletNode.port.postMessage({ type: 'setInputSource', source: sourceStr });
		} catch (err) {
			console.warn('[AudioEngine] Failed to acquire input source:', err);
			state.inputError = err instanceof Error ? err.message : String(err);
			// Map common DOMException names onto the user-visible status.
			const name = (err as { name?: string })?.name ?? '';
			if (name === 'NotAllowedError' || name === 'PermissionDeniedError') {
				state.inputStatus = 'denied';
			} else if (name === 'NotFoundError' || name === 'OverconstrainedError') {
				state.inputStatus = 'unavailable';
			} else {
				state.inputStatus = 'error';
			}
			state.inputKind = 'none';
		}
	}

	async function listInputDevices(): Promise<MediaDeviceInfo[]> {
		return enumerateInputDevices();
	}

	/**
	 * Register an uploaded file under `name`. The raw ArrayBuffer is kept in
	 * memory so subsequent setInputSource({kind:'file', fileName: name}) can
	 * decode and loop it. Returns the registered name (to surface in the UI).
	 */
	function registerInputFile(name: string, data: ArrayBuffer): string {
		inputFileBuffers.set(name, data);
		return name;
	}

	function unregisterInputFile(name: string) {
		inputFileBuffers.delete(name);
		if (state.inputKind === 'file' && state.inputFileName === name) {
			void setInputSource({ kind: 'none' });
		}
	}

	function getInputFileNames(): string[] {
		return Array.from(inputFileBuffers.keys()).sort();
	}

	function setInputConstraints(c: Partial<InputConstraints>) {
		const next = { ...state.inputConstraints, ...c };
		state.inputConstraints = next;
		// If a mic source is active, re-acquire so the new constraints take effect.
		if (state.inputKind === 'mic' && activeInput) {
			void setInputSource({
				kind: 'mic',
				deviceId: state.inputDeviceId ?? undefined,
				constraints: next
			});
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
		setInputSource,
		setInputConstraints,
		listInputDevices,
		registerInputFile,
		unregisterInputFile,
		getInputFileNames,

		// MIDI input (prd-midi-input)
		get midi() { return midiInput; },
		setDefaultMidiDevice(name: string) {
			settingsStore.setDefaultMidiDevice(name);
			midiInput.setDefaultDeviceName(name);
		},
		async ensureMidiAccess() {
			return midiInput.ensureAccess();
		},

		initialize,
		play,
		pause,
		stop,
		restartAudio,
		setBpm,
		setVolume,
		toggleVisualizations,
		compile,
		loadProgram,
		setParam,
		getAnalyserNode,
		getAudioContext,
		getTimeDomainData,
		getFrequencyData,
		loadSample,
		loadSampleFromFile,
		loadSampleFromBytes,
		loadSamplePack,
		clearSamples,
		forgetSample,
		// Sample bank API
		loadBank,
		getBankNames,
		// SoundFont API
		loadSoundFont,
		forgetSoundFont,
		// MIDI file API (prd-midi-input Phase 5)
		loadMidiFile,
		unregisterMidiFile,
		midiBank,
		// Wavetable API (Smooch)
		loadWavetable,
		clearWavetables,
		// Unified URI-keyed asset loader
		loadAsset,
		getBankSampleNames,
		getBankSampleVariantCount,
		hasBank,
		getBuiltins,
		getShapeIndex,
		// Parameter exposure API
		setParamValue,
		getParamValue,
		pressButton,
		releaseButton,
		toggleParam,
		resetParam,
		clearParams,
		// Pattern highlighting API
		getPatternInfo,
		queryPatternPreview,
		getCurrentBeatPosition,
		getActiveSteps,
		// State inspection API
		inspectState,
		// Pattern debug API
		getPatternDebug,
		// Visualization probe data
		getProbeData,
		getFFTProbeData
	};
}

export const audioEngine = createAudioEngine();
