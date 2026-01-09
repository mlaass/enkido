/**
 * Cedar AudioWorklet Processor
 *
 * Runs in the AudioWorklet thread and processes audio using the Cedar VM WASM module.
 * The WASM module code and binary are sent from the main thread since AudioWorklets
 * have limited API access (no fetch, no importScripts for cross-origin).
 */

class CedarProcessor extends AudioWorkletProcessor {
	constructor() {
		super();

		this.module = null;
		this.isInitialized = false;
		this.outputLeftPtr = 0;
		this.outputRightPtr = 0;
		this.blockSize = 128;

		// Queue messages until module is ready
		this.messageQueue = [];

		// Handle messages from main thread
		this.port.onmessage = (event) => {
			const msg = event.data;

			if (msg.type === 'init') {
				// Receive WASM JS code and binary from main thread
				this.initFromCode(msg.jsCode, msg.wasmBinary);
			} else if (this.isInitialized) {
				this.handleMessage(msg);
			} else {
				// Queue messages until ready
				this.messageQueue.push(msg);
			}
		};

		// Request initialization from main thread
		this.port.postMessage({ type: 'requestInit' });
	}

	async initFromCode(jsCode, wasmBinary) {
		try {
			console.log('[CedarProcessor] Initializing from code...');

			// Store the WASM binary for the module to use
			this.wasmBinary = wasmBinary;

			// Patch the Emscripten code to:
			// 1. Remove the registerProcessor call that conflicts with our processor
			// 2. Prevent it from thinking it's in a pthread/worker context that needs bootstrap
			let patchedCode = jsCode
				// Remove the em-bootstrap processor registration
				.replace(/registerProcessor\s*\(\s*["']em-bootstrap["'].*?\)\s*\}/g, '}')
				// Make it think we're not in a worker context that needs special setup
				.replace(/ENVIRONMENT_IS_PTHREAD\s*=\s*ENVIRONMENT_IS_WORKER/g, 'ENVIRONMENT_IS_PTHREAD=false')
				.replace(/ENVIRONMENT_IS_WASM_WORKER\s*=\s*true/g, 'ENVIRONMENT_IS_WASM_WORKER=false');

			// Create a function that returns the module factory
			const moduleFactory = new Function(patchedCode + '\nreturn createEnkidoModule;')();

			if (typeof moduleFactory !== 'function') {
				throw new Error('createEnkidoModule not found after evaluating code');
			}

			console.log('[CedarProcessor] Creating module with wasmBinary:', wasmBinary.byteLength, 'bytes');

			// Create the module with custom options
			this.module = await moduleFactory({
				// Provide custom WASM instantiation to avoid fetch
				instantiateWasm: (imports, successCallback) => {
					console.log('[CedarProcessor] Custom instantiateWasm called');
					WebAssembly.instantiate(wasmBinary, imports)
						.then(result => {
							console.log('[CedarProcessor] WASM instantiated successfully');
							successCallback(result.instance, result.module);
						})
						.catch(err => {
							console.error('[CedarProcessor] WASM instantiation failed:', err);
						});
					return {}; // Return empty exports, will be filled by callback
				},
				// Add print handlers for debugging
				print: (text) => console.log('[WASM]', text),
				printErr: (text) => console.error('[WASM Error]', text)
			});

			console.log('[CedarProcessor] Module created successfully');

			// Initialize Cedar VM
			this.module._cedar_init();
			this.module._cedar_set_sample_rate(sampleRate);

			// Get output buffer pointers
			this.outputLeftPtr = this.module._cedar_get_output_left();
			this.outputRightPtr = this.module._cedar_get_output_right();
			this.blockSize = this.module._enkido_get_block_size();

			this.isInitialized = true;
			console.log('[CedarProcessor] Module initialized, block size:', this.blockSize);

			// Process queued messages
			for (const msg of this.messageQueue) {
				this.handleMessage(msg);
			}
			this.messageQueue = [];

			// Notify main thread
			this.port.postMessage({ type: 'initialized' });
		} catch (err) {
			console.error('[CedarProcessor] Failed to initialize:', err);
			this.port.postMessage({ type: 'error', message: String(err) });
		}
	}

	handleMessage(msg) {
		switch (msg.type) {
			case 'loadProgram':
				this.loadProgram(msg.bytecode);
				break;

			case 'setBpm':
				if (this.module) {
					this.module._cedar_set_bpm(msg.bpm);
				}
				break;

			case 'setParam':
				this.setParam(msg.name, msg.value, msg.slewMs);
				break;

			case 'reset':
				if (this.module) {
					this.module._cedar_reset();
				}
				break;
		}
	}

	loadProgram(bytecodeBuffer) {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized' });
			return;
		}

		const bytecode = new Uint8Array(bytecodeBuffer);
		console.log('[CedarProcessor] Loading program, bytecode size:', bytecode.length);

		// Allocate bytecode in WASM memory
		const ptr = this.module._enkido_malloc(bytecode.length);
		if (ptr === 0) {
			this.port.postMessage({ type: 'error', message: 'Failed to allocate bytecode' });
			return;
		}

		try {
			// Copy bytecode to WASM memory using setValue (HEAPU8 may not be exposed)
			if (this.module.HEAPU8) {
				this.module.HEAPU8.set(bytecode, ptr);
			} else if (this.module.setValue) {
				for (let i = 0; i < bytecode.length; i++) {
					this.module.setValue(ptr + i, bytecode[i], 'i8');
				}
			} else {
				throw new Error('No way to write to WASM memory');
			}

			// Load program into Cedar VM
			const result = this.module._cedar_load_program(ptr, bytecode.length);

			if (result === 0) {
				console.log('[CedarProcessor] Program loaded successfully');
				this.port.postMessage({ type: 'programLoaded' });
			} else {
				console.error('[CedarProcessor] Load failed with code:', result);
				this.port.postMessage({ type: 'error', message: `Load failed with code ${result}` });
			}
		} finally {
			this.module._enkido_free(ptr);
		}
	}

	setParam(name, value, slewMs) {
		if (!this.module) return;

		// Allocate name string in WASM memory
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

	process(inputs, outputs, parameters) {
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

		try {
			// Process one block with Cedar VM
			this.module._cedar_process_block();

			// Copy output from WASM memory
			// outputLeftPtr and outputRightPtr are byte offsets, convert to float index
			const leftFloatIdx = this.outputLeftPtr / 4;
			const rightFloatIdx = this.outputRightPtr / 4;

			if (this.module.HEAPF32) {
				for (let i = 0; i < this.blockSize && i < outLeft.length; i++) {
					outLeft[i] = this.module.HEAPF32[leftFloatIdx + i] || 0;
					outRight[i] = this.module.HEAPF32[rightFloatIdx + i] || 0;
				}
			} else if (this.module.getValue) {
				for (let i = 0; i < this.blockSize && i < outLeft.length; i++) {
					outLeft[i] = this.module.getValue(this.outputLeftPtr + i * 4, 'float') || 0;
					outRight[i] = this.module.getValue(this.outputRightPtr + i * 4, 'float') || 0;
				}
			} else {
				outLeft.fill(0);
				outRight.fill(0);
			}
		} catch (err) {
			// On error, output silence
			outLeft.fill(0);
			outRight.fill(0);
		}

		return true;
	}
}

registerProcessor('cedar-processor', CedarProcessor);
