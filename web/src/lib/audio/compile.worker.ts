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

function copyBytecode(bytecodePtr: number, bytecodeSize: number): Uint8Array {
    const out = new Uint8Array(bytecodeSize);
    if (wasm.HEAPU8) {
        out.set(wasm.HEAPU8.subarray(bytecodePtr, bytecodePtr + bytecodeSize));
    } else if (wasm.wasmMemory) {
        const heap = new Uint8Array(wasm.wasmMemory.buffer);
        out.set(heap.subarray(bytecodePtr, bytecodePtr + bytecodeSize));
    } else {
        for (let i = 0; i < bytecodeSize; i++) {
            out[i] = wasm.getValue(bytecodePtr + i, 'i8') & 0xff;
        }
    }
    return out;
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
                const bytecodePtr = wasm._akkado_get_bytecode();
                const bytecodeSize = wasm._akkado_get_bytecode_size();
                const bytecode = copyBytecode(bytecodePtr, bytecodeSize);
                wasm._akkado_clear_result();
                self.postMessage(
                    {
                        type: 'compileResult',
                        gen,
                        success: true,
                        bytecode
                    },
                    [bytecode.buffer]
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
