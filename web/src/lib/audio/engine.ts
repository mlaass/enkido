/**
 * Audio Engine
 *
 * High-level interface for the Cedar audio engine.
 * Manages AudioContext, AudioWorklet, and communication with the processor.
 */

import { compile, type CompileResult, type Diagnostic } from '$lib/compiler/akkado';

export type EngineState = 'uninitialized' | 'loading' | 'ready' | 'running' | 'error';

export interface EngineStatus {
	state: EngineState;
	error?: string;
	isCrossfading: boolean;
	hasProgram: boolean;
}

export interface EngineCallbacks {
	onStateChange?: (state: EngineState) => void;
	onCompileError?: (diagnostics: Diagnostic[]) => void;
	onCompileSuccess?: () => void;
	onVisualizationData?: (data: VisualizationData) => void;
}

export interface VisualizationData {
	waveformLeft: Float32Array;
	waveformRight: Float32Array;
	beatPhase: number;
	barPhase: number;
}

class AudioEngine {
	private audioContext: AudioContext | null = null;
	private workletNode: AudioWorkletNode | null = null;
	private gainNode: GainNode | null = null;
	private analyserNode: AnalyserNode | null = null;

	private _state: EngineState = 'uninitialized';
	private _bpm = 120;
	private _volume = 0.8;
	private _hasProgram = false;
	private _isCrossfading = false;

	private callbacks: EngineCallbacks = {};
	private messageQueue: Array<{ type: string; [key: string]: unknown }> = [];

	/**
	 * Initialize the audio engine
	 * Must be called in response to user gesture (click/touch)
	 */
	async initialize(): Promise<void> {
		if (this._state !== 'uninitialized') return;

		this.setState('loading');

		try {
			// Create AudioContext
			this.audioContext = new AudioContext({
				sampleRate: 48000,
				latencyHint: 'interactive'
			});

			// Create gain node for volume control
			this.gainNode = this.audioContext.createGain();
			this.gainNode.gain.value = this._volume;

			// Create analyser for visualizations
			this.analyserNode = this.audioContext.createAnalyser();
			this.analyserNode.fftSize = 2048;
			this.analyserNode.smoothingTimeConstant = 0.8;

			// Load AudioWorklet processor
			// The processor script must be served from the static directory
			await this.audioContext.audioWorklet.addModule('/worklet/cedar-processor.js');

			// Create worklet node
			this.workletNode = new AudioWorkletNode(this.audioContext, 'cedar-processor', {
				numberOfInputs: 0,
				numberOfOutputs: 1,
				outputChannelCount: [2]
			});

			// Set up message handling
			this.workletNode.port.onmessage = (event) => {
				this.handleWorkerMessage(event.data);
			};

			// Connect: worklet -> gain -> analyser -> destination
			this.workletNode.connect(this.gainNode);
			this.gainNode.connect(this.analyserNode);
			this.analyserNode.connect(this.audioContext.destination);

			// Flush queued messages
			for (const msg of this.messageQueue) {
				this.workletNode.port.postMessage(msg);
			}
			this.messageQueue = [];

			this.setState('ready');
		} catch (err) {
			console.error('[AudioEngine] Initialization failed:', err);
			this.setState('error');
			throw err;
		}
	}

	/**
	 * Compile and load an Akkado program
	 */
	async loadProgram(source: string): Promise<CompileResult> {
		const result = await compile(source);

		if (result.success && result.bytecode) {
			// Send bytecode to worklet
			this.postMessage({
				type: 'loadProgram',
				bytecode: result.bytecode.buffer
			});
			this._hasProgram = true;
			this.callbacks.onCompileSuccess?.();
		} else {
			this.callbacks.onCompileError?.(result.diagnostics);
		}

		return result;
	}

	/**
	 * Start audio playback
	 */
	async start(): Promise<void> {
		if (!this.audioContext) {
			await this.initialize();
		}

		if (this.audioContext?.state === 'suspended') {
			await this.audioContext.resume();
		}

		this.setState('running');
	}

