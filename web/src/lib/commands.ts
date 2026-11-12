// A device's commands as people think of them, mapped to and from the API's action catalog.
// Bluetooth exposes families of catalog commands (press / press_down / press_up with a `command`
// param, and the same for keys and raw usages); here they become the device's own commands by name
// (Home, Menu, Play/Pause...), plus "Keyboard key" and "Custom code". A button's binding is a Press
// (passed straight through, or a tap when it also has a Long press) and a Long press. Pure: no
// React, relative imports.
import type { Action, Bindings, ButtonId, Command, EventKind, ParamSpec } from '../api/types.ts'
import { defaultParams, EVENT_ORDER, paramLabel, triggerKey } from './activity.ts'
import { keyLabel } from './keys.ts'
import { humanize, same, sentenceCase } from './utils.ts'

/** A catalog family: the momentary form, its hold and release, and the param that names what's sent. */
interface Family {
  tap: string
  down: string
  up: string
  param: string
}
const FAMILIES: readonly Family[] = [
  { tap: 'press', down: 'press_down', up: 'press_up', param: 'command' },
  { tap: 'key_tap', down: 'key_down', up: 'key_up', param: 'key' },
  { tap: 'usage_tap', down: 'usage_down', up: 'usage_up', param: 'usage' },
]
const familyOf = (commandId: string) => FAMILIES.find((f) => f.tap === commandId || f.down === commandId || f.up === commandId)

const NAME_LABEL: Record<string, string> = {
  DirectionUp: 'Up',
  DirectionDown: 'Down',
  DirectionLeft: 'Left',
  DirectionRight: 'Right',
  PlayPause: 'Play/Pause',
  ControlCenter: 'Control Center',
}

/** A profile command's name as shown: `DirectionUp` → "Up", `FastForward` → "Fast forward". */
export const commandName = (name: string) => NAME_LABEL[name] ?? humanize(name.replace(/([a-z0-9])([A-Z])/g, '$1 $2').toLowerCase())

export type Where = 'binding' | 'sequence' | 'test'

/** One entry in a command picker. `id` is `press:<name>`, `key`, `usage`, `cmd:<command_id>` or `set:<command_id>:<true|false>`. */
export interface Choice {
  id: string
  label: string
  /** The quieter entries after the device's own commands (keyboard keys, custom codes). */
  more?: boolean
}

/** A choice's name without the "…" that says it asks for more (a row heading, an aria label). */
export const choiceName = (c: Choice) => c.label.replace(/…$/, '')

const find = (commands: Command[], id: string) => commands.find((c) => c.command_id === id)

/** A command's name as shown: its catalog label, unless that's just the id (`sonos.set_sleep_timer` → "Set sleep timer"). */
export function commandTitle(c: Command): string {
  const raw = c.label === c.command_id || /^[a-z0-9_]+\.[a-z0-9_]+$/.test(c.label)
  return raw ? humanize(c.label.slice(c.label.indexOf('.') + 1)) : sentenceCase(c.label)
}

/** An optional param with a default changes how a command runs: it sits in the drawer and isn't sent unless changed. */
const isModifier = (p: ParamSpec) => !p.required && p.default !== undefined

/**
 * On/off commands, split into an on and an off command per setting-like bool: one required bool
 * (HA's mute, a light's HEV cycle `power`; any other params follow as usual), or optional bools with
 * no default (Sonos set_option's night sound...), each sending just that one. A bool with a default
 * (`power_on`, `with_group`) is a modifier and doesn't split.
 */
export function toggleParams(c: Command | undefined): ParamSpec[] | null {
  if (!c?.params.length) return null
  const required = c.params.filter((p) => p.required)
  if (required.length === 1 && required[0]!.type === 'bool') return required
  const settings = c.params.filter((p) => p.type === 'bool' && !p.required && p.default === undefined)
  return settings.length && c.params.every((p) => settings.includes(p) || isModifier(p)) ? settings : null
}
const TOGGLE_LABEL: Record<string, [on: string, off: string]> = { muted: ['Mute', 'Unmute'] }
/** One side of an on/off command: Mute / Unmute, HEV cycle state on / off, Night sound on / off. */
export function toggleLabel(c: Command, param: string, on: boolean): string {
  const p = c.params.find((x) => x.name === param)
  const [yes, no] = TOGGLE_LABEL[param] ?? (() => {
    const name = !p || p.required ? commandTitle(c).replace(/^Set (\S)/, (_, ch: string) => ch.toUpperCase()) : paramLabel(p) // "Set HEV cycle state" → "HEV cycle state on"
    return [`${name} on`, `${name} off`]
  })()
  return on ? yes : no
}
/** The one toggle an action sets (`[param, value]`), or null if it sets none or several. */
export function toggleOf(a: Action, c: Command | undefined): [string, boolean] | null {
  const toggles = toggleParams(c) ?? []
  const set = toggles.filter((p) => typeof a.params[p.name] === 'boolean')
  const others = Object.keys(a.params).filter((k) => !toggles.some((p) => p.name === k))
  // An optional-bool split sends only its one bool; a required bool may carry the command's other params.
  return set.length === 1 && (set[0]!.required || others.length === 0) ? [set[0]!.name, a.params[set[0]!.name] === true] : null
}

