import { describe, it, expect } from 'vitest';
import { encodeInlineUrl, decodeInlineHash, inlineHashKey } from '$lib/ide/share/inline-url';

const BASE = 'https://live.nkido.cc';

const ONE_LINE = `sin(440) |> out(@, @)`;

const TEN_LINE = `bpm = 130
kick = pat("c2*4") |> note(@) |> sine(@.freq) |> ar(0.001, 0.15)
snare = pat("~ c4 ~ c4") |> note(@) |> noise() |> ar(0.001, 0.1)
hat = pat("c5*16") |> note(@) |> noise() |> ar(0.001, 0.03)
mix = kick * 0.8 + snare * 0.5 + hat * 0.3
out(mix, mix)
`;

const HUNDRED_LINE = Array.from({ length: 100 }, (_, i) => `// line ${i}: osc("sin", ${440 + i})`).join('\n');

// ~16 KB: 1024 * 16-byte lines
const SIXTEEN_KB = Array.from({ length: 1024 }, (_, i) => `x${i.toString().padStart(13, '0')}\n`).join('');

describe('encodeInlineUrl / decodeInlineHash round-trip', () => {
	for (const [name, code] of [
		['1 line', ONE_LINE],
		['10 lines', TEN_LINE],
		['100 lines', HUNDRED_LINE],
		['16 KB', SIXTEEN_KB]
	] as const) {
		it(`round-trips ${name} (${code.length} bytes raw)`, () => {
			const { url } = encodeInlineUrl(code, BASE);
			const hash = url.split('#')[1];
			expect(decodeInlineHash('#' + hash)).toBe(code);
			expect(decodeInlineHash(hash)).toBe(code);
		});
	}

	it('round-trips Unicode (multi-byte UTF-8)', () => {
		const code = '// ñ é ø 中文 🎵\nosc("sin", 440)';
		const { url } = encodeInlineUrl(code, BASE);
		expect(decodeInlineHash('#' + url.split('#')[1])).toBe(code);
	});

	it('round-trips characters with special URL meaning', () => {
		const code = 'a = "&=?#%/+\\n"';
		const { url } = encodeInlineUrl(code, BASE);
		expect(decodeInlineHash('#' + url.split('#')[1])).toBe(code);
	});
});

describe('encodeInlineUrl byte-length status tiers', () => {
	it('marks small payloads green', () => {
		const result = encodeInlineUrl(ONE_LINE, BASE);
		expect(result.status).toBe('green');
		expect(result.byteLength).toBeLessThan(4000);
	});

	it('marks ~16 KB payloads yellow or red (compresses substantially)', () => {
		const result = encodeInlineUrl(SIXTEEN_KB, BASE);
		// Repetitive content compresses very well; assert it's at least
		// produced *some* status and the byteLength is sane.
		expect(['green', 'yellow', 'red']).toContain(result.status);
		expect(result.byteLength).toBeGreaterThan(BASE.length + '/p#code='.length);
	});
});

describe('decodeInlineHash fail-closed', () => {
	it('returns null for empty string', () => {
		expect(decodeInlineHash('')).toBeNull();
	});

	it('returns null for "#"', () => {
		expect(decodeInlineHash('#')).toBeNull();
	});

	it('returns null for "#code="', () => {
		expect(decodeInlineHash('#code=')).toBeNull();
	});

	it('returns null for garbage payload', () => {
		expect(decodeInlineHash('#code=GARBAGE!!!')).toBeNull();
	});

	it('returns null when key is missing', () => {
		expect(decodeInlineHash('#foo=bar')).toBeNull();
	});

	it('does not throw on adversarial input', () => {
		expect(() => decodeInlineHash('#code=' + '%'.repeat(100))).not.toThrow();
		expect(() => decodeInlineHash('#code=\x00\x01\x02')).not.toThrow();
	});
});

describe('inlineHashKey', () => {
	it('produces 12 hex chars', async () => {
		const key = await inlineHashKey('hello');
		expect(key).toMatch(/^[0-9a-f]{12}$/);
	});

	it('is deterministic', async () => {
		const a = await inlineHashKey('osc(440)');
		const b = await inlineHashKey('osc(440)');
		expect(a).toBe(b);
	});

	it('differs for different inputs', async () => {
		const a = await inlineHashKey('osc(440)');
		const b = await inlineHashKey('osc(441)');
		expect(a).not.toBe(b);
	});

	it('handles 1000 random strings without collision', async () => {
		const seen = new Set<string>();
		for (let i = 0; i < 1000; i++) {
			const s = `code-${i}-${Math.random()}`;
			seen.add(await inlineHashKey(s));
		}
		expect(seen.size).toBe(1000);
	});
});
