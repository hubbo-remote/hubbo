import type { Command } from '@/api/types'
import { humanize } from '@/lib/utils'

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

/** How a param's value reads: Bluetooth keys get their names, anything else shows as stored. */
export const paramValueLabel = (param: string, value: string) => (param === 'key' ? keyLabel(value) : value)

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
