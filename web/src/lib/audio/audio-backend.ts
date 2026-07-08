/**
 * `AudioBackend` — the transport seam between the shared SvelteKit UI and
 * whichever engine actually renders audio (prd-web-audio-backend.md).
 *
 * The store shell (`$stores/audio.svelte`) owns all reactive UI state and
 * delegates transport / compile / asset / query calls to the active backend:
 *
 * - `WasmAudioBackend` (`./wasm-backend`) — browser path: AudioContext +
 *   AudioWorklet + compile worker + WASM.
 * - `JuceAudioBackend` (`./juce-backend`) — native path: `window.__JUCE__`
 *   bridge per nkido-studio `bridge-protocol.md`. Dormant in a plain browser.
 *
 * Transport is modelled as *request + backend-pushed truth*: `play()` /
 * `setBpm()` are requests; the backend reports actual state through the
 * `AudioBackendHost` callbacks the shell registers when it installs the
 * backend. This file is the source of truth for the interface (PRD §4.1;
 * pure shell bookkeeping like `getParamValue`/`pressButton` stays on the
 * store and is intentionally not part of the transport surface).
 */

import type { ShapeIndex as ShapeIndexData } from '$lib/editor/akkado-shape-index';
import type {
	InputConstraints,
	InputSourceConfig,
	InputSourceKind,
	InputStatus
} from '$lib/audio/input-source';
import type {
	MidiInputController,
	RequiredMidiSource,
	RequiredMidiCcRoute
} from '$lib/midi/midi-input.svelte';

// ===========================================================================
// Shared transport-facing types (moved from stores/audio.svelte.ts; the
// store re-exports them so the ~34 existing consumers are untouched).
// ===========================================================================

export interface Diagnostic {
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
export interface DisassemblyInstruction {
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

export interface DisassemblySummary {
	totalInstructions: number;
	statefulCount: number;
	uniqueStateIds: number;
	stateIds: number[];
}

export interface DisassemblyInfo {
	instructions: DisassemblyInstruction[];
	summary: DisassemblySummary;
}

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

export interface RequiredSoundFont {
	filename: string;
	preset: number;
}

export interface RequiredWavetable {
	name: string; // bank name (matches the smooch("...") string arg)
	path: string; // path/URL to fetch the WAV from
	id: number; // sequential slot ID assigned by the compiler
}

// URI kinds emitted by the akkado compiler. Mirrors `akkado::UriKind`.
// 0 = SampleBank manifest (strudel.json), 1 = SoundFont (.sf2),
// 2 = Wavetable (.wav), 3 = standalone sample.
export type UriKind = 0 | 1 | 2 | 3;

export interface UriRequest {
	uri: string;
	kind: UriKind;
}

export interface CompileResult {
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
	// Whether this compile was superseded by a newer one before it landed.
	// The store treats `superseded` as a no-op (no diagnostic, no UI churn).
	superseded?: boolean;
}

// Builtins metadata from the compiler
export interface OptionFieldSpec {
	name: string;
	type: 'number' | 'string' | 'bool' | 'enum';
	default?: string;
	description?: string;
	values?: string;
}

export interface BuiltinParam {
	name: string;
	required: boolean;
	default?: number;
	type?: string;
	optionFields?: OptionFieldSpec[];
	acceptsSpread?: boolean;
	extended?: boolean;
}

export interface BuiltinInfo {
	params: BuiltinParam[];
	description: string;
}

export interface BuiltinsData {
	functions: Record<string, BuiltinInfo>;
	aliases: Record<string, string>;
	keywords: string[];
}

// Pattern highlighting types
export interface PatternInfo {
	stateId: number;
	docOffset: number;
	docLength: number;
	cycleLength: number;
}

export interface PatternEvent {
	time: number;
	duration: number;
	value: number;
	sourceOffset: number;
	sourceLength: number;
}

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

export type { ShapeIndexData };

// ===========================================================================
// Backend → shell callbacks ("backend-pushed truth", PRD §4)
// ===========================================================================

export interface TransportState {
	isPlaying: boolean;
	bpm: number;
	currentBeat: number;
	currentBar: number;
}

/**
 * Non-transport engine status the backend drives. The shell mirrors these
 * into its reactive `$state`; partial updates patch only the given keys.
 */
export interface BackendStatus {
	isInitialized: boolean;
	isLoading: boolean;
	hasProgram: boolean;
	isLoadingSamples: boolean;
	activeSampleRate: number | null;
	// Audio input (audio-input PRD)
	inputKind: InputSourceKind;
	inputDeviceId: string | null;
	inputFileName: string | null;
	inputConstraints: InputConstraints;
	inputStatus: InputStatus;
	inputError: string | null;
}

/** Registry update pushed when the engine acknowledges an asset load. */
export type AssetLoadedEvent =
	| { kind: 'sample'; name: string; origin: 'builtin' | 'user'; sourceUri?: string }
	| { kind: 'soundfont'; info: SoundFontInfo }
	| { kind: 'midi'; name: string };

/**
 * Callback surface the shell hands to the backend on construction. The
 * backend is the only writer; every call lands in the shell's `$state`.
 */
export interface AudioBackendHost {
	/** Engine/host-owned transport truth (partial patch). */
	onTransport(t: Partial<TransportState>): void;
	/** Host automation write-back (native); web backends never call it. */
	onParam(name: string, value: number): void;
	/** Engine-surfaced error string for the status UI (`null` clears it). */
	onError(message: string | null): void;
	/** Boot / loading / input status flags (partial patch). */
	onStatus(s: Partial<BackendStatus>): void;
	/** Asset registry updates (Files panel). */
	onAssetLoaded(ev: AssetLoadedEvent): void;
	/** Registry wipe, e.g. MIDI sequences dropped by an engine reset. */
	onAssetsCleared(kind: 'sample' | 'midi'): void;
	/** Compile metadata to apply (paramDecls / vizDecls / disassembly). */
	onCompileResult(result: CompileResult): void;
	/** Fired by restart() between teardown and re-initialize so the shell
	 *  resets its per-engine state exactly once, in order. */
	onEngineReset(): void;
}

// ===========================================================================
// The backend interface (PRD §4.1)
// ===========================================================================

export interface AudioBackend {
	// Lifecycle
	initialize(): Promise<void>;
	restart(): Promise<void>; // shell method: restartAudio()
	dispose(): void; // teardown incl. terminateCompileWorker()

