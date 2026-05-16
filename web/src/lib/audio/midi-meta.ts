// Minimal Standard MIDI File scanner. Walks chunk-by-chunk just enough to
// surface what the Files panel needs: which channels carry note events,
// how many notes each channel plays, and the last PROGRAM_CHANGE seen so
// we can label it with a GM instrument name.
//
// Not a general MIDI parser — it ignores tempo, time signatures, lyrics,
// and anything else that doesn't move a note. Corrupt input returns an
// empty meta rather than throwing, so the UI degrades to filename-only.

export interface MidiChannelInfo {
	channel: number;
	noteCount: number;
	program: number;
	isDrum: boolean;
}

export interface MidiMeta {
	channels: MidiChannelInfo[];
	trackCount: number;
	format: number;
}

const EMPTY: MidiMeta = { channels: [], trackCount: 0, format: 0 };

export function parseMidiMeta(bytes: ArrayBuffer): MidiMeta {
	try {
		const view = new DataView(bytes);
		const total = view.byteLength;
		let pos = 0;

		if (total < 14 || readAscii(view, 0, 4) !== 'MThd') return EMPTY;
		const headerLen = view.getUint32(4);
		const format = view.getUint16(8);
		const trackCount = view.getUint16(10);
		pos = 8 + headerLen;

		const perChannel = new Map<number, MidiChannelInfo>();

		while (pos + 8 <= total) {
			const chunkId = readAscii(view, pos, 4);
			const chunkLen = view.getUint32(pos + 4);
			pos += 8;
			const chunkEnd = pos + chunkLen;
			if (chunkEnd > total) break;

			if (chunkId === 'MTrk') {
				scanTrack(view, pos, chunkEnd, perChannel);
			}
			pos = chunkEnd;
		}

		const channels = [...perChannel.values()].sort((a, b) => a.channel - b.channel);
		return { channels, trackCount, format };
	} catch {
		return EMPTY;
	}
}

function scanTrack(
	view: DataView,
	start: number,
	end: number,
	perChannel: Map<number, MidiChannelInfo>
): void {
	let pos = start;
	let runningStatus = 0;

	while (pos < end) {
		// Delta time — VLQ; we discard the value, just advance past it.
		const dt = readVlq(view, pos, end);
		if (dt < 0) return;
		pos += dt;

		if (pos >= end) return;
		let status = view.getUint8(pos);

		if (status < 0x80) {
			// Running status: reuse last channel-voice status, no advance.
			status = runningStatus;
			if (status === 0) return;
		} else {
			pos += 1;
			if (status < 0xf0) runningStatus = status;
		}

		const high = status & 0xf0;
		const channel = status & 0x0f;

		if (status === 0xff) {
			// Meta event: type + VLQ length + data.
			if (pos >= end) return;
			pos += 1; // type
			const lenInfo = readVlqWithLen(view, pos, end);
			if (lenInfo.byteLen < 0) return;
			pos += lenInfo.byteLen + lenInfo.value;
			continue;
		}
		if (status === 0xf0 || status === 0xf7) {
			// Sysex: VLQ length + data.
			const lenInfo = readVlqWithLen(view, pos, end);
			if (lenInfo.byteLen < 0) return;
			pos += lenInfo.byteLen + lenInfo.value;
			continue;
		}

		switch (high) {
			case 0x80: // Note Off — 2 bytes
			case 0xa0: // Polyphonic Aftertouch — 2 bytes
			case 0xb0: // Control Change — 2 bytes
			case 0xe0: // Pitch Bend — 2 bytes
				pos += 2;
				break;
			case 0x90: {
				// Note On — 2 bytes; velocity 0 means a note-off and should not count.
				if (pos + 1 >= end) return;
				const velocity = view.getUint8(pos + 1);
				if (velocity > 0) {
					const info = ensureChannel(perChannel, channel);
					info.noteCount += 1;
				}
				pos += 2;
				break;
			}
			case 0xc0: {
				// Program Change — 1 byte
				if (pos >= end) return;
				const program = view.getUint8(pos);
				const info = ensureChannel(perChannel, channel);
				info.program = program;
				pos += 1;
				break;
			}
			case 0xd0: // Channel Aftertouch — 1 byte
				pos += 1;
				break;
			default:
				return;
		}
	}
}

function ensureChannel(
	perChannel: Map<number, MidiChannelInfo>,
	channel: number
): MidiChannelInfo {
	let info = perChannel.get(channel);
	if (!info) {
		info = { channel, noteCount: 0, program: 0, isDrum: channel === 9 };
		perChannel.set(channel, info);
	}
	return info;
}

function readAscii(view: DataView, offset: number, length: number): string {
	let out = '';
	for (let i = 0; i < length; i++) out += String.fromCharCode(view.getUint8(offset + i));
	return out;
}

// Returns the *number of bytes consumed* by the VLQ at `offset`, or -1 on
// malformed input. The caller uses this to advance `pos`; the actual VLQ
// value is irrelevant for delta-times here (we just want to skip past it).
function readVlq(view: DataView, offset: number, end: number): number {
	let bytes = 0;
	while (offset + bytes < end && bytes < 4) {
		const b = view.getUint8(offset + bytes);
		bytes += 1;
		if ((b & 0x80) === 0) return bytes;
	}
	return -1;
}

// Reads a VLQ and returns both its byte length and decoded value. Used
// for meta/sysex event payload lengths where the value is needed.
function readVlqWithLen(
	view: DataView,
	offset: number,
	end: number
): { value: number; byteLen: number } {
	let value = 0;
	let bytes = 0;
	while (offset + bytes < end && bytes < 4) {
		const b = view.getUint8(offset + bytes);
		value = (value << 7) | (b & 0x7f);
		bytes += 1;
		if ((b & 0x80) === 0) return { value, byteLen: bytes };
	}
	return { value: 0, byteLen: -1 };
}
