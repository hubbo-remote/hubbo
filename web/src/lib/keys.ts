import type { Command } from '@/api/types'
import { humanize, titleCase } from '@/lib/utils'

// Display names for Bluetooth keys (the `key` param of key_tap/key_down/key_up). The wire names stay.

const KEY_LABEL: Record<string, string> = {
  FAST_FORWARD: 'Fast forward',
  REWIND: 'Rewind',
  STOP: 'Stop',
  AC_BACK: 'Back',
  AC_SEARCH: 'Search',
  AC_HOME: 'Home',
  MENU_ESCAPE: 'Exit',
  CHANNEL_UP: 'Channel up',
  CHANNEL_DOWN: 'Channel down',
  INFO: 'Info',
  SLEEP: 'Sleep',
  KEYPAD_ENTER: 'Enter',
  BACKSPACE: 'Backspace',
  ESCAPE: 'Esc',
  PLAY_PAUSE: 'Play/Pause',
  SCAN_NEXT: 'Next',
  SCAN_PREVIOUS: 'Previous',
}

/** `ARROW_UP` → "Arrow up", `KEY_A` → "A"; a few have their own names. */
export function keyLabel(key: string): string {
  const own = KEY_LABEL[key]
  if (own) return own
  return humanize(key.replace(/^KEY_/, '').toLowerCase())
}

const NAVIGATION = new Set(['SELECT', 'MENU', 'AC_HOME', 'AC_BACK', 'AC_SEARCH', 'MENU_ESCAPE', 'ENTER', 'KEYPAD_ENTER', 'ESCAPE', 'TAB', 'BACKSPACE', 'SPACE', 'HOME', 'END', 'PAGE_UP', 'PAGE_DOWN', 'INSERT', 'DELETE'])
const MEDIA = new Set(['PLAY_PAUSE', 'SCAN_NEXT', 'SCAN_PREVIOUS', 'RECORD', 'VOLUME_UP', 'VOLUME_DOWN', 'MUTE', 'FAST_FORWARD', 'REWIND', 'STOP', 'CHANNEL_UP', 'CHANNEL_DOWN', 'INFO', 'SLEEP'])
const SYMBOLS = new Set(['MINUS', 'EQUAL', 'LEFT_BRACKET', 'RIGHT_BRACKET', 'BACKSLASH', 'NON_US_HASH', 'NON_US_BACKSLASH', 'SEMICOLON', 'APOSTROPHE', 'GRAVE', 'COMMA', 'PERIOD', 'SLASH', 'KEYPAD_SLASH', 'KEYPAD_ASTERISK', 'KEYPAD_MINUS', 'KEYPAD_PLUS', 'KEYPAD_PERIOD'])
const KEY_GROUPS = ['Navigation', 'Letters', 'Numbers', 'Function keys', 'Media', 'Symbols', 'Other'] as const

/** The section a key is listed under in a key picker. */
export function keyGroup(key: string): (typeof KEY_GROUPS)[number] {
  if (NAVIGATION.has(key) || /^(ARROW|KEYBOARD)_/.test(key)) return 'Navigation'
  if (/^KEY_[A-Z]$/.test(key)) return 'Letters'
  if (/^(KEY|KEYPAD)_\d$/.test(key)) return 'Numbers'
  if (/^F\d+$/.test(key)) return 'Function keys'
  if (MEDIA.has(key)) return 'Media'
  if (SYMBOLS.has(key)) return 'Symbols'
  return 'Other'
}

/** Keys in their sections, in section order (each keeps the table's order). */
export function groupKeys(keys: string[]): [string, string[]][] {
  const by = new Map<string, string[]>()
  for (const k of keys) {
    const g = keyGroup(k)
    by.set(g, [...(by.get(g) ?? []), k])
  }
  return KEY_GROUPS.flatMap((g): [string, string[]][] => (by.has(g) ? [[g, by.get(g)!]] : []))
}

/** How an option reads (the label only; the value is sent as is): Bluetooth keys by name, the rest in title case. */
export const paramValueLabel = (param: string, value: string) => (param === 'key' ? keyLabel(value) : titleCase(value))

// Bluetooth commands come in groups: the device's remote commands, every key, and custom codes
// (advanced, shown on request). Other integrations have no groups.
const GROUPS = [
  ['remote', 'Remote commands'],
  ['keys', 'All keys'],
  ['custom', 'Custom code'],
] as const

/** The picker's command list: `[label, commands]` in group order, or one unlabelled group. */
export function commandGroups(commands: Command[], showCustom: boolean): [string | null, Command[]][] {
  if (!commands.some((c) => c.group)) return [[null, commands]]
  return GROUPS.filter(([g]) => g !== 'custom' || showCustom)
    .map(([g, label]): [string, Command[]] => [label, commands.filter((c) => c.group === g)])
    .filter(([, list]) => list.length > 0)
}
