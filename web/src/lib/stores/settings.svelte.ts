/**
 * Settings store with localStorage persistence
 */

interface Settings {
	theme: 'dark' | 'light' | 'system';
	panelPosition: 'left' | 'right';
	fontSize: number;
	bufferSize: 128 | 256 | 512 | 1024;
	sampleRate: 44100 | 48000;
}

const DEFAULT_SETTINGS: Settings = {
	theme: 'dark',
	panelPosition: 'left',
	fontSize: 14,
	bufferSize: 128,
	sampleRate: 48000
};

function loadSettings(): Settings {
	if (typeof localStorage === 'undefined') return DEFAULT_SETTINGS;

	try {
		const stored = localStorage.getItem('enkido-settings');
		if (stored) {
			return { ...DEFAULT_SETTINGS, ...JSON.parse(stored) };
		}
	} catch (e) {
		console.warn('Failed to load settings:', e);
	}
	return DEFAULT_SETTINGS;
}

function createSettingsStore() {
	let settings = $state<Settings>(loadSettings());

	function save() {
		if (typeof localStorage === 'undefined') return;
		try {
			localStorage.setItem('enkido-settings', JSON.stringify(settings));
		} catch (e) {
			console.warn('Failed to save settings:', e);
		}
	}

	function setTheme(theme: Settings['theme']) {
		settings.theme = theme;
		save();
	}

	function setPanelPosition(position: Settings['panelPosition']) {
		settings.panelPosition = position;
		save();
	}

	function setFontSize(size: number) {
		settings.fontSize = Math.max(10, Math.min(24, size));
		save();
	}

	function setBufferSize(size: Settings['bufferSize']) {
		settings.bufferSize = size;
		save();
	}

	function setSampleRate(rate: Settings['sampleRate']) {
		settings.sampleRate = rate;
		save();
	}

	function reset() {
		Object.assign(settings, DEFAULT_SETTINGS);
		save();
	}

	return {
		get theme() { return settings.theme; },
		get panelPosition() { return settings.panelPosition; },
		get fontSize() { return settings.fontSize; },
		get bufferSize() { return settings.bufferSize; },
		get sampleRate() { return settings.sampleRate; },

		setTheme,
		setPanelPosition,
		setFontSize,
		setBufferSize,
		setSampleRate,
		reset
	};
}

export const settingsStore = createSettingsStore();
