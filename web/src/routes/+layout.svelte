<script lang="ts">
	import '../app.css';
	import { onMount } from 'svelte';
	import { initializeDocs } from '$lib/docs';
	import { themeStore } from '$lib/stores/theme.svelte';

	let { children } = $props();

	// Initialize documentation and theme systems on mount (client-side only)
	onMount(() => {
		initializeDocs();
		themeStore.initialize();
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
