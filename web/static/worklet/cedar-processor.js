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
		this.pendingProgram = null; // Pre-extracted compile result for loadCompiledProgram()

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

			// Patch the Emscripten code to work in AudioWorklet context
			// Must handle both DEBUG and RELEASE build output formats
			let patchedCode = jsCode
				// === RELEASE BUILD patterns ===
				// Remove the em-bootstrap processor registration (release format: inside block)
				.replace(/registerProcessor\s*\(\s*["']em-bootstrap["'].*?\)\s*\}/g, '}')
				// Remove auto-execution in AudioWorklet context (release format)
				.replace(/isWW\s*\|\|=\s*typeof AudioWorkletGlobalScope.*?isWW\s*&&\s*createEnkidoModule\s*\(\s*\)\s*;?/gs, '')

				// === DEBUG BUILD patterns ===
				// Disable AudioWorklet detection (debug format)
				.replace(/ENVIRONMENT_IS_AUDIO_WORKLET\s*=\s*typeof AudioWorkletGlobalScope\s*!==?\s*["']undefined["']/g,
					'ENVIRONMENT_IS_AUDIO_WORKLET=false')
				// Remove em-bootstrap registerProcessor call (debug format: standalone statement)
				.replace(/registerProcessor\s*\(\s*["']em-bootstrap["'][^;]*;/g, '/* em-bootstrap removed */')
				// Neutralize conditional WASM_WORKER assignment from AudioWorklet check
				.replace(/if\s*\(\s*ENVIRONMENT_IS_AUDIO_WORKLET\s*\)\s*ENVIRONMENT_IS_WASM_WORKER\s*=\s*true;?/g, '')

				// === COMMON patterns (both builds) ===
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
				// Provide WASM binary directly - bypasses all fetch/streaming logic
				wasmBinary: wasmBinary,
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

			case 'getBuiltins':
				this.getBuiltins();
				break;

			case 'getPatternInfo':
				this.getPatternInfo();
				break;

			case 'queryPatternPreview':
				this.queryPatternPreview(msg.patternIndex, msg.startBeat, msg.endBeat);
				break;

			case 'getCurrentBeatPosition':
				this.getCurrentBeatPosition();
				break;

			case 'getActiveSteps':
				this.getActiveSteps(msg.stateIds);
				break;

			case 'inspectState':
				this.inspectState(msg.stateId);
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
	 * Extract a float array from WASM memory, making a copy.
	 * Always creates fresh heap views to handle memory growth safely.
	 */
	extractFloatArray(ptr, count) {
		if (!ptr || count === 0) {
			return new Float32Array(count);
		}

		const result = new Float32Array(count);
		if (this.module.wasmMemory) {
			// Always get fresh heap view - stale views cause corruption after memory growth
			const heap = new Float32Array(this.module.wasmMemory.buffer);
			const floatIdx = ptr / 4;
			for (let i = 0; i < count; i++) {
				result[i] = heap[floatIdx + i];
			}
		} else if (this.module.HEAPF32) {
			const floatIdx = ptr / 4;
			for (let i = 0; i < count; i++) {
				result[i] = this.module.HEAPF32[floatIdx + i];
			}
		} else if (this.module.getValue) {
			for (let i = 0; i < count; i++) {
				result[i] = this.module.getValue(ptr + i * 4, 'float');
			}
		}
		return result;
	}

	/**
	 * Write a byte array to WASM memory.
	 * Always creates fresh heap views to handle memory growth safely.
	 */
	writeByteArray(ptr, data) {
		if (this.module.wasmMemory) {
			// Always get fresh heap view - stale views cause corruption after memory growth
			const heap = new Uint8Array(this.module.wasmMemory.buffer);
			heap.set(data, ptr);
		} else if (this.module.HEAPU8) {
			this.module.HEAPU8.set(data, ptr);
		} else if (this.module.setValue) {
			for (let i = 0; i < data.length; i++) {
				this.module.setValue(ptr + i, data[i], 'i8');
			}
		}
	}

	/**
	 * Write a float array to WASM memory.
	 * Always creates fresh heap views to handle memory growth safely.
	 */
	writeFloatArray(ptr, data) {
		if (this.module.wasmMemory) {
			// Always get fresh heap view - stale views cause corruption after memory growth
			const heap = new Float32Array(this.module.wasmMemory.buffer);
			heap.set(data, ptr / 4);
		} else if (this.module.HEAPF32) {
			this.module.HEAPF32.set(data, ptr / 4);
		} else if (this.module.setValue) {
			for (let i = 0; i < data.length; i++) {
				this.module.setValue(ptr + i * 4, data[i], 'float');
			}
		}
	}

	/**
	 * Extract all parameter declarations from compile result into JS objects.
	 * Must be called immediately after compilation, before any memory growth.
	 */
	extractParamDecls() {
		const count = this.module._akkado_get_param_decl_count();
		const paramDecls = [];

		for (let i = 0; i < count; i++) {
			const namePtr = this.module._akkado_get_param_name(i);
			const name = namePtr ? this.module.UTF8ToString(namePtr) : '';
			const type = this.module._akkado_get_param_type(i);
			const defaultValue = this.module._akkado_get_param_default(i);
			const min = this.module._akkado_get_param_min(i);
			const max = this.module._akkado_get_param_max(i);
			const sourceOffset = this.module._akkado_get_param_source_offset(i);
			const sourceLength = this.module._akkado_get_param_source_length(i);

			// Extract options for Select type
			const options = [];
			if (type === 3) { // Select
				const optCount = this.module._akkado_get_param_option_count(i);
				for (let j = 0; j < optCount; j++) {
					const optPtr = this.module._akkado_get_param_option(i, j);
					if (optPtr) {
						options.push(this.module.UTF8ToString(optPtr));
					}
				}
			}

			paramDecls.push({
				name,
				type,  // 0=Continuous, 1=Button, 2=Toggle, 3=Select
				defaultValue,
				min,
				max,
				options,
				sourceOffset,
				sourceLength
			});
		}

		return paramDecls;
	}

	/**
	 * Extract all state initialization data from compile result into JS objects.
	 * Must be called immediately after compilation, before any memory growth.
	 */
	extractStateInits() {
		const count = this.module._akkado_get_state_init_count();
		const stateInits = [];

		for (let i = 0; i < count; i++) {
			const stateId = this.module._akkado_get_state_init_id(i);
			const type = this.module._akkado_get_state_init_type(i);
			const valuesCount = this.module._akkado_get_state_init_values_count(i);
			const cycleLength = this.module._akkado_get_state_init_cycle_length(i);

			// Extract times array
			const timesPtr = this.module._akkado_get_state_init_times(i);
			const times = this.extractFloatArray(timesPtr, valuesCount);

			// Extract values array
			const valuesPtr = this.module._akkado_get_state_init_values(i);
			const values = this.extractFloatArray(valuesPtr, valuesCount);

			// Extract velocities array
			const velocitiesPtr = this.module._akkado_get_state_init_velocities(i);
			const velocities = this.extractFloatArray(velocitiesPtr, valuesCount);

			// Extract sample names (for later resolution)
			const sampleNamesCount = this.module._akkado_get_state_init_sample_names_count(i);
			const sampleNames = [];
			for (let j = 0; j < sampleNamesCount; j++) {
				const namePtr = this.module._akkado_get_state_init_sample_name(i, j);
				sampleNames.push(namePtr ? this.module.UTF8ToString(namePtr) : null);
			}

			stateInits.push({
				stateId,
				type,
				times,
				values,
				velocities,
				sampleNames,
				cycleLength
			});
		}

		return stateInits;
	}

	/**
	 * Get sample ID by name (helper for resolving sample names in state inits)
	 */
	getSampleId(name) {
		const nameLen = this.module.lengthBytesUTF8(name) + 1;
		const namePtr = this.module._enkido_malloc(nameLen);
		if (!namePtr) return 0;

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);
			return this.module._cedar_get_sample_id(namePtr);
		} finally {
			this.module._enkido_free(namePtr);
		}
	}

	/**
	 * Compile source code (does not load into VM)
	 * Returns required samples so runtime can load them before calling loadCompiledProgram
	 */
	compile(source) {
		console.log('[CedarProcessor] compile() ENTRY');

		// CRITICAL: Always send 'compiled' response to prevent main thread hang
		if (!this.module) {
			this.port.postMessage({
				type: 'compiled',
				success: false,
				diagnostics: [{ severity: 2, message: 'Module not initialized', line: 1, column: 1 }]
			});
			return;
		}

		try {
			// Clear any previous compile result and pending program
			console.log('[CedarProcessor] Before _akkado_clear_result');
			this.module._akkado_clear_result();
			console.log('[CedarProcessor] After _akkado_clear_result');
			this.pendingProgram = null;

			// Get actual UTF-8 byte length (not JS string length which counts UTF-16 code units)
			const utf8ByteLen = this.module.lengthBytesUTF8(source);
			const allocLen = utf8ByteLen + 1; // +1 for null terminator

			console.log('[CedarProcessor] Compiling source, utf8 bytes:', utf8ByteLen);

			// Allocate source string in WASM memory
			const sourcePtr = this.module._enkido_malloc(allocLen);
			if (sourcePtr === 0) {
				this.port.postMessage({ type: 'compiled', success: false, diagnostics: [{ severity: 2, message: 'Failed to allocate memory', line: 1, column: 1 }] });
				return;
			}

			try {
				this.module.stringToUTF8(source, sourcePtr, allocLen);

				// Compile - pass actual UTF-8 byte length, not JS string length
				const success = this.module._akkado_compile(sourcePtr, utf8ByteLen);

				if (success) {
					// Extract ALL data immediately, before any memory growth can happen

					// Extract bytecode as Uint8Array using fresh heap view
					const bytecodePtr = this.module._akkado_get_bytecode();
					const bytecodeSize = this.module._akkado_get_bytecode_size();
					const bytecode = new Uint8Array(bytecodeSize);
					if (this.module.wasmMemory) {
						// Always get fresh heap view - stale views cause corruption after memory growth
						const heap = new Uint8Array(this.module.wasmMemory.buffer);
						for (let i = 0; i < bytecodeSize; i++) {
							bytecode[i] = heap[bytecodePtr + i];
						}
					} else if (this.module.HEAPU8) {
						for (let i = 0; i < bytecodeSize; i++) {
							bytecode[i] = this.module.HEAPU8[bytecodePtr + i];
						}
					} else if (this.module.getValue) {
						for (let i = 0; i < bytecodeSize; i++) {
							bytecode[i] = this.module.getValue(bytecodePtr + i, 'i8') & 0xFF;
						}
					}

					// Extract required sample names
					const requiredSamples = this.getRequiredSamples();

					// Extract all state initialization data
					const stateInits = this.extractStateInits();

					// Extract parameter declarations for UI generation
					const paramDecls = this.extractParamDecls();

					// CRITICAL: Clear the compile result NOW, before returning
					// This ensures we don't have stale pointers when memory grows
					this.module._akkado_clear_result();

					// Extract disassembly for debug panel
					let disassembly = null;
					if (this.module._akkado_get_disassembly) {
						const disasmPtr = this.module._akkado_get_disassembly();
						if (disasmPtr) {
							disassembly = this.module.UTF8ToString(disasmPtr);
							try {
								disassembly = JSON.parse(disassembly);
							} catch (e) {
								console.warn('[CedarProcessor] Failed to parse disassembly JSON:', e);
								disassembly = null;
							}
						}
					}

					// Store extracted data for loadCompiledProgram()
					this.pendingProgram = { bytecode, stateInits, requiredSamples };

					console.log('[CedarProcessor] Compiled successfully, bytecode size:', bytecodeSize,
						'required samples:', requiredSamples, 'state inits:', stateInits.length,
						'param decls:', paramDecls.length,
						'unique states:', disassembly?.summary?.uniqueStateIds ?? 'N/A');

					this.port.postMessage({
						type: 'compiled',
						success: true,
						bytecodeSize,
						requiredSamples,
						paramDecls,
						disassembly
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
			}
		} catch (err) {
			// CRITICAL: Catch any exception and send error response
			// Without this, the Promise in audio.svelte.ts hangs forever
			console.error('[CedarProcessor] Compile error:', err);
			this.port.postMessage({
				type: 'compiled',
				success: false,
				diagnostics: [{ severity: 2, message: 'Internal error: ' + String(err), line: 1, column: 1 }]
			});
		}
	}

	/**
	 * Load the compiled program after samples are ready.
	 * Uses pre-extracted data from compile() - no WASM compile result access needed.
	 */
	loadCompiledProgram() {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized' });
			return;
		}

		if (!this.pendingProgram) {
			this.port.postMessage({ type: 'error', message: 'No pending program to load' });
			return;
		}

		const { bytecode, stateInits } = this.pendingProgram;

		console.log('[CedarProcessor] Loading program, bytecode size:', bytecode.length,
			'state inits:', stateInits.length);

		// Allocate and copy bytecode to WASM memory
		let bytecodePtr = this.module._enkido_malloc(bytecode.length);
		if (bytecodePtr === 0) {
			this.port.postMessage({ type: 'error', message: 'Failed to allocate bytecode' });
			return;
		}

		try {
			this.writeByteArray(bytecodePtr, bytecode);

			// Load program into Cedar VM
			const result = this.module._cedar_load_program(bytecodePtr, bytecode.length);

			if (result === 1) {
				// SlotBusy - all slots occupied (crossfade in progress)
				// Keep pendingProgram for retry - DO NOT clear it here
				console.warn('[CedarProcessor] Slot busy - VM is crossfading');
				this.module._enkido_free(bytecodePtr);
				bytecodePtr = 0; // Prevent double-free in finally
				this.port.postMessage({ type: 'error', message: 'VM busy - crossfade in progress, try again' });
				return;
			}

			if (result !== 0) {
				console.error('[CedarProcessor] Load failed with code:', result);
				this.pendingProgram = null; // Clear on permanent error
				this.port.postMessage({ type: 'error', message: `Load failed with code ${result}` });
				return;
			}

			// Apply state initializations using WASM function
			// First resolve sample names to IDs (uses samples already loaded in sample bank)
			this.module._akkado_resolve_sample_ids();
			// Then apply all state inits (handles both SeqStep and SequenceProgram types)
			const stateInitsApplied = this.module._cedar_apply_state_inits();
			if (stateInitsApplied > 0) {
				console.log('[CedarProcessor] Applied', stateInitsApplied, 'state initializations');
			}

			// Diagnostic logging after load
			const hasPendingSwap = this.module._cedar_debug_has_pending_swap?.() ?? 'N/A';
			const swapCount = this.module._cedar_debug_swap_count?.() ?? 'N/A';
			const currentInst = this.module._cedar_debug_current_slot_instruction_count?.() ?? 'N/A';
			console.log(`[CedarProcessor] Program loaded successfully: pendingSwap=${hasPendingSwap} swapCount=${swapCount} instructions=${currentInst}`);
			this.pendingProgram = null; // Clear only on success
			this.port.postMessage({ type: 'programLoaded' });

		} finally {
			// Guard against double-free (bytecodePtr set to 0 on SlotBusy)
			if (bytecodePtr) {
				this.module._enkido_free(bytecodePtr);
			}
			// Note: pendingProgram is cleared on success, NOT here
			// This preserves it for retry on SlotBusy
		}
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
			// Copy bytecode to WASM memory using fresh heap view
			this.writeByteArray(ptr, bytecode);

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

			// Copy audio data to WASM memory using fresh heap view
			this.writeFloatArray(audioPtr, audioData);

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

			// Copy WAV data to WASM memory using fresh heap view
			this.writeByteArray(wavPtr, wavArray);

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

	/**
	 * Get all builtin function metadata as JSON
	 * Sends back to main thread for autocomplete
	 */
	getBuiltins() {
		if (!this.module) {
			this.port.postMessage({
				type: 'builtins',
				success: false,
				error: 'Module not initialized'
			});
			return;
		}

		try {
			const jsonPtr = this.module._akkado_get_builtins_json();
			if (!jsonPtr) {
				this.port.postMessage({
					type: 'builtins',
					success: false,
					error: 'Failed to get builtins JSON'
				});
				return;
			}

			const jsonStr = this.module.UTF8ToString(jsonPtr);
			const data = JSON.parse(jsonStr);

			this.port.postMessage({
				type: 'builtins',
				success: true,
				data
			});
		} catch (err) {
			this.port.postMessage({
				type: 'builtins',
				success: false,
				error: String(err)
			});
		}
	}

	/**
	 * Get pattern info for all patterns in the compile result
	 * Used by UI for pattern highlighting
	 */
	getPatternInfo() {
		if (!this.module) {
			this.port.postMessage({
				type: 'patternInfo',
				success: false,
				error: 'Module not initialized'
			});
			return;
		}

		try {
			const count = this.module._akkado_get_pattern_init_count();
			const patterns = [];

			for (let i = 0; i < count; i++) {
				patterns.push({
					stateId: this.module._akkado_get_pattern_state_id(i),
					docOffset: this.module._akkado_get_pattern_doc_offset(i),
					docLength: this.module._akkado_get_pattern_doc_length(i),
					cycleLength: this.module._akkado_get_pattern_cycle_length(i)
				});
			}

			this.port.postMessage({
				type: 'patternInfo',
				success: true,
				patterns
			});
		} catch (err) {
			this.port.postMessage({
				type: 'patternInfo',
				success: false,
				error: String(err)
			});
		}
	}

	/**
	 * Query pattern for preview events
	 * @param {number} patternIndex - Pattern index
	 * @param {number} startBeat - Query window start
	 * @param {number} endBeat - Query window end
	 */
	queryPatternPreview(patternIndex, startBeat, endBeat) {
		if (!this.module) {
			this.port.postMessage({
				type: 'patternPreview',
				success: false,
				error: 'Module not initialized'
			});
			return;
		}

		try {
			const eventCount = this.module._akkado_query_pattern_preview(patternIndex, startBeat, endBeat);
			const events = [];

			for (let i = 0; i < eventCount; i++) {
				events.push({
					time: this.module._akkado_get_preview_event_time(i),
					duration: this.module._akkado_get_preview_event_duration(i),
					value: this.module._akkado_get_preview_event_value(i),
					sourceOffset: this.module._akkado_get_preview_event_source_offset(i),
					sourceLength: this.module._akkado_get_preview_event_source_length(i)
				});
			}

			this.port.postMessage({
				type: 'patternPreview',
				success: true,
				patternIndex,
				events
			});
		} catch (err) {
			this.port.postMessage({
				type: 'patternPreview',
				success: false,
				error: String(err)
			});
		}
	}

	/**
	 * Get current beat position from VM
	 */
	getCurrentBeatPosition() {
		if (!this.module) {
			this.port.postMessage({
				type: 'beatPosition',
				position: 0
			});
			return;
		}

		const position = this.module._cedar_get_current_beat_position();
		this.port.postMessage({
			type: 'beatPosition',
			position
		});
	}

	/**
	 * Get active step source ranges for multiple patterns
	 * @param {number[]} stateIds - Array of state IDs to query
	 */
	getActiveSteps(stateIds) {
		if (!this.module || !stateIds) {
			this.port.postMessage({
				type: 'activeSteps',
				steps: {}
			});
			return;
		}

		const steps = {};
		for (const stateId of stateIds) {
			const offset = this.module._cedar_get_pattern_active_offset(stateId);
			const length = this.module._cedar_get_pattern_active_length(stateId);
			if (length > 0) {
				steps[stateId] = { offset, length };
			}
		}

		this.port.postMessage({
			type: 'activeSteps',
			steps
		});
	}

	/**
	 * Inspect state by ID, returning JSON representation
	 * @param {number} stateId - State ID to inspect
	 */
	inspectState(stateId) {
		if (!this.module) {
			this.port.postMessage({
				type: 'stateInspection',
				stateId,
				data: null
			});
			return;
		}

		try {
			const jsonPtr = this.module._cedar_inspect_state(stateId);
			if (!jsonPtr) {
				this.port.postMessage({
					type: 'stateInspection',
					stateId,
					data: null
				});
				return;
			}

			const jsonStr = this.module.UTF8ToString(jsonPtr);
			if (!jsonStr || jsonStr.length === 0) {
				this.port.postMessage({
					type: 'stateInspection',
					stateId,
					data: null
				});
				return;
			}

			const data = JSON.parse(jsonStr);
			this.port.postMessage({
				type: 'stateInspection',
				stateId,
				data
			});
		} catch (err) {
			this.port.postMessage({
				type: 'stateInspection',
				stateId,
				data: null,
				error: String(err)
			});
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
			// Periodic diagnostic logging (every 1000 blocks = ~2.7s at 48kHz)
			this.blockCount = (this.blockCount || 0) + 1;
			if (this.blockCount % 1000 === 0) {
				const hasProgram = this.module._cedar_has_program();
				const isCrossfading = this.module._cedar_is_crossfading();
				const hasPendingSwap = this.module._cedar_debug_has_pending_swap?.() ?? 'N/A';
				const swapCount = this.module._cedar_debug_swap_count?.() ?? 'N/A';
				const currentInst = this.module._cedar_debug_current_slot_instruction_count?.() ?? 'N/A';
				console.log(`[CedarProcessor] block=${this.blockCount} hasProgram=${hasProgram} crossfading=${isCrossfading} pendingSwap=${hasPendingSwap} swaps=${swapCount} instructions=${currentInst}`);
			}

			// Process one block with Cedar VM
			// Cedar handles crossfade internally when programs are swapped
			this.module._cedar_process_block();

			// Copy output from WASM memory
			// outputLeftPtr and outputRightPtr are byte offsets, convert to float index
			const leftFloatIdx = this.outputLeftPtr / 4;
			const rightFloatIdx = this.outputRightPtr / 4;

			if (this.module.wasmMemory) {
				// Always get fresh heap view - stale views cause corruption after memory growth
				const heap = new Float32Array(this.module.wasmMemory.buffer);
				for (let i = 0; i < this.blockSize && i < outLeft.length; i++) {
					outLeft[i] = heap[leftFloatIdx + i] || 0;
					outRight[i] = heap[rightFloatIdx + i] || 0;
				}
			} else if (this.module.HEAPF32) {
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
