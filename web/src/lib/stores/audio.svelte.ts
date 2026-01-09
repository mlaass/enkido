/**
 * Audio engine state store using Svelte 5 runes
 *
 * Wraps the Cedar AudioWorklet-based engine with reactive state.
 */

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
		error: null
	});

	let audioContext: AudioContext | null = null;
	let workletNode: AudioWorkletNode | null = null;
	let gainNode: GainNode | null = null;
	let analyserNode: AnalyserNode | null = null;

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

			// Set initial BPM
			workletNode.port.postMessage({ type: 'setBpm', bpm: state.bpm });

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
			case 'initialized':
				console.log('[AudioEngine] Worklet initialized');
				break;
			case 'programLoaded':
				state.hasProgram = true;
				console.log('[AudioEngine] Program loaded');
				break;
			case 'error':
				state.error = String(msg.message);
				console.error('[AudioEngine] Worklet error:', msg.message);
				break;
		}
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
	 * Load bytecode into the Cedar VM
	 */
	function loadProgram(bytecode: Uint8Array) {
		if (!workletNode) {
			console.warn('[AudioEngine] Cannot load program - not initialized');
			return;
		}

		// Transfer bytecode to worklet
		workletNode.port.postMessage(
			{ type: 'loadProgram', bytecode: bytecode.buffer },
			[bytecode.buffer]
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

		initialize,
		play,
		pause,
		stop,
		setBpm,
		setVolume,
		toggleVisualizations,
		loadProgram,
		setParam,
		getAnalyserNode,
		getAudioContext,
		getTimeDomainData,
		getFrequencyData
	};
}

export const audioEngine = createAudioEngine();
