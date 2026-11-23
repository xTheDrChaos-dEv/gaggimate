import { t } from '@lingui/core/macro';

// People who share the machine (firmware: src/display/core/Users.h). Each has a name, a display theme and a list
// of profile ids only they see; the firmware keeps each person's brew defaults.
export const MAX_PEOPLE = 4;
export const MAX_NAME_LENGTH = 16;

// Display theme values understood by the round screen (2 is the automatic AMOLED black variant of 0).
export const displayThemes = () => [
  { value: 0, label: t`Dark Theme` },
  { value: 1, label: t`Light Theme` },
  { value: 3, label: t({ message: 'Olive', context: 'theme' }) },
  { value: 4, label: t({ message: 'Purple', context: 'theme' }) },
];

// Web theme that matches a display theme, for the "match the machine" web theme option.
export const webThemeForDisplayTheme = theme =>
  ({ 0: 'dark', 1: 'light', 2: 'dark', 3: 'olive', 4: 'purple' })[theme] ?? 'dark';

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
