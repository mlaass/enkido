/**
 * Web MIDI input controller (prd-midi-input §4.10).
 *
 * Lives on the main thread (Web MIDI lives only there). Responsibilities:
 *   1. Lazy `requestMIDIAccess()` — only fires on first user interaction.
 *   2. Track the device list and surface it for the UI dropdown.
 *   3. After compile, build a route table from required_midi_sources and
 *      hook `onmidimessage` on every relevant MIDIInput.
 *   4. Fan-out: every incoming event is matched against routes for that
 *      device, channel-filtered, and posted to the worklet as
 *      `{type:'midi', state_id, status, d1, d2}`. The worklet calls
 *      `_cedar_push_midi_event(state_id, status, d1, d2)` on receipt.
 *
 * SMF (.mid) playback and CC routing live in later phases; only live
 * channel-voice messages (note-on/off + their kin) flow through this
 * controller today.
 */

export type MidiSourceKind = 0 | 1 | 2; // DefaultDevice | NamedDevice | File

export interface RequiredMidiSource {
	stateId: number;
	kind: MidiSourceKind;
	name: string;
	channel: number; // 0 = any, 1-16
	loop: boolean;
	tempo: number; // 0 = Follow, 1 = File
}

/**
 * Compile-time CC / pitch-bend / channel-aftertouch route (PRD §4.8).
 * ccNum sentinels: 0..127 = MIDI CC#; -1 = pitch-bend; -2 = channel AT.
 */
export interface RequiredMidiCcRoute {
	paramName: string;
	ccNum: number;
	channel: number;   // 0 = any, 1..16
	scale: number;     // max - min
	bias: number;      // min
	slewMs: number;
}

export interface MidiDeviceInfo {
	id: string;
	name: string;
	manufacturer: string;
	state: 'connected' | 'disconnected';
}

export type MidiStatus =
	| 'unsupported' // navigator.requestMIDIAccess missing
	| 'idle'        // haven't asked yet
	| 'pending'     // requestMIDIAccess() in flight
	| 'denied'      // user denied permission
	| 'unavailable' // requestMIDIAccess threw / no devices
	| 'active';     // permission granted

interface ResolvedRoute {
	stateId: number;
	channelFilter: number; // 0 = any
}

function statusBaseByte(status: number): number {
	return status & 0xF0;
}

/**
 * Channel-voice message? (0x80..0xEF). System / RT / sysex are filtered out
 * here — the worklet's `_cedar_push_midi_event` only knows how to push
 * channel-voice events into the SPSC ring.
 */
function isChannelVoice(status: number): boolean {
	const base = statusBaseByte(status);
	return base >= 0x80 && base <= 0xE0;
}

