<script lang="ts">
	import '../app.css';
	import { onMount } from 'svelte';
	import { initializeDocs } from '$lib/docs';
	import { settingsStore } from '$lib/stores/settings.svelte';

	let { children } = $props();

	// Initialize documentation system on mount (client-side only)
	onMount(() => {
		initializeDocs();
	});

	// Apply theme to document root
	$effect(() => {
		const theme = settingsStore.theme;
		if (typeof document === 'undefined') return;

		if (theme === 'system') {
			document.documentElement.removeAttribute('data-theme');
		} else {
			document.documentElement.setAttribute('data-theme', theme);
		}
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