/** An HA entity's domain (`media_player`), whose services are its standard ones. */
export const domainOf = (d: { provider: { type: string; entity_id?: string } } | undefined) => d?.provider.entity_id?.split('.')[0]

/** An integration's own service (`sonos.play_queue` on a media_player), listed under More. */
const isExtra = (c: Command, domain?: string) => !!domain && c.command_id.includes('.') && !c.command_id.startsWith(`${domain}.`)

/**
 * What a device offers in a picker: its own commands first, then (under More) an integration's own
 * services, "Keyboard key…" and "Custom code…". `domain` is an HA entity's (`media_player`).
 */
export function choices(commands: Command[], where: Where, domain?: string): Choice[] {
  const out: Choice[] = (find(commands, 'press')?.params.find((p) => p.name === 'command')?.options ?? []).map((n) => ({ id: `press:${n}`, label: commandName(n) }))
  const extra: Choice[] = []
  for (const c of commands) {
    if (familyOf(c.command_id) || c.effect === 'hold') continue // holds come from the Hold choice
    if (c.effect === 'wait' && where !== 'sequence') continue
    const to = isExtra(c, domain) ? extra : out
    const more = to === extra || undefined
    const t = toggleParams(c)
    if (t) for (const p of t) to.push({ id: `set:${c.command_id}:${p.name}:true`, label: toggleLabel(c, p.name, true), more }, { id: `set:${c.command_id}:${p.name}:false`, label: toggleLabel(c, p.name, false), more })
    else to.push({ id: `cmd:${c.command_id}`, label: commandTitle(c), more })
  }
  out.push(...extra)
  if (find(commands, 'key_tap')) out.push({ id: 'key', label: 'Keyboard key…', more: true })
  if (find(commands, 'usage_tap')) out.push({ id: 'usage', label: 'Custom code…', more: true })
  return out
}

/** Whether the friendly picker can show this action as it is (a tap form, one toggle, or a plain command). */
export function isFriendly(a: Action, commands: Command[]): boolean {
  const f = familyOf(a.command_id)
  if (f) return a.command_id === f.tap
  const c = find(commands, a.command_id)
  if (toggleParams(c)) return !!toggleOf(a, c)
  return !c || c.effect !== 'hold'
}

/** The picker entry an action shows as. */
export function choiceOf(a: Action, commands: Command[]): string {
  const f = familyOf(a.command_id)
  if (f?.param === 'command') return `press:${String(a.params.command ?? '')}`
  if (f) return f.param
  const t = toggleOf(a, find(commands, a.command_id))
  return t ? `set:${a.command_id}:${t[0]}:${t[1]}` : `cmd:${a.command_id}`
}

/** `set:<command_id>:<param>:<value>` → its parts (command ids have no colons). */
const toggleChoice = (choice: string) => {
  const [, command_id, param, value] = choice.split(':')
  return { command_id: command_id!, param: param!, on: value === 'true' }
}
const commandIdOf = (choice: string) =>
  choice.startsWith('press:') ? 'press' : choice === 'key' ? 'key_tap' : choice === 'usage' ? 'usage_tap' : choice.startsWith('set:') ? toggleChoice(choice).command_id : choice.slice(4)

/** The tap-form action for a picker entry on this device, with default params. */
export function actionFor(choice: string, device: { integration_id: string; device_id: string | null }, commands: Command[]): Action {
  const command_id = commandIdOf(choice)
  const t = choice.startsWith('set:') ? toggleChoice(choice) : null
  const all = find(commands, command_id)?.params ?? []
  const params = choice.startsWith('press:') ? { command: choice.slice(6) } : t ? { ...defaultParams(all.filter((p) => p.name !== t.param), command_id), [t.param]: t.on } : defaultParams(all, command_id)
  return { integration_id: device.integration_id, device_id: device.device_id, command_id, params }
}

/** The params a picker entry asks for: a key, a code, an HA command's own; a press only its timed hold, in steps. */
export function choiceParams(choice: string, commands: Command[], where: Where): ParamSpec[] {
  const c = find(commands, commandIdOf(choice))
  const params = c?.params ?? []
  if (choice.startsWith('set:')) return params.find((p) => p.name === toggleChoice(choice).param)?.required ? params.filter((p) => !toggleParams(c)!.includes(p)) : []
  return choice.startsWith('press:') ? (where === 'sequence' ? params.filter((p) => p.name !== 'command') : []) : params
}

