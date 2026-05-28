/// <reference lib="webworker" />

// Compile Worker — runs akkado::compile and all metadata extraction off
// the AudioWorklet thread. PRD docs/prd-compile-off-audio-thread.md §4.
//
// The main thread pushes the WASM JS code + binary in via the same shape
// the AudioWorklet uses, so this worker owns its own nkido.wasm instance
// and never touches the worklet's VM. After init it accepts:
//
//   {type:'compile', gen, source}
//
// and replies with:
//
//   {type:'compileResult', gen, success, bytecode?, diagnostics?, …}
//
// Commit 2 (PRD §8 step 2): scaffolding only — produces bytecode +
// diagnostics. Commit 3 adds stateInitsBuf / midiSourcesBuf / blockTable
// packing and the full required-assets metadata.

// eslint-disable-next-line @typescript-eslint/no-explicit-any
type NkidoModule = any;

interface Diagnostic {
    severity: number;
    message: string;
    line: number;
    column: number;
}

interface ExtendedSample {
    bank: string | null;
    name: string;
    variant: number;
    qualifiedName: string;
}

interface RequiredSoundFont {
    filename: string;
    preset: number;
}

interface RequiredWavetable {
    name: string;
    path: string;
    id: number;
}

interface UriRequest {
    uri: string;
    kind: number;
}

interface RequiredMidiSource {
    stateId: number;
    kind: number;
    name: string;
    channel: number;
    loop: boolean;
    tempo: number;
}

interface RequiredMidiCcRoute {
    paramName: string;
    ccNum: number;
    channel: number;
    scale: number;
    bias: number;
    slewMs: number;
}

interface ParamDecl {
    name: string;
    type: number;
    defaultValue: number;
    min: number;
    max: number;
    options: string[];
    sourceOffset: number;
    sourceLength: number;
}

interface VizDecl {
    name: string;
    type: number;
    stateId: number;
    sourceOffset: number;
    sourceLength: number;
    patternIndex: number;
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    options: any;
}

interface BuiltinVarOverride {
    name: string;
    value: number;
}

let wasm: NkidoModule | null = null;
let isReady = false;
const messageQueue: unknown[] = [];

self.onmessage = (event: MessageEvent) => {
    const msg = event.data;
    if (msg?.type === 'init') {
        void initFromCode(msg.jsCode, msg.wasmBinary);
        return;
    }
    if (!isReady) {
        messageQueue.push(msg);
        return;
    }
    dispatch(msg);
};

function dispatch(msg: { type?: string; [key: string]: unknown }) {
    switch (msg?.type) {
        case 'compile':
            compile(msg.gen as number, msg.source as string);
            break;
        default:
            // Unknown messages get logged and ignored — forward-compat with
            // future main-thread message types.
            console.warn('[CompileWorker] unknown message', msg?.type);
    }
}

