import { t } from '@lingui/core/macro';

// People who share the machine (firmware: src/display/core/Users.h). Each has a name, a display theme and a list
// of profile ids only they see; the firmware keeps each person's brew defaults.
export const MAX_PEOPLE = 4;
export const MAX_NAME_LENGTH = 16;

// The display theme is one number on the machine (2 is the automatic AMOLED black variant of 0), but it is
// picked as a colour and a light or dark mode: every colour comes in both.
const THEME_IDS = {
  oxblood: { dark: 0, light: 1 },
  olive: { dark: 3, light: 5 },
  purple: { dark: 4, light: 6 },
};
export const displayColors = () => [
  { value: 'oxblood', label: t({ message: 'Oxblood', context: 'theme' }) },
  { value: 'olive', label: t({ message: 'Olive', context: 'theme' }) },
  { value: 'purple', label: t({ message: 'Purple', context: 'theme' }) },
];
export const displayModes = () => [
  { value: 'dark', label: t({ message: 'Dark', context: 'theme' }) },
  { value: 'light', label: t({ message: 'Light', context: 'theme' }) },
];
export const themeFor = (color, mode) => THEME_IDS[color]?.[mode] ?? 0;
export const themeParts = theme => {
  for (const [color, modes] of Object.entries(THEME_IDS)) {
    for (const [mode, id] of Object.entries(modes)) {
      if (id === Number(theme)) return { color, mode };
    }
  }
  return { color: 'oxblood', mode: 'dark' };
};

// What is drawn behind the screens (firmware: backdrop::Style). Any theme works with any background.
export const DEFAULT_BACKGROUND = 1;
// Backgrounds used to be able to drift; a saved drifting value now means the same picture standing still.
export const stillBackground = style => ({ 2: 1, 3: 1, 6: 5, 7: 5 })[Number(style)] ?? Number(style);
export const backgroundStyles = () => [
  { value: 0, label: t`Solid color` },
  { value: 1, label: t`Coffee beans` },
  { value: 5, label: t`Floral` },
  { value: 4, label: t`Theme artwork` },
];

// The data-bg value the page styles use for a background style.
export const webBackgroundForStyle = style =>
  ({
    0: 'solid',
    1: 'beans',
    2: 'beans',
    3: 'beans',
    4: 'art',
    5: 'floral',
    6: 'floral',
    7: 'floral',
  })[style] ?? 'solid';

// Web theme that matches a display theme, for the "match the machine" web theme option.
export const webThemeForDisplayTheme = theme =>
  ({ 0: 'dark', 1: 'light', 2: 'dark', 3: 'olive', 4: 'purple', 5: 'olive-light', 6: 'purple-light' })[
    theme
  ] ?? 'dark';

export const initialOf = name => (String(name ?? '').match(/[a-z]/i)?.[0] ?? '?').toUpperCase();

export async function fetchPeople() {
  const settings = await (await fetch('/api/settings')).json();
  const users = Array.isArray(settings.users) ? settings.users : [];
  const activeUser = Math.min(
    Math.max(Number(settings.activeUser ?? 0), 0),
    Math.max(users.length - 1, 0),
  );
  return { users, activeUser };
}

// Saves the list (names, themes, private profile ids). The firmware keeps each person's stored defaults by name.
export async function savePeople(users) {
  const response = await fetch('/api/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ users: JSON.stringify(users) }).toString(),
  });
  if (!response.ok) throw new Error(`POST /api/settings ${response.status}`);
}
