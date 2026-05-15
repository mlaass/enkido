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
		// Input buffer pointers, populated after init. The audio-input PRD
		// keeps the WASM heap as the source of truth: the worklet writes
		// captured frames directly into these byte offsets each block.
		this.inputLeftPtr = 0;
		this.inputRightPtr = 0;
		this.blockSize = 128;
		this.pendingProgram = null; // Pre-extracted compile result for loadCompiledProgram()
		this.pendingLoadRetry = false; // Set when load hit SlotBusy; retried from process() each block
		this.pendingLoadRefreshId = 0; // ID of the latest refresh request awaiting load

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
				.replace(/isWW\s*\|\|=\s*typeof AudioWorkletGlobalScope.*?isWW\s*&&\s*createNkidoModule\s*\(\s*\)\s*;?/gs, '')

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
			const moduleFactory = new Function(patchedCode + '\nreturn createNkidoModule;')();

			if (typeof moduleFactory !== 'function') {
				throw new Error('createNkidoModule not found after evaluating code');
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
			// Get input buffer pointers (audio-input PRD §4.5).
			if (this.module._cedar_get_input_left && this.module._cedar_get_input_right) {
				this.inputLeftPtr = this.module._cedar_get_input_left();
				this.inputRightPtr = this.module._cedar_get_input_right();
			}
			this.blockSize = this.module._nkido_get_block_size();

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
				this.loadCompiledProgram(msg.refreshId);
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

			case 'loadSampleAudio':
				this.loadSampleAudio(msg.name, msg.audioData);
				break;

			case 'clearSamples':
				if (this.module) {
					this.module._cedar_clear_samples();
				}
				break;

			case 'loadSoundFont':
				this.loadSoundFont(msg.name, msg.data);
				break;

			case 'loadMidiFile':
				this.loadMidiFile(msg.name, msg.data);
				break;

			case 'loadWavetable':
				this.loadWavetable(msg.name, msg.data);
				break;

			case 'clearWavetables':
				if (this.module) {
					this.module._cedar_clear_wavetables();
				}
				break;

			case 'getBuiltins':
				this.getBuiltins();
				break;

			case 'getShapeIndex':
				// Phase 2 records-system-unification: editor pulls the
				// analyzer-driven shape index for r./@./record-literal
				// completions. Debounced ~300 ms on the main thread.
				this.getShapeIndex(msg.requestId, msg.source, msg.cursor);
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

			case 'getPatternDebug':
				this.getPatternDebug(msg.patternIndex);
				break;

			case 'getProbeData':
				this.getProbeData(msg.stateId);
				break;

			case 'getFFTProbeData':
				this.getFFTProbeData(msg.stateId);
				break;

			case 'setInputSource':
				// Forward the host-resolved source string into the WASM module
				// so anything compile-time that wants it (logging, debugging)
				// can read it back via cedar_get_input_source().
				if (this.module && this.module._cedar_set_input_source) {
					const src = msg.source || '';
					const len = this.module.lengthBytesUTF8(src) + 1;
					const ptr = this.module._nkido_malloc(len);
					if (ptr !== 0) {
						try {
							this.module.stringToUTF8(src, ptr, len);
							this.module._cedar_set_input_source(ptr);
						} finally {
							this.module._nkido_free(ptr);
						}
					}
				}
				break;

			case 'midi':
				// Live MIDI event from the main-thread Web MIDI dispatcher
				// (prd-midi-input §4.10). The main thread has already applied
				// the route table's device + channel filter; here we just
				// push into the addressed MidiQueueState's SPSC ring.
				if (this.module && this.module._cedar_push_midi_event) {
					this.module._cedar_push_midi_event(
						msg.state_id >>> 0,
						msg.status & 0xFF,
						msg.d1 & 0xFF,
						msg.d2 & 0xFF
					);
				}
				break;

			case 'setDefaultMidiDevice':
				// Record the user-selected default MIDI device label inside
				// the WASM module. The main thread is the source of truth
				// for routing, but this lets diagnostics / state inspection
				// observe the active selection.
				if (this.module && this.module._cedar_set_default_midi_device) {
					const name = msg.device || '';
					const len = this.module.lengthBytesUTF8(name) + 1;
					const ptr = this.module._nkido_malloc(len);
					if (ptr !== 0) {
						try {
							this.module.stringToUTF8(name, ptr, len);
							this.module._cedar_set_default_midi_device(ptr);
						} finally {
							this.module._nkido_free(ptr);
						}
					}
				}
				break;
		}
	}

	/**
	 * Get required sample names from the compile result (legacy simple names)
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
	 * Get required samples with extended info (bank, name, variant)
	 * @returns {Array<{bank: string|null, name: string, variant: number, qualifiedName: string}>}
	 */
	getRequiredSamplesExtended() {
		// Check if extended API is available
		if (!this.module._akkado_get_required_samples_extended_count) {
			return [];
		}

		const count = this.module._akkado_get_required_samples_extended_count();
		const samples = [];
		for (let i = 0; i < count; i++) {
			// Get bank name (may be null for default bank)
			const bankPtr = this.module._akkado_get_required_sample_bank(i);
			const bank = bankPtr ? this.module.UTF8ToString(bankPtr) : null;

			// Get sample name
			const namePtr = this.module._akkado_get_required_sample_name(i);
			const name = namePtr ? this.module.UTF8ToString(namePtr) : '';

			// Get variant
			const variant = this.module._akkado_get_required_sample_variant(i);

			// Get qualified name for Cedar lookup
			const qualifiedPtr = this.module._akkado_get_required_sample_qualified(i);
			const qualifiedName = qualifiedPtr ? this.module.UTF8ToString(qualifiedPtr) : name;

			samples.push({ bank, name, variant, qualifiedName });
		}
		return samples;
	}

	/**
	 * Get required input source strings (one per in() call) from the compile
	 * result. Empty string means the call had no argument; the host should
	 * fall back to its UI default. Returns [] when the program does not use in().
	 * @returns {string[]}
	 */
	getRequiredInputSources() {
		if (!this.module._akkado_get_required_input_sources_count) {
			return [];
		}
		const count = this.module._akkado_get_required_input_sources_count();
		const sources = [];
		for (let i = 0; i < count; i++) {
			const ptr = this.module._akkado_get_required_input_source(i);
			sources.push(ptr ? this.module.UTF8ToString(ptr) : '');
		}
		return sources;
	}

	/**
	 * Get the list of midi() call sites from the compile result
	 * (prd-midi-input §4.7). One entry per call; the host turns this into
	 * a route table that maps device events → cedar_push_midi_event(state_id).
	 * @returns {Array<{stateId: number, kind: number, name: string, channel: number, loop: boolean, tempo: number}>}
	 */
	getRequiredMidiSources() {
		if (!this.module._akkado_get_required_midi_sources_count) {
			return [];
		}
		const count = this.module._akkado_get_required_midi_sources_count();
		const sources = [];
		for (let i = 0; i < count; i++) {
			const namePtr = this.module._akkado_get_required_midi_source_name(i);
			sources.push({
				stateId: this.module._akkado_get_required_midi_source_state_id(i) >>> 0,
				kind: this.module._akkado_get_required_midi_source_kind(i),
				name: namePtr ? this.module.UTF8ToString(namePtr) : '',
				channel: this.module._akkado_get_required_midi_source_channel(i),
				loop: this.module._akkado_get_required_midi_source_loop(i) === 1,
				tempo: this.module._akkado_get_required_midi_source_tempo(i)
			});
		}
		return sources;
	}

	/**
	 * Get the list of midi_cc() routes from the compile result
	 * (prd-midi-input §4.8). One entry per call site; the host turns this into
	 * a per-device route table that maps incoming CC/PB/AT events →
	 * cedar_set_param_slew(name, value, slew_ms). cc_num sentinels: 0..127 =
	 * MIDI CC#; -1 = pitch-bend; -2 = channel aftertouch.
	 * @returns {Array<{paramName: string, ccNum: number, channel: number,
	 *                  scale: number, bias: number, slewMs: number}>}
	 */
	getRequiredMidiCcRoutes() {
		if (!this.module._akkado_get_required_midi_cc_routes_count) {
			return [];
		}
		const count = this.module._akkado_get_required_midi_cc_routes_count();
		const routes = [];
		for (let i = 0; i < count; i++) {
			const namePtr = this.module._akkado_get_required_midi_cc_route_name(i);
			routes.push({
				paramName: namePtr ? this.module.UTF8ToString(namePtr) : '',
				ccNum:   this.module._akkado_get_required_midi_cc_route_cc(i) | 0,
				channel: this.module._akkado_get_required_midi_cc_route_channel(i) | 0,
				scale:   this.module._akkado_get_required_midi_cc_route_scale(i),
				bias:    this.module._akkado_get_required_midi_cc_route_bias(i),
				slewMs:  this.module._akkado_get_required_midi_cc_route_slew_ms(i)
			});
		}
		return routes;
	}

	/**
	 * Get required SoundFont files from the compile result
	 * @returns {Array<{filename: string, preset: number}>}
	 */
	getRequiredSoundFonts() {
		if (!this.module._akkado_get_required_soundfonts_count) {
			return [];
		}

		const count = this.module._akkado_get_required_soundfonts_count();
		const soundfonts = [];
		for (let i = 0; i < count; i++) {
			const filenamePtr = this.module._akkado_get_required_soundfont_filename(i);
			const filename = filenamePtr ? this.module.UTF8ToString(filenamePtr) : '';
			const preset = this.module._akkado_get_required_soundfont_preset(i);
			soundfonts.push({ filename, preset });
		}
		return soundfonts;
	}

	/**
	 * Get required wavetable banks from the compile result. The compiler
	 * assigns sequential bank IDs in source order (the index here matches
	 * the runtime registry's slot ID after the host loads them in this
	 * same order).
	 * @returns {Array<{name: string, path: string, id: number}>}
	 */
	getRequiredWavetables() {
		if (!this.module._akkado_get_required_wavetables_count) {
			return [];
		}
		const count = this.module._akkado_get_required_wavetables_count();
		const wavetables = [];
		for (let i = 0; i < count; i++) {
			const namePtr = this.module._akkado_get_required_wavetable_name(i);
			const name = namePtr ? this.module.UTF8ToString(namePtr) : '';
			const pathPtr = this.module._akkado_get_required_wavetable_path(i);
			const path = pathPtr ? this.module.UTF8ToString(pathPtr) : '';
			wavetables.push({ name, path, id: i });
		}
		return wavetables;
	}

	/**
	 * Get URI declarations from the compile result. The kind tag mirrors
	 * akkado::UriKind: 0=SampleBank, 1=SoundFont, 2=Wavetable, 3=Sample.
	 * The host iterates this list in order and dispatches each URI to the
	 * appropriate registry; bytecode swap blocks until every URI resolves.
	 * @returns {Array<{uri: string, kind: number}>}
	 */
	getRequiredUris() {
		if (!this.module._akkado_get_required_uri_count) {
			return [];
		}
		const count = this.module._akkado_get_required_uri_count();
		const uris = [];
		for (let i = 0; i < count; i++) {
			const uriPtr = this.module._akkado_get_required_uri(i);
			const uri = uriPtr ? this.module.UTF8ToString(uriPtr) : '';
			const kind = this.module._akkado_get_required_uri_kind(i);
			uris.push({ uri, kind });
		}
		return uris;
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
	 * Extract all visualization declarations from compile result into JS objects.
	 * Must be called immediately after compilation, before any memory growth.
	 */
	extractVizDecls() {
		const count = this.module._akkado_get_viz_count ? this.module._akkado_get_viz_count() : 0;
		const vizDecls = [];

		for (let i = 0; i < count; i++) {
			const namePtr = this.module._akkado_get_viz_name(i);
			const name = namePtr ? this.module.UTF8ToString(namePtr) : '';
			const type = this.module._akkado_get_viz_type(i);
			const stateId = this.module._akkado_get_viz_state_id(i);
			const sourceOffset = this.module._akkado_get_viz_source_offset(i);
			const sourceLength = this.module._akkado_get_viz_source_length(i);
			const patternIndex = this.module._akkado_get_viz_pattern_index(i);

			// Extract options JSON if present
			let options = null;
			const optionsPtr = this.module._akkado_get_viz_options ? this.module._akkado_get_viz_options(i) : null;
			if (optionsPtr) {
				const optionsStr = this.module.UTF8ToString(optionsPtr);
				try {
					options = JSON.parse(optionsStr);
				} catch (e) {
					console.warn('[CedarProcessor] Failed to parse viz options:', e);
				}
			}

			vizDecls.push({
				name,
				type,  // 0=PianoRoll, 1=Oscilloscope, 2=Waveform, 3=Spectrum
				stateId,
				sourceOffset,
				sourceLength,
				patternIndex,  // Index into stateInits array for piano roll, or -1
				options
			});
		}

		return vizDecls;
	}

	/**
	 * Extract builtin variable overrides from compile result into JS objects.
	 * Must be called immediately after compilation, before any memory growth.
	 */
	extractBuiltinVarOverrides() {
		if (!this.module._akkado_get_builtin_var_override_count) return [];
		const count = this.module._akkado_get_builtin_var_override_count();
		const overrides = [];
		for (let i = 0; i < count; i++) {
			const namePtr = this.module._akkado_get_builtin_var_override_name(i);
			const name = namePtr ? this.module.UTF8ToString(namePtr) : '';
			const value = this.module._akkado_get_builtin_var_override_value(i);
			overrides.push({ name, value });
		}
		return overrides;
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
			const cycleLength = this.module._akkado_get_state_init_cycle_length(i);

			stateInits.push({
				stateId,
				type,
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
		const namePtr = this.module._nkido_malloc(nameLen);
		if (!namePtr) return 0;

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);
			return this.module._cedar_get_sample_id(namePtr);
		} finally {
			this.module._nkido_free(namePtr);
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
			const sourcePtr = this.module._nkido_malloc(allocLen);
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

					// Extract required sample names (both legacy and extended)
					const requiredSamples = this.getRequiredSamples();
					const requiredSamplesExtended = this.getRequiredSamplesExtended();
					const requiredSoundfonts = this.getRequiredSoundFonts();
					const requiredWavetables = this.getRequiredWavetables();
					const requiredUris = this.getRequiredUris();
					const requiredInputSources = this.getRequiredInputSources();
					const requiredMidiSources = this.getRequiredMidiSources();
					const requiredMidiCcRoutes = this.getRequiredMidiCcRoutes();

					// Extract all state initialization data
					const stateInits = this.extractStateInits();

					// Extract parameter declarations for UI generation
					const paramDecls = this.extractParamDecls();

					// Extract visualization declarations for UI generation
					const vizDecls = this.extractVizDecls();

					// Extract builtin variable overrides (bpm, sr)
					const builtinVarOverrides = this.extractBuiltinVarOverrides();

					// CRITICAL: Clear the compile result NOW, before returning
					// This ensures we don't have stale pointers when memory grows
					this.module._akkado_clear_result();

					// Apply builtin variable overrides
					for (const override of builtinVarOverrides) {
						if (override.name === 'bpm') {
							this.module._cedar_set_bpm(override.value);
						}
						// Write to EnvMap so ENV_GET reads the value
						this.setParam('__' + override.name, override.value);
					}

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
					this.pendingProgram = { bytecode, stateInits, requiredSamples, requiredSamplesExtended };

					console.log('[CedarProcessor] Compiled successfully, bytecode size:', bytecodeSize,
						'required samples:', requiredSamples, 'extended samples:', requiredSamplesExtended.length,
						'required soundfonts:', requiredSoundfonts.length,
						'required wavetables:', requiredWavetables.length,
						'state inits:', stateInits.length,
						'param decls:', paramDecls.length, 'viz decls:', vizDecls.length,
						'unique states:', disassembly?.summary?.uniqueStateIds ?? 'N/A');

					this.port.postMessage({
						type: 'compiled',
						success: true,
						bytecodeSize,
						requiredSamples,
						requiredSamplesExtended,
						requiredSoundfonts,
						requiredWavetables,
						requiredUris,
						requiredInputSources,
						requiredMidiSources,
						requiredMidiCcRoutes,
						paramDecls,
						vizDecls,
						builtinVarOverrides,
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
				this.module._nkido_free(sourcePtr);
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
	loadCompiledProgram(refreshId) {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized', refreshId });
			return;
		}

		if (refreshId !== undefined) {
			this.pendingLoadRefreshId = refreshId;
		}

		if (!this.pendingProgram) {
			this.port.postMessage({ type: 'error', message: 'No pending program to load', refreshId: this.pendingLoadRefreshId });
			return;
		}

		const { bytecode, stateInits } = this.pendingProgram;

		console.log('[CedarProcessor] Loading program, bytecode size:', bytecode.length,
			'state inits:', stateInits.length);

		// Allocate and copy bytecode to WASM memory
		let bytecodePtr = this.module._nkido_malloc(bytecode.length);
		if (bytecodePtr === 0) {
			this.port.postMessage({ type: 'error', message: 'Failed to allocate bytecode', refreshId: this.pendingLoadRefreshId });
			return;
		}

		try {
			this.writeByteArray(bytecodePtr, bytecode);

			// Patch direct sample("name") references in the bytecode using the
			// currently loaded sample bank. Codegen emits a PUSH_CONST 0 placeholder
			// for each such call and records its instruction index; this walks the
			// scalar_sample_mappings ledger and writes the resolved sample ID into
			// each PUSH_CONST's state_id immediate before the VM sees the program.
			if (this.module._akkado_patch_sample_ids_in_bytecode) {
				this.module._akkado_patch_sample_ids_in_bytecode(bytecodePtr, bytecode.length);
			}

			// Load program into Cedar VM
			const result = this.module._cedar_load_program(bytecodePtr, bytecode.length);

			if (result === 1) {
				// SlotBusy - all slots are occupied (crossfade in progress).
				// Keep pendingProgram and arm a retry from process(); the audio
				// thread is the only thing that can free a slot.
				this.module._nkido_free(bytecodePtr);
				bytecodePtr = 0; // Prevent double-free in finally
				this.pendingLoadRetry = true;
				return;
			}

			if (result !== 0) {
				console.error('[CedarProcessor] Load failed with code:', result);
				this.pendingProgram = null; // Clear on permanent error
				this.pendingLoadRetry = false;
				this.port.postMessage({ type: 'error', message: `Load failed with code ${result}`, refreshId: this.pendingLoadRefreshId });
				return;
			}

			// Apply state initializations using WASM function
			// First resolve sample names to IDs (uses samples already loaded in sample bank)
			this.module._akkado_resolve_sample_ids();
			// Then apply all state inits (SequenceProgram types)
			const stateInitsApplied = this.module._cedar_apply_state_inits();
			if (stateInitsApplied > 0) {
				console.log('[CedarProcessor] Applied', stateInitsApplied, 'state initializations');
			}
			// Initialize one MidiQueueState per midi() call site
			// (prd-midi-input §4.7). Must run after load_program so the new
			// program's state pool is the active one.
			if (this.module._cedar_apply_midi_sources) {
				const midiApplied = this.module._cedar_apply_midi_sources();
				if (midiApplied > 0) {
					console.log('[CedarProcessor] Initialized', midiApplied, 'MIDI queue states');
				}
			}

			// Diagnostic logging after load
			const hasPendingSwap = this.module._cedar_debug_has_pending_swap?.() ?? 'N/A';
			const swapCount = this.module._cedar_debug_swap_count?.() ?? 'N/A';
			const currentInst = this.module._cedar_debug_current_slot_instruction_count?.() ?? 'N/A';
			console.log(`[CedarProcessor] Program loaded successfully: pendingSwap=${hasPendingSwap} swapCount=${swapCount} instructions=${currentInst}`);
			this.pendingProgram = null; // Clear only on success
			this.pendingLoadRetry = false;
			this.port.postMessage({ type: 'programLoaded', refreshId: this.pendingLoadRefreshId });

		} finally {
			// Guard against double-free (bytecodePtr set to 0 on SlotBusy)
			if (bytecodePtr) {
				this.module._nkido_free(bytecodePtr);
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
		const ptr = this.module._nkido_malloc(bytecode.length);
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
			this.module._nkido_free(ptr);
		}
	}

	setParam(name, value, slewMs) {
		if (!this.module) return;

		// Allocate name string in WASM memory
		const len = this.module.lengthBytesUTF8(name) + 1;
		const ptr = this.module._nkido_malloc(len);
		if (ptr === 0) return;

		try {
			this.module.stringToUTF8(name, ptr, len);

			if (slewMs !== undefined) {
				this.module._cedar_set_param_slew(ptr, value, slewMs);
			} else {
				this.module._cedar_set_param(ptr, value);
			}
		} finally {
			this.module._nkido_free(ptr);
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
		const namePtr = this.module._nkido_malloc(nameLen);
		if (namePtr === 0) {
			this.port.postMessage({ type: 'error', message: 'Failed to allocate name' });
			return;
		}

		// Allocate audio data
		const audioPtr = this.module._nkido_malloc(audioData.length * 4); // 4 bytes per float
		if (audioPtr === 0) {
			this.module._nkido_free(namePtr);
			this.port.postMessage({ type: 'error', message: 'Failed to allocate audio data' });
			return;
		}

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);

			// Copy audio data to WASM memory using fresh heap view
			this.writeFloatArray(audioPtr, audioData);

			// Load sample
			const sampleId = this.module._cedar_load_sample(namePtr, audioPtr, audioData.length, channels, sampleRate);

			if (sampleId >= 0) {
				console.log('[CedarProcessor] Sample loaded successfully, ID:', sampleId);
				this.port.postMessage({ type: 'sampleLoaded', name, sampleId });
			} else {
				console.error('[CedarProcessor] Failed to load sample');
				this.port.postMessage({ type: 'error', message: 'Failed to load sample: ' + name });
			}
		} finally {
			this.module._nkido_free(namePtr);
			this.module._nkido_free(audioPtr);
		}
	}

	/**
	 * Load a sample from audio data in any supported format (WAV, OGG, FLAC, MP3).
	 * Uses cedar_load_audio_data which auto-detects format from magic bytes
	 * and decodes entirely in C++/WASM.
	 */
	loadSampleAudio(name, audioData) {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized' });
			return;
		}

		console.log('[CedarProcessor] Loading audio sample:', name, 'size:', audioData.byteLength);

		// Allocate name string
		const nameLen = this.module.lengthBytesUTF8(name) + 1;
		const namePtr = this.module._nkido_malloc(nameLen);
		if (namePtr === 0) {
			this.port.postMessage({ type: 'error', message: 'Failed to allocate name' });
			return;
		}

		// Allocate audio data
		const dataArray = new Uint8Array(audioData);
		const dataPtr = this.module._nkido_malloc(dataArray.length);
		if (dataPtr === 0) {
			this.module._nkido_free(namePtr);
			this.port.postMessage({ type: 'error', message: 'Failed to allocate audio data' });
			return;
		}

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);
			this.writeByteArray(dataPtr, dataArray);

			const sampleId = this.module._cedar_load_audio_data(namePtr, dataPtr, dataArray.length);

			if (sampleId >= 0) {
				console.log('[CedarProcessor] Audio sample loaded successfully, ID:', sampleId);
				this.port.postMessage({ type: 'sampleLoaded', name, sampleId });
			} else {
				console.error('[CedarProcessor] Failed to load audio sample');
				this.port.postMessage({ type: 'error', message: 'Failed to load audio sample: ' + name });
			}
		} finally {
			this.module._nkido_free(namePtr);
			this.module._nkido_free(dataPtr);
		}
	}

	/**
	 * Load a SoundFont (SF2) file.
	 * Parses in C++/WASM, extracts samples into SampleBank, returns preset info.
	 */
	loadSoundFont(name, audioData) {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized' });
			return;
		}

		console.log('[CedarProcessor] Loading SoundFont:', name, 'size:', audioData.byteLength);

		// Allocate name string
		const nameLen = this.module.lengthBytesUTF8(name) + 1;
		const namePtr = this.module._nkido_malloc(nameLen);
		if (namePtr === 0) {
			this.port.postMessage({ type: 'soundFontLoaded', name, success: false, error: 'Failed to allocate name' });
			return;
		}

		// Allocate SF2 data
		const dataArray = new Uint8Array(audioData);
		const dataPtr = this.module._nkido_malloc(dataArray.length);
		if (dataPtr === 0) {
			const heapMB = this.module.wasmMemory
				? (this.module.wasmMemory.buffer.byteLength / 1048576).toFixed(0)
				: '?';
			const needMB = (dataArray.length / 1048576).toFixed(1);
			console.error(`[CedarProcessor] Failed to allocate ${needMB} MB for SF2 data (heap: ${heapMB} MB)`);
			this.module._nkido_free(namePtr);
			this.port.postMessage({ type: 'soundFontLoaded', name, success: false, error: `Out of memory: could not allocate ${needMB} MB for SoundFont data` });
			return;
		}

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);
			this.writeByteArray(dataPtr, dataArray);

			const sfId = this.module._cedar_load_soundfont(namePtr, dataPtr, dataArray.length);

			if (sfId >= 0) {
				// Get preset list
				const presetCount = this.module._cedar_soundfont_preset_count(sfId);
				const presetsJsonPtr = this.module._cedar_soundfont_presets_json(sfId);
				let presets = [];
				if (presetsJsonPtr) {
					try {
						presets = JSON.parse(this.module.UTF8ToString(presetsJsonPtr));
					} catch (e) {
						console.warn('[CedarProcessor] Failed to parse presets JSON:', e);
					}
				}

				console.log('[CedarProcessor] SoundFont loaded, ID:', sfId, 'presets:', presetCount);
				this.port.postMessage({
					type: 'soundFontLoaded',
					name,
					success: true,
					sfId,
					presetCount,
					presets
				});
			} else {
				console.error('[CedarProcessor] Failed to load SoundFont:', name);
				this.port.postMessage({
					type: 'soundFontLoaded',
					name,
					success: false,
					error: 'SF2 parsing failed'
				});
			}
		} finally {
			this.module._nkido_free(namePtr);
			this.module._nkido_free(dataPtr);
		}
	}

	/**
	 * Load a `.mid` file (prd-midi-input Phase 5). Parses the bytes in the
	 * VM's name-keyed registry; subsequent cedar_apply_midi_sources() calls
	 * (run inside loadCompiledProgram) attach the parsed sequence to each
	 * matching MidiQueueState. Mirrors loadSoundFont's lifecycle.
	 */
	loadMidiFile(name, audioData) {
		if (!this.module) {
			this.port.postMessage({ type: 'midiFileLoaded', name, success: false, error: 'Module not initialized' });
			return;
		}
		if (!this.module._cedar_load_midi_file) {
			this.port.postMessage({ type: 'midiFileLoaded', name, success: false, error: 'cedar_load_midi_file not exported' });
			return;
		}

		const nameLen = this.module.lengthBytesUTF8(name) + 1;
		const namePtr = this.module._nkido_malloc(nameLen);
		if (namePtr === 0) {
			this.port.postMessage({ type: 'midiFileLoaded', name, success: false, error: 'Failed to allocate name' });
			return;
		}

		const dataArray = new Uint8Array(audioData);
		const dataPtr = this.module._nkido_malloc(dataArray.length);
		if (dataPtr === 0) {
			this.module._nkido_free(namePtr);
			const needKB = (dataArray.length / 1024).toFixed(1);
			this.port.postMessage({ type: 'midiFileLoaded', name, success: false, error: `Out of memory: could not allocate ${needKB} KB for MIDI file data` });
			return;
		}

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);
			this.writeByteArray(dataPtr, dataArray);
			const result = this.module._cedar_load_midi_file(namePtr, dataPtr, dataArray.length);
			if (result === 0) {
				this.port.postMessage({ type: 'midiFileLoaded', name, success: true });
			} else {
				this.port.postMessage({
					type: 'midiFileLoaded',
					name,
					success: false,
					error: 'MIDI parse failed (unsupported format, malformed bytes, or arena exhausted)'
				});
			}
		} finally {
			this.module._nkido_free(namePtr);
			this.module._nkido_free(dataPtr);
		}
	}

	/**
	 * Load a wavetable bank from WAV file bytes. The bank is preprocessed
	 * (FFT mip pyramid, fundamental-phase alignment, RMS normalization)
	 * inside the WASM call. The returned bankId equals the registry slot,
	 * which matches the compiler's source-order ID provided the host
	 * called clearWavetables before loading required_wavetables in order.
	 */
	loadWavetable(name, wavData) {
		if (!this.module) {
			this.port.postMessage({ type: 'wavetableLoaded', name, success: false, error: 'Module not initialized' });
			return;
		}

		const nameLen = this.module.lengthBytesUTF8(name) + 1;
		const namePtr = this.module._nkido_malloc(nameLen);
		if (namePtr === 0) {
			this.port.postMessage({ type: 'wavetableLoaded', name, success: false, error: 'Failed to allocate name' });
			return;
		}

		const dataArray = new Uint8Array(wavData);
		const dataPtr = this.module._nkido_malloc(dataArray.length);
		if (dataPtr === 0) {
			const needKB = (dataArray.length / 1024).toFixed(0);
			this.module._nkido_free(namePtr);
			this.port.postMessage({ type: 'wavetableLoaded', name, success: false, error: `Out of memory: could not allocate ${needKB} KB for wavetable data` });
			return;
		}

		try {
			this.module.stringToUTF8(name, namePtr, nameLen);
			this.writeByteArray(dataPtr, dataArray);

			const bankId = this.module._cedar_load_wavetable_wav(namePtr, dataPtr, dataArray.length);

			if (bankId >= 0) {
				console.log('[CedarProcessor] Wavetable loaded:', name, 'ID:', bankId);
				this.port.postMessage({
					type: 'wavetableLoaded',
					name,
					success: true,
					bankId
				});
			} else {
				console.error('[CedarProcessor] Failed to load wavetable:', name);
				this.port.postMessage({
					type: 'wavetableLoaded',
					name,
					success: false,
					error: 'Wavetable preprocessing failed (must be mono, length multiple of 2048)'
				});
			}
		} finally {
			this.module._nkido_free(namePtr);
			this.module._nkido_free(dataPtr);
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
	 * Get the analyzer-driven shape index for editor autocomplete.
	 * Phase 2 records-system-unification PRD §5.2.
	 * @param {number|undefined} requestId - correlator passed back unchanged
	 * @param {string} source - editor buffer
	 * @param {number} cursor - UTF-8 byte offset; pass 0xFFFFFFFF to skip patternHole resolution
	 */
	getShapeIndex(requestId, source, cursor) {
		if (!this.module) {
			this.port.postMessage({
				type: 'shapeIndex',
				requestId,
				success: false,
				error: 'Module not initialized'
			});
			return;
		}
		let ptr = 0;
		try {
			const text = source || '';
			const utf8ByteLen = this.module.lengthBytesUTF8(text);
			ptr = this.module._nkido_malloc(utf8ByteLen + 1);
			if (ptr === 0) {
				this.port.postMessage({
					type: 'shapeIndex',
					requestId,
					success: false,
					error: 'allocation failed'
				});
				return;
			}
			this.module.stringToUTF8(text, ptr, utf8ByteLen + 1);
			const cursorVal = (typeof cursor === 'number' && cursor >= 0)
				? cursor >>> 0
				: 0xFFFFFFFF;
			const jsonPtr = this.module._akkado_get_shape_index(ptr, utf8ByteLen, cursorVal);
			if (!jsonPtr) {
				this.port.postMessage({
					type: 'shapeIndex',
					requestId,
					success: false,
					error: 'shape_index_json returned null'
				});
				return;
			}
			const jsonStr = this.module.UTF8ToString(jsonPtr);
			const data = JSON.parse(jsonStr);
			this.port.postMessage({
				type: 'shapeIndex',
				requestId,
				success: true,
				data
			});
		} catch (err) {
			this.port.postMessage({
				type: 'shapeIndex',
				requestId,
				success: false,
				error: String(err)
			});
		} finally {
			if (ptr !== 0) this.module._nkido_free(ptr);
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

	/**
	 * Get probe data for a visualization (oscilloscope, waveform, spectrum)
	 * @param {number} stateId - The probe's state_id
	 */
	getProbeData(stateId) {
		if (!this.module) {
			this.port.postMessage({
				type: 'probeData',
				stateId,
				samples: null
			});
			return;
		}

		try {
			// Check if WASM exports are available
			if (!this.module._cedar_get_probe_sample_count || !this.module._cedar_get_probe_data) {
				this.port.postMessage({
					type: 'probeData',
					stateId,
					samples: null,
					error: 'Probe exports not available'
				});
				return;
			}

			const sampleCount = this.module._cedar_get_probe_sample_count(stateId);
			if (sampleCount === 0) {
				this.port.postMessage({
					type: 'probeData',
					stateId,
					samples: null
				});
				return;
			}

			const ptr = this.module._cedar_get_probe_data(stateId);
			if (!ptr) {
				this.port.postMessage({
					type: 'probeData',
					stateId,
					samples: null
				});
				return;
			}

			// Copy the data from WASM memory
			const samples = this.extractFloatArray(ptr, sampleCount);

			this.port.postMessage({
				type: 'probeData',
				stateId,
				samples
			});
		} catch (err) {
			this.port.postMessage({
				type: 'probeData',
				stateId,
				samples: null,
				error: String(err)
			});
		}
	}

	/**
	 * Get FFT probe data for waterfall/spectrum visualization
	 * @param {number} stateId - The FFT probe's state_id
	 */
	getFFTProbeData(stateId) {
		if (!this.module) {
			this.port.postMessage({
				type: 'fftProbeData',
				stateId,
				magnitudes: null,
				binCount: 0,
				frameCounter: 0
			});
			return;
		}

		try {
			if (!this.module._cedar_get_fft_frame_counter ||
				!this.module._cedar_get_fft_magnitudes ||
				!this.module._cedar_get_fft_bin_count) {
				this.port.postMessage({
					type: 'fftProbeData',
					stateId,
					magnitudes: null,
					binCount: 0,
					frameCounter: 0
				});
				return;
			}

			const frameCounter = this.module._cedar_get_fft_frame_counter(stateId);
			const binCount = this.module._cedar_get_fft_bin_count(stateId);

			if (binCount === 0) {
				this.port.postMessage({
					type: 'fftProbeData',
					stateId,
					magnitudes: null,
					binCount: 0,
					frameCounter
				});
				return;
			}

			const ptr = this.module._cedar_get_fft_magnitudes(stateId);
			if (!ptr) {
				this.port.postMessage({
					type: 'fftProbeData',
					stateId,
					magnitudes: null,
					binCount,
					frameCounter
				});
				return;
			}

			const magnitudes = this.extractFloatArray(ptr, binCount);

			this.port.postMessage({
				type: 'fftProbeData',
				stateId,
				magnitudes,
				binCount,
				frameCounter
			});
		} catch (err) {
			this.port.postMessage({
				type: 'fftProbeData',
				stateId,
				magnitudes: null,
				binCount: 0,
				frameCounter: 0,
				error: String(err)
			});
		}
	}

	/**
	 * Get detailed pattern debug info (AST, sequences, events)
	 * @param {number} patternIndex - Pattern index
	 */
	getPatternDebug(patternIndex) {
		if (!this.module) {
			this.port.postMessage({
				type: 'patternDebug',
				patternIndex,
				success: false,
				error: 'Module not initialized'
			});
			return;
		}

		try {
			const jsonPtr = this.module._akkado_get_pattern_debug_json(patternIndex);
			if (!jsonPtr) {
				this.port.postMessage({
					type: 'patternDebug',
					patternIndex,
					success: false,
					error: 'Failed to get pattern debug JSON'
				});
				return;
			}

			const jsonStr = this.module.UTF8ToString(jsonPtr);
			const data = JSON.parse(jsonStr);

			this.port.postMessage({
				type: 'patternDebug',
				patternIndex,
				success: true,
				data
			});
		} catch (err) {
			this.port.postMessage({
				type: 'patternDebug',
				patternIndex,
				success: false,
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
			return true;
		}

		// Retry a SlotBusy load. The audio thread is the only thing that can
		// free a slot (by advancing the crossfade), so retry from here every
		// block until the load succeeds. Crossfades complete in 3-5 blocks.
		if (this.pendingLoadRetry && this.pendingProgram) {
			this.pendingLoadRetry = false;
			this.loadCompiledProgram();
		}

		// Initialize diagnostic counters
		if (!this._diagInit) {
			this._diagInit = true;
			this._silentBlocks = 0;
			this._wasSilent = false;
			this._nanCount = 0;
			this._nanLogged = false;
		}

		// Periodic diagnostic logging (every 1000 blocks = ~2.7s at 48kHz)
		this.blockCount = (this.blockCount || 0) + 1;
		if (this.blockCount % 1000 === 0) {
			const hasProgram = this.module._cedar_has_program();
			const isCrossfading = this.module._cedar_is_crossfading();
			const hasPendingSwap = this.module._cedar_debug_has_pending_swap?.() ?? 'N/A';
			const swapCount = this.module._cedar_debug_swap_count?.() ?? 'N/A';
			const currentInst = this.module._cedar_debug_current_slot_instruction_count?.() ?? 'N/A';
			console.log(`[CedarProcessor] block=${this.blockCount} hasProgram=${hasProgram} crossfading=${isCrossfading} pendingSwap=${hasPendingSwap} swaps=${swapCount} instructions=${currentInst} peakLevel=${this._lastPeak?.toFixed(4) ?? '?'} nanCount=${this._nanCount} silentBlocks=${this._silentBlocks}`);
		}

		// Forward live audio input (in() opcode) to the WASM heap before
		// processing. inputs[0] is the first input port — when the AudioWorklet
		// has nothing connected, it's an empty array. inputs[0][0] is the
		// left/mono channel; inputs[0][1] (if present) is the right channel.
		// Mono sources are duplicated into both L and R per the audio-input
		// PRD §4.5 contract.
		const input = inputs && inputs[0];
		const hasInput = input && input.length > 0 &&
			input[0] && input[0].length > 0;
		if (hasInput && this.inputLeftPtr && this.inputRightPtr &&
			this.module._cedar_set_input_active) {
			const ch0 = input[0];
			const ch1 = (input.length > 1 && input[1] && input[1].length > 0)
				? input[1] : ch0;  // mono → duplicate to right
			const n = Math.min(ch0.length, this.blockSize);
			if (this.module.wasmMemory) {
				const heap = new Float32Array(this.module.wasmMemory.buffer);
				const lIdx = this.inputLeftPtr / 4;
				const rIdx = this.inputRightPtr / 4;
				for (let i = 0; i < n; i++) {
					heap[lIdx + i] = ch0[i];
					heap[rIdx + i] = ch1[i];
				}
				// Zero remaining samples if input was shorter than blockSize.
				for (let i = n; i < this.blockSize; i++) {
					heap[lIdx + i] = 0;
					heap[rIdx + i] = 0;
				}
			} else if (this.module.HEAPF32) {
				const lIdx = this.inputLeftPtr / 4;
				const rIdx = this.inputRightPtr / 4;
				for (let i = 0; i < n; i++) {
					this.module.HEAPF32[lIdx + i] = ch0[i];
					this.module.HEAPF32[rIdx + i] = ch1[i];
				}
				for (let i = n; i < this.blockSize; i++) {
					this.module.HEAPF32[lIdx + i] = 0;
					this.module.HEAPF32[rIdx + i] = 0;
				}
			}
			this.module._cedar_set_input_active(1);
		} else if (this.module._cedar_set_input_active) {
			this.module._cedar_set_input_active(0);
		}

		// Process one block with Cedar VM
		// Cedar handles crossfade internally when programs are swapped
		this.module._cedar_process_block();

		// Copy output from WASM memory
		// outputLeftPtr and outputRightPtr are byte offsets, convert to float index
		const leftFloatIdx = this.outputLeftPtr / 4;
		const rightFloatIdx = this.outputRightPtr / 4;

		let blockNanCount = 0;

		if (this.module.wasmMemory) {
			// Always get fresh heap view - stale views cause corruption after memory growth
			const heap = new Float32Array(this.module.wasmMemory.buffer);
			for (let i = 0; i < this.blockSize && i < outLeft.length; i++) {
				let l = heap[leftFloatIdx + i];
				let r = heap[rightFloatIdx + i];
				if (!isFinite(l)) { blockNanCount++; l = 0; }
				if (!isFinite(r)) { blockNanCount++; r = 0; }
				outLeft[i] = l;
				outRight[i] = r;
			}
		} else if (this.module.HEAPF32) {
			for (let i = 0; i < this.blockSize && i < outLeft.length; i++) {
				let l = this.module.HEAPF32[leftFloatIdx + i];
				let r = this.module.HEAPF32[rightFloatIdx + i];
				if (!isFinite(l)) { blockNanCount++; l = 0; }
				if (!isFinite(r)) { blockNanCount++; r = 0; }
				outLeft[i] = l;
				outRight[i] = r;
			}
		} else if (this.module.getValue) {
			for (let i = 0; i < this.blockSize && i < outLeft.length; i++) {
				let l = this.module.getValue(this.outputLeftPtr + i * 4, 'float');
				let r = this.module.getValue(this.outputRightPtr + i * 4, 'float');
				if (!isFinite(l)) { blockNanCount++; l = 0; }
				if (!isFinite(r)) { blockNanCount++; r = 0; }
				outLeft[i] = l;
				outRight[i] = r;
			}
		}

		// NaN/Inf detection
		if (blockNanCount > 0) {
			this._nanCount += blockNanCount;
			if (!this._nanLogged) {
				this._nanLogged = true;
				console.warn(`[CedarProcessor] NaN/Inf detected in output (${blockNanCount} samples in block ${this.blockCount})`);
			}
		}

		// Silence detection: track peak level and consecutive silent blocks
		let peak = 0;
		for (let i = 0; i < outLeft.length; i++) {
			const absL = Math.abs(outLeft[i]);
			const absR = Math.abs(outRight[i]);
			if (absL > peak) peak = absL;
			if (absR > peak) peak = absR;
		}
		this._lastPeak = peak;

		if (peak < 1e-10) {
			this._silentBlocks++;
			if (this._silentBlocks === 100 && !this._wasSilent) {
				this._wasSilent = true;
				console.warn(`[CedarProcessor] Output silent for ${this._silentBlocks} blocks (started at block ${this.blockCount - 100}). nanCount=${this._nanCount}`);
			}
		} else {
			if (this._wasSilent) {
				console.log(`[CedarProcessor] Audio recovered after ${this._silentBlocks} silent blocks (block ${this.blockCount})`);
			}
			this._silentBlocks = 0;
			this._wasSilent = false;
			this._nanLogged = false; // Reset so we log again on next NaN burst
		}

		return true;
	}
}

registerProcessor('cedar-processor', CedarProcessor);
