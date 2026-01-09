/**
 * Cedar AudioWorklet Processor
 *
 * This file runs in the AudioWorklet thread and processes audio
 * using the Cedar VM WASM module.
 *
 * Note: This file is loaded as a separate script in the AudioWorklet,
 * not bundled with the main application.
 */

// Message types for communication with main thread
interface WorkletMessage {
	type: string;
	[key: string]: unknown;
}

interface LoadProgramMessage extends WorkletMessage {
	type: 'loadProgram';
	bytecode: ArrayBuffer;
}

interface SetBpmMessage extends WorkletMessage {
	type: 'setBpm';
	bpm: number;
}

interface SetParamMessage extends WorkletMessage {
	type: 'setParam';
	name: string;
	value: number;
	slewMs?: number;
}

interface ResetMessage extends WorkletMessage {
	type: 'reset';
}

// AudioWorklet processor class
class CedarProcessor extends AudioWorkletProcessor {
	private module: any = null;
	private isInitialized = false;
	private outputLeftPtr = 0;
	private outputRightPtr = 0;
	private blockSize = 128;

	constructor() {
		super();

		// Handle messages from main thread
		this.port.onmessage = (event: MessageEvent<WorkletMessage>) => {
			this.handleMessage(event.data);
		};

		// Initialize WASM module
		this.initModule();
	}

	private async initModule(): Promise<void> {
		try {
			// Import the WASM module
			// @ts-expect-error - Module is loaded via importScripts in worklet
			const createModule = globalThis.createEnkidoModule;

			if (!createModule) {
				// Module not yet loaded - will be loaded via message
				return;
			}

			this.module = await createModule();

			// Initialize Cedar VM
			this.module._cedar_init();
			this.module._cedar_set_sample_rate(sampleRate);

			// Get output buffer pointers
			this.outputLeftPtr = this.module._cedar_get_output_left();
			this.outputRightPtr = this.module._cedar_get_output_right();
			this.blockSize = this.module._enkido_get_block_size();

			this.isInitialized = true;

			// Notify main thread
			this.port.postMessage({ type: 'initialized' });
		} catch (err) {
			console.error('[CedarProcessor] Failed to initialize:', err);
			this.port.postMessage({ type: 'error', message: String(err) });
		}
	}

	private handleMessage(msg: WorkletMessage): void {
		if (!this.module && msg.type !== 'initModule') {
			// Queue message until module is ready
			return;
		}

		switch (msg.type) {
			case 'initModule': {
				// Module code passed as string - evaluate it
				// This is used when the module can't be loaded via URL
				break;
			}

			case 'loadProgram': {
				const { bytecode } = msg as LoadProgramMessage;
				this.loadProgram(new Uint8Array(bytecode));
				break;
			}

			case 'setBpm': {
				const { bpm } = msg as SetBpmMessage;
				if (this.module) {
					this.module._cedar_set_bpm(bpm);
				}
				break;
			}

			case 'setParam': {
				const { name, value, slewMs } = msg as SetParamMessage;
				this.setParam(name, value, slewMs);
				break;
			}

			case 'reset': {
				if (this.module) {
					this.module._cedar_reset();
				}
				break;
			}
		}
	}

	private loadProgram(bytecode: Uint8Array): void {
		if (!this.module) return;

		// Allocate bytecode in WASM memory
		const ptr = this.module._enkido_malloc(bytecode.length);
		if (ptr === 0) {
			this.port.postMessage({ type: 'error', message: 'Failed to allocate bytecode' });
			return;
		}

		try {
			// Copy bytecode to WASM memory
			this.module.HEAPU8.set(bytecode, ptr);

			// Load program
			const result = this.module._cedar_load_program(ptr, bytecode.length);

			if (result === 0) {
				this.port.postMessage({ type: 'programLoaded' });
			} else {
				this.port.postMessage({ type: 'error', message: `Load failed with code ${result}` });
			}
		} finally {
			this.module._enkido_free(ptr);
		}
	}

	private setParam(name: string, value: number, slewMs?: number): void {
		if (!this.module) return;

		// Allocate name string
		const len = this.module.lengthBytesUTF8(name) + 1;
		const ptr = this.module._enkido_malloc(len);
		if (ptr === 0) return;

		try {
			this.module.stringToUTF8(name, ptr, len);

			if (slewMs !== undefined) {
				this.module._cedar_set_param_slew(ptr, value, slewMs);
			} else {
				this.module._cedar_set_param(ptr, value);
			}
		} finally {
			this.module._enkido_free(ptr);
		}
	}

	process(
		_inputs: Float32Array[][],
		outputs: Float32Array[][],
		_parameters: Record<string, Float32Array>
	): boolean {
		const output = outputs[0];
		if (!output || output.length < 2) return true;

		const outLeft = output[0];
		const outRight = output[1];

		if (!this.isInitialized || !this.module) {
			// Output silence if not ready
			outLeft.fill(0);
			outRight.fill(0);
			return true;
		}

		// Process one block
		this.module._cedar_process_block();

		// Copy output from WASM memory
		const wasmLeft = new Float32Array(
			this.module.HEAPF32.buffer,
			this.outputLeftPtr,
			this.blockSize
		);
		const wasmRight = new Float32Array(
			this.module.HEAPF32.buffer,
			this.outputRightPtr,
			this.blockSize
		);

		outLeft.set(wasmLeft);
		outRight.set(wasmRight);

		// Send visualization data periodically
		// (every ~10 blocks to reduce overhead)
		// TODO: Implement visualization data extraction

		return true;
	}
}

registerProcessor('cedar-processor', CedarProcessor);