	// Transport (request; truth arrives via AudioBackendHost.onTransport)
	play(): Promise<void>;
	pause(): Promise<void>;
	stop(): Promise<void>;
	setBpm(bpm: number): void;
	setVolume(volume: number): void;

	// Compile
	compile(source: string): Promise<CompileResult>;

	// Params (shell owns value bookkeeping; host automation writes back
	// via AudioBackendHost.onParam)
	setParam(name: string, value: number, slewMs?: number): void;

	// Assets
	loadSample(
		name: string,
		audioData: Float32Array,
		channels: number,
		sampleRate: number
	): Promise<void>;
	loadSampleFromBytes(name: string, data: ArrayBuffer, origin?: 'builtin' | 'user'): Promise<boolean>;
	loadSoundFont(
		name: string,
		data: ArrayBuffer,
		origin?: 'builtin' | 'user'
	): Promise<SoundFontInfo | null>;
	loadMidiFile(name: string, data: ArrayBuffer): Promise<boolean>;
	loadWavetable(name: string, data: ArrayBuffer): Promise<number>;
	loadBank(url: string, name?: string): Promise<boolean>;
	loadAsset(uri: string, kind: 'sample', name: string, origin?: 'builtin' | 'user'): Promise<boolean>;
	loadAsset(
		uri: string,
		kind: 'soundfont',
		name: string,
		origin?: 'builtin' | 'user'
	): Promise<SoundFontInfo | null>;
	loadAsset(uri: string, kind: 'wavetable', name: string): Promise<number>;
	loadAsset(uri: string, kind: 'sample_bank', name?: string): Promise<boolean>;
	loadAsset(uri: string, kind: 'midi', name: string): Promise<boolean>;
	clearSamples(): void;
	clearWavetables(): void;
	/** Drop internal load tracking so the next compile re-fetches. The
	 *  shell owns the registry row + persistence manifest. */
	forgetSample(name: string): void;
	forgetSoundFont(name: string): void;
	forgetMidiFile(name: string): void;

	// Queries (native: hot ones — probe/FFT/beat — served from last pushed frame)
	getBuiltins(): Promise<BuiltinsData | null>;
	getShapeIndex(source: string, cursorOffset: number): Promise<ShapeIndexData | null>;
	getPatternInfo(): Promise<PatternInfo[]>;
	queryPatternPreview(
		patternIndex: number,
		startBeat: number,
		endBeat: number
	): Promise<PatternEvent[]>;
	getCurrentBeatPosition(): Promise<number>;
	getActiveSteps(stateIds: number[]): Promise<Record<number, { offset: number; length: number }>>;
	inspectState(stateId: number): Promise<StateInspection | null>;
	getPatternDebug(patternIndex: number): Promise<PatternDebugInfo | null>;
	getProbeData(stateId: number): Promise<Float32Array | null>;
	getFFTProbeData(stateId: number): Promise<FFTProbeData | null>;

	// MIDI / input (native: DAW/host MIDI + input bus; Web MIDI /
	// getUserMedia absent → no-op / null)
	setInputSource(config: InputSourceConfig): Promise<void>;
	setInputConstraints(c: Partial<InputConstraints>): void;
	listInputDevices(): Promise<MediaDeviceInfo[]>;
	registerInputFile(name: string, data: ArrayBuffer): string;
	unregisterInputFile(name: string): void;
	getInputFileNames(): string[];
	getMidiController(): MidiInputController | null;
	setDefaultMidiDevice(name: string): void;
	ensureMidiAccess(): Promise<unknown>;

	// Web-only escape hatches (null / no-op on native — only test-hooks.ts
	// and the worker-recovery e2e consume these)
	getAnalyserNode(): AnalyserNode | null;
	getAudioContext(): AudioContext | null;
	getTimeDomainData(): Uint8Array;
	getFrequencyData(): Uint8Array;
	terminateCompileWorker(): void;
}
