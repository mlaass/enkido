/**
 * Studio panel state + RPC (prd-studio-daw-core, closed repo `nkido-studio`).
 *
 * Native side: `hostcore::NativeBridge` exposes the daw-core services as
 * `callNativeFunction` endpoints and pushes one batched `studioStatus` event per
 * 30 Hz tick (transport + capture always; mixer strips only when they changed).
 * That single batched event is the bridge law — never one event per strip.
 *
 * Host-only: this module lives under `$lib/native/` and ships only in the
 * `__IS_NATIVE__` bundle.
 */

export interface MixerStrip {
	index: number;
	label: string;
	faderDb: number;
	mute: boolean;
	solo: boolean;
}

export interface TransportState {
	playing: boolean;
	bpm: number;
	positionBeats: number;
	sync: 'none' | 'link' | 'midi-follow';
	midiClockOut: boolean;
	linkAvailable: boolean;
}

export interface CaptureState {
	retroOn: boolean;
	windowSeconds: number;
	bufferedSeconds: number;
	recording: boolean;
	overflow: number;
	spilling: boolean;
}

interface JuceBackend {
	callNativeFunction(name: string, args: unknown[]): Promise<unknown>;
	addEventListener(id: string, fn: (payload: unknown) => void): unknown;
}

function backend(): JuceBackend | undefined {
	return (window as { __JUCE__?: { backend?: JuceBackend } }).__JUCE__?.backend;
}

async function call<T>(name: string, args: unknown[] = []): Promise<T | undefined> {
	const b = backend();
	if (!b) return undefined;
	try {
		return (await b.callNativeFunction(name, args)) as T;
	} catch (err) {
		console.error(`[studio] ${name} failed:`, err);
		return undefined;
	}
}

/** Peak/RMS per bus, keyed by bus index — fed by the existing `meters` event. */
export type MeterMap = Record<number, { peak: number; rms: number }>;

class StudioStore {
	strips = $state<MixerStrip[]>([]);
	transport = $state<TransportState>({
		playing: false,
		bpm: 120,
		positionBeats: 0,
		sync: 'none',
		midiClockOut: false,
		linkAvailable: false
	});
	capture = $state<CaptureState>({
		retroOn: false,
		windowSeconds: 0,
		bufferedSeconds: 0,
		recording: false,
		overflow: 0,
		spilling: false
	});
	meters = $state<MeterMap>({});
	captures = $state<string[]>([]);
	renders = $state<string[]>([]);
	notice = $state('');
	rendering = $state(false);

	/** True once the native bridge answered — panels stay hidden otherwise. */
	available = $state(false);

	start() {
		const b = backend();
		if (!b) return;

		b.addEventListener('studioStatus', (payload) => {
			const p = payload as {
				transport?: TransportState;
				capture?: CaptureState;
				mixer?: { strips: MixerStrip[] };
			};
			if (p.transport) this.transport = p.transport;
			if (p.capture) this.capture = p.capture;
			if (p.mixer) this.strips = p.mixer.strips; // only sent when changed
			this.available = true;
		});

		// The batched meter event is [bus, peak, rms, bus, peak, rms, …].
		b.addEventListener('meters', (payload) => {
			const buses = (payload as { buses?: number[] }).buses;
			if (!buses) return;
			const next: MeterMap = {};
			for (let i = 0; i + 2 < buses.length; i += 3)
				next[buses[i]] = { peak: buses[i + 1], rms: buses[i + 2] };
			this.meters = next;
		});

		void this.refresh();
		void this.refreshTakes();
	}

	async refresh() {
		const m = await call<{ strips: MixerStrip[] }>('mixerState');
		if (m) this.strips = m.strips;
		const t = await call<TransportState>('transportState');
		if (t) this.transport = t;
		const c = await call<CaptureState>('captureState');
		if (c) this.capture = c;
		if (m || t || c) this.available = true;
	}

	async refreshTakes() {
		const r = await call<{ captures: string[]; renders: string[] }>('listTakes');
		if (r) {
			this.captures = r.captures ?? [];
			this.renders = r.renders ?? [];
		}
	}

	// --- Mixer (§5.3) --------------------------------------------------------
	// The native side collapses fader/mute/solo into one gain per bus and pokes
	// the engine's per-bus trim, so a fader reaches the real master.
	async setStrip(bus: number, patch: Partial<Omit<MixerStrip, 'index' | 'label'>>) {
		const s = this.strips.find((x) => x.index === bus);
		if (!s) return;
		const next = { ...s, ...patch };
		this.strips = this.strips.map((x) => (x.index === bus ? next : x)); // optimistic
		await call('setMixerStrip', [bus, next.faderDb, next.mute, next.solo]);
	}

	// --- Transport (§5.4) ----------------------------------------------------
	async command(cmd: string, value?: number | string | boolean) {
		const res = await call<{ note?: string }>(
			'transportCommand',
			value === undefined ? [cmd] : [cmd, value]
		);
		this.notice = res?.note ?? '';
		await this.refresh();
	}

	// --- Capture (§5.2) ------------------------------------------------------
	async keep(seconds: number) {
		const r = await call<{ ok: boolean; take: string }>('captureKeep', [seconds]);
		this.notice = r?.ok ? `Kept ${r.take}` : 'Nothing to keep';
		await this.refreshTakes();
	}

	async record(on: boolean) {
		const r = await call<{ ok: boolean; take: string }>('captureRecord', [on]);
		if (!on && r?.ok) this.notice = `Recorded ${r.take}`;
		await this.refresh();
		await this.refreshTakes();
	}

	// --- Render (§5.5) -------------------------------------------------------
	async render(source: string, seconds: number, cycles: number, sampleRate: number, stems: boolean) {
		this.rendering = true;
		try {
			const r = await call<{ ok: boolean; take: string }>('renderRun', [
				source,
				seconds,
				cycles,
				sampleRate,
				stems
			]);
			this.notice = r?.ok ? `Rendered ${r.take}` : 'Render failed';
			await this.refreshTakes();
			return r?.ok ?? false;
		} finally {
			this.rendering = false;
		}
	}
}

export const studio = new StudioStore();
