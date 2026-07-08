/**
 * `WasmAudioBackend` — the browser transport behind the `AudioBackend`
 * interface (prd-web-audio-backend.md §5.2).
 *
 * Owns everything that talks to the engine in a plain browser: the
 * AudioContext + AudioWorklet graph (gain → analyser → destination), the
 * WASM fetch + `init` byte injection, the compile worker, every
 * `port.postMessage` RPC site, Web MIDI, and getUserMedia/getDisplayMedia
 * live input. Moved verbatim from `stores/audio.svelte.ts`; state that the
 * UI reads is pushed back through the `AudioBackendHost` callbacks.
 *
 * Documented cleanups vs. the pre-extraction store (behavior-preserving):
 * - dropped dead, never-called `loadDefaultSamples` / `loadDefaultSoundFonts`
 *   / `ensureSampleLoaded` (no references anywhere in `web/src`)
 * - the shell's `loadedSamplesIndex` / `loadedMidiFilesIndex` /
 *   `state.loadedSoundfonts` existence checks are backed here by plain
 *   Sets/Maps (`loadedSampleNames`, `loadedMidiNames`, `loadedSoundFonts`)
 *   instead of reaching into reactive state.
 */

import { DEFAULT_DRUM_KIT } from '$lib/audio/default-samples';
import { resolveDefaultSoundFontUrls } from '$lib/audio/default-soundfonts';
import { midiBank } from '$lib/audio/midi-bank';
import { settingsStore } from '$lib/stores/settings.svelte';
import { bankRegistry } from '$lib/audio/bank-registry';
import { ensureCatalogLoaded, hasCatalogBank } from '$lib/audio/bank-catalog';
import { loadFile } from '$lib/io/file-loader';
import { pathToFetchUri } from '$lib/io/path-to-uri';
import { listUploads } from '$lib/io/upload-manifest';
import {
	acquireMicSource,
	acquireTabSource,
	acquireFileSource,
	enumerateInputDevices,
	DEFAULT_INPUT_CONSTRAINTS,
	type ActiveInputSource,
	type InputConstraints,
	type InputSourceConfig,
	type InputSourceKind
} from '$lib/audio/input-source';
import {
	createMidiInputController,
	type RequiredMidiSource,
	type RequiredMidiCcRoute
} from '$lib/midi/midi-input.svelte';
import type {
	AudioBackend,
	AudioBackendHost,
	BuiltinsData,
	CompileResult,
	Diagnostic,
	DisassemblyInfo,
	FFTProbeData,
	ParamDecl,
	PatternDebugInfo,
	PatternEvent,
	PatternInfo,
	RequiredSampleExtended,
	RequiredSoundFont,
	RequiredWavetable,
	ShapeIndexData,
	SoundFontInfo,
	SoundFontPresetInfo,
	StateInspection,
	UriRequest,
	VizDecl
} from './audio-backend';

// Internal shape of the worker's `compileResult` message — same as the
// public CompileResult plus the four wire-format buffers and the
// builtin-var overrides the worklet applies on load.
interface CompileResultInternal extends CompileResult {
	bytecode?: Uint8Array;
	stateInitsBuf?: Uint8Array;
	midiSourcesBuf?: Uint8Array;
	blockTable?: Uint8Array;
	mainInstCount?: number;
	builtinVarOverrides?: Array<{ name: string; value: number }>;
}

