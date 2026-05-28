import { expect, test } from '@playwright/test';

// E2E regression: recompiling identical source must not introduce an
// audible gap in the audio output (user-reported 2026-05-28).
//
// The C++/CLI tests (akkado/tests/test_hot_swap_event_transforms.cpp)
// proved the cedar runtime is bit-perfect across hot-swap. So any gap
// observed in the browser is something the WASM/worklet/audio-context
// layer adds on top. This test reproduces against that exact path.
//
// Methodology:
//   1. Open the app and wait for the audio engine to initialize.
//   2. Click play (Transport button) to grant the user-gesture the
//      AudioContext needs to start (browser autoplay policy).
//   3. Set a simple known-good patch via the editor store.
//   4. Compile, wait for audio to stabilise (~250 ms).
//   5. Capture an RMS trace for ~3 s while triggering 6 identical
//      recompiles spaced ~400 ms apart.
//   6. Assert: no contiguous run of >= 3 RMS samples (~60 ms) below
//      30 % of the pre-swap median. A real audible gap is much longer.
//
// The test uses `window.__nkidoTest` (installed by src/lib/test-hooks.ts).

const SIMPLE_PATCH = 'osc("sin", 440) * 0.15 |> out(@, @)';

const PAD_PATCH = `
bpm = 135
fn pad_voice(freq, gate, vel, ext) ->
    saw(freq, ext.phase) * adsr(gate, 0.5, 0.6, 0.7, 1.2) * vel
    |> lp(@, 2800)

fn fat({freq, gate, vel}) ->
    unison(freq, gate, vel, pad_voice,
           voices: 3, detune: 0.3, width: .7, phase: 0.2)

c"Cmaj9 Am11@2 Fmaj7 G9@2".slow(2).transpose(-12)
    |> poly(@, fat, 8, 3) * 0.2
    |> reverb(@, 0.75, 0.6, ..{wet: 0.1})
    |> out(@)
`;

async function bootAudioAndCompile(page: import('@playwright/test').Page, source: string) {
	await page.goto('/');

	// Wait for the test hooks + audio engine to be ready.
	await page.waitForFunction(() => window.__nkidoTest?.audioEngine?.isInitialized, {
		timeout: 30_000
	});

	// Click the play button to satisfy the user-gesture requirement for
	// the AudioContext to leave the `suspended` state. The Transport
	// component renders the play button with title "Play (Space)".
	await page.locator('button[title="Play (Space)"]').click();

	// Set the source and trigger initial compile via the test hook.
	await page.evaluate(async (src) => {
		const hook = window.__nkidoTest!;
		hook.editor.setCode(src);
		await hook.editor.evaluate();
	}, source);

	// Let audio stabilise after first load (warmup + crossfade settle).
	await page.waitForTimeout(500);
}

