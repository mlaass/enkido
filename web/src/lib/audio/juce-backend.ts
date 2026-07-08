/**
 * `JuceAudioBackend` — native adapter behind the `AudioBackend` interface
 * (prd-web-audio-backend.md §5.3), speaking nkido-studio bridge protocol v1
 * over `window.__JUCE__`.
 *
 * Only constructed when the bridge exists (backend-select), so it is inert
 * on the plain-browser site. The native Cedar engine owns DSP and the host
 * owns the clock: transport calls are *requests*; truth arrives as pushed
 * bridge events (`playhead`) and lands in the shell via `AudioBackendHost`.
 *
 * Pull-model viz (PRD §1: "viz stays pull/poll in v1") is answered from
 * the last pushed bridge frame: the newest `meters` batch, the newest
 * scope frame (fetched from the ResourceProvider on the `scopeReady`
 * doorbell), and the latest `playhead`. Polls before the first frame
 * resolve empty/null — never throw (PRD §9).
 *
 * Bridge v1 surface is narrower than the interface (protocol growth is
 * owned by bridge-protocol.md, PRD §3.2). Guaranteed v1 functions:
 * `compile`, `getBuiltins`, `inspectState`, `setParam`, `deviceStatus`.
 * Everything else is attempted via `callNativeFunction` and degrades
 * gracefully when the host doesn't know it yet (§6 forward-compat:
 * unknown function → promise rejects → mapped to a failure value here).
 */

import type {
	AudioBackend,
	AudioBackendHost,
	BuiltinsData,
	CompileResult,
	Diagnostic,
	FFTProbeData,
	PatternDebugInfo,
	PatternEvent,
	PatternInfo,
	ShapeIndexData,
	SoundFontInfo,
	StateInspection
} from './audio-backend';
import type { InputConstraints, InputSourceConfig } from './input-source';

export const BRIDGE_PROTOCOL_VERSION = 1;

/** The subset of JUCE's WebView native-integration API the adapter uses
 *  (bridge-protocol.md §2/§3). */
export interface JuceBridge {
	callNativeFunction(name: string, args: unknown[]): Promise<unknown>;
	addEventListener(id: string, fn: (payload: unknown) => void): unknown;
	emitEvent(id: string, payload: unknown): void;
}

declare global {
	interface Window {
		__JUCE__?: { backend?: JuceBridge };
	}
}

interface BridgeDiagnostic {
	severity: number;
	message: string;
	line: number;
	col: number;
}

interface BridgeCompileResult {
	generation: number;
	ok: boolean;
	diagnostics?: BridgeDiagnostic[];
}

interface PlayheadFrame {
	beat: number;
	bar: number;
}

interface ScopeFrame {
	seq: number;
	samples: number;
	sampleRate: number;
	/** Interleaved L,R float32, length = 2 × samples. */
	data: Float32Array;
}

