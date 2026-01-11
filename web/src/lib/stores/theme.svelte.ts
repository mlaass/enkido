/**
 * Theme store with localStorage persistence and CSS variable application
 */

import type { Theme, ThemeColors, ThemeStorageData } from '$lib/types/theme';
import { COLOR_KEYS, colorKeyToCssVar } from '$lib/types/theme';
import { PRESET_THEMES, DEFAULT_THEME_ID, getPresetById } from '$lib/themes/presets';

const STORAGE_KEY = 'enkido-theme';

function loadThemeData(): ThemeStorageData {
	const defaultData: ThemeStorageData = {
		activeThemeId: DEFAULT_THEME_ID,
		followSystem: false,
		customThemes: []
	};

	if (typeof localStorage === 'undefined') return defaultData;

	try {
		const stored = localStorage.getItem(STORAGE_KEY);
		if (stored) {
			return { ...defaultData, ...JSON.parse(stored) };
		}

		// Migration: check old settings store for theme preference
		const oldSettings = localStorage.getItem('enkido-settings');
		if (oldSettings) {
			const parsed = JSON.parse(oldSettings);
			if (parsed.theme) {
				if (parsed.theme === 'light') {
					defaultData.activeThemeId = 'github-light';
				} else if (parsed.theme === 'system') {
					defaultData.followSystem = true;
				}
				// Remove theme from old settings
				delete parsed.theme;
				localStorage.setItem('enkido-settings', JSON.stringify(parsed));
			}
		}
	} catch (e) {
		console.warn('Failed to load theme data:', e);
	}

	return defaultData;
}

function getSystemPreference(): 'dark' | 'light' {
	if (typeof window === 'undefined') return 'dark';
	return window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
}

function createThemeStore() {
	const data = loadThemeData();

	let activeThemeId = $state(data.activeThemeId);
	let followSystem = $state(data.followSystem);
	let customThemes = $state<Theme[]>(data.customThemes);
	let systemPreference = $state<'dark' | 'light'>(getSystemPreference());

	// Listen for system preference changes
	if (typeof window !== 'undefined') {
		const mediaQuery = window.matchMedia('(prefers-color-scheme: light)');
		mediaQuery.addEventListener('change', (e) => {
			systemPreference = e.matches ? 'light' : 'dark';
			if (followSystem) {
				applyTheme(getActiveTheme());
			}
		});
	}

	function save() {
		if (typeof localStorage === 'undefined') return;
		try {
			const data: ThemeStorageData = {
				activeThemeId,
				followSystem,
				customThemes
			};
			localStorage.setItem(STORAGE_KEY, JSON.stringify(data));
		} catch (e) {
			console.warn('Failed to save theme data:', e);
		}
	}

	function getAllThemes(): Theme[] {
		return [...PRESET_THEMES, ...customThemes];
	}

	function getThemeById(id: string): Theme | undefined {
		return getPresetById(id) || customThemes.find((t) => t.id === id);
	}

	function getActiveTheme(): Theme {
		if (followSystem) {
			// Map system preference to default themes
			return systemPreference === 'light'
				? getPresetById('github-light')!
				: getPresetById('github-dark')!;
		}
		return getThemeById(activeThemeId) || getPresetById(DEFAULT_THEME_ID)!;
	}

	function applyTheme(theme: Theme) {
		if (typeof document === 'undefined') return;

		const root = document.documentElement;
		for (const key of COLOR_KEYS) {
			const cssVar = colorKeyToCssVar(key);
			root.style.setProperty(cssVar, theme.colors[key]);
		}
	}

	function setActiveTheme(id: string) {
		activeThemeId = id;
		save();
		applyTheme(getActiveTheme());
	}

	function setFollowSystem(follow: boolean) {
		followSystem = follow;
		save();
		applyTheme(getActiveTheme());
	}

	function saveCustomTheme(theme: Theme) {
		const existing = customThemes.findIndex((t) => t.id === theme.id);
		if (existing >= 0) {
			customThemes[existing] = theme;
		} else {
			customThemes = [...customThemes, theme];
		}
		save();
	}

	function deleteCustomTheme(id: string) {
		customThemes = customThemes.filter((t) => t.id !== id);
		// If deleted theme was active, switch to default
		if (activeThemeId === id) {
			activeThemeId = DEFAULT_THEME_ID;
		}
		save();
		applyTheme(getActiveTheme());
	}

	function applyColors(colors: ThemeColors) {
		if (typeof document === 'undefined') return;

		const root = document.documentElement;
		for (const key of COLOR_KEYS) {
			const cssVar = colorKeyToCssVar(key);
			root.style.setProperty(cssVar, colors[key]);
		}
	}

	function initialize() {
		applyTheme(getActiveTheme());
	}

	return {
		get activeThemeId() {
			return activeThemeId;
		},
		get followSystem() {
			return followSystem;
		},
		get customThemes() {
			return customThemes;
		},
		get systemPreference() {
			return systemPreference;
		},
		get activeTheme() {
			return getActiveTheme();
		},
		get allThemes() {
			return getAllThemes();
		},
		get presetThemes() {
			return PRESET_THEMES;
		},

		setActiveTheme,
		setFollowSystem,
		saveCustomTheme,
		deleteCustomTheme,
		applyColors,
		getThemeById,
		initialize
	};
}

export const themeStore = createThemeStore();
