import { t } from '@lingui/core/macro';

const THEME_STORAGE_KEY = 'gaggimate-daisyui-theme';
const MACHINE_THEME_KEY = 'gaggimate-machine-theme'; // last web theme derived from the machine's display theme
const AVAILABLE_THEMES = [
  'system',
  'machine',
  'light',
  'dark',
  'olive',
  'olive-light',
  'purple',
  'purple-light',
  'coffee',
  'nord',
];

const mediaQuery = window.matchMedia('(prefers-color-scheme: dark)');

export function getSystemTheme() {
  return mediaQuery.matches ? 'dark' : 'light';
}

export function getStoredTheme() {
  try {
    const stored = localStorage.getItem(THEME_STORAGE_KEY);
    return stored && AVAILABLE_THEMES.includes(stored) ? stored : 'system';
  } catch (error) {
    console.warn('Failed to get stored theme:', error);
    return 'system';
  }
}

export function setStoredTheme(theme) {
  try {
    if (AVAILABLE_THEMES.includes(theme)) {
      localStorage.setItem(THEME_STORAGE_KEY, theme);
      applyTheme(theme);
      return true;
    }
    return false;
  } catch (error) {
    console.warn('Failed to set stored theme:', error);
    return false;
  }
}

export function applyTheme(theme) {
  if (!AVAILABLE_THEMES.includes(theme)) {
    return;
  }
  const actualTheme =
    theme === 'system' ? getSystemTheme() : theme === 'machine' ? getMachineTheme() : theme;
  document.documentElement.setAttribute('data-theme', actualTheme);
}

export function getAvailableThemes() {
  const labels = {
    system: t({ message: 'System', context: 'theme' }),
    machine: t({ message: 'Match the machine', context: 'theme' }),
    light: t({ message: 'Light', context: 'theme' }),
    dark: t({ message: 'Dark', context: 'theme' }),
    olive: t({ message: 'Olive', context: 'theme' }),
    'olive-light': t({ message: 'Olive Light', context: 'theme' }),
    'purple-light': t({ message: 'Purple Light', context: 'theme' }),
    purple: t({ message: 'Purple', context: 'theme' }),
    coffee: t({ message: 'Coffee', context: 'theme' }),
  };
  return AVAILABLE_THEMES.map(theme => ({
    value: theme,
    label: labels[theme] ?? theme.charAt(0).toUpperCase() + theme.slice(1),
  }));
}

function onSystemThemeChange() {
  if (getStoredTheme() === 'system') {
    applyTheme('system');
  }
}

// 'machine' follows the display theme of whoever is brewing; until the machine reports one, the last known
// value (or the system theme) is used.
function getMachineTheme() {
  try {
    return localStorage.getItem(MACHINE_THEME_KEY) || getSystemTheme();
  } catch {
    return getSystemTheme();
  }
}

export function setMachineTheme(webTheme) {
  try {
    if (localStorage.getItem(MACHINE_THEME_KEY) === webTheme) return;
    localStorage.setItem(MACHINE_THEME_KEY, webTheme);
  } catch {
    // Without storage the theme still follows the machine for this page view.
  }
  if (getStoredTheme() === 'machine') {
    document.documentElement.setAttribute('data-theme', webTheme);
  }
}

// The page background mirrors the machine's background style (data-bg on <html>, styled in style.css). The
// last value is kept so the next page load shows it before the machine has reported in (see index.html).
const MACHINE_BACKGROUND_KEY = 'gaggimate-machine-background';

export function setMachineBackground(background) {
  if (document.documentElement.getAttribute('data-bg') === background) return;
  document.documentElement.setAttribute('data-bg', background);
  try {
    localStorage.setItem(MACHINE_BACKGROUND_KEY, background);
  } catch {
    // Without storage the background still follows the machine for this page view.
  }
}

// Initialize theme on load
export function initializeTheme() {
  const theme = getStoredTheme();
  mediaQuery.addEventListener('change', onSystemThemeChange);
  applyTheme(theme);
}

// Simple function to handle theme change from select element
export function handleThemeChange(event) {
  const theme = event.target.value;
  setStoredTheme(theme);
}
