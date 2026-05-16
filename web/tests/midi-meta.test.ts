import { describe, it, expect } from 'vitest';
import { parseMidiMeta } from '../src/lib/audio/midi-meta';

function buildFixture(): ArrayBuffer {
	// Hand-built Standard MIDI File, Format 1, 2 tracks, division 96.
	// Track 1: channel 0, program 4 (Electric Piano 1), one note.
	// Track 2: channel 9 (drum), two notes via running status.
	const headerChunk = [
		0x4d, 0x54, 0x68, 0x64, // "MThd"
		0x00, 0x00, 0x00, 0x06, // length 6
		0x00, 0x01,             // format 1
		0x00, 0x02,             // 2 tracks
		0x00, 0x60              // division 96
	];

	const track1Events = [
		0x00, 0xc0, 0x04,                   // delta 0, program change ch0 -> 4
		0x00, 0x90, 0x3c, 0x64,             // delta 0, note on ch0 note 60 vel 100
		0x60, 0x80, 0x3c, 0x00,             // delta 96, note off ch0
		0x00, 0xff, 0x2f, 0x00              // end of track
	];
	const track1Chunk = [
		0x4d, 0x54, 0x72, 0x6b,             // "MTrk"
		0x00, 0x00, 0x00, track1Events.length,
		...track1Events
	];

	const track2Events = [
		0x00, 0x99, 0x24, 0x64,             // delta 0, note on ch9 (drum) note 36 vel 100
		0x10, 0x26, 0x50,                   // delta 16, running status -> note on ch9 note 38 vel 80
		0x00, 0xff, 0x2f, 0x00              // end of track
	];
	const track2Chunk = [
		0x4d, 0x54, 0x72, 0x6b,
		0x00, 0x00, 0x00, track2Events.length,
		...track2Events
	];

	return new Uint8Array([...headerChunk, ...track1Chunk, ...track2Chunk]).buffer;
}

describe('parseMidiMeta', () => {
	it('extracts channels, note counts, and program changes from a Format 1 file', () => {
		const meta = parseMidiMeta(buildFixture());

		expect(meta.format).toBe(1);
		expect(meta.trackCount).toBe(2);
		expect(meta.channels).toHaveLength(2);

		const ch0 = meta.channels.find((c) => c.channel === 0);
		expect(ch0).toBeDefined();
		expect(ch0?.noteCount).toBe(1);
		expect(ch0?.program).toBe(4);
		expect(ch0?.isDrum).toBe(false);

		const ch9 = meta.channels.find((c) => c.channel === 9);
		expect(ch9).toBeDefined();
		expect(ch9?.noteCount).toBe(2); // running status carries the second note
		expect(ch9?.isDrum).toBe(true);
	});

	it('returns empty meta for non-MIDI bytes without throwing', () => {
		const garbage = new Uint8Array([0x00, 0x01, 0x02, 0xff, 0xfe]).buffer;
		const meta = parseMidiMeta(garbage);
		expect(meta.channels).toHaveLength(0);
		expect(meta.trackCount).toBe(0);
	});

	it('treats velocity-0 note-on as note-off (does not count it)', () => {
		// Format 0, single track, two events: note-on vel 100, note-on vel 0.
		const events = [
			0x00, 0x90, 0x3c, 0x64,
			0x60, 0x90, 0x3c, 0x00, // velocity 0 = note-off equivalent
			0x00, 0xff, 0x2f, 0x00
		];
		const bytes = new Uint8Array([
			0x4d, 0x54, 0x68, 0x64,
			0x00, 0x00, 0x00, 0x06,
			0x00, 0x00, // format 0
			0x00, 0x01, // 1 track
			0x00, 0x60,
			0x4d, 0x54, 0x72, 0x6b,
			0x00, 0x00, 0x00, events.length,
			...events
		]).buffer;

		const meta = parseMidiMeta(bytes);
		expect(meta.channels).toHaveLength(1);
		expect(meta.channels[0].noteCount).toBe(1);
	});
});
