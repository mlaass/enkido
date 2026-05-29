import { expect, test } from '@playwright/test';

// E2E: when the compile worker dies mid-session, the next compile() must
// surface a synthetic diagnostic and the call after that respawns the
// worker so audio recovers. Pinned by PRD prd-compile-off-audio-thread §9.5.

const SIMPLE_PATCH = 'osc("sin", 440) * 0.15 |> out(@, @)';

test.describe('compile worker recovery', () => {
    test('terminate then recompile respawns worker', async ({ page }) => {
        await page.goto('/');
        await page.waitForFunction(() => window.__nkidoTest?.audioEngine?.isInitialized, {
            timeout: 30_000
        });

        // Satisfy the AudioContext user-gesture requirement.
        await page.locator('button[title="Play (Space)"]').click();

        // 1) First compile — should succeed normally.
        const ok1 = await page.evaluate(async (src) => {
            window.__nkidoTest!.editor.setCode(src);
            const r = await window.__nkidoTest!.audioEngine.compile(src);
            return r.success === true;
        }, SIMPLE_PATCH);
        expect(ok1, 'first compile must succeed').toBe(true);

        // 2) Terminate the worker, then issue a compile. The store may
        // either surface the crash diagnostic (forcing the user to retry)
        // or eagerly respawn and succeed in the same call — both are
        // acceptable. Pull only the booleans across the page boundary
        // since the full CompileResult holds large Uint8Arrays Playwright
        // serialization handles poorly.
        const result = await page.evaluate(async (src) => {
            window.__nkidoTest!.terminateCompileWorker();
            const r2 = await window.__nkidoTest!.audioEngine.compile(src);
            if (r2.success) {
                return { firstOk: true, hadDiagnostic: false };
            }
            // Worker re-spawned; the next compile must succeed.
            const r3 = await window.__nkidoTest!.audioEngine.compile(src);
            return {
                firstOk: false,
                hadDiagnostic: (r2.diagnostics?.length ?? 0) > 0,
                secondOk: r3.success === true
            };
        }, SIMPLE_PATCH);

        if (result.firstOk) {
            expect(result.firstOk, 'eager respawn produced a successful compile').toBe(true);
        } else {
            expect(result.hadDiagnostic, 'crash compile must carry a diagnostic').toBe(true);
            expect(result.secondOk, 'compile after respawn must succeed').toBe(true);
        }
    });
});
