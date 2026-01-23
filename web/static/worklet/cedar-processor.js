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

			// Polyfill crypto for AudioWorklet context (not available in worklets)
			if (typeof crypto === 'undefined') {
				globalThis.crypto = {
					getRandomValues: (array) => {
						// Simple PRNG for non-cryptographic use in audio context
						for (let i = 0; i < array.length; i++) {
							array[i] = Math.floor(Math.random() * 256);
						}
						return array;
					}
				};
			}

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
				.replace(/ENVIRONMENT_IS_WASM_WORKER\s*=\s*true/g, 'ENVIRONMENT_IS_WASM_WORKER=false')
				// Remove auto-execution in AudioWorklet context (causes spurious fetch errors)
				.replace(/isWW\|\|=typeof AudioWorkletGlobalScope.*?isWW&&createEnkidoModule\(\);?/g, '');

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
			case 'compile':
				this.compile(msg.source);
				break;

			case 'loadCompiledProgram':
				this.loadCompiledProgram();
				break;

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

			case 'loadSample':
				this.loadSample(msg.name, msg.audioData, msg.channels, msg.sampleRate);
				break;

			case 'loadSampleWav':
				this.loadSampleWav(msg.name, msg.wavData);
				break;

			case 'clearSamples':
				if (this.module) {
					this.module._cedar_clear_samples();
				}
				break;
		}
	}

	/**
	 * Get required sample names from the compile result
	 * @returns {string[]} Array of sample names used in the compiled code
	 */
	getRequiredSamples() {
		const count = this.module._akkado_get_required_samples_count();
		const samples = [];
		for (let i = 0; i < count; i++) {
			const ptr = this.module._akkado_get_required_sample(i);
			if (ptr) {
				samples.push(this.module.UTF8ToString(ptr));
			}
		}
		return samples;
	}

	/**
	 * Compile source code (does not load into VM)
	 * Returns required samples so runtime can load them before calling loadCompiledProgram
	 */
	compile(source) {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized' });
			return;
		}

		console.log('[CedarProcessor] Compiling source, length:', source.length);

		// Allocate source string in WASM memory
		const len = this.module.lengthBytesUTF8(source) + 1;
		const sourcePtr = this.module._enkido_malloc(len);
		if (sourcePtr === 0) {
			this.port.postMessage({ type: 'compiled', success: false, diagnostics: [{ severity: 2, message: 'Failed to allocate memory', line: 1, column: 1 }] });
			return;
		}

		try {
			this.module.stringToUTF8(source, sourcePtr, len);

			// Compile
			const success = this.module._akkado_compile(sourcePtr, source.length);

			if (success) {
				const bytecodeSize = this.module._akkado_get_bytecode_size();
				const requiredSamples = this.getRequiredSamples();

				console.log('[CedarProcessor] Compiled successfully, bytecode size:', bytecodeSize,
					'required samples:', requiredSamples);

				this.port.postMessage({
					type: 'compiled',
					success: true,
					bytecodeSize,
					requiredSamples
				});
			} else {
				// Extract diagnostics
				const diagnostics = this.extractDiagnostics();
				console.log('[CedarProcessor] Compilation failed:', diagnostics);
				this.port.postMessage({
					type: 'compiled',
					success: false,
					diagnostics
				});
				// Clear result on failure
				this.module._akkado_clear_result();
			}
		} finally {
			this.module._enkido_free(sourcePtr);
			// Note: don't clear result on success - we need it for loadCompiledProgram
		}
	}

	/**
	 * Load the compiled program after samples are ready
	 * Call this after compile() succeeds and required samples are loaded
	 */
	loadCompiledProgram() {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized' });
			return;
		}

		// Resolve sample IDs now that samples are loaded
		this.module._akkado_resolve_sample_ids();

		// Get bytecode pointer and size
		const bytecodePtr = this.module._akkado_get_bytecode();
		const bytecodeSize = this.module._akkado_get_bytecode_size();

		// Load program - Cedar handles crossfade internally
		const result = this.module._cedar_load_program(bytecodePtr, bytecodeSize);

		if (result === 0) {
			// Apply state initializations for SEQ_STEP patterns (now with resolved IDs)
			const stateInitsApplied = this.module._cedar_apply_state_inits();
			if (stateInitsApplied > 0) {
				console.log('[CedarProcessor] Applied', stateInitsApplied, 'state initializations');
			}

			console.log('[CedarProcessor] Program loaded successfully');
			this.port.postMessage({ type: 'programLoaded' });
		} else {
			console.error('[CedarProcessor] Load failed with code:', result);
			this.port.postMessage({ type: 'error', message: `Load failed with code ${result}` });
		}

		// Clear compile result
		this.module._akkado_clear_result();
	}

	/**
	 * Extract compilation diagnostics from WASM
	 */
	extractDiagnostics() {
		const count = this.module._akkado_get_diagnostic_count();
		const diagnostics = [];
		for (let i = 0; i < count; i++) {
			const messagePtr = this.module._akkado_get_diagnostic_message(i);
			diagnostics.push({
				severity: this.module._akkado_get_diagnostic_severity(i),
				message: this.module.UTF8ToString(messagePtr),
				line: this.module._akkado_get_diagnostic_line(i),
				column: this.module._akkado_get_diagnostic_column(i)
			});
		}
		return diagnostics;
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

	loadSample(name, audioData, channels, sampleRate) {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized' });
			return;
		}

		console.log('[CedarProcessor] Loading sample:', name, 'samples:', audioData.length, 'channels:', channels);

		// Allocate name string
		const nameLen = this.module.lengthBytesUTF8(name) + 1;
		const namePtr = this.module._enkido_malloc(nameLen);
		if (namePtr === 0) {
			this.port.postMessage({ type: 'error', message: 'Failed to allocate name' });
			return;
		}

		// Allocate audio data
		const audioPtr = this.module._enkido_malloc(audioData.length * 4); // 4 bytes per float
		if (audioPtr === 0) {
			this.module._enkido_free(namePtr);
			this.port.postMessage({ type: 'error', message: 'Failed to allocate audio data' });
			return;
		}

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);

			// Copy audio data to WASM memory
			if (this.module.HEAPF32) {
				this.module.HEAPF32.set(audioData, audioPtr / 4);
			} else {
				for (let i = 0; i < audioData.length; i++) {
					this.module.setValue(audioPtr + i * 4, audioData[i], 'float');
				}
			}

			// Load sample
			const sampleId = this.module._cedar_load_sample(namePtr, audioPtr, audioData.length, channels, sampleRate);

			if (sampleId > 0) {
				console.log('[CedarProcessor] Sample loaded successfully, ID:', sampleId);
				this.port.postMessage({ type: 'sampleLoaded', name, sampleId });
			} else {
				console.error('[CedarProcessor] Failed to load sample');
				this.port.postMessage({ type: 'error', message: 'Failed to load sample: ' + name });
			}
		} finally {
			this.module._enkido_free(namePtr);
			this.module._enkido_free(audioPtr);
		}
	}

	loadSampleWav(name, wavData) {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized' });
			return;
		}

		console.log('[CedarProcessor] Loading WAV sample:', name, 'size:', wavData.byteLength);

		// Allocate name string
		const nameLen = this.module.lengthBytesUTF8(name) + 1;
		const namePtr = this.module._enkido_malloc(nameLen);
		if (namePtr === 0) {
			this.port.postMessage({ type: 'error', message: 'Failed to allocate name' });
			return;
		}

		// Allocate WAV data
		const wavArray = new Uint8Array(wavData);
		const wavPtr = this.module._enkido_malloc(wavArray.length);
		if (wavPtr === 0) {
			this.module._enkido_free(namePtr);
			this.port.postMessage({ type: 'error', message: 'Failed to allocate WAV data' });
			return;
		}

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);

			// Copy WAV data to WASM memory
			if (this.module.HEAPU8) {
				this.module.HEAPU8.set(wavArray, wavPtr);
			} else {
				for (let i = 0; i < wavArray.length; i++) {
					this.module.setValue(wavPtr + i, wavArray[i], 'i8');
				}
			}

			// Load sample from WAV
			const sampleId = this.module._cedar_load_sample_wav(namePtr, wavPtr, wavArray.length);

			if (sampleId > 0) {
				console.log('[CedarProcessor] WAV sample loaded successfully, ID:', sampleId);
				this.port.postMessage({ type: 'sampleLoaded', name, sampleId });
			} else {
				console.error('[CedarProcessor] Failed to load WAV sample');
				this.port.postMessage({ type: 'error', message: 'Failed to load WAV sample: ' + name });
			}
		} finally {
			this.module._enkido_free(namePtr);
			this.module._enkido_free(wavPtr);
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
			// Cedar handles crossfade internally when programs are swapped
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
