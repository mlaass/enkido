/**
 * JuceAudioBackend unit tests (prd-web-audio-backend §10) against a fake
 * `__JUCE__` bridge implementing bridge-protocol v1: readiness handshake,
 * compile → compileResult event, pushed-frame buffering for the pull-model
 * viz queries, and graceful degradation for not-in-v1 surface.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';

import {
	createJuceAudioBackend,
	BRIDGE_PROTOCOL_VERSION,
	type JuceBridge
} from '../src/lib/audio/juce-backend';
import type { AudioBackendHost } from '../src/lib/audio/audio-backend';

function makeBridge(fns: Record<string, (...args: unknown[]) => unknown> = {}) {
	const listeners = new Map<string, (payload: unknown) => void>();
	const emitted: Array<{ id: string; payload: unknown }> = [];
	const bridge: JuceBridge = {
		callNativeFunction: vi.fn(async (name: string, args: unknown[]) => {
			const fn = fns[name];
			if (!fn) throw new Error(`unknown native function: ${name}`);
			return fn(...args);
		}),
		addEventListener: (id, fn) => {
			listeners.set(id, fn);
		},
		emitEvent: (id, payload) => {
			emitted.push({ id, payload });
		}
	};
	return {
		bridge,
		emitted,
		fire: (id: string, payload: unknown) => listeners.get(id)?.(payload),
		hasListener: (id: string) => listeners.has(id)
	};
}

function makeHost(): AudioBackendHost {
	return {
		onTransport: vi.fn(),
		onParam: vi.fn(),
		onError: vi.fn(),
		onStatus: vi.fn(),
		onAssetLoaded: vi.fn(),
		onAssetsCleared: vi.fn(),
		onCompileResult: vi.fn(),
		onEngineReset: vi.fn()
	};
}

const tick = () => new Promise<void>((r) => setTimeout(r, 0));

afterEach(() => {
	vi.unstubAllGlobals();
	vi.restoreAllMocks();
});

describe('readiness handshake (bridge-protocol §1)', () => {
	it('registers event listeners, then emits ready with the protocol version', async () => {
		const { bridge, emitted, hasListener } = makeBridge({ deviceStatus: () => ({}) });
		const host = makeHost();
		const backend = createJuceAudioBackend(host, bridge);

		expect(emitted).toHaveLength(0); // nothing before initialize()
		await backend.initialize();

		expect(emitted).toEqual([{ id: 'ready', payload: { protocolVersion: BRIDGE_PROTOCOL_VERSION } }]);
		for (const id of ['compileResult', 'playhead', 'meters', 'scopeReady']) {
			expect(hasListener(id)).toBe(true);
		}
		expect(host.onStatus).toHaveBeenCalledWith({ isInitialized: true, isLoading: false });
	});

	it('surfaces the deviceStatus sample rate', async () => {
		const { bridge } = makeBridge({ deviceStatus: () => ({ sampleRate: 44100 }) });
		const host = makeHost();
		const backend = createJuceAudioBackend(host, bridge);
		await backend.initialize();
		await tick();
		expect(host.onStatus).toHaveBeenCalledWith({ activeSampleRate: 44100 });
	});

	it('queues native calls until ready — nothing reaches the bridge before initialize()', async () => {
		const { bridge } = makeBridge({ setParam: () => ({ ok: true }) });
		const host = makeHost();
		const backend = createJuceAudioBackend(host, bridge);

		backend.setParam('freq', 440);
		await tick();
		expect(bridge.callNativeFunction).not.toHaveBeenCalled();

		await backend.initialize();
		await tick();
		expect(bridge.callNativeFunction).toHaveBeenCalledWith('setParam', ['freq', 440]);
	});
});

describe('compile (bridge-protocol §2/§3)', () => {
	it('resolves with mapped diagnostics from the compileResult event (col → column)', async () => {
		const { bridge, fire } = makeBridge({
			deviceStatus: () => ({}),
			compile: () => ({ submitted: true })
		});
		const host = makeHost();
		const backend = createJuceAudioBackend(host, bridge);
		await backend.initialize();

		const pending = backend.compile('sine(440) |> out(@)');
		await tick();
		fire('compileResult', {
			generation: 7,
			ok: false,
			diagnostics: [{ severity: 2, message: 'boom', line: 3, col: 9 }]
		});

		const result = await pending;
		expect(result.success).toBe(false);
		expect(result.diagnostics).toEqual([{ severity: 2, message: 'boom', line: 3, column: 9 }]);
		expect(host.onCompileResult).toHaveBeenCalledWith(result);
	});

	it('supersedes older pending compiles when a newer one lands', async () => {
		const { bridge, fire } = makeBridge({
			deviceStatus: () => ({}),
			compile: () => ({ submitted: true })
		});
		const backend = createJuceAudioBackend(makeHost(), bridge);
		await backend.initialize();

		const first = backend.compile('a');
		const second = backend.compile('b');
		await tick();
		fire('compileResult', { generation: 2, ok: true, diagnostics: [] });

		expect(await first).toEqual({ success: false, superseded: true });
		expect((await second).success).toBe(true);
	});

	it('fails cleanly when the compile submit itself rejects', async () => {
		const { bridge } = makeBridge({ deviceStatus: () => ({}) }); // no `compile` fn
		const backend = createJuceAudioBackend(makeHost(), bridge);
		await backend.initialize();

		const result = await backend.compile('x');
		expect(result.success).toBe(false);
		expect(result.diagnostics?.[0].message).toMatch(/Compile submit failed/);
	});
});

describe('pushed-frame buffering for pull-model viz (PRD §5.3 / §9)', () => {
	it('answers polls before any frame arrived with empty values, never throwing', async () => {
		const { bridge } = makeBridge({ deviceStatus: () => ({}) });
		const backend = createJuceAudioBackend(makeHost(), bridge);
		await backend.initialize();

		expect(await backend.getCurrentBeatPosition()).toBe(0);
		expect(await backend.getProbeData(123)).toBeNull();
		expect(await backend.getFFTProbeData(123)).toBeNull();
		expect(await backend.getPatternInfo()).toEqual([]);
		expect(await backend.getActiveSteps([1, 2])).toEqual({});
	});

	it('serves getCurrentBeatPosition from the latest playhead event and pushes onTransport', async () => {
		const { bridge, fire } = makeBridge({ deviceStatus: () => ({}) });
		const host = makeHost();
		const backend = createJuceAudioBackend(host, bridge);
		await backend.initialize();

		fire('playhead', { beat: 5.25, bar: 2 });
		expect(host.onTransport).toHaveBeenCalledWith({ currentBeat: 5.25, currentBar: 2 });
		expect(await backend.getCurrentBeatPosition()).toBe(5.25);
	});

	it('fetches scope.bin on the scopeReady doorbell and serves the left channel to getProbeData', async () => {
		// Interleaved L,R: L = [0.1, 0.2], R = [0.9, 0.8]
		const interleaved = new Float32Array([0.1, 0.9, 0.2, 0.8]);
		vi.stubGlobal(
			'fetch',
			vi.fn(async () => ({ ok: true, arrayBuffer: async () => interleaved.buffer }))
		);

		const { bridge, fire } = makeBridge({ deviceStatus: () => ({}) });
		const backend = createJuceAudioBackend(makeHost(), bridge);
		await backend.initialize();

		fire('scopeReady', { seq: 1, samples: 2, sampleRate: 48000 });
		await tick();
		await tick();

		expect(fetch).toHaveBeenCalledWith('scope.bin');
		const probe = await backend.getProbeData(42);
		expect(probe).not.toBeNull();
		expect(Array.from(probe!)).toEqual([expect.closeTo(0.1), expect.closeTo(0.2)]);
	});
});

describe('transport requests + graceful degradation (bridge-protocol §6)', () => {
	it('play()/pause() survive a v1 host without transport functions and stay advisory', async () => {
		const { bridge } = makeBridge({ deviceStatus: () => ({}) });
		const host = makeHost();
		const backend = createJuceAudioBackend(host, bridge);
		await backend.initialize();

		await backend.play(); // unknown fn rejects internally — must not throw
		expect(host.onTransport).toHaveBeenCalledWith({ isPlaying: true });
		await backend.pause();
		expect(host.onTransport).toHaveBeenCalledWith({ isPlaying: false });
	});

	it('parses getBuiltins / inspectState JSON strings and nulls on garbage', async () => {
		const { bridge } = makeBridge({
			deviceStatus: () => ({}),
			getBuiltins: () => JSON.stringify({ functions: {}, aliases: {}, keywords: ['out'] }),
			inspectState: () => 'not json'
		});
		const backend = createJuceAudioBackend(makeHost(), bridge);
		await backend.initialize();

		expect(await backend.getBuiltins()).toEqual({ functions: {}, aliases: {}, keywords: ['out'] });
		expect(await backend.inspectState(7)).toBeNull();
	});

	it('web-only escape hatches return null / empty on native (PRD §4)', async () => {
		const { bridge } = makeBridge({ deviceStatus: () => ({}) });
		const backend = createJuceAudioBackend(makeHost(), bridge);
		expect(backend.getAnalyserNode()).toBeNull();
		expect(backend.getAudioContext()).toBeNull();
		expect(backend.getTimeDomainData()).toHaveLength(0);
		expect(backend.getFrequencyData()).toHaveLength(0);
		expect(backend.getMidiController()).toBeNull();
	});
});
