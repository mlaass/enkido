<script lang="ts">
	interface Props {
		size?: number;
		showBackground?: boolean;
	}

	let { size = 24, showBackground = false }: Props = $props();
</script>

<svg
	width={size}
	height={size}
	viewBox="0 0 512 512"
	aria-label="NKIDO"
	class="logo"
>
	{#if showBackground}
		<rect
			width="512"
			height="512"
			rx="32"
			ry="32"
			fill="var(--bg-tertiary)"
		/>
	{/if}
	<g fill="currentColor">
		<!-- Left vertical bars -->
		<rect
			x="32.64"
			y="29.76"
			width="64"
			height="224"
			rx="21.79"
			ry="20.11"
		/>
		<rect
			x="160.64"
			y="29.76"
			width="64"
			height="224"
			rx="21.79"
			ry="20.11"
		/>
		<!-- Right vertical bar -->
		<rect
			x="386.64"
			y="27.76"
			width="64"
			height="224"
			rx="21.79"
			ry="20.11"
		/>
		<!-- Diagonal bar -->
		<rect
			x="-12.04"
			y="70.53"
			width="64"
			height="243.1"
			rx="18.39"
			ry="20.11"
			transform="rotate(-36.25)"
		/>
		<!-- K crossbars -->
		<g transform="translate(38.64,-98.24)">
			<rect
				x="-61.11"
				y="293.07"
				width="64"
				height="161.93"
				rx="21.79"
				ry="20.11"
				transform="rotate(-45.88)"
			/>
			<rect
				x="-350.02"
				y="-50.81"
				width="64"
				height="161.93"
				rx="21.79"
				ry="20.11"
				transform="rotate(-135.88)"
			/>
		</g>
		<!-- O ring -->
		<path
			d="m 349.44,266.85 c -61.48,0 -112,50.52 -112,112 0,61.48 50.52,112 112,112 61.48,0 112,-50.52 112,-112 0,-61.48 -50.52,-112 -112,-112 z m 0,64 c 26.89,0 48,21.11 48,48 0,26.89 -21.11,48 -48,48 -26.89,0 -48,-21.11 -48,-48 0,-26.89 21.11,-48 48,-48 z"
		/>
		<!-- D shape -->
		<path
			d="m 57.88,267.85 c -14.22,2.64 -24.44,10.17 -24.44,19.11 v 0 91.89 92.53 0 c 0,8.46 9.16,15.66 22.2,18.64 57.1,-4.26 107.6,-4.39 135.36,-41.98 32.71,-42.79 32.71,-95.51 0,-138.3 -28.36,-37.11 -77.9,-37.22 -134.12,-41.85 z"
		/>
	</g>
</svg>

<style>
	.logo {
		display: block;
		flex-shrink: 0;
	}
</style>
