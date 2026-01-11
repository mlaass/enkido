/**
 * Theme color definitions matching CSS custom properties
 */
export interface ThemeColors {
	bgPrimary: string;
	bgSecondary: string;
	bgTertiary: string;
	bgHover: string;
	textPrimary: string;
	textSecondary: string;
	textMuted: string;
	accentPrimary: string;
	accentSecondary: string;
	accentWarning: string;
	accentError: string;
	borderDefault: string;
	borderMuted: string;
}

/**
 * Complete theme definition
 */
export interface Theme {
	id: string;
	name: string;
	isBuiltin: boolean;
	isDark: boolean;
	colors: ThemeColors;
}

/**
 * Theme storage schema for localStorage
 */
export interface ThemeStorageData {
	activeThemeId: string;
	followSystem: boolean;
	customThemes: Theme[];
}

/**
 * Color keys for iteration in UI
 */
export const COLOR_KEYS: (keyof ThemeColors)[] = [
	'bgPrimary',
	'bgSecondary',
	'bgTertiary',
	'bgHover',
	'textPrimary',
	'textSecondary',
	'textMuted',
	'accentPrimary',
	'accentSecondary',
	'accentWarning',
	'accentError',
	'borderDefault',
	'borderMuted'
];

/**
 * Color groups for organized UI display
 */
export const COLOR_GROUPS: { label: string; keys: (keyof ThemeColors)[] }[] = [
	{
		label: 'Background',
		keys: ['bgPrimary', 'bgSecondary', 'bgTertiary', 'bgHover']
	},
	{
		label: 'Text',
		keys: ['textPrimary', 'textSecondary', 'textMuted']
	},
	{
		label: 'Accent',
		keys: ['accentPrimary', 'accentSecondary', 'accentWarning', 'accentError']
	},
	{
		label: 'Border',
		keys: ['borderDefault', 'borderMuted']
	}
];

/**
 * Human-readable labels for color keys
 */
export const COLOR_LABELS: Record<keyof ThemeColors, string> = {
	bgPrimary: 'Primary',
	bgSecondary: 'Secondary',
	bgTertiary: 'Tertiary',
	bgHover: 'Hover',
	textPrimary: 'Primary',
	textSecondary: 'Secondary',
	textMuted: 'Muted',
	accentPrimary: 'Primary',
	accentSecondary: 'Secondary',
	accentWarning: 'Warning',
	accentError: 'Error',
	borderDefault: 'Default',
	borderMuted: 'Muted'
};

/**
 * Map theme color keys to CSS variable names
 */
export function colorKeyToCssVar(key: keyof ThemeColors): string {
	const map: Record<keyof ThemeColors, string> = {
		bgPrimary: '--bg-primary',
		bgSecondary: '--bg-secondary',
		bgTertiary: '--bg-tertiary',
		bgHover: '--bg-hover',
		textPrimary: '--text-primary',
		textSecondary: '--text-secondary',
		textMuted: '--text-muted',
		accentPrimary: '--accent-primary',
		accentSecondary: '--accent-secondary',
		accentWarning: '--accent-warning',
		accentError: '--accent-error',
		borderDefault: '--border-default',
		borderMuted: '--border-muted'
	};
	return map[key];
}
