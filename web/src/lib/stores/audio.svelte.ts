/**
 * Audio engine state store using Svelte 5 runes
 *
 * Wraps the Cedar AudioWorklet-based engine with reactive state.
 */

import { DEFAULT_DRUM_KIT } from '$lib/audio/default-samples';

interface Diagnostic {
	severity: number;
	message: string;
	line: number;
	column: number;
}

interface CompileResult {
	success: boolean;
	bytecodeSize?: number;
	diagnostics?: Diagnostic[];
	requiredSamples?: string[];
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
		samplesLoading: false
	});

	let audioContext: AudioContext | null = null;
	let workletNode: AudioWorkletNode | null = null;
	let gainNode: GainNode | null = null;
	let analyserNode: AnalyserNode | null = null;
	let wasmJsCode: string | null = null;
	let wasmBinary: ArrayBuffer | null = null;

	// Compile result callback (resolved when worklet responds)
	let compileResolve: ((result: CompileResult) => void) | null = null;

	// Track sample loading state: 'pending' | 'loading' | 'loaded' | 'error'
	const sampleLoadState = new Map<string, 'pending' | 'loading' | 'loaded' | 'error'>();
	// Track loaded sample names
	const loadedSamples = new Set<string>();

	async function initialize() {
		if (state.isInitialized || state.isLoading) return;

		state.isLoading = true;
		state.error = null;

		try {
			// Create AudioContext
			audioContext = new AudioContext({
				sampleRate: 48000,
				latencyHint: 'interactive'
			});

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
					requiredSamples: msg.requiredSamples as string[] | undefined
				};
				if (result.success) {
					console.log(
						'[AudioEngine] Compiled successfully, bytecode size:',
						result.bytecodeSize,
						'required samples:',
						result.requiredSamples
					);
				} else {
					console.error('[AudioEngine] Compilation failed:', result.diagnostics);
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
				break;
			}
			case 'error':
				state.error = String(msg.message);
				console.error('[AudioEngine] Worklet error:', msg.message);
				break;
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
				if (success) {
					sampleLoadState.set(name, 'loaded');
					loadedSamples.add(name);
					return true;
				} else {
					sampleLoadState.set(name, 'error');
					return false;
				}
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

		// Step 3: Load the compiled program (samples are ready)
		workletNode.port.postMessage({ type: 'loadCompiledProgram' });

		return compileResult;
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

			// Send WAV data to worklet
			workletNode.port.postMessage({
				type: 'loadSampleWav',
				name,
				wavData: arrayBuffer
			});

			return true;
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

		initialize,
		play,
		pause,
		stop,
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
		clearSamples
	};
}

export const audioEngine = createAudioEngine();
