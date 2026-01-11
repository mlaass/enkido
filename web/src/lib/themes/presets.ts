import type { Theme } from '$lib/types/theme';

export const PRESET_THEMES: Theme[] = [
	{
		id: 'github-dark',
		name: 'GitHub Dark',
		isBuiltin: true,
		isDark: true,
		colors: {
			bgPrimary: '#0d1117',
			bgSecondary: '#161b22',
			bgTertiary: '#21262d',
			bgHover: '#30363d',
			textPrimary: '#e6edf3',
			textSecondary: '#8b949e',
			textMuted: '#6e7681',
			accentPrimary: '#58a6ff',
			accentSecondary: '#3fb950',
			accentWarning: '#d29922',
			accentError: '#f85149',
			borderDefault: '#30363d',
			borderMuted: '#21262d'
		}
	},
	{
		id: 'github-light',
		name: 'GitHub Light',
		isBuiltin: true,
		isDark: false,
		colors: {
			bgPrimary: '#ffffff',
			bgSecondary: '#f6f8fa',
			bgTertiary: '#ebeef1',
			bgHover: '#d0d7de',
			textPrimary: '#1f2328',
			textSecondary: '#656d76',
			textMuted: '#8c959f',
			accentPrimary: '#0969da',
			accentSecondary: '#1a7f37',
			accentWarning: '#9a6700',
			accentError: '#cf222e',
			borderDefault: '#d0d7de',
			borderMuted: '#ebeef1'
		}
	},
	{
		id: 'monokai',
		name: 'Monokai',
		isBuiltin: true,
		isDark: true,
		colors: {
			bgPrimary: '#272822',
			bgSecondary: '#1e1f1c',
			bgTertiary: '#3e3d32',
			bgHover: '#49483e',
			textPrimary: '#f8f8f2',
			textSecondary: '#a6a69c',
			textMuted: '#75715e',
			accentPrimary: '#66d9ef',
			accentSecondary: '#a6e22e',
			accentWarning: '#e6db74',
			accentError: '#f92672',
			borderDefault: '#49483e',
			borderMuted: '#3e3d32'
		}
	},
	{
		id: 'dracula',
		name: 'Dracula',
		isBuiltin: true,
		isDark: true,
		colors: {
			bgPrimary: '#282a36',
			bgSecondary: '#21222c',
			bgTertiary: '#343746',
			bgHover: '#44475a',
			textPrimary: '#f8f8f2',
			textSecondary: '#bfbfbf',
			textMuted: '#6272a4',
			accentPrimary: '#bd93f9',
			accentSecondary: '#50fa7b',
			accentWarning: '#ffb86c',
			accentError: '#ff5555',
			borderDefault: '#44475a',
			borderMuted: '#343746'
		}
	},
	{
		id: 'solarized-dark',
		name: 'Solarized Dark',
		isBuiltin: true,
		isDark: true,
		colors: {
			bgPrimary: '#002b36',
			bgSecondary: '#073642',
			bgTertiary: '#094352',
			bgHover: '#0a5565',
			textPrimary: '#839496',
			textSecondary: '#657b83',
			textMuted: '#586e75',
			accentPrimary: '#268bd2',
			accentSecondary: '#859900',
			accentWarning: '#b58900',
			accentError: '#dc322f',
			borderDefault: '#094352',
			borderMuted: '#073642'
		}
	},
	{
		id: 'nord',
		name: 'Nord',
		isBuiltin: true,
		isDark: true,
		colors: {
			bgPrimary: '#2e3440',
			bgSecondary: '#3b4252',
			bgTertiary: '#434c5e',
			bgHover: '#4c566a',
			textPrimary: '#eceff4',
			textSecondary: '#d8dee9',
			textMuted: '#7b88a1',
			accentPrimary: '#88c0d0',
			accentSecondary: '#a3be8c',
			accentWarning: '#ebcb8b',
			accentError: '#bf616a',
			borderDefault: '#4c566a',
			borderMuted: '#434c5e'
		}
	},
	{
		id: 'high-contrast',
		name: 'High Contrast',
		isBuiltin: true,
		isDark: true,
		colors: {
			bgPrimary: '#000000',
			bgSecondary: '#0a0a0a',
			bgTertiary: '#1a1a1a',
			bgHover: '#2a2a2a',
			textPrimary: '#ffffff',
			textSecondary: '#cccccc',
			textMuted: '#888888',
			accentPrimary: '#00ffff',
			accentSecondary: '#00ff00',
			accentWarning: '#ffff00',
			accentError: '#ff0000',
			borderDefault: '#444444',
			borderMuted: '#333333'
		}
	}
];

export const DEFAULT_THEME_ID = 'github-dark';

export function getPresetById(id: string): Theme | undefined {
	return PRESET_THEMES.find((t) => t.id === id);
}

export function getPresetByName(name: string): Theme | undefined {
	return PRESET_THEMES.find((t) => t.name === name);
}