	/**
	 * Pause audio playback
	 */
	async pause(): Promise<void> {
		if (this.audioContext?.state === 'running') {
			await this.audioContext.suspend();
		}

		this.setState('ready');
	}

	/**
	 * Stop and reset
	 */
	async stop(): Promise<void> {
		this.postMessage({ type: 'reset' });
		await this.pause();
	}

	/**
	 * Set BPM
	 */
	setBpm(bpm: number): void {
		this._bpm = Math.max(20, Math.min(999, bpm));
		this.postMessage({ type: 'setBpm', bpm: this._bpm });
	}

	/**
	 * Set volume (0-1)
	 */
	setVolume(volume: number): void {
		this._volume = Math.max(0, Math.min(1, volume));
		if (this.gainNode) {
			this.gainNode.gain.setTargetAtTime(
				this._volume,
				this.audioContext?.currentTime ?? 0,
				0.01
			);
		}
	}

	/**
	 * Set an external parameter
	 */
	setParam(name: string, value: number, slewMs?: number): void {
		this.postMessage({ type: 'setParam', name, value, slewMs });
	}

	/**
	 * Get analyzer node for visualizations
	 */
	getAnalyserNode(): AnalyserNode | null {
		return this.analyserNode;
	}

	/**
	 * Get frequency data from analyzer
	 */
	getFrequencyData(): Uint8Array {
		if (!this.analyserNode) return new Uint8Array(0);
		const data = new Uint8Array(this.analyserNode.frequencyBinCount);
		this.analyserNode.getByteFrequencyData(data);
		return data;
	}

	/**
	 * Get time domain data from analyzer
	 */
	getTimeDomainData(): Uint8Array {
		if (!this.analyserNode) return new Uint8Array(0);
		const data = new Uint8Array(this.analyserNode.fftSize);
		this.analyserNode.getByteTimeDomainData(data);
		return data;
	}

	/**
	 * Register callbacks
	 */
	setCallbacks(callbacks: EngineCallbacks): void {
		this.callbacks = { ...this.callbacks, ...callbacks };
	}

	/**
	 * Get current state
	 */
	get state(): EngineState {
		return this._state;
	}

	get bpm(): number {
		return this._bpm;
	}

	get volume(): number {
		return this._volume;
	}

	get hasProgram(): boolean {
		return this._hasProgram;
	}

	get isCrossfading(): boolean {
		return this._isCrossfading;
	}

	get isRunning(): boolean {
		return this._state === 'running';
	}

	/**
	 * Cleanup
	 */
	async dispose(): Promise<void> {
		if (this.workletNode) {
			this.workletNode.disconnect();
			this.workletNode = null;
		}
		if (this.gainNode) {
			this.gainNode.disconnect();
			this.gainNode = null;
		}
		if (this.analyserNode) {
			this.analyserNode.disconnect();
			this.analyserNode = null;
		}
		if (this.audioContext) {
			await this.audioContext.close();
			this.audioContext = null;
		}
		this.setState('uninitialized');
	}

	private setState(state: EngineState): void {
		this._state = state;
		this.callbacks.onStateChange?.(state);
	}

	private postMessage(msg: { type: string; [key: string]: unknown }): void {
		if (this.workletNode) {
			this.workletNode.port.postMessage(msg);
		} else {
			// Queue message for when worklet is ready
			this.messageQueue.push(msg);
		}
	}

	private handleWorkerMessage(msg: { type: string; [key: string]: unknown }): void {
		switch (msg.type) {
			case 'initialized':
				console.log('[AudioEngine] Worklet initialized');
				// Set initial BPM
				this.postMessage({ type: 'setBpm', bpm: this._bpm });
				break;

			case 'programLoaded':
				console.log('[AudioEngine] Program loaded');
				break;

			case 'error':
				console.error('[AudioEngine] Worklet error:', msg.message);
				break;

			case 'visualizationData':
				// Forward to callback
				if (msg.data && this.callbacks.onVisualizationData) {
					this.callbacks.onVisualizationData(msg.data as VisualizationData);
				}
				break;
		}
	}
}

// Singleton instance
export const audioEngine = new AudioEngine();
