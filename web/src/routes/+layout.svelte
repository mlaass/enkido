<script lang="ts">
	import '../app.css';
	import { initializeDocs } from '$lib/docs';

	let { children } = $props();

	// Initialize documentation system immediately
	initializeDocs();
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