export function createJuceAudioBackend(
	host: AudioBackendHost,
	bridge?: JuceBridge
): AudioBackend {
	const resolved = bridge ?? window.__JUCE__?.backend;
	if (!resolved) {
		throw new Error('[JuceBackend] constructed without a __JUCE__ bridge');
	}
	const juce: JuceBridge = resolved;

	let initialized = false;
	// Readiness gate (bridge-protocol §1 / PRD §9): every native call waits
	// for the page→native `ready` emit so nothing reaches an uninitialised
	// bridge. initialize() resolves it.
	let readyResolve!: () => void;
	const ready = new Promise<void>((resolve) => (readyResolve = resolve));

	// Last pushed bridge frames (PRD §5.3) — answer pull-model polls.
	let latestPlayhead: PlayheadFrame | null = null;
	let latestMeters: unknown = null;
	let latestScope: ScopeFrame | null = null;
	let scopeFetchInFlight = false;

	// Outstanding compile() promises, newest last. The host compiles
	// supersede-by-newest, so a `compileResult` event answers the newest
	// submit; every older pending one resolves `superseded`.
	const pendingCompiles: Array<(r: CompileResult) => void> = [];

	// Input-file bookkeeping kept for interface parity; there is no native
	// input bus route for these in bridge v1.
	const inputFileBuffers = new Map<string, ArrayBuffer>();

	/**
	 * Call a native function after the readiness handshake; unknown
	 * functions reject (bridge-protocol §6) and resolve `undefined` here so
	 * v1 hosts and newer hosts are both fine.
	 */
	async function tryNative(name: string, ...args: unknown[]): Promise<unknown> {
		await ready;
		try {
			return await juce.callNativeFunction(name, args);
		} catch {
			return undefined;
		}
	}

	function mapDiagnostics(diags: BridgeDiagnostic[] | undefined): Diagnostic[] {
		return (diags ?? []).map((d) => ({
			severity: d.severity,
			message: d.message,
			line: d.line,
			column: d.col
		}));
	}

	function handleCompileResult(payload: unknown) {
		const r = payload as BridgeCompileResult;
		const result: CompileResult = {
			success: !!r.ok,
			diagnostics: mapDiagnostics(r.diagnostics)
		};
		// paramDecls / vizDecls / disassembly are not in bridge v1; the
		// shell applies the (empty) metadata uniformly.
		host.onCompileResult(result);
		const newest = pendingCompiles.pop();
		for (const stale of pendingCompiles.splice(0)) {
			stale({ success: false, superseded: true });
		}
		newest?.(result);
	}

	function handlePlayhead(payload: unknown) {
		const p = payload as PlayheadFrame;
		latestPlayhead = p;
		host.onTransport({ currentBeat: p.beat, currentBar: p.bar });
	}

	function handleScopeReady(payload: unknown) {
		const p = payload as { seq: number; samples: number; sampleRate: number };
		// Doorbell (bridge-protocol §4): fetch the binary frame from the
		// ResourceProvider. Double-buffered natively, so a fetch always sees
		// a complete frame; drop doorbells that ring mid-fetch.
		if (scopeFetchInFlight) return;
		scopeFetchInFlight = true;
		fetch('scope.bin')
			.then((resp) => (resp.ok ? resp.arrayBuffer() : Promise.reject(resp.status)))
			.then((buf) => {
				latestScope = {
					seq: p.seq,
					samples: p.samples,
					sampleRate: p.sampleRate,
					data: new Float32Array(buf)
				};
			})
			.catch((err) => console.warn('[JuceBackend] scope.bin fetch failed:', err))
			.finally(() => {
				scopeFetchInFlight = false;
			});
	}

	async function initialize(): Promise<void> {
		if (initialized) return;

		juce.addEventListener('compileResult', handleCompileResult);
		juce.addEventListener('playhead', handlePlayhead);
		juce.addEventListener('meters', (payload) => {
			latestMeters = payload;
		});
		juce.addEventListener('scopeReady', handleScopeReady);

		// Readiness handshake (bridge-protocol §1): the page confirms the
		// bridge exists and tells the native side it may start emitting.
		juce.emitEvent('ready', { protocolVersion: BRIDGE_PROTOCOL_VERSION });
		initialized = true;
		readyResolve();
		host.onStatus({ isInitialized: true, isLoading: false });

		// Surface the device sample rate in the status bar if the host
		// answers (v1 guaranteed, but tolerate absence).
		const status = (await tryNative('deviceStatus')) as { sampleRate?: number } | undefined;
		if (status?.sampleRate) {
			host.onStatus({ activeSampleRate: status.sampleRate });
		}
	}

	async function compile(source: string): Promise<CompileResult> {
		await ready;
		return new Promise<CompileResult>((resolve) => {
			pendingCompiles.push(resolve);
			juce
				.callNativeFunction('compile', [source])
				.catch((err) => {
					const idx = pendingCompiles.indexOf(resolve);
					if (idx !== -1) {
						pendingCompiles.splice(idx, 1);
						resolve({
							success: false,
							diagnostics: [
								{ severity: 2, message: `Compile submit failed: ${err}`, line: 1, column: 1 }
							]
						});
					}
				});
			// Same 10s safety net as the WASM path — covers a host that
			// never fires `compileResult`.
			setTimeout(() => {
				const idx = pendingCompiles.indexOf(resolve);
				if (idx !== -1) {
					pendingCompiles.splice(idx, 1);
					resolve({
						success: false,
						diagnostics: [
							{ severity: 2, message: 'Native compile unresponsive (timeout)', line: 1, column: 1 }
						]
					});
				}
			}, 10000);
		});
	}

	// Transport requests. Bridge v1 defines no transport functions yet (the
	// studio/DAW owns the clock); attempt them for forward-compat — an old
	// host rejects the unknown name and the request stays advisory. Truth
	// arrives via the `playhead` stream either way (PRD §5.4 inversion).
	async function play() {
		await initialize();
		await tryNative('play');
		host.onTransport({ isPlaying: true });
	}

	async function pause() {
		await tryNative('pause');
		host.onTransport({ isPlaying: false });
	}

	async function stop() {
		await tryNative('stop');
		host.onTransport({ isPlaying: false, currentBeat: 0, currentBar: 0 });
	}

	function setBpm(bpm: number) {
		void tryNative('setBpm', bpm);
	}

	function setVolume(volume: number) {
		void tryNative('setVolume', volume);
	}

	function setParam(name: string, value: number, slewMs?: number) {
		// v1 guaranteed native fn. Production widgets may additionally bind
		// JUCE relays (bridge-protocol §5); the direct call is always valid.
		void tryNative('setParam', name, value, ...(slewMs !== undefined ? [slewMs] : []));
	}

	async function getBuiltins(): Promise<BuiltinsData | null> {
		const json = await tryNative('getBuiltins');
		if (typeof json !== 'string') return null;
		try {
			return JSON.parse(json) as BuiltinsData;
		} catch {
			return null;
		}
	}

	async function inspectState(stateId: number): Promise<StateInspection | null> {
		const json = await tryNative('inspectState', stateId);
		if (typeof json !== 'string') return null;
		try {
			return JSON.parse(json) as StateInspection;
		} catch {
			return null;
		}
	}

	// ------------------------------------------------------------------
	// Pull-model viz served from buffered bridge frames (PRD §5.3, §9)
	// ------------------------------------------------------------------

	async function getCurrentBeatPosition(): Promise<number> {
		return latestPlayhead?.beat ?? 0;
	}

	async function getProbeData(): Promise<Float32Array | null> {
		// ponytail: v1 has one master scope stream, no per-stateId probes —
		// every probe poll gets the left channel of the latest scope frame.
		// Per-bus probe paths are documented bridge growth (§4).
		if (!latestScope) return null;
		const { samples, data } = latestScope;
		const mono = new Float32Array(samples);
		for (let i = 0; i < samples; i++) mono[i] = data[i * 2];
		return mono;
	}

	async function getFFTProbeData(): Promise<FFTProbeData | null> {
		// No FFT-magnitude frame in bridge v1 (documented growth path).
		return null;
	}

	// ------------------------------------------------------------------
	// Not-in-v1 surface: resolve to well-formed empty values, never throw.
	// ------------------------------------------------------------------

	async function getShapeIndex(): Promise<ShapeIndexData | null> {
		return null;
	}
	async function getPatternInfo(): Promise<PatternInfo[]> {
		return [];
	}
	async function queryPatternPreview(): Promise<PatternEvent[]> {
		return [];
	}
	async function getActiveSteps(): Promise<Record<number, { offset: number; length: number }>> {
		return {};
	}
	async function getPatternDebug(): Promise<PatternDebugInfo | null> {
		return null;
	}

	// Assets are resolved natively by the engine resolver (PRD §5.4); the
	// page-side loaders report failure without throwing until the bridge
	// grows asset functions.
	async function loadSample(): Promise<void> {}
	async function loadSampleFromBytes(): Promise<boolean> {
		return false;
	}
	async function loadSoundFont(): Promise<SoundFontInfo | null> {
		return null;
	}
	async function loadMidiFile(): Promise<boolean> {
		return false;
	}
	async function loadWavetable(): Promise<number> {
		return -1;
	}
	async function loadBank(): Promise<boolean> {
		return false;
	}
	function loadAsset(uri: string, kind: 'sample', name: string, origin?: 'builtin' | 'user'): Promise<boolean>;
	function loadAsset(uri: string, kind: 'soundfont', name: string, origin?: 'builtin' | 'user'): Promise<SoundFontInfo | null>;
	function loadAsset(uri: string, kind: 'wavetable', name: string): Promise<number>;
	function loadAsset(uri: string, kind: 'sample_bank', name?: string): Promise<boolean>;
	function loadAsset(uri: string, kind: 'midi', name: string): Promise<boolean>;
	async function loadAsset(
		_uri: string,
		kind: 'sample' | 'soundfont' | 'wavetable' | 'sample_bank' | 'midi'
	): Promise<boolean | SoundFontInfo | null | number> {
		switch (kind) {
			case 'soundfont':
				return null;
			case 'wavetable':
				return -1;
			default:
				return false;
		}
	}

	return {
		initialize,
		// Native engine lifetime is host-owned; restart just re-runs the
		// (idempotent) handshake after the shell reset.
		restart: async () => {
			host.onEngineReset();
			await initialize();
		},
		dispose: () => {},
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
		clearSamples: () => {},
		clearWavetables: () => {},
		forgetSample: () => {},
		forgetSoundFont: () => {},
		forgetMidiFile: () => {},
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
		// MIDI / live input: DAW/host-owned natively (PRD §5.4); Web MIDI
		// and getUserMedia don't exist in the WebView contract.
		setInputSource: async (_config: InputSourceConfig) => {
			host.onStatus({ inputStatus: 'unavailable', inputKind: 'none' });
		},
		setInputConstraints: (_c: Partial<InputConstraints>) => {},
		listInputDevices: async () => [],
		registerInputFile: (name: string, data: ArrayBuffer) => {
			inputFileBuffers.set(name, data);
			return name;
		},
		unregisterInputFile: (name: string) => {
			inputFileBuffers.delete(name);
		},
		getInputFileNames: () => Array.from(inputFileBuffers.keys()).sort(),
		getMidiController: () => null,
		setDefaultMidiDevice: () => {},
		ensureMidiAccess: async () => false,
		// Web-only escape hatches (PRD §4: null / no-op on native; only
		// test-hooks.ts consumes them).
		getAnalyserNode: () => null,
		getAudioContext: () => null,
		getTimeDomainData: () => new Uint8Array(0),
		getFrequencyData: () => new Uint8Array(0),
		terminateCompileWorker: () => {}
	} satisfies AudioBackend;
}
