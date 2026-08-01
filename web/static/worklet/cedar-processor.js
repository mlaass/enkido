/**
 * Cedar AudioWorklet Processor
 *
 * Runs in the AudioWorklet thread and processes audio using the Cedar VM WASM module.
 * The WASM module code and binary are sent from the main thread since AudioWorklets
 * have limited API access (no fetch, no importScripts for cross-origin).
 *
 * Worklet thread contract — see docs/prd-compile-off-audio-thread.md §1.
 * The audio thread runs `process()` every 2.67 ms (128 samples @ 48 kHz); a
 * single blocking message handler starves the audio output for its full
 * duration. To keep that from happening, this worklet may only execute:
 *
 *   - `_cedar_process_block` (audio rendering)
 *   - `_cedar_set_block_table` + `_cedar_load_program` (fast, fixed-cost)
 *   - the four `*_from_buffer` apply paths and
 *     `_akkado_patch_sample_ids_in_bytecode_from_buffer`
 *   - parameter / MIDI / CC poke-throughs
 *
 * Anything CPU-heavy — `akkado_compile`, metadata extraction, fetch /
 * decode, string parsing, JSON walking — runs in the compile worker
 * (web/src/lib/audio/compile.worker.ts). Do not reintroduce a compile
 * handler here. If a new code path needs the AudioWorklet to do work
 * beyond the list above, it almost certainly belongs in the worker or
 * on the main thread.
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
		// Set when _cedar_load_program returned SlotBusy. Holds the JS-side
		// buffers (bytecode, stateInits, midiSources, blockTable) so the
		// next process() block can replay loadProgram() once the swap slot
		// frees up. Presence of pendingProgram is the retry signal; we no
		// longer keep a separate pendingLoadRetry flag.
		this.pendingProgram = null;

		// PRD prd-midi-input §7.4: file-CC dispatch plan, pushed by the main
		// thread via `setFileCcPlan` whenever apply_midi_route_plan runs. We
		// drain pending file CCs after every _cedar_process_block and route
		// them through this in-worklet CC table — same scale/bias math as
		// midi-input.svelte.ts's live-CC dispatch, no IPC hop per event.
		this.fileMidiStateIds = [];     // uint32[]
		this.fileCcRoutes = [];         // [{paramName, ccNum, channel, scale, bias, slewMs}]
		this.fileCcScratchPtr = 0;      // WASM heap pointer; allocated lazily
		this.fileCcScratchCap = 0;      // events that fit in scratchPtr (4 bytes each)

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
			case 'loadProgram':
				// New (PRD prd-compile-off-audio-thread): packed buffers
				// produced by the compile worker travel as transferable
				// typed arrays. The legacy 'compile' / 'loadCompiledProgram'
				// flow is intentionally gone — compile lives in the worker.
				this.loadProgram(msg);
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

			// 'getShapeIndex' removed (hardening Phase 8 / PRD-2): the shape
			// index is serialized from compile artifacts in the compile
			// worker — no parsing on the audio thread.

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

			case 'setFileCcPlan':
				// PRD §7.4: main thread publishes the list of File-kind midi
				// state IDs plus the CC route table. We drain pending CCs from
				// each state after every process_block and dispatch through
				// the table without an IPC hop. Idempotent — safe to call on
				// every compile.
				this.fileMidiStateIds = Array.isArray(msg.fileMidiStateIds)
					? msg.fileMidiStateIds.map((id) => id >>> 0)
					: [];
				this.fileCcRoutes = Array.isArray(msg.fileCcRoutes)
					? msg.fileCcRoutes.slice()
					: [];
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

	// getRequiredSamples / getRequiredSamplesExtended / getRequiredSoundFonts
	// / getRequiredWavetables / getRequiredUris / getRequiredInputSources /
	// getRequiredMidiSources / getRequiredMidiCcRoutes lived here pre-PRD
	// prd-compile-off-audio-thread. They walked g_compile_result, which is
	// now owned by the compile worker's WASM instance — see
	// web/src/lib/audio/compile.worker.ts. Do not reintroduce them.

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
	 * Load a compiled program from the four packed buffers produced by the
	 * compile worker (PRD prd-compile-off-audio-thread §4.2):
	 *   bytecode       — cedar::Instruction[]
	 *   stateInitsBuf  — kind=0/1/2 records (state inits + sample mappings)
	 *   midiSourcesBuf — RequiredMidiSource[] records
	 *   blockTable     — cedar::BlockEntry[] (memcpy)
	 *   mainInstCount  — main/body boundary inside the bytecode stream
	 *   builtinVarOverrides — `bpm = …` etc. applied to the live VM
	 *
	 * The worklet stages the block table, patches scalar sample IDs into the
	 * bytecode, calls cedar_load_program, then applies state inits, resolves
	 * sequence sample IDs into arena events and applies MIDI sources. If
	 * load_program returns SlotBusy (other crossfade still in flight) the
	 * JS-side buffers are parked in this.pendingProgram and the next
	 * process() block retries — single per-block retry, no multi-attempt
	 * state machine. A fresh loadProgram message that arrives while a
	 * previous one is parked supersedes it (newest wins, matching the main
	 * thread's supersede-by-newest queueing).
	 */
	loadProgram(msg) {
		if (!this.module) {
			this.port.postMessage({ type: 'error', message: 'Module not initialized', refreshId: msg?.refreshId });
			return;
		}

		// Newest-wins: drop any earlier parked load. We held only JS refs,
		// so there are no WASM allocations to free.
		this.pendingProgram = null;

		const {
			refreshId,
			bytecode,
			stateInitsBuf,
			midiSourcesBuf,
			blockTable,
			mainInstCount,
			builtinVarOverrides
		} = msg;

		// Apply builtin variable overrides (bpm, env-map entries the compiler
		// resolved at compile time). bpm goes through the dedicated setter so
		// the cycle clock picks it up; everything else lands in the EnvMap.
		if (builtinVarOverrides) {
			for (const override of builtinVarOverrides) {
				if (override.name === 'bpm') {
					this.module._cedar_set_bpm(override.value);
				}
				this.setParam('__' + override.name, override.value);
			}
		}

		// Stage the FOREACH_EVENT / BLOCK_CALL subprogram table BEFORE
		// cedar_load_program. The VM consumes-and-clears the staged table
		// during the next load. We always stage so the main/body boundary
		// (mainInstCount) is the source of truth — even when the table is
		// empty, the boundary fixes the dispatch loop bounds.
		const blockEntryCount = blockTable && blockTable.byteLength > 0
			? blockTable.byteLength / 12
			: 0;
		let blockTablePtr = 0;
		if (blockEntryCount > 0) {
			blockTablePtr = this.module._nkido_malloc(blockTable.byteLength);
			if (blockTablePtr === 0) {
				this.port.postMessage({ type: 'error', message: 'malloc(blockTable) failed', refreshId });
				return;
			}
			this.writeByteArray(blockTablePtr, blockTable);
		}
		this.module._cedar_set_block_table(blockTablePtr, blockEntryCount, mainInstCount || 0);
		if (blockTablePtr) this.module._nkido_free(blockTablePtr);

		// All remaining WASM allocations are released before this method
		// returns (success, hard error, or SlotBusy retry). On SlotBusy the
		// JS-side typed-array refs in this.pendingProgram are what survive;
		// the retry re-allocates and re-copies into the WASM heap.
		this.runProgramLoad(bytecode, stateInitsBuf, midiSourcesBuf, refreshId);
	}

	runProgramLoad(bytecode, stateInitsBuf, midiSourcesBuf, refreshId) {
		const bcPtr = this.module._nkido_malloc(bytecode.byteLength);
		if (bcPtr === 0) {
			this.port.postMessage({ type: 'error', message: 'malloc(bytecode) failed', refreshId });
			return;
		}

		let siPtr = 0;
		let midiPtr = 0;

		try {
			this.writeByteArray(bcPtr, new Uint8Array(bytecode.buffer, bytecode.byteOffset, bytecode.byteLength));

			// stateInitsBuf is non-empty whenever any record exists; if a
			// program has no sequences / state inits / sample mappings the
			// worker still ships an 8-byte header.
			if (stateInitsBuf && stateInitsBuf.byteLength > 0) {
				siPtr = this.module._nkido_malloc(stateInitsBuf.byteLength);
				if (siPtr === 0) {
					this.port.postMessage({ type: 'error', message: 'malloc(stateInits) failed', refreshId });
					return;
				}
				this.writeByteArray(siPtr, stateInitsBuf);
			}

			// Patch scalar sample("name") PUSH_CONST immediates in the
			// bytecode using kind=2 mapping records from stateInitsBuf.
			// The helper returns the # of patches; negative on bad input,
			// which we log and ignore (other types may still be valid).
			if (siPtr) {
				this.module._akkado_patch_sample_ids_in_bytecode_from_buffer(
					bcPtr, bytecode.byteLength, siPtr, stateInitsBuf.byteLength);
			}

			const loadResult = this.module._cedar_load_program(bcPtr, bytecode.byteLength);

			if (loadResult === 1) {
				// SlotBusy: park the JS buffers and bail; the next process()
				// block retries once the audio thread has finished its
				// in-flight crossfade.
				this.pendingProgram = { bytecode, stateInitsBuf, midiSourcesBuf, refreshId };
				return;
			}

			if (loadResult !== 0) {
				console.error('[CedarProcessor] cedar_load_program failed with code', loadResult);
				this.port.postMessage({ type: 'error', message: `Load failed with code ${loadResult}`, refreshId });
				return;
			}

			// Order matters: apply state inits before resolve_sample_ids so
			// the events live in the arena when we look up bank IDs. midi
			// sources go last (they touch the new program's state pool).
			if (siPtr) {
				const applied = this.module._cedar_apply_state_inits_from_buffer(siPtr, stateInitsBuf.byteLength);
				if (applied < 0) console.error('[CedarProcessor] state-inits apply returned', applied);
				const resolved = this.module._akkado_resolve_sample_ids_from_buffer(siPtr, stateInitsBuf.byteLength);
				if (resolved < 0) console.error('[CedarProcessor] resolve sample ids returned', resolved);
			}

			if (midiSourcesBuf && midiSourcesBuf.byteLength > 0) {
				midiPtr = this.module._nkido_malloc(midiSourcesBuf.byteLength);
				if (midiPtr) {
					this.writeByteArray(midiPtr, midiSourcesBuf);
					const m = this.module._cedar_apply_midi_sources_from_buffer(midiPtr, midiSourcesBuf.byteLength);
					if (m < 0) console.error('[CedarProcessor] midi-sources apply returned', m);
				}
			}

			this.port.postMessage({ type: 'programLoaded', refreshId });
		} finally {
			if (bcPtr) this.module._nkido_free(bcPtr);
			if (siPtr) this.module._nkido_free(siPtr);
			if (midiPtr) this.module._nkido_free(midiPtr);
		}
	}

	tryParkedLoad() {
		if (!this.pendingProgram || !this.module) return;
		const p = this.pendingProgram;
		// Clear before retry so a recursive bail clears cleanly. If load
		// returns SlotBusy again, runProgramLoad re-parks via the same
		// "pendingProgram" assignment.
		this.pendingProgram = null;
		this.runProgramLoad(p.bytecode, p.stateInitsBuf, p.midiSourcesBuf, p.refreshId);
	}

	/**
	 * PRD prd-midi-input §7.4: drain pending file-CC events from every
	 * File-kind midi state and dispatch through `this.fileCcRoutes`. Mirrors
	 * the host-side `dispatch_midi_cc` math so file CCs route through the
	 * same param assignment users see for live CCs.
	 *
	 * Channel encoding matches the WASM export — `cedar_drain_pending_file_ccs`
	 * writes (status, d1, d2, channel) per event, channel 0-indexed. We add
	 * +1 before matching against route.channel (1-indexed, 0 = any).
	 */
	drainFileCcs() {
		if (!this.module || !this.module._cedar_drain_pending_file_ccs) return;

		const MAX_PER_DRAIN = 64;
		if (this.fileCcScratchPtr === 0 || this.fileCcScratchCap < MAX_PER_DRAIN) {
			if (this.fileCcScratchPtr !== 0) {
				this.module._nkido_free(this.fileCcScratchPtr);
			}
			this.fileCcScratchPtr = this.module._nkido_malloc(MAX_PER_DRAIN * 4);
			this.fileCcScratchCap = this.fileCcScratchPtr === 0 ? 0 : MAX_PER_DRAIN;
			if (this.fileCcScratchPtr === 0) return;
		}

		const heap = this.module.HEAPU8 || new Uint8Array(this.module.wasmMemory.buffer);

		for (const stateId of this.fileMidiStateIds) {
			const written = this.module._cedar_drain_pending_file_ccs(
				stateId >>> 0,
				this.fileCcScratchPtr,
				MAX_PER_DRAIN
			);
			if (written <= 0) continue;
			for (let i = 0; i < written; ++i) {
				const base = this.fileCcScratchPtr + i * 4;
				const status = heap[base];
				const d1 = heap[base + 1];
				const d2 = heap[base + 2];
				const channel = heap[base + 3];
				const ch1 = (channel & 0x0F) + 1;
				const baseStatus = status & 0xF0;
				for (const r of this.fileCcRoutes) {
					if (r.channel !== 0 && r.channel !== ch1) continue;
					let value = null;
					if (baseStatus === 0xB0) {
						if (r.ccNum < 0 || r.ccNum > 127) continue;
						if (d1 !== r.ccNum) continue;
						value = (d2 / 127) * r.scale + r.bias;
					} else if (baseStatus === 0xE0) {
						if (r.ccNum !== -1) continue;
						const v14 = (d2 << 7) | d1;
						const n = (v14 / 16383) * 2 - 1;
						value = n * (r.scale * 0.5) + (r.bias + r.scale * 0.5);
					} else if (baseStatus === 0xD0) {
						if (r.ccNum !== -2) continue;
						value = (d1 / 127) * r.scale + r.bias;
					}
					if (value === null) continue;
					this.setParam(r.paramName, value, r.slewMs);
				}
			}
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
	 * VM's name-keyed registry; subsequent
	 * _cedar_apply_midi_sources_from_buffer calls (run inside loadProgram)
	 * attach the parsed sequence to each matching MidiQueueState. Mirrors
	 * loadSoundFont's lifecycle.
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
		if (this.pendingProgram) {
			this.tryParkedLoad();
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

		// PRD prd-midi-input §7.4: drain pending file CCs and dispatch via
		// the in-worklet CC route table (set by setFileCcPlan).
		if (this.fileMidiStateIds.length > 0 && this.fileCcRoutes.length > 0) {
			this.drainFileCcs();
		}

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
