/**
 * Default 808 drum kit sample definitions
 * These samples are auto-loaded when the audio engine initializes
 */

const BASE = '/samples/bpb_808_clean';

export interface SampleDefinition {
	name: string;
	url: string;
}

/**
 * Default drum kit - BPB Cassette 808 (Clean)
 * Maps standard drum names to 808 WAV files
 */
export const DEFAULT_DRUM_KIT: SampleDefinition[] = [
	// Core drums
	{ name: 'bd', url: `${BASE}/kick01rr1.wav` },
	{ name: 'sd', url: `${BASE}/snare01.wav` },
	{ name: 'hh', url: `${BASE}/hhclosed.wav` },
	{ name: 'oh', url: `${BASE}/hhopen.wav` },
	{ name: 'cp', url: `${BASE}/clap.wav` },
	{ name: 'rim', url: `${BASE}/rimshot.wav` },
	{ name: 'tom', url: `${BASE}/tommid.wav` },
	{ name: 'perc', url: `${BASE}/clave.wav` },
	{ name: 'cymbal', url: `${BASE}/cymbal.wav` },
	{ name: 'cowbell', url: `${BASE}/cowbell.wav` },
	{ name: 'conga', url: `${BASE}/congamid.wav` },

	// Kick variations (bd2-bd16)
	{ name: 'bd2', url: `${BASE}/kick01rr2.wav` },
	{ name: 'bd3', url: `${BASE}/kick01rr3.wav` },
	{ name: 'bd4', url: `${BASE}/kick01rr4.wav` },
	{ name: 'bd5', url: `${BASE}/kick01rr5.wav` },
	{ name: 'bd6', url: `${BASE}/kick01rr6.wav` },
	{ name: 'bd7', url: `${BASE}/kick02.wav` },
	{ name: 'bd8', url: `${BASE}/kick03.wav` },
	{ name: 'bd9', url: `${BASE}/kick04.wav` },
	{ name: 'bd10', url: `${BASE}/kick05.wav` },
	{ name: 'bd11', url: `${BASE}/kick06.wav` },
	{ name: 'bd12', url: `${BASE}/kick07.wav` },
	{ name: 'bd13', url: `${BASE}/kick08.wav` },
	{ name: 'bd14', url: `${BASE}/kick09.wav` },
	{ name: 'bd15', url: `${BASE}/kick10.wav` },
	{ name: 'bd16', url: `${BASE}/kick11.wav` },

	// Snare variations (sd2-sd8)
	{ name: 'sd2', url: `${BASE}/snare02.wav` },
	{ name: 'sd3', url: `${BASE}/snare03.wav` },
	{ name: 'sd4', url: `${BASE}/snare04.wav` },
	{ name: 'sd5', url: `${BASE}/snare05.wav` },
	{ name: 'sd6', url: `${BASE}/snare06.wav` },
	{ name: 'sd7', url: `${BASE}/snare07.wav` },
	{ name: 'sd8', url: `${BASE}/snare08.wav` },

	// Additional percussion
	{ name: 'tomhi', url: `${BASE}/tomhi.wav` },
	{ name: 'tomlo', url: `${BASE}/tomlo.wav` },
	{ name: 'congahi', url: `${BASE}/congahi.wav` },
	{ name: 'congalo', url: `${BASE}/congalo.wav` },
	{ name: 'maracas', url: `${BASE}/maracas.wav` }
];
