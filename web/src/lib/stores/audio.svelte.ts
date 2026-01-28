/**
 * Audio engine state store using Svelte 5 runes
 *
 * Wraps the Cedar AudioWorklet-based engine with reactive state.
 */

import { DEFAULT_DRUM_KIT } from '$lib/audio/default-samples';
import { settingsStore } from './settings.svelte';

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

interface CompileResult {
	success: boolean;
	bytecodeSize?: number;
	diagnostics?: Diagnostic[];
	requiredSamples?: string[];
	paramDecls?: ParamDecl[];
	disassembly?: DisassemblyInfo;
}

// Builtins metadata from the compiler
interface BuiltinParam {
	name: string;
	required: boolean;
	default?: number;
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

export type { BuiltinsData, BuiltinInfo, BuiltinParam };

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
	params: ParamDecl[];
	paramValues: Map<string, number>;
	disassembly: DisassemblyInfo | null;
	activeSampleRate: number | null;
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
		params: [],
		paramValues: new Map(),
		disassembly: null,
		activeSampleRate: null
	});

	let audioContext: AudioContext | null = null;
	let workletNode: AudioWorkletNode | null = null;
	let gainNode: GainNode | null = null;
	let analyserNode: AnalyserNode | null = null;
	let wasmJsCode: string | null = null;
	let wasmBinary: ArrayBuffer | null = null;

	// Compile result callback (resolved when worklet responds)
	let compileResolve: ((result: CompileResult) => void) | null = null;

	// Builtins metadata cache
	let builtinsCache: BuiltinsData | null = null;
	let builtinsResolve: ((data: BuiltinsData | null) => void) | null = null;

	// Pattern highlighting resolve functions
	let patternInfoResolve: ((patterns: PatternInfo[]) => void) | null = null;
	let patternPreviewResolve: ((events: PatternEvent[]) => void) | null = null;
	let beatPositionResolve: ((position: number) => void) | null = null;
	let activeStepsResolve: ((steps: Record<number, { offset: number; length: number }>) => void) | null = null;

	// Track sample loading state: 'pending' | 'loading' | 'loaded' | 'error'
	const sampleLoadState = new Map<string, 'pending' | 'loading' | 'loaded' | 'error'>();
	// Track loaded sample names
	const loadedSamples = new Set<string>();
	// Pending sample load promises (for waiting on worklet confirmation)
	const pendingSampleLoads = new Map<string, { resolve: (success: boolean) => void }>();

	async function initialize() {
		if (state.isInitialized || state.isLoading) return;

		state.isLoading = true;
		state.error = null;

		try {
			// Create AudioContext with sample rate from settings
			audioContext = new AudioContext({
				sampleRate: settingsStore.sampleRate,
				latencyHint: 'interactive'
			});
			state.activeSampleRate = audioContext.sampleRate;

			// Create gain node
			gainNode = audioContext.createGain();
			gainNode.gain.value = state.volume;

			// Create analyser for visualizations
			analyserNode = audioContext.createAnalyser();
			analyserNode.fftSize = 2048;
			analyserNode.smoothingTimeConstant = 0.8;

			// Pre-fetch WASM JS code and binary in parallel
			console.log('[AudioEngine] Fetching WASM module...');
			const [jsResponse, wasmResponse] = await Promise.all([
				fetch('/wasm/enkido.js'),
				fetch('/wasm/enkido.wasm')
			]);
			wasmJsCode = await jsResponse.text();
			wasmBinary = await wasmResponse.arrayBuffer();
			console.log('[AudioEngine] WASM fetched:', wasmJsCode.length, 'bytes JS,', wasmBinary.byteLength, 'bytes WASM');

			// Load AudioWorklet processor
			await audioContext.audioWorklet.addModule('/worklet/cedar-processor.js');

			// Create worklet node
			workletNode = new AudioWorkletNode(audioContext, 'cedar-processor', {
				numberOfInputs: 0,
				numberOfOutputs: 1,
				outputChannelCount: [2]
			});

			// Handle messages from worklet
			workletNode.port.onmessage = (event) => {
				handleWorkletMessage(event.data);
			};

			// Connect: worklet -> gain -> analyser -> destination
			workletNode.connect(gainNode);
			gainNode.connect(analyserNode);
			analyserNode.connect(audioContext.destination);

			state.isInitialized = true;
			state.isLoading = false;
			console.log('[AudioEngine] Initialized with AudioWorklet');
		} catch (err) {
			console.error('[AudioEngine] Failed to initialize:', err);
			state.error = err instanceof Error ? err.message : String(err);
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
				// Load default samples
				loadDefaultSamples();
				break;
			case 'compiled': {
				// Compilation result from worklet
				const result: CompileResult = {
					success: msg.success as boolean,
					bytecodeSize: msg.bytecodeSize as number | undefined,
					diagnostics: msg.diagnostics as Diagnostic[] | undefined,
					requiredSamples: msg.requiredSamples as string[] | undefined,
					paramDecls: msg.paramDecls as ParamDecl[] | undefined,
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
					// Store disassembly for debug panel
					state.disassembly = result.disassembly ?? null;
				} else {
					console.error('[AudioEngine] Compilation failed:', result.diagnostics);
					state.disassembly = null;
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
				loadedSamples.add(name);
				sampleLoadState.set(name, 'loaded');
				console.log('[AudioEngine] Sample loaded:', name);
				// Resolve any pending load promise
				const pending = pendingSampleLoads.get(name);
				if (pending) {
					pending.resolve(true);
					pendingSampleLoads.delete(name);
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
				if (beatPositionResolve) {
					beatPositionResolve(msg.position as number);
					beatPositionResolve = null;
				}
				break;
			}
			case 'activeSteps': {
				if (activeStepsResolve) {
					activeStepsResolve(msg.steps as Record<number, { offset: number; length: number }>);
					activeStepsResolve = null;
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

		// Close existing audio context
		if (audioContext) {
			await audioContext.close();
			audioContext = null;
		}

		// Clear references
		workletNode = null;
		gainNode = null;
		analyserNode = null;

		// Reset state
		state.isInitialized = false;
		state.isLoading = false;
		state.hasProgram = false;
		state.samplesLoaded = false;
		state.samplesLoading = false;
		state.activeSampleRate = null;
		state.params = [];
		state.paramValues = new Map();
		state.disassembly = null;

		// Clear sample tracking
		sampleLoadState.clear();
		loadedSamples.clear();
		pendingSampleLoads.clear();

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
		if (loadedSamples.has(name)) {
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
				const success = await loadSampleFromUrl(name, defaultSample.url);
				// Note: loadSampleFromUrl now waits for worklet confirmation
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

		// Step 2: Load any required samples that aren't loaded yet
		const requiredSamples = compileResult.requiredSamples || [];
		const missingSamples: string[] = [];

		for (const name of requiredSamples) {
			const loaded = await ensureSampleLoaded(name);
			if (!loaded) {
				missingSamples.push(name);
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

		// Step 3: Load the compiled program with retry on SlotBusy
		const maxRetries = 5;
		const retryDelayMs = 20; // ~7.5 blocks (crossfade is typically 3-4 blocks)
		const node = workletNode; // Capture for closure (TypeScript null-check)

		for (let attempt = 0; attempt < maxRetries; attempt++) {
			const loadResult = await new Promise<{ success: boolean; error?: string }>((resolve) => {
				const timeout = setTimeout(() => resolve({ success: false, error: 'Load timeout' }), 1000);

				const handler = (event: MessageEvent) => {
					if (event.data.type === 'programLoaded') {
						clearTimeout(timeout);
						node.port.removeEventListener('message', handler);
						resolve({ success: true });
					} else if (event.data.type === 'error' && event.data.message?.includes('busy')) {
						clearTimeout(timeout);
						node.port.removeEventListener('message', handler);
						resolve({ success: false, error: 'SlotBusy' });
					} else if (event.data.type === 'error') {
						clearTimeout(timeout);
						node.port.removeEventListener('message', handler);
						resolve({ success: false, error: event.data.message });
					}
				};
				node.port.addEventListener('message', handler);
				node.port.postMessage({ type: 'loadCompiledProgram' });
			});

			if (loadResult.success) {
				return compileResult;
			}

			if (loadResult.error === 'SlotBusy' && attempt < maxRetries - 1) {
				console.log(`[AudioEngine] Slot busy, retrying load (attempt ${attempt + 2}/${maxRetries})...`);
				await new Promise((r) => setTimeout(r, retryDelayMs));
				continue;
			}

			// Non-retryable error or exhausted retries
			console.error('[AudioEngine] Load failed:', loadResult.error);
			return {
				success: false,
				diagnostics: [{ severity: 2, message: loadResult.error || 'Load failed', line: 1, column: 1 }]
			};
		}

		// Should not reach here, but just in case
		return {
			success: false,
			diagnostics: [{ severity: 2, message: 'Load failed after retries', line: 1, column: 1 }]
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
	function loadSample(name: string, audioData: Float32Array, channels: number, sampleRate: number) {
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load sample - worklet not initialized');
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
	 * Load a sample from a WAV file
	 * @param name Sample name (e.g., "kick", "snare")
	 * @param file File object or Blob containing WAV data
	 */
	async function loadSampleFromFile(name: string, file: File | Blob): Promise<boolean> {
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load sample - worklet not initialized');
			return false;
		}

		try {
			const arrayBuffer = await file.arrayBuffer();
			console.log('[AudioEngine] Loading WAV sample:', name, 'size:', arrayBuffer.byteLength);

			// Send WAV data to worklet
			workletNode.port.postMessage({
				type: 'loadSampleWav',
				name,
				wavData: arrayBuffer
			});

			return true;
		} catch (err) {
			console.error('[AudioEngine] Failed to load sample from file:', err);
			return false;
		}
	}

	/**
	 * Load a sample from a URL
	 * @param name Sample name (e.g., "kick", "snare")
	 * @param url URL to WAV file
	 */
	async function loadSampleFromUrl(name: string, url: string): Promise<boolean> {
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load sample - worklet not initialized');
			return false;
		}

		try {
			console.log('[AudioEngine] Fetching sample from URL:', url);
			const response = await fetch(url);
			if (!response.ok) {
				throw new Error(`HTTP ${response.status}: ${response.statusText}`);
			}

			const arrayBuffer = await response.arrayBuffer();
			console.log('[AudioEngine] Loaded sample from URL:', name, 'size:', arrayBuffer.byteLength);

			// Create a promise that will be resolved when worklet confirms load
			const loadPromise = new Promise<boolean>((resolve) => {
				pendingSampleLoads.set(name, { resolve });
				// Timeout after 10 seconds
				setTimeout(() => {
					if (pendingSampleLoads.has(name)) {
						console.error('[AudioEngine] Sample load timeout:', name);
						pendingSampleLoads.delete(name);
						resolve(false);
					}
				}, 10000);
			});

			// Send WAV data to worklet
			workletNode.port.postMessage({
				type: 'loadSampleWav',
				name,
				wavData: arrayBuffer
			});

			// Wait for worklet to confirm sample is loaded
			return await loadPromise;
		} catch (err) {
			console.error('[AudioEngine] Failed to load sample from URL:', err);
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
			const success = await loadSampleFromUrl(sample.name, sample.url);
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
						const success = await loadSampleFromUrl(sample.name, sample.url);
						if (success) {
							sampleLoadState.set(sample.name, 'loaded');
							loadedSamples.add(sample.name);
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
		workletNode?.port.postMessage({ type: 'setParam', name, value: 1 });
	}

	/**
	 * Release a button (set to 0)
	 */
	function releaseButton(name: string) {
		state.paramValues.set(name, 0);
		workletNode?.port.postMessage({ type: 'setParam', name, value: 0 });
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
			beatPositionResolve = resolve;
			setTimeout(() => {
				if (beatPositionResolve === resolve) {
					beatPositionResolve = null;
					resolve(0);
				}
			}, 100);
			workletNode!.port.postMessage({ type: 'getCurrentBeatPosition' });
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
		// Parameter exposure
		get params() { return state.params; },
		get paramValues() { return state.paramValues; },
		// Debug info
		get disassembly() { return state.disassembly; },
		// Audio config
		get activeSampleRate() { return state.activeSampleRate; },

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
		loadSampleFromUrl,
		loadSamplePack,
		clearSamples,
		getBuiltins,
		// Parameter exposure API
		setParamValue,
		getParamValue,
		pressButton,
		releaseButton,
		toggleParam,
		resetParam,
		// Pattern highlighting API
		getPatternInfo,
		queryPatternPreview,
		getCurrentBeatPosition,
		getActiveSteps
	};
}

export const audioEngine = createAudioEngine();
