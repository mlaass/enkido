<script lang="ts">
	import { X, AlertTriangle } from 'lucide-svelte';
	import { getProvider } from '$lib/ide/storage';
	import { WorkerShareApiError } from '$lib/ide/storage/worker-share';
	import { markReported } from '$lib/ide/share/reported-slugs';

	type Props = {
		slug: string;
		onClose: () => void;
		onReported: () => void;
	};
	let { slug, onClose, onReported }: Props = $props();

	const provider = getProvider();

	let reason = $state('');
	let submitting = $state(false);
	let errorMsg = $state<string | null>(null);

	async function submit() {
		if (!provider.reportShare) {
			errorMsg = 'Reporting is not available in this build.';
			return;
		}
		submitting = true;
		errorMsg = null;
		try {
			await provider.reportShare(slug, reason.trim() || undefined);
			markReported(slug);
			onReported();
		} catch (e) {
			if (e instanceof WorkerShareApiError) {
				errorMsg = e.code === 'not_found'
					? 'This patch has already been removed.'
					: `Couldn't report: ${e.code}`;
			} else {
				errorMsg = `Couldn't report: ${(e as Error).message}`;
			}
		} finally {
			submitting = false;
		}
	}

	function onBackdrop(e: MouseEvent) {
		if (e.target === e.currentTarget) onClose();
	}

	function onKey(e: KeyboardEvent) {
		if (e.key === 'Escape') onClose();
	}
</script>

<svelte:window onkeydown={onKey} />

<div class="backdrop" role="presentation" onclick={onBackdrop}>
	<div class="modal" role="dialog" aria-modal="true" aria-labelledby="report-title">
		<header class="modal-header">
			<h2 id="report-title">Report patch</h2>
			<button class="icon-button close" title="Close" onclick={onClose}>
				<X size={16} />
			</button>
		</header>

		<div class="tab-body">
			<p class="info-line">
				<AlertTriangle size={14} />
				<span>Flag <code>/p/{slug}</code> for operator review. The reason is optional and visible only to operators.</span>
			</p>

			<label class="field">
				<span class="label">Reason (optional)</span>
				<textarea
					rows="2"
					maxlength="500"
					placeholder="e.g. spam, harassment, copyright"
					bind:value={reason}
					disabled={submitting}
				></textarea>
			</label>

			{#if errorMsg}
				<p class="error">{errorMsg}</p>
			{/if}

			<div class="actions">
				<button class="secondary" onclick={onClose} disabled={submitting}>Cancel</button>
				<button class="primary" onclick={submit} disabled={submitting}>
					{submitting ? 'Reporting…' : 'Report'}
				</button>
			</div>
		</div>
	</div>
</div>

<style>
	.backdrop {
		position: fixed;
		inset: 0;
		display: flex;
		align-items: center;
		justify-content: center;
		background: rgba(0, 0, 0, 0.5);
		z-index: 1000;
		padding: var(--spacing-md);
	}

	.modal {
		width: 100%;
		max-width: 460px;
		background: var(--bg-secondary);
		border: 1px solid var(--border-default);
		border-radius: 8px;
		box-shadow: 0 20px 60px rgba(0, 0, 0, 0.4);
		display: flex;
		flex-direction: column;
		max-height: 90vh;
		overflow: hidden;
	}

	.modal-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		padding: 12px 16px;
		border-bottom: 1px solid var(--border-default);
	}

	.modal-header h2 {
		margin: 0;
		font-size: 14px;
		font-weight: 600;
		color: var(--text-primary);
	}

	.icon-button {
		display: inline-flex;
		align-items: center;
		justify-content: center;
		width: 28px;
		height: 28px;
		border: none;
		background: transparent;
		color: var(--text-secondary);
		border-radius: 4px;
		cursor: pointer;
	}

	.icon-button:hover {
		background: var(--bg-hover);
		color: var(--text-primary);
	}

	.tab-body {
		padding: 16px;
		overflow-y: auto;
	}

	.info-line {
		display: flex;
		align-items: flex-start;
		gap: 6px;
		font-size: 12px;
		color: var(--text-secondary);
		margin: 0 0 12px;
		line-height: 1.4;
	}

	.field {
		display: block;
		margin-bottom: 12px;
	}

	.label {
		display: block;
		margin-bottom: 4px;
		font-size: 11px;
		font-weight: 600;
		text-transform: uppercase;
		letter-spacing: 0.04em;
		color: var(--text-secondary);
	}

	.field textarea {
		width: 100%;
		padding: 6px 8px;
		font-size: 13px;
		font-family: var(--font-sans);
		color: var(--text-primary);
		background: var(--bg-primary);
		border: 1px solid var(--border-default);
		border-radius: 4px;
		box-sizing: border-box;
		resize: vertical;
		min-height: 38px;
	}

	.field textarea:disabled {
		opacity: 0.6;
		cursor: not-allowed;
	}

	.primary, .secondary {
		display: inline-flex;
		align-items: center;
		gap: 4px;
		padding: 6px 12px;
		font-size: 12px;
		font-weight: 500;
		border-radius: 4px;
		border: 1px solid transparent;
		cursor: pointer;
		white-space: nowrap;
	}

	.primary {
		color: var(--bg-primary);
		background: var(--accent-primary);
	}

	.primary:disabled { opacity: 0.5; cursor: not-allowed; }
	.primary:hover:not(:disabled) { opacity: 0.9; }

	.secondary {
		color: var(--text-primary);
		background: var(--bg-primary);
		border-color: var(--border-default);
	}

	.secondary:hover:not(:disabled) { background: var(--bg-hover); }
	.secondary:disabled { opacity: 0.5; cursor: not-allowed; }

	.actions {
		display: flex;
		justify-content: flex-end;
		gap: 8px;
		margin-top: 12px;
	}

	.error {
		font-size: 12px;
		color: var(--accent-danger, #c0392b);
		margin: 4px 0 12px;
	}

	code {
		font-family: var(--font-mono);
		font-size: 11px;
		background: var(--bg-primary);
		padding: 1px 4px;
		border-radius: 2px;
	}
</style>