export function createMidiInputController() {
	const supported = typeof navigator !== 'undefined' &&
		typeof navigator.requestMIDIAccess === 'function';

	let status = $state<MidiStatus>(supported ? 'idle' : 'unsupported');
	let error = $state<string | null>(null);
	let devices = $state<MidiDeviceInfo[]>([]);
	let defaultDeviceName = $state<string>('');
	let lastEventTime = $state<number>(0);
	let lastError = $state<string | null>(null);

	let access: MIDIAccess | null = null;
	let workletPort: MessagePort | null = null;
	// Routes keyed by the physical device's `port.name`. One MIDI input may
	// fan out to multiple routes (different channel filters per compile-time
	// midi() call site).
	let routesByDevice = new Map<string, ResolvedRoute[]>();
	// CC-routes keyed by the device the CC events should be sourced from. In
	// v1 (PRD §4.8) every cc-route rides the resolved default device.
	let ccRoutesByDevice = new Map<string, RequiredMidiCcRoute[]>();
	// Held-onto MIDIInput refs so we can remove our listeners on rebuild.
	let attachedInputs = new Map<string, MIDIInput>();
	// Raw required_midi_sources kept around so we can re-resolve when the
	// device list changes (plug-in / hot-swap of default device).
	let pendingSources: RequiredMidiSource[] = [];
	let pendingCcRoutes: RequiredMidiCcRoute[] = [];

	function refreshDevices() {
		if (!access) {
			devices = [];
			return;
		}
		const list: MidiDeviceInfo[] = [];
		access.inputs.forEach((input) => {
			list.push({
				id: input.id,
				name: input.name || '(unnamed device)',
				manufacturer: input.manufacturer || '',
				state: input.state === 'connected' ? 'connected' : 'disconnected'
			});
		});
		// Stable order by name so the dropdown doesn't jitter.
		list.sort((a, b) => a.name.localeCompare(b.name));
		devices = list;
	}

	function detachAll() {
		for (const input of attachedInputs.values()) {
			input.onmidimessage = null;
		}
		attachedInputs.clear();
	}

	function attachForRoutes() {
		if (!access) return;
		detachAll();
		access.inputs.forEach((input) => {
			const name = input.name || '';
			// Attach if this device hosts EITHER note routes OR CC routes.
			if (!routesByDevice.has(name) && !ccRoutesByDevice.has(name)) return;
			input.onmidimessage = (ev: MIDIMessageEvent) => handleMessage(name, ev);
			attachedInputs.set(name, input);
		});
	}

	function handleMessage(deviceName: string, ev: MIDIMessageEvent) {
		const data = ev.data;
		if (!data || data.length < 1) return;
		const statusByte = data[0];
		if (!isChannelVoice(statusByte)) return;

		const d1 = data.length > 1 ? data[1] : 0;
		const d2 = data.length > 2 ? data[2] : 0;
		const channel1Based = (statusByte & 0x0F) + 1;
		const base = statusByte & 0xF0;

		lastEventTime = Date.now();

		// PRD §4.8: dispatch CC / PB / AT to param() slots via setParam. The
		// math runs on the main thread (same topology as note routing) and
		// the worklet's existing 'setParam' message hits cedar_set_param_slew.
		if (base === 0xB0 || base === 0xD0 || base === 0xE0) {
			const ccRoutes = ccRoutesByDevice.get(deviceName);
			if (ccRoutes && ccRoutes.length > 0) {
				for (const r of ccRoutes) {
					if (r.channel !== 0 && r.channel !== channel1Based) continue;
					let value: number | null = null;
					if (base === 0xB0) {
						if (r.ccNum < 0 || r.ccNum > 127) continue;
						if (d1 !== r.ccNum) continue;
						value = (d2 / 127) * r.scale + r.bias;
					} else if (base === 0xE0) {
						if (r.ccNum !== -1) continue;
						const v14 = (d2 << 7) | d1; // 14-bit unsigned
						const n = (v14 / 16383) * 2 - 1; // -1..+1
						value = n * (r.scale * 0.5) + (r.bias + r.scale * 0.5);
					} else {
						if (r.ccNum !== -2) continue;
						value = (d1 / 127) * r.scale + r.bias;
					}
					if (value === null) continue;
					workletPort?.postMessage({
						type: 'setParam',
						name: r.paramName,
						value,
						slewMs: r.slewMs
					});
				}
			}
		}

		const matchingRoutes = routesByDevice.get(deviceName);
		if (!matchingRoutes || matchingRoutes.length === 0) return;

		for (const route of matchingRoutes) {
			if (route.channelFilter !== 0 && route.channelFilter !== channel1Based) {
				continue;
			}
			// Velocity-0 note-on is canonically a note-off; passing it through
			// unchanged is fine — the VM's drain path handles both spellings
			// (see cedar/src/opcodes/midi.cpp). Keep the original status byte.
			workletPort?.postMessage({
				type: 'midi',
				state_id: route.stateId,
				status: statusByte,
				d1,
				d2
			});
		}
	}

	function onStateChange(ev: MIDIConnectionEvent) {
		refreshDevices();
		// Re-resolve routes against the new device list. A device plugged
		// in after compile and matching a `device: "..."` substring should
		// start dispatching without a recompile. Same for default-device
		// routes when the user's preferred device becomes available.
		if (ev.port && ev.port.type === 'input') {
			applyRoutes();
		}
	}

	async function ensureAccess(): Promise<boolean> {
		if (!supported) {
			status = 'unsupported';
			return false;
		}
		if (status === 'active') return true;
		if (status === 'pending') return false;

		status = 'pending';
		error = null;
		try {
			access = await navigator.requestMIDIAccess({ sysex: false });
			status = 'active';
			refreshDevices();
			access.onstatechange = onStateChange;
			// Late-resolve routes that were registered before access was
			// granted — a typical sequence (panel opens, program already
			// compiled with midi() calls, user clicks "enable").
			applyRoutes();
			return true;
		} catch (err) {
			const msg = err instanceof Error ? err.message : String(err);
			// SecurityError = denied; otherwise unavailable.
			const name = err instanceof DOMException ? err.name : '';
			status = name === 'SecurityError' || name === 'NotAllowedError'
				? 'denied'
				: 'unavailable';
			error = msg;
			lastError = msg;
			access = null;
			return false;
		}
	}

	function setWorkletPort(port: MessagePort | null) {
		workletPort = port;
	}

	function setDefaultDeviceName(name: string) {
		defaultDeviceName = name;
		// Tell the worklet so the WASM side has the label for diagnostics.
		workletPort?.postMessage({ type: 'setDefaultMidiDevice', device: name });
		// Re-resolve any pending DefaultDevice routes (kind=0) and CC routes
		// against the new label — no recompile required.
		if (pendingSources.length > 0 || pendingCcRoutes.length > 0) {
			applyRoutes();
		}
	}

	/**
	 * Substring-match a `device:` field against the current device list.
	 * Returns the resolved physical device name, or empty string when no
	 * device matches yet (the route is still recorded — a later plug-in
	 * triggers `onstatechange` and the route activates).
	 */
	function resolveNamedDevice(query: string): string {
		if (!query) return '';
		const q = query.toLowerCase();
		// Prefer exact (case-insensitive) match, fall back to substring.
		for (const d of devices) {
			if (d.name.toLowerCase() === q) return d.name;
		}
		for (const d of devices) {
			if (d.name.toLowerCase().includes(q)) return d.name;
		}
		return '';
	}

	function resolveDefaultDevice(): string {
		if (defaultDeviceName) {
			const explicit = resolveNamedDevice(defaultDeviceName);
			if (explicit) return explicit;
		}
		// No explicit selection — first connected device wins. Deterministic
		// (devices are sorted by name in refreshDevices).
		for (const d of devices) {
			if (d.state === 'connected') return d.name;
		}
		return '';
	}

	function applyRoutes() {
		const next = new Map<string, ResolvedRoute[]>();

		for (const src of pendingSources) {
			let deviceName = '';
			if (src.kind === 1) {
				// NamedDevice — substring-match against the device list.
				deviceName = resolveNamedDevice(src.name);
			} else if (src.kind === 0) {
				// DefaultDevice — settings + first-available fallback.
				deviceName = resolveDefaultDevice();
			} else {
				// File (kind=2) — Phase 5; nothing to route from a physical
				// device. Skip.
				continue;
			}

			if (!deviceName) continue;
			const list = next.get(deviceName) || [];
			list.push({ stateId: src.stateId, channelFilter: src.channel });
			next.set(deviceName, list);
		}

		routesByDevice = next;

		// PRD §4.8 v1: all CC routes ride the resolved default device.
		const nextCc = new Map<string, RequiredMidiCcRoute[]>();
		if (pendingCcRoutes.length > 0) {
			const defaultDev = resolveDefaultDevice();
			if (defaultDev) {
				nextCc.set(defaultDev, pendingCcRoutes.slice());
			}
			// If no default device matches yet, ccRoutes stay pending —
			// they'll resolve when a device plugs in (onStateChange) or the
			// user picks a default in the UI (setDefaultDeviceName).
		}
		ccRoutesByDevice = nextCc;

		attachForRoutes();
	}

	/**
	 * Apply the required_midi_sources list from the most recent compile.
	 * Builds a per-device route table and rewires MIDIInput listeners.
	 * Idempotent — safe to call on every compile.
	 *
	 * Pre-condition: ensureAccess() has been called and resolved. Calling
	 * this with `status !== 'active'` records the routes for when access
	 * is eventually granted; resolution is re-attempted on each device
	 * statechange and after a successful ensureAccess() call.
	 */
	function setRoutes(sources: RequiredMidiSource[], ccRoutes: RequiredMidiCcRoute[] = []) {
		pendingSources = sources.slice();
		pendingCcRoutes = ccRoutes.slice();
		applyRoutes();
	}

	function destroy() {
		detachAll();
		if (access) {
			access.onstatechange = null;
		}
		access = null;
		routesByDevice = new Map();
		ccRoutesByDevice = new Map();
		workletPort = null;
	}

	return {
		get supported() { return supported; },
		get status() { return status; },
		get error() { return error; },
		get devices() { return devices; },
		get defaultDeviceName() { return defaultDeviceName; },
		get lastEventTime() { return lastEventTime; },
		get lastError() { return lastError; },
		get routeCount() { return routesByDevice.size; },

		ensureAccess,
		setWorkletPort,
		setDefaultDeviceName,
		setRoutes,
		refreshDevices,
		destroy
	};
}

export type MidiInputController = ReturnType<typeof createMidiInputController>;

/**
 * Inert controller for backends without Web-MIDI. The native (JUCE) backend
 * returns null from getMidiController() — the HOST owns MIDI devices there
 * (prd-midi-input is web-only) — but panel components dereference
 * `audioEngine.midi.status` unconditionally. Handing them this stub renders
 * an honest "Not supported" instead of a TypeError that aborts the whole
 * route mount (the native black-screen bug).
 */
export const inertMidiInputController: MidiInputController = {
	supported: false,
	status: 'unsupported',
	error: null,
	devices: [],
	defaultDeviceName: '',
	lastEventTime: 0,
	lastError: null,
	routeCount: 0,

	ensureAccess: async () => false,
	setWorkletPort: () => {},
	setDefaultDeviceName: () => {},
	setRoutes: () => {},
	refreshDevices: () => {},
	destroy: () => {}
};