export function createWasmAudioBackend(host: AudioBackendHost): AudioBackend {
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
	let initialized = false;

	// Transport mirrors: the shell owns the reactive copies; these exist so
	// transport-internal paths (the worklet `initialized` handshake, gain
	// setup after restart) don't have to read back out of the shell.
	let bpm = 120;
	let volume = 0.8;
	// Mirror of the latest onError message; setInputSource surfaces it when
	// bootstrap failed (previously read `state.error`).
	let lastError: string | null = null;

	function reportError(message: string | null) {
		lastError = message;
		host.onError(message);
	}

	// Compile worker (PRD prd-compile-off-audio-thread §4). Owns its own
	// nkido.wasm instance; runs akkado_compile and all metadata extraction
	// off the AudioWorklet thread. Spawned during initialize(); recreated
	// lazily on worker death.
	let compileWorker: Worker | null = null;
	let compileWorkerReady: Promise<void> | null = null;
	let compileWorkerDead = false;
	// Outstanding compile() promises keyed by generation. Supersede-by-newest
	// drops stale results in compile() itself; the map exists so two compiles
	// in flight don't trample each other's resolver.
	const pendingCompileResolves = new Map<number, (r: CompileResultInternal) => void>();
	let nextCompileGen = 0;
	let latestRequestedCompileGen = 0;

	// Currently connected live-input source (audio-input PRD §4.5).
	// Null = no input attached; in() then returns silence.
	let activeInput: ActiveInputSource | null = null;
	let inputKind: InputSourceKind = 'none';
	let inputDeviceId: string | null = null;
	let inputFileName: string | null = null;
	let inputConstraints: InputConstraints = { ...DEFAULT_INPUT_CONSTRAINTS };

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
	let activeStepsResolve:
		| ((steps: Record<number, { offset: number; length: number }>) => void)
		| null = null;
	let stateInspectionResolve: ((data: StateInspection | null) => void) | null = null;
	// Map from stateId to resolve callback - supports multiple concurrent probe requests
	const probeDataResolvers = new Map<number, (samples: Float32Array | null) => void>();
	const fftProbeDataResolvers = new Map<number, (data: FFTProbeData | null) => void>();
	let patternDebugResolve: ((data: PatternDebugInfo | null) => void) | null = null;

	// Track sample loading state: 'pending' | 'loading' | 'loaded' | 'error'
	const sampleLoadState = new Map<string, 'pending' | 'loading' | 'loaded' | 'error'>();
	// Names the worklet has acked. The shell's reactive registry mirrors
	// this via onAssetLoaded; these Sets/Maps are the transport's own O(1)
	// existence checks (compile-time "already loaded?" gating).
	const loadedSampleNames = new Set<string>();
	const loadedMidiNames = new Set<string>();
	const loadedSoundFonts = new Map<string, SoundFontInfo>();
	// Pending sample load promises. Origin / sourceUri are stashed here so
	// the `sampleLoaded` worklet ack can attach them to the reactive entry
	// without the handler knowing which path called it.
	const pendingSampleLoads = new Map<
		string,
		{
			resolve: (success: boolean) => void;
			origin: 'builtin' | 'user';
			sourceUri?: string;
		}
	>();
	// Pending SoundFont load promises (origin threaded for the Files panel).
	const pendingSoundFontLoads = new Map<
		string,
		{
			resolve: (info: SoundFontInfo | null) => void;
			origin: 'builtin' | 'user';
		}
	>();
	// Pending wavetable bank load promises (resolves to assigned bank ID, -1 on failure)
	const pendingWavetableLoads = new Map<string, { resolve: (bankId: number) => void }>();
	// Track which wavetable bank IDs are currently registered in the worklet
	// (kept in sync via clearWavetables / loadWavetable). Map: name → bankId.
	const loadedWavetables = new Map<string, number>();
	// Pending .mid file load promises (prd-midi-input Phase 5).
	const pendingMidiFileLoads = new Map<string, { resolve: (success: boolean) => void }>();

	async function initialize(): Promise<void> {
		if (initialized) return;
		if (initPromise) return initPromise;

		host.onStatus({ isLoading: true });
		reportError(null);

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
			host.onStatus({ activeSampleRate: audioContext.sampleRate });

			gainNode = audioContext.createGain();
			gainNode.gain.value = volume;

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

			// Spawn the compile worker alongside the worklet. The worker uses
			// the same nkido.wasm bytes already fetched above, so there's no
			// second network round-trip. We await `ready` so subsequent
			// compile() calls don't have to special-case boot.
			spawnCompileWorker();

			initialized = true;
			host.onStatus({ isInitialized: true });
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
			reportError(err instanceof Error ? err.message : String(err));
			// Clear so a later caller (e.g. user clicking Play) can retry.
			initPromise = null;
		} finally {
			host.onStatus({ isLoading: false });
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
				workletNode?.port.postMessage({ type: 'setBpm', bpm });
				// Default samples load lazily when compile() needs them
				break;
			case 'programLoaded':
				host.onStatus({ hasProgram: true });
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
				loadedSampleNames.add(name);
				host.onAssetLoaded({
					kind: 'sample',
					name,
					origin: pending?.origin ?? 'user',
					sourceUri: pending?.sourceUri
				});
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
					loadedSoundFonts.set(sfName, sfInfo);
					host.onAssetLoaded({ kind: 'soundfont', info: sfInfo });
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
					loadedMidiNames.add(midiName);
					host.onAssetLoaded({ kind: 'midi', name: midiName });
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
				reportError(errorMsg);
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
					resolver(
						magnitudes
							? {
									magnitudes: new Float32Array(magnitudes),
									binCount,
									frameCounter
								}
							: null
					);
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

	/**
	 * Test-only: simulate a worker crash by force-terminating the worker.
	 * Verifies that the next compile() surfaces the synthetic diagnostic
	 * and the one after respawns and succeeds.
	 */
	function terminateCompileWorker() {
		if (!compileWorker) return;
		compileWorker.terminate();
		// Trip the same path that an unexpected onerror takes — without
		// dispatching an actual ErrorEvent (terminate() doesn't emit one).
		compileWorker = null;
		compileWorkerReady = null;
		compileWorkerDead = true;
		for (const resolve of pendingCompileResolves.values()) {
			resolve({
				success: false,
				diagnostics: [
					{ severity: 2, message: 'Compile worker crashed — restarting on next compile', line: 1, column: 1 }
				]
			});
		}
		pendingCompileResolves.clear();
	}

	function spawnCompileWorker() {
		if (compileWorker) return;
		if (!wasmJsCode || !wasmBinary) {
			console.error('[AudioEngine] Cannot spawn compile worker — WASM not loaded');
			return;
		}

		compileWorker = new Worker(new URL('./compile.worker.ts', import.meta.url), {
			type: 'module'
		});
		compileWorkerDead = false;

		compileWorkerReady = new Promise((resolve, reject) => {
			const w = compileWorker;
			if (!w) {
				reject(new Error('Compile worker null after spawn'));
				return;
			}
			const onReadyMessage = (event: MessageEvent) => {
				const msg = event.data;
				if (msg?.type === 'ready') {
					console.log('[AudioEngine] Compile worker ready');
					w.removeEventListener('message', onReadyMessage);
					resolve();
				} else if (msg?.type === 'initError') {
					console.error('[AudioEngine] Compile worker init failed:', msg.message);
					w.removeEventListener('message', onReadyMessage);
					reject(new Error(String(msg.message)));
				}
			};
			w.addEventListener('message', onReadyMessage);
		});

		compileWorker.addEventListener('message', (event: MessageEvent) => {
			handleCompileWorkerMessage(event.data);
		});

		const handleWorkerCrash = (reason: string) => {
			if (compileWorkerDead) return;
			compileWorkerDead = true;
			console.error('[AudioEngine] Compile worker crashed:', reason);
			compileWorker?.terminate();
			compileWorker = null;
			compileWorkerReady = null;
			// Resolve every outstanding compile with a synthetic diagnostic.
			for (const resolve of pendingCompileResolves.values()) {
				resolve({
					success: false,
					diagnostics: [
						{
							severity: 2,
							message: 'Compile worker crashed — restarting on next compile',
							line: 1,
							column: 1
						}
					]
				});
			}
			pendingCompileResolves.clear();
		};
		compileWorker.addEventListener('error', (e) => {
			handleWorkerCrash(e.message || 'worker error');
		});
		compileWorker.addEventListener('messageerror', () => {
			handleWorkerCrash('worker messageerror');
		});

		compileWorker.postMessage({
			type: 'init',
			jsCode: wasmJsCode,
			wasmBinary: wasmBinary.slice(0)
		});
	}

	function handleCompileWorkerMessage(msg: { type?: string; [key: string]: unknown }) {
		if (msg?.type !== 'compileResult') return;
		const gen = msg.gen as number;
		const resolver = pendingCompileResolves.get(gen);
		if (!resolver) {
			// Stale or unknown gen — drop. Most common cause is the worker
			// crashed and respawned mid-flight; the resolvers were already
			// drained synthetically in handleWorkerCrash.
			return;
		}
		pendingCompileResolves.delete(gen);

		const result: CompileResultInternal = {
			success: msg.success as boolean,
			bytecodeSize: msg.bytecodeSize as number | undefined,
			diagnostics: msg.diagnostics as Diagnostic[] | undefined,
			requiredSamples: msg.requiredSamples as string[] | undefined,
			requiredSamplesExtended: msg.requiredSamplesExtended as
				RequiredSampleExtended[] | undefined,
			requiredSoundfonts: msg.requiredSoundfonts as RequiredSoundFont[] | undefined,
			requiredWavetables: msg.requiredWavetables as RequiredWavetable[] | undefined,
			requiredUris: msg.requiredUris as UriRequest[] | undefined,
			requiredInputSources: msg.requiredInputSources as string[] | undefined,
			requiredMidiSources: msg.requiredMidiSources as RequiredMidiSource[] | undefined,
			requiredMidiCcRoutes: msg.requiredMidiCcRoutes as
				RequiredMidiCcRoute[] | undefined,
			paramDecls: msg.paramDecls as ParamDecl[] | undefined,
			vizDecls: msg.vizDecls as VizDecl[] | undefined,
			disassembly: msg.disassembly as DisassemblyInfo | undefined,
			bytecode: msg.bytecode as Uint8Array | undefined,
			stateInitsBuf: msg.stateInitsBuf as Uint8Array | undefined,
			midiSourcesBuf: msg.midiSourcesBuf as Uint8Array | undefined,
			blockTable: msg.blockTable as Uint8Array | undefined,
			mainInstCount: msg.mainInstCount as number | undefined,
			builtinVarOverrides: msg.builtinVarOverrides as
				Array<{ name: string; value: number }> | undefined
		};
		resolver(result);
	}

	function applyCompileResult(result: CompileResultInternal) {
		if (!result.success) {
			console.error('[AudioEngine] Compilation failed:', result.diagnostics);
			host.onCompileResult(result);
			return;
		}

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

		if (result.builtinVarOverrides) {
			for (const override of result.builtinVarOverrides) {
				if (override.name === 'bpm') {
					bpm = override.value;
					host.onTransport({ bpm: override.value });
				}
			}
		}

		// Shell applies paramDecls / vizDecls / disassembly from this.
		host.onCompileResult(result);

		lastRequiredMidiSources = result.requiredMidiSources ?? [];
		lastRequiredMidiCcRoutes = result.requiredMidiCcRoutes ?? [];
		midiInput.setRoutes(lastRequiredMidiSources, lastRequiredMidiCcRoutes);

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

	async function play() {
		if (!initialized) {
			await initialize();
		}

		if (audioContext?.state === 'suspended') {
			await audioContext.resume();
		}

		host.onTransport({ isPlaying: true });
	}

	async function pause() {
		if (audioContext?.state === 'running') {
			await audioContext.suspend();
		}
		host.onTransport({ isPlaying: false });
	}

	async function stop() {
		workletNode?.port.postMessage({ type: 'reset' });
		await pause();
		host.onTransport({ currentBeat: 0, currentBar: 0 });

		// _cedar_reset() resets audio_arena_, which owns every parsed
		// MidiSequence (prd-midi-input Phase 5). SampleBank /
		// SoundFontRegistry / WavetableRegistry use heap storage and
		// survive reset, so only MIDI needs to be re-uploaded. The bytes
		// still live behind midiBank's native blob URLs from the original
		// drop/fetch — re-fetch and re-push to the worklet so the next
		// compile can resolve the file from the worklet's registry. (The
		// compile-time loadAsset path can't reuse these URLs because the
		// uri-resolver blob handler only recognises blob:nkido: URIs.)
		const names = [...loadedMidiNames];
		loadedMidiNames.clear();
		host.onAssetsCleared('midi');
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

	function disconnectActiveInput() {
		if (activeInput) {
			try {
				activeInput.stop();
			} catch (e) {
				console.warn('[AudioEngine] disconnectActiveInput error', e);
			}
			activeInput = null;
		}
	}

	function teardownAudioGraph() {
		disconnectActiveInput();
		midiInput.setWorkletPort(null);
		if (audioContext) {
			void audioContext.close();
			audioContext = null;
		}
		workletNode = null;
		gainNode = null;
		analyserNode = null;
		// Drop the memoized bootstrap so initialize() actually re-runs
		// instead of resolving instantly off the prior (now-stale) promise.
		initPromise = null;
		initialized = false;
	}

	/**
	 * Restart the audio engine with updated settings (e.g., new sample rate).
	 * The shell stops playback first (it owns `isPlaying`); this method
	 * tears down the transport, lets the shell reset its per-engine state
	 * via onEngineReset, and boots again.
	 */
	async function restart(): Promise<void> {
		console.log('[AudioEngine] Restarting audio with new settings...');

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
		initPromise = null;
		initialized = false;

		inputKind = 'none';
		inputDeviceId = null;
		inputFileName = null;

		// Clear transport-side load tracking (same set the pre-extraction
		// store cleared: samples + soundfonts; MIDI/wavetable maps survive
		// exactly as before).
		sampleLoadState.clear();
		loadedSampleNames.clear();
		pendingSampleLoads.clear();
		pendingSoundFontLoads.clear();
		loadedSoundFonts.clear();

		// Shell resets its reactive per-engine state here, in order,
		// before re-init repopulates it (IndexedDB restore acks).
		host.onEngineReset();

		// Reinitialize
		await initialize();
	}

	function dispose() {
		terminateCompileWorker();
		teardownAudioGraph();
	}

	function setBpm(value: number) {
		bpm = value;
		workletNode?.port.postMessage({ type: 'setBpm', bpm: value });
	}

	function setVolume(value: number) {
		volume = value;
		if (gainNode && audioContext) {
			gainNode.gain.setTargetAtTime(value, audioContext.currentTime, 0.01);
		}
	}

	/**
	 * Ensure an extended sample (with bank context) is loaded
	 * @returns true if sample is loaded, false if loading failed
	 */
	async function ensureBankSampleLoaded(sample: RequiredSampleExtended): Promise<boolean> {
		const { bank, name, variant, qualifiedName } = sample;

		// Already loaded?
		if (loadedSampleNames.has(qualifiedName)) {
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
	 * Compile source code in the worker and load into the worklet's Cedar VM.
	 * Steps:
	 *   1. Route source to the compile worker; await compileResult.
	 *      Supersede-by-newest drops stale results so the user always sees
	 *      the latest source applied.
	 *   2. Apply UI-side metadata (paramDecls, vizDecls, MIDI routes, bpm).
	 *   3. Load required samples / SF2s / wavetables / .mid files.
	 *   4. Post `loadProgram` to the worklet with the four packed buffers
	 *      and await `programLoaded`.
	 */
	async function compile(source: string): Promise<CompileResult> {
		if (!workletNode) {
			return {
				success: false,
				diagnostics: [{ severity: 2, message: 'Worklet not initialized', line: 1, column: 1 }]
			};
		}

		const gen = ++nextCompileGen;
		latestRequestedCompileGen = gen;

		console.log('[AudioEngine] Compile gen', gen, 'length', source.length);

		// Lazy respawn after a worker crash. spawnCompileWorker() awaits WASM
		// fetched in initialize(), so it cannot run before initialize() has
		// resolved — callers are expected to call initialize() first.
		if (compileWorkerDead || !compileWorker) {
			spawnCompileWorker();
		}

		// Boot-time wait: if the worker hasn't reported {ready} yet, every
		// pending compile() call awaits the same promise. Supersede-by-
		// newest is enforced by the gen-vs-latestRequested check below, so
		// the boot stream collapses naturally — only the last gen's result
		// is applied; everything else returns `superseded:true` without
		// touching the UI.
		if (compileWorkerReady) {
			try {
				await compileWorkerReady;
			} catch (err) {
				return {
					success: false,
					diagnostics: [
						{ severity: 2, message: 'Compile worker failed to init: ' + String(err), line: 1, column: 1 }
					]
				};
			}
		}

		if (gen !== latestRequestedCompileGen) {
			return { success: false, superseded: true };
		}

		if (!compileWorker || compileWorkerDead) {
			return {
				success: false,
				diagnostics: [
					{ severity: 2, message: 'Compile worker unavailable', line: 1, column: 1 }
				]
			};
		}

		const worker = compileWorker;
		const compileInternal = await new Promise<CompileResultInternal>((resolve) => {
			pendingCompileResolves.set(gen, resolve);
			worker.postMessage({ type: 'compile', gen, source });
			// 10s safety timeout — covers worker hang / crash. A real compile
			// is ~110ms even on heavy patches.
			setTimeout(() => {
				if (pendingCompileResolves.delete(gen)) {
					resolve({
						success: false,
						diagnostics: [
							{ severity: 2, message: 'Compile worker unresponsive (timeout)', line: 1, column: 1 }
						]
					});
				}
			}, 10000);
		});

		// Supersede-by-newest: discard stale compiles silently. The store
		// treats {superseded:true} as a no-op.
		if (gen !== latestRequestedCompileGen) {
			return { success: false, superseded: true };
		}

		applyCompileResult(compileInternal);

		const compileResult: CompileResult = compileInternal;
		if (!compileResult.success) {
			return compileResult;
		}

		// Step 2: Load any required samples and soundfonts (await ALL before proceeding)
		host.onStatus({ isLoadingSamples: true });
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

			// Register the Tidal Drum Machines catalog before resolving
			// samples, so `.bank("<Machine>")` references find a known bank.
			// Only pay the fetch when the program actually uses a named bank.
			if (extendedSamples.some((s) => s.bank && s.bank !== 'default')) {
				await ensureCatalogLoaded();
			}

			// A sample that fails to load is a hard error for user banks and
			// the default kit, but a *soft* failure for the built-in
			// drum-machine catalog: a missing catalog sample (offline, 404,
			// GitHub down) warns and leaves that one voice silent — the rest
			// of the program keeps running.
			const missingSamples: string[] = [];
			const missingCatalog: string[] = [];
			for (const sample of extendedSamples) {
				const loaded = await ensureBankSampleLoaded(sample);
				if (!loaded) {
					const displayName = sample.bank
						? `${sample.bank}/${sample.name}:${sample.variant}`
						: sample.name;
					if (sample.bank && hasCatalogBank(sample.bank)) {
						missingCatalog.push(displayName);
					} else {
						missingSamples.push(displayName);
					}
				}
			}

			if (missingCatalog.length > 0) {
				console.warn(
					`[AudioEngine] ${missingCatalog.length} drum-machine sample(s) ` +
						`could not be loaded — those voices are silent:`,
					missingCatalog
				);
			}

			// If any non-catalog samples couldn't be loaded, report as error
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
				if (loadedSoundFonts.has(sf.filename)) continue;

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
				if (loadedMidiNames.has(src.name)) continue;
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
			host.onStatus({ isLoadingSamples: false });
		}

		// Step 3: Load the compiled program. The worklet retries SlotBusy from
		// process() each block, so we just wait for the tagged response.
		const node = workletNode; // Capture for closure (TypeScript null-check)
		const refreshId = ++loadRefreshCounter;

		const bytecode = compileInternal.bytecode ?? new Uint8Array(0);
		const stateInitsBuf = compileInternal.stateInitsBuf ?? new Uint8Array(0);
		const midiSourcesBuf = compileInternal.midiSourcesBuf ?? new Uint8Array(0);
		const blockTable = compileInternal.blockTable ?? new Uint8Array(0);

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
			node.port.postMessage(
				{
					type: 'loadProgram',
					refreshId,
					bytecode,
					stateInitsBuf,
					midiSourcesBuf,
					blockTable,
					mainInstCount: compileInternal.mainInstCount ?? 0,
					builtinVarOverrides: compileInternal.builtinVarOverrides ?? []
				},
				[bytecode.buffer, stateInitsBuf.buffer, midiSourcesBuf.buffer, blockTable.buffer]
			);
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

	// Forget*: drop transport-side load tracking so the next compile that
	// references the name re-fetches it. The engine keeps the asset in WASM
	// heap until reload (`cedar_remove_sample` doesn't exist yet); the shell
	// owns the registry row + persistence manifest.
	function forgetSample(name: string) {
		// Deliberately leaves `sampleLoadState` at 'loaded' — the sample is
		// still resident in the worklet, so the next compile that wants it
		// doesn't re-fetch (pre-extraction behavior).
		loadedSampleNames.delete(name);
	}

	function forgetSoundFont(name: string) {
		loadedSoundFonts.delete(name);
	}

	function forgetMidiFile(name: string) {
		loadedMidiNames.delete(name);
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
		if (!initialized) {
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
		if (!initialized) {
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

	/**
	 * Switch the live audio input source. Pass {kind:'none'} to disconnect.
	 * UI surfaces granted/denied/etc via the pushed input status.
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
			host.onStatus({
				inputError: lastError ?? 'Audio not initialized',
				inputStatus: 'error'
			});
			return;
		}

		// Always tear down the previous source first to avoid summing two streams.
		disconnectActiveInput();

		inputKind = config.kind;
		inputDeviceId = config.deviceId ?? null;
		inputFileName = config.fileName ?? null;
		if (config.constraints) inputConstraints = { ...config.constraints };
		host.onStatus({
			inputKind,
			inputDeviceId,
			inputFileName,
			inputConstraints,
			inputError: null
		});

		if (config.kind === 'none') {
			host.onStatus({ inputStatus: 'idle' });
			return;
		}

		host.onStatus({ inputStatus: 'connecting' });
		try {
			let acquired: ActiveInputSource;
			if (config.kind === 'mic') {
				acquired = await acquireMicSource(
					audioContext,
					config.deviceId,
					config.constraints ?? inputConstraints
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
			host.onStatus({ inputStatus: 'active' });

			// Forward source string to WASM for any compile-time consumers
			// (currently informational; future per-call overrides will use it).
			const sourceStr = config.kind === 'mic' ? 'mic'
				: config.kind === 'tab' ? 'tab'
				: config.kind === 'file' ? `file:${config.fileName ?? ''}`
				: '';
			workletNode.port.postMessage({ type: 'setInputSource', source: sourceStr });
		} catch (err) {
			console.warn('[AudioEngine] Failed to acquire input source:', err);
			const message = err instanceof Error ? err.message : String(err);
			// Map common DOMException names onto the user-visible status.
			const name = (err as { name?: string })?.name ?? '';
			let inputStatus: 'denied' | 'unavailable' | 'error';
			if (name === 'NotAllowedError' || name === 'PermissionDeniedError') {
				inputStatus = 'denied';
			} else if (name === 'NotFoundError' || name === 'OverconstrainedError') {
				inputStatus = 'unavailable';
			} else {
				inputStatus = 'error';
			}
			inputKind = 'none';
			host.onStatus({ inputError: message, inputStatus, inputKind: 'none' });
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
		if (inputKind === 'file' && inputFileName === name) {
			void setInputSource({ kind: 'none' });
		}
	}

	function getInputFileNames(): string[] {
		return Array.from(inputFileBuffers.keys()).sort();
	}

	function setInputConstraints(c: Partial<InputConstraints>) {
		const next = { ...inputConstraints, ...c };
		inputConstraints = next;
		host.onStatus({ inputConstraints: next });
		// If a mic source is active, re-acquire so the new constraints take effect.
		if (inputKind === 'mic' && activeInput) {
			void setInputSource({
				kind: 'mic',
				deviceId: inputDeviceId ?? undefined,
				constraints: next
			});
		}
	}

	return {
		initialize,
		restart,
		dispose,
		play,
		pause,
		stop,
		setBpm,
		setVolume,
		compile,
		setParam,
		loadSample,
		loadSampleFromBytes,
		loadSoundFont,
		loadMidiFile,
		loadWavetable,
		loadBank,
		loadAsset,
		clearSamples,
		clearWavetables,
		forgetSample,
		forgetSoundFont,
		forgetMidiFile,
		getBuiltins,
		getShapeIndex,
		getPatternInfo,
		queryPatternPreview,
		getCurrentBeatPosition,
		getActiveSteps,
		inspectState,
		getPatternDebug,
		getProbeData,
		getFFTProbeData,
		setInputSource,
		setInputConstraints,
		listInputDevices,
		registerInputFile,
		unregisterInputFile,
		getInputFileNames,
		getMidiController: () => midiInput,
		setDefaultMidiDevice: (name: string) => midiInput.setDefaultDeviceName(name),
		ensureMidiAccess: () => midiInput.ensureAccess(),
		getAnalyserNode,
		getAudioContext,
		getTimeDomainData,
		getFrequencyData,
		terminateCompileWorker
	} satisfies AudioBackend;
}