async function initFromCode(jsCode: string, wasmBinary: ArrayBuffer) {
    try {
        console.log('[CompileWorker] Initializing WASM...');

        // Patch the Emscripten code identically to cedar-processor.js:
        // suppress the AudioWorklet/WASM-Worker auto-execute paths so the
        // wasm-module factory is the only entry point we run.
        const patchedCode = jsCode
            .replace(/registerProcessor\s*\(\s*["']em-bootstrap["'].*?\)\s*\}/g, '}')
            .replace(
                /isWW\s*\|\|=\s*typeof AudioWorkletGlobalScope.*?isWW\s*&&\s*createNkidoModule\s*\(\s*\)\s*;?/gs,
                ''
            )
            .replace(
                /ENVIRONMENT_IS_AUDIO_WORKLET\s*=\s*typeof AudioWorkletGlobalScope\s*!==?\s*["']undefined["']/g,
                'ENVIRONMENT_IS_AUDIO_WORKLET=false'
            )
            .replace(/registerProcessor\s*\(\s*["']em-bootstrap["'][^;]*;/g, '/* em-bootstrap removed */')
            .replace(
                /if\s*\(\s*ENVIRONMENT_IS_AUDIO_WORKLET\s*\)\s*ENVIRONMENT_IS_WASM_WORKER\s*=\s*true;?/g,
                ''
            )
            .replace(/ENVIRONMENT_IS_PTHREAD\s*=\s*ENVIRONMENT_IS_WORKER/g, 'ENVIRONMENT_IS_PTHREAD=false')
            .replace(/ENVIRONMENT_IS_WASM_WORKER\s*=\s*true/g, 'ENVIRONMENT_IS_WASM_WORKER=false');

        // eslint-disable-next-line @typescript-eslint/no-implied-eval
        const wasmFactory = new Function(
            patchedCode + '\nreturn createNkidoModule;'
        )();
        if (typeof wasmFactory !== 'function') {
            throw new Error('createNkidoModule not found after evaluating code');
        }

        wasm = await wasmFactory({
            wasmBinary,
            print: (t: string) => console.log('[CompileWorker WASM]', t),
            printErr: (t: string) => console.error('[CompileWorker WASM Error]', t)
        });

        // No `_cedar_init` here — the worker only ever calls compile-side
        // exports (`_akkado_compile`, `_akkado_get_*`). The VM lives in the
        // worklet's WASM instance.

        isReady = true;
        console.log('[CompileWorker] Ready');
        self.postMessage({ type: 'ready' });

        for (const queued of messageQueue.splice(0)) {
            dispatch(queued as { type?: string });
        }
    } catch (err) {
        console.error('[CompileWorker] Init failed:', err);
        self.postMessage({ type: 'initError', message: String(err) });
    }
}

function copyBytes(ptr: number, byteCount: number): Uint8Array {
    const out = new Uint8Array(byteCount);
    if (byteCount === 0) return out;
    if (wasm.HEAPU8) {
        out.set(wasm.HEAPU8.subarray(ptr, ptr + byteCount));
    } else if (wasm.wasmMemory) {
        const heap = new Uint8Array(wasm.wasmMemory.buffer);
        out.set(heap.subarray(ptr, ptr + byteCount));
    } else {
        for (let i = 0; i < byteCount; i++) {
            out[i] = wasm.getValue(ptr + i, 'i8') & 0xff;
        }
    }
    return out;
}

function utf8(ptr: number): string {
    return ptr ? wasm.UTF8ToString(ptr) : '';
}

function getRequiredSamples(): string[] {
    const count = wasm._akkado_get_required_samples_count();
    const out: string[] = [];
    for (let i = 0; i < count; i++) {
        out.push(utf8(wasm._akkado_get_required_sample(i)));
    }
    return out;
}

function getRequiredSamplesExtended(): ExtendedSample[] {
    if (!wasm._akkado_get_required_samples_extended_count) return [];
    const count = wasm._akkado_get_required_samples_extended_count();
    const out: ExtendedSample[] = [];
    for (let i = 0; i < count; i++) {
        const bankPtr = wasm._akkado_get_required_sample_bank(i);
        out.push({
            bank: bankPtr ? wasm.UTF8ToString(bankPtr) : null,
            name: utf8(wasm._akkado_get_required_sample_name(i)),
            variant: wasm._akkado_get_required_sample_variant(i),
            qualifiedName: utf8(wasm._akkado_get_required_sample_qualified(i))
        });
    }
    return out;
}

function getRequiredSoundFonts(): RequiredSoundFont[] {
    if (!wasm._akkado_get_required_soundfonts_count) return [];
    const count = wasm._akkado_get_required_soundfonts_count();
    const out: RequiredSoundFont[] = [];
    for (let i = 0; i < count; i++) {
        out.push({
            filename: utf8(wasm._akkado_get_required_soundfont_filename(i)),
            preset: wasm._akkado_get_required_soundfont_preset(i)
        });
    }
    return out;
}

function getRequiredWavetables(): RequiredWavetable[] {
    if (!wasm._akkado_get_required_wavetables_count) return [];
    const count = wasm._akkado_get_required_wavetables_count();
    const out: RequiredWavetable[] = [];
    for (let i = 0; i < count; i++) {
        out.push({
            name: utf8(wasm._akkado_get_required_wavetable_name(i)),
            path: utf8(wasm._akkado_get_required_wavetable_path(i)),
            id: i
        });
    }
    return out;
}

function getRequiredUris(): UriRequest[] {
    if (!wasm._akkado_get_required_uri_count) return [];
    const count = wasm._akkado_get_required_uri_count();
    const out: UriRequest[] = [];
    for (let i = 0; i < count; i++) {
        out.push({
            uri: utf8(wasm._akkado_get_required_uri(i)),
            kind: wasm._akkado_get_required_uri_kind(i)
        });
    }
    return out;
}

function getRequiredInputSources(): string[] {
    if (!wasm._akkado_get_required_input_sources_count) return [];
    const count = wasm._akkado_get_required_input_sources_count();
    const out: string[] = [];
    for (let i = 0; i < count; i++) {
        out.push(utf8(wasm._akkado_get_required_input_source(i)));
    }
    return out;
}

function getRequiredMidiSources(): RequiredMidiSource[] {
    if (!wasm._akkado_get_required_midi_sources_count) return [];
    const count = wasm._akkado_get_required_midi_sources_count();
    const out: RequiredMidiSource[] = [];
    for (let i = 0; i < count; i++) {
        out.push({
            stateId: wasm._akkado_get_required_midi_source_state_id(i) >>> 0,
            kind: wasm._akkado_get_required_midi_source_kind(i),
            name: utf8(wasm._akkado_get_required_midi_source_name(i)),
            channel: wasm._akkado_get_required_midi_source_channel(i),
            loop: wasm._akkado_get_required_midi_source_loop(i) === 1,
            tempo: wasm._akkado_get_required_midi_source_tempo(i)
        });
    }
    return out;
}

function getRequiredMidiCcRoutes(): RequiredMidiCcRoute[] {
    if (!wasm._akkado_get_required_midi_cc_routes_count) return [];
    const count = wasm._akkado_get_required_midi_cc_routes_count();
    const out: RequiredMidiCcRoute[] = [];
    for (let i = 0; i < count; i++) {
        out.push({
            paramName: utf8(wasm._akkado_get_required_midi_cc_route_name(i)),
            ccNum: wasm._akkado_get_required_midi_cc_route_cc(i) | 0,
            channel: wasm._akkado_get_required_midi_cc_route_channel(i) | 0,
            scale: wasm._akkado_get_required_midi_cc_route_scale(i),
            bias: wasm._akkado_get_required_midi_cc_route_bias(i),
            slewMs: wasm._akkado_get_required_midi_cc_route_slew_ms(i)
        });
    }
    return out;
}

function extractParamDecls(): ParamDecl[] {
    if (!wasm._akkado_get_param_decl_count) return [];
    const count = wasm._akkado_get_param_decl_count();
    const out: ParamDecl[] = [];
    for (let i = 0; i < count; i++) {
        const type = wasm._akkado_get_param_type(i);
        const options: string[] = [];
        if (type === 3) {
            const optCount = wasm._akkado_get_param_option_count(i);
            for (let j = 0; j < optCount; j++) {
                options.push(utf8(wasm._akkado_get_param_option(i, j)));
            }
        }
        out.push({
            name: utf8(wasm._akkado_get_param_name(i)),
            type,
            defaultValue: wasm._akkado_get_param_default(i),
            min: wasm._akkado_get_param_min(i),
            max: wasm._akkado_get_param_max(i),
            options,
            sourceOffset: wasm._akkado_get_param_source_offset(i),
            sourceLength: wasm._akkado_get_param_source_length(i)
        });
    }
    return out;
}

function extractVizDecls(): VizDecl[] {
    if (!wasm._akkado_get_viz_count) return [];
    const count = wasm._akkado_get_viz_count();
    const out: VizDecl[] = [];
    for (let i = 0; i < count; i++) {
        let options = null;
        const optionsPtr = wasm._akkado_get_viz_options ? wasm._akkado_get_viz_options(i) : 0;
        if (optionsPtr) {
            const optionsStr = wasm.UTF8ToString(optionsPtr);
            try { options = JSON.parse(optionsStr); } catch (e) {
                console.warn('[CompileWorker] viz options parse fail:', e);
            }
        }
        out.push({
            name: utf8(wasm._akkado_get_viz_name(i)),
            type: wasm._akkado_get_viz_type(i),
            stateId: wasm._akkado_get_viz_state_id(i),
            sourceOffset: wasm._akkado_get_viz_source_offset(i),
            sourceLength: wasm._akkado_get_viz_source_length(i),
            patternIndex: wasm._akkado_get_viz_pattern_index(i),
            options
        });
    }
    return out;
}

function extractBuiltinVarOverrides(): BuiltinVarOverride[] {
    if (!wasm._akkado_get_builtin_var_override_count) return [];
    const count = wasm._akkado_get_builtin_var_override_count();
    const out: BuiltinVarOverride[] = [];
    for (let i = 0; i < count; i++) {
        out.push({
            name: utf8(wasm._akkado_get_builtin_var_override_name(i)),
            value: wasm._akkado_get_builtin_var_override_value(i)
        });
    }
    return out;
}

function extractDisassembly(): unknown {
    if (!wasm._akkado_get_disassembly) return null;
    const ptr = wasm._akkado_get_disassembly();
    if (!ptr) return null;
    try {
        return JSON.parse(wasm.UTF8ToString(ptr));
    } catch (e) {
        console.warn('[CompileWorker] disassembly parse fail:', e);
        return null;
    }
}

function packStateInits(): Uint8Array {
    const ptr = wasm._akkado_pack_state_inits_buffer();
    const size = wasm._akkado_get_state_inits_buffer_size();
    return copyBytes(ptr, size);
}

function packMidiSources(): Uint8Array {
    const ptr = wasm._akkado_pack_midi_sources_buffer();
    const size = wasm._akkado_get_midi_sources_buffer_size();
    return copyBytes(ptr, size);
}

function packBlockTable(): Uint8Array {
    const ptr = wasm._akkado_pack_block_table_buffer();
    const size = wasm._akkado_get_block_table_buffer_size();
    return copyBytes(ptr, size);
}

function extractDiagnostics(): Diagnostic[] {
    const count = wasm._akkado_get_diagnostic_count();
    const out: Diagnostic[] = [];
    for (let i = 0; i < count; i++) {
        const msgPtr = wasm._akkado_get_diagnostic_message(i);
        out.push({
            severity: wasm._akkado_get_diagnostic_severity(i),
            message: msgPtr ? wasm.UTF8ToString(msgPtr) : '',
            line: wasm._akkado_get_diagnostic_line(i),
            column: wasm._akkado_get_diagnostic_column(i)
        });
    }
    return out;
}

function compile(gen: number, source: string) {
    if (!wasm) {
        self.postMessage({
            type: 'compileResult',
            gen,
            success: false,
            diagnostics: [{ severity: 2, message: 'Worker not initialized', line: 1, column: 1 }]
        });
        return;
    }

    try {
        wasm._akkado_clear_result();

        const utf8ByteLen = wasm.lengthBytesUTF8(source);
        const sourcePtr = wasm._nkido_malloc(utf8ByteLen + 1);
        if (sourcePtr === 0) {
            self.postMessage({
                type: 'compileResult',
                gen,
                success: false,
                diagnostics: [{ severity: 2, message: 'malloc failed', line: 1, column: 1 }]
            });
            return;
        }

        try {
            wasm.stringToUTF8(source, sourcePtr, utf8ByteLen + 1);
            const success = wasm._akkado_compile(sourcePtr, utf8ByteLen);
            if (success) {
                // Extract every JS-side metadata field FIRST (each call
                // walks the live WASM heap), then pack the three buffers,
                // then copy bytecode, then clear_result. Pack calls grow
                // their own static vectors which may invalidate previous
                // heap views — copyBytes always fetches a fresh view.
                const requiredSamples = getRequiredSamples();
                const requiredSamplesExtended = getRequiredSamplesExtended();
                const requiredSoundfonts = getRequiredSoundFonts();
                const requiredWavetables = getRequiredWavetables();
                const requiredUris = getRequiredUris();
                const requiredInputSources = getRequiredInputSources();
                const requiredMidiSources = getRequiredMidiSources();
                const requiredMidiCcRoutes = getRequiredMidiCcRoutes();
                const paramDecls = extractParamDecls();
                const vizDecls = extractVizDecls();
                const builtinVarOverrides = extractBuiltinVarOverrides();
                const disassembly = extractDisassembly();

                const stateInitsBuf = packStateInits();
                const midiSourcesBuf = packMidiSources();
                const blockTable = packBlockTable();
                const mainInstCount = wasm._akkado_get_main_instruction_count();

                const bytecodePtr = wasm._akkado_get_bytecode();
                const bytecodeSize = wasm._akkado_get_bytecode_size();
                const bytecode = copyBytes(bytecodePtr, bytecodeSize);

                wasm._akkado_clear_result();

                self.postMessage(
                    {
                        type: 'compileResult',
                        gen,
                        success: true,
                        bytecode,
                        bytecodeSize,
                        stateInitsBuf,
                        midiSourcesBuf,
                        blockTable,
                        mainInstCount,
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
                    },
                    [
                        bytecode.buffer,
                        stateInitsBuf.buffer,
                        midiSourcesBuf.buffer,
                        blockTable.buffer
                    ]
                );
            } else {
                const diagnostics = extractDiagnostics();
                wasm._akkado_clear_result();
                self.postMessage({
                    type: 'compileResult',
                    gen,
                    success: false,
                    diagnostics
                });
            }
        } finally {
            wasm._nkido_free(sourcePtr);
        }
    } catch (err) {
        console.error('[CompileWorker] Compile threw:', err);
        self.postMessage({
            type: 'compileResult',
            gen,
            success: false,
            diagnostics: [
                { severity: 2, message: 'Worker compile error: ' + String(err), line: 1, column: 1 }
            ]
        });
    }
}