test.describe('hot-swap audio continuity', () => {
	test('simple osc patch: identical recompile maintains audio level', async ({ page }) => {
		await bootAudioAndCompile(page, SIMPLE_PATCH);

		// Capture in parallel: start RMS capture, kick off 6 recompiles
		// inside the capture window.
		const { trace, swapCount } = await page.evaluate(async () => {
			const hook = window.__nkidoTest!;
			const traceP = hook.captureRmsTrace(3000, 20);
			const swaps: Promise<unknown>[] = [];
			for (let i = 0; i < 6; i++) {
				await new Promise((r) => setTimeout(r, 400));
				swaps.push(hook.editor.evaluate());
			}
			await Promise.all(swaps);
			const t = await traceP;
			return { trace: t.rms, swapCount: 6 };
		});

		expect(swapCount).toBe(6);
		expect(trace.length).toBeGreaterThan(50);

		// Median of the first 25 RMS samples (~500 ms) is the pre-storm
		// baseline (recompiles start at t=400 ms).
		const baselineSorted = [...trace.slice(0, 25)].sort((a, b) => a - b);
		const baselineMedian = baselineSorted[Math.floor(baselineSorted.length / 2)];

		expect(baselineMedian).toBeGreaterThan(0.001);  // audio is actually playing

		// Scan for a run of 3+ samples (60 ms) below 30% of baseline.
		const threshold = 0.3 * baselineMedian;
		let cur = 0;
		let worstRun = 0;
		let worstRunStart = 0;
		for (let i = 0; i < trace.length; i++) {
			if (trace[i] < threshold) {
				if (cur === 0) worstRunStart = i;
				cur++;
				if (cur > worstRun) worstRun = cur;
			} else {
				cur = 0;
			}
		}

		const detail = `baseline median=${baselineMedian.toFixed(4)} threshold=${threshold.toFixed(4)} worst gap run=${worstRun} samples (${worstRun * 20}ms) starting at sample ${worstRunStart} (${worstRunStart * 20}ms)`;
		console.log(`[hot-swap test] ${detail}`);
		console.log(`[hot-swap test] trace (first 60): ${trace.slice(0, 60).map((v) => v.toFixed(3)).join(' ')}`);

		expect(worstRun, detail).toBeLessThan(3);
	});

	test("user's unison-pad: identical recompile maintains audio level", async ({ page }) => {
		const logs: string[] = [];
		page.on('console', (msg) => {
			const text = msg.text();
			// Capture only worklet + audio-engine-relevant logs to keep output manageable.
			if (
				text.includes('CedarProcessor') ||
				text.includes('AudioEngine') ||
				text.includes('[Editor]') ||
				text.includes('silent') ||
				text.includes('Load') ||
				text.includes('swap')
			) {
				logs.push(`[${msg.type()}] ${text}`);
			}
		});

		await bootAudioAndCompile(page, PAD_PATCH);

		const { trace, swapCount } = await page.evaluate(async () => {
			const hook = window.__nkidoTest!;
			const traceP = hook.captureRmsTrace(3500, 20);
			const swaps: Promise<unknown>[] = [];
			for (let i = 0; i < 6; i++) {
				await new Promise((r) => setTimeout(r, 500));
				swaps.push(hook.editor.evaluate());
			}
			await Promise.all(swaps);
			const t = await traceP;
			return { trace: t.rms, swapCount: 6 };
		});

		expect(swapCount).toBe(6);
		expect(trace.length).toBeGreaterThan(50);

		// For the pad, median over the first 25 samples (~500 ms before
		// recompiles begin) is the baseline.
		const baselineSorted = [...trace.slice(0, 25)].sort((a, b) => a - b);
		const baselineMedian = baselineSorted[Math.floor(baselineSorted.length / 2)];

		expect(baselineMedian).toBeGreaterThan(0.001);

		// Pad has natural ADSR envelope variation, so use a slightly more
		// permissive deep threshold (15 %) but a longer minimum gap (5
		// samples = 100 ms) — well below a single ADSR cycle.
		const threshold = 0.15 * baselineMedian;
		let cur = 0;
		let worstRun = 0;
		let worstRunStart = 0;
		for (let i = 0; i < trace.length; i++) {
			if (trace[i] < threshold) {
				if (cur === 0) worstRunStart = i;
				cur++;
				if (cur > worstRun) worstRun = cur;
			} else {
				cur = 0;
			}
		}

		const detail = `baseline median=${baselineMedian.toFixed(4)} threshold=${threshold.toFixed(4)} worst gap run=${worstRun} samples (${worstRun * 20}ms) starting at sample ${worstRunStart} (${worstRunStart * 20}ms)`;
		console.log(`[hot-swap pad test] ${detail}`);
		console.log(`[hot-swap pad test] trace (first 60): ${trace.slice(0, 60).map((v) => v.toFixed(3)).join(' ')}`);

		console.log('\n=== BROWSER CONSOLE (filtered) ===');
		for (const l of logs) console.log(l);
		console.log('=== END BROWSER CONSOLE ===\n');

		expect(worstRun, detail).toBeLessThan(5);
	});
});
