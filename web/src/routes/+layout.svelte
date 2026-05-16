<script lang="ts">
	import '../app.css';
	import { onMount } from 'svelte';
	import { initializeDocs } from '$lib/docs';
	import { themeStore } from '$lib/stores/theme.svelte';
	import { audioEngine } from '$lib/stores/audio.svelte';

	let { children } = $props();

	onMount(() => {
		initializeDocs();
		themeStore.initialize();
		// Bootstrap the audio engine eagerly so WASM + worklet are ready by
		// the time the user drops a file or presses play. The AudioContext
		// starts suspended (no user gesture yet); play() will resume it.
		// Load functions also await initialize() themselves, so a drop
		// that races this fire-and-forget call still resolves correctly.
		audioEngine.initialize().catch((err) => {
			console.error('[AudioEngine] Eager init failed:', err);
		});
	});
</script>

<svelte:head>
	<link rel="preconnect" href="https://fonts.googleapis.com" />
	<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin="anonymous" />
	<link
		href="https://fonts.googleapis.com/css2?family=Fira+Code:wght@400;500;600&display=swap"
		rel="stylesheet"
	/>
</svelte:head>

{@render children()}