/** How an action reads, by the thing it sends ("Home", "Arrow up", "Custom code 205"), or null. */
export function friendlyName(a: Action): string | null {
  const f = familyOf(a.command_id)
  const v = f && a.params[f.param]
  if (!f || v === undefined) return null
  const base = f.param === 'command' ? commandName(String(v)) : f.param === 'key' ? keyLabel(String(v)) : `Custom code ${String(v)}`
  return a.command_id === f.down ? `${base} down` : a.command_id === f.up ? `${base} up` : base
}

/** The param a friendly name already says (so summaries leave it out). */
export const namedParam = (a: Action) => familyOf(a.command_id)?.param

// ---- A button's bindings as Press / Long press ----

export interface ButtonView {
  /** The button's Press action, in tap form (press, key_tap, an HA command...), if its down/up/tap triggers fit. */
  main: Action | null
  /** The long-press (`hold` event) action, if it's a single one. */
  longPress: Action | null
  /** Triggers that don't fit and show as they are. */
  raw: EventKind[]
}

const sameTarget = (a: Action, b: Action) => a.integration_id === b.integration_id && a.device_id === b.device_id && same(a.params, b.params)

/**
 * Read a button. Press is a tap, a matching down/up pair (the mirror: the button passes through), or
 * one HA send on down; anything else stays raw.
 */
export function readButton(bindings: Bindings, button: ButtonId): ButtonView {
  const at = (e: EventKind) => bindings[triggerKey(button, e)] ?? []
  const down = at('down'), up = at('up'), tap = at('tap'), hold = at('hold')
  const view: ButtonView = { main: null, longPress: hold.length === 1 ? hold[0]! : null, raw: hold.length > 1 ? ['hold'] : [] }
  const d = down[0]
  const f = d && familyOf(d.command_id)
  if (!down.length && !up.length && tap.length === 1) view.main = tap[0]!
  else if (!tap.length && down.length === 1 && up.length === 1 && f && d.command_id === f.down && up[0]!.command_id === f.up && sameTarget(d, up[0]!)) {
    view.main = { ...d, command_id: f.tap }
  } else if (d && !tap.length && !up.length && down.length === 1 && !f) view.main = d
  else if (down.length || up.length || tap.length) view.raw = [...(['down', 'up', 'tap'] as const).filter((e) => at(e).length), ...view.raw]
  return view
}

/**
 * Write Press as Harmony does: with no long press, the button passes straight through (down sends
 * press_down, up press_up, so a hold reaches the device as one; an HA command, which has no down/up,
 * is sent once on down). With a long press, Press is the tap (sent on release, when the press was short).
 */
function pressTriggers(button: ButtonId, a: Action, tap: boolean): Bindings {
  const f = familyOf(a.command_id)
  if (tap || !a.device_id) return { [triggerKey(button, 'tap')]: [a] } // Hubbo's own (switch activity) acts on a press
  if (!f) return { [triggerKey(button, 'down')]: [a] }
  // Held while the button is held: only what's sent carries over (a timed hold_ms doesn't apply).
  const params = { [f.param]: a.params[f.param]! }
  return { [triggerKey(button, 'down')]: [{ ...a, command_id: f.down, params }], [triggerKey(button, 'up')]: [{ ...a, command_id: f.up, params }] }
}
const withoutPress = (bindings: Bindings, button: ButtonId) => {
  const next = { ...bindings }
  for (const e of ['down', 'up', 'tap'] as const) delete next[triggerKey(button, e)]
  return next
}

/** A command a button can't pass through as down/up: a profile command with its own timed hold (only a `press` times it). */
export type TimedTest = (a: Action) => boolean

/**
 * Bindings with this button's Press replaced (null clears it). Long press and other buttons are kept.
 * Press is a tap while a long press is bound, or when `timed` says the command is a timed press.
 */
export function writeMain(bindings: Bindings, button: ButtonId, action: Action | null, timed?: TimedTest): Bindings {
  const next = withoutPress(bindings, button)
  return action ? { ...next, ...pressTriggers(button, action, !!next[triggerKey(button, 'hold')]?.length || !!timed?.(action)) } : next
}

/** Bindings with this button's long press (the `hold` event) replaced (null clears it); a folded Press is rewritten to match. */
export function writeLongPress(bindings: Bindings, button: ButtonId, action: Action | null, timed?: TimedTest): Bindings {
  const main = readButton(bindings, button).main
  const next = { ...bindings }
  if (action) next[triggerKey(button, 'hold')] = [action]
  else delete next[triggerKey(button, 'hold')]
  return main ? writeMain(next, button, main, timed) : next
}

/** What the remote picture marks a button with: Press (as `tap`), Long press (`hold`), raw triggers as they are. */
export function viewKinds(v: ButtonView): EventKind[] {
  const out = new Set<EventKind>(v.raw)
  if (v.main) out.add('tap')
  if (v.longPress) out.add('hold')
  return EVENT_ORDER.filter((e) => out.has(e))
}
