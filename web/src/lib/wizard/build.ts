// The new-activity wizard's starter: devices plus answers → bindings, startup and shutdown.
// Pure, so it's unit-tested without the UI. Each device is first turned into a WizardDevice
// (its commands under the profile spec's names, its power and inputs as actions), whatever its
// integration: Bluetooth devices from their profile, Home Assistant devices from what HA reports.
import type { Action, ActivityInput, Bindings, ButtonId, Capabilities, Device, DeviceDetail, ProfileButtons, ProfileSteps } from '../../api/types.ts'
import { isWait, waitAction as wait } from '../activity.ts'
import { writeMain } from '../commands.ts'
import { HA_NAMES, HA_SELECT_SOURCE } from './haNames.ts'

/** One rule of profiles/buttons.json: a button's role and its candidates. */
export interface ButtonRule {
  button: ButtonId
  role: 'watch' | 'volume'
  candidates: string[]
}
type ProfileStep = ProfileSteps[number]

// ---- The wizard's view of a device

/** How a named command is sent: `tap` is the button's Press (bound as the editor binds it). */
export interface NamedCommand {
  tap: Action
  /** A profile command with its own timed hold: Press stays a tap (see lib/commands `writeMain`). */
  timed?: boolean
}

export interface WizardDevice {
  id: string
  name: string
  /** Sent before the device's first startup step, if it has any (Bluetooth: wait until the host connects). */
  connectFirst?: Action
  /** Spec command name, lower-cased → how to send it. */
  commands: Map<string, NamedCommand>
  /** A profile's overrides of the button table: a name, or null for "leave it empty". */
  buttons?: ProfileButtons
  /** `on` and `off` are the steps that run, as actions; toggle-only power gets no automatic step. */
  power: { type: 'discrete' | 'toggle' | 'none'; on: Action[]; off: Action[]; onDelayMs: number }
  inputs: { name: string; steps: Action[] }[]
  inputDelayMs: number
  keyDelayMs: number
  /** A wait between power on and the input when there's no on delay (HA: TVs ignore inputs while waking). */
  wakeForInputMs: number
}

export interface WizardAnswers {
  name: string
  /** Device ids, in the order picked. */
  devices: string[]
  /** Gets the watch buttons: navigation, playback, colours, numbers and channels. */
  watch: string | null
  /** Gets the volume buttons. */
  volume: string | null
  /** Device id → input name; missing or null means don't switch. */
  inputs: Record<string, string | null | undefined>
  /** Device id → turn on at the start / off at the end (both on by default). */
  power: Record<string, { on: boolean; off: boolean } | undefined>
}

export interface FilledButton {
  button: ButtonId
  deviceId: string
  /** The command name used, as the table or profile spells it. */
  command: string
}

export interface Starter {
  activity: ActivityInput
  filled: FilledButton[]
}

export const POWER_DEFAULT = { on: true, off: true }
/** A device's power answers: both on unless changed. */
export const powerOf = (a: WizardAnswers, id: string) => a.power[id] ?? POWER_DEFAULT
/** How long a startup waits for a Bluetooth device to connect before it powers it on. */
export const WAIT_CONNECTED_MS = 15000
/** Between an HA device's turn_on and its input. */
export const HA_WAKE_FOR_INPUT_MS = 3000

/** An action on this device. */
const actionOn = (d: Device) => (command_id: string, params: Action['params'] = {}): Action => ({ integration_id: d.integration_id, device_id: d.id, command_id, params })

/** The command a button gets on this device: the profile's override, else the first candidate it has. */
export function matchButton(d: WizardDevice, rule: ButtonRule): string | null {
  const own = d.buttons?.[rule.button]
  if (own !== undefined) return own !== null && d.commands.has(own.toLowerCase()) ? own : null
  return rule.candidates.find((n) => d.commands.has(n.toLowerCase())) ?? null
}

/** A wait of the device's key delay between two steps in a row to the same device. */
function spaced(steps: Action[], byId: Map<string, WizardDevice>): Action[] {
  const out: Action[] = []
  for (const a of steps) {
    const prev = out[out.length - 1]
    const gap = a.device_id ? (byId.get(a.device_id)?.keyDelayMs ?? 0) : 0
    if (prev && !isWait(prev) && !isWait(a) && prev.device_id === a.device_id && gap > 0) out.push(wait(gap))
    out.push(a)
  }
  return out
}

/** Builds the starter activity from the picked devices, the answers and the button rules (buttons.json). */
export function buildStarter(all: WizardDevice[], answers: WizardAnswers, rules: readonly ButtonRule[]): Starter {
  const byId = new Map(all.map((d) => [d.id, d]))
  const picked = answers.devices.map((id) => byId.get(id)).filter((d): d is WizardDevice => !!d)
  let bindings: Bindings = {}
  const filled: FilledButton[] = []
  for (const rule of rules) {
    const d = byId.get((rule.role === 'volume' ? answers.volume : answers.watch) ?? '')
    if (!d || !picked.includes(d)) continue
    const name = matchButton(d, rule)
    if (!name) continue
    const c = d.commands.get(name.toLowerCase())!
    bindings = writeMain({ bindings, macros: [] }, rule.button, c.tap, { timed: () => !!c.timed }).bindings
    filled.push({ button: rule.button, deviceId: d.id, command: name })
  }

  // Devices that don't wait to connect first, then those that do (Bluetooth), each in the order picked.
  const ordered = [...picked.filter((d) => !d.connectFirst), ...picked.filter((d) => d.connectFirst)]
  const startup: Action[] = []
  const shutdown: Action[] = []
  for (const d of ordered) {
    const power = powerOf(answers, d.id)
    const input = d.inputs.find((i) => i.name === answers.inputs[d.id])
    const own: Action[] = []
    if (d.power.type === 'discrete' && power.on) {
      own.push(...d.power.on)
      const delay = d.power.onDelayMs || (input ? d.wakeForInputMs : 0)
      if (delay > 0) own.push(wait(delay))
    }
    if (input) {
      own.push(...input.steps)
      if (d.inputDelayMs > 0) own.push(wait(d.inputDelayMs))
    }
    if (own.length && d.connectFirst) startup.push(d.connectFirst)
    startup.push(...own)
    if (d.power.type === 'discrete' && power.off) shutdown.push(...d.power.off)
  }
  return { activity: { name: answers.name.trim(), startup: spaced(startup, byId), shutdown: spaced(shutdown, byId), bindings, macros: [] }, filled }
}

// ---- Adapters

/** A Home Assistant device, from what HA reports: its commands by name (HA_NAMES), its sources as inputs. */
export function haDevice(d: Device, caps: Capabilities): WizardDevice {
  const action = actionOn(d)
  const commands = new Map<string, NamedCommand>()
  let inputs: WizardDevice['inputs'] = []
  for (const c of caps.commands) {
    const name = HA_NAMES[c.command_id]
    if (name) commands.set(name.toLowerCase(), { tap: action(c.command_id) })
    else if (c.command_id === HA_SELECT_SOURCE) {
      const sources = c.params.find((p) => p.name === 'source')?.options ?? []
      inputs = sources.map((s) => ({ name: s, steps: [action(HA_SELECT_SOURCE, { source: s })] }))
    }
  }
  const on = commands.get('poweron')
  const off = commands.get('poweroff')
  const power: WizardDevice['power'] =
    on && off ? { type: 'discrete', on: [on.tap], off: [off.tap], onDelayMs: 0 } : { type: commands.has('powertoggle') ? 'toggle' : 'none', on: [], off: [], onDelayMs: 0 }
  return { id: d.id, name: d.name, commands, power, inputs, inputDelayMs: 0, keyDelayMs: 0, wakeForInputMs: HA_WAKE_FOR_INPUT_MS }
}

/**
 * A profile device (Bluetooth or infrared), from its profile: `press` for each command (a button
 * passes it through as press_down/press_up). Only Bluetooth waits for its host to connect.
 */
export function profileDevice(d: DeviceDetail): WizardDevice {
  const action = actionOn(d)
  const steps = (list: ProfileStep[] = []): Action[] =>
    list.map((s) => ('delay_ms' in s ? wait(s.delay_ms) : action('press', s.hold_ms ? { command: s.command, hold_ms: s.hold_ms } : { command: s.command })))
  const commands = new Map<string, NamedCommand>()
  for (const [name, c] of Object.entries(d.commands ?? {})) {
    commands.set(name.toLowerCase(), { tap: action('press', { command: name }), timed: !!c.hold_ms })
  }
  const p = d.power ?? { type: 'none' }
  const power: WizardDevice['power'] =
    p.type === 'discrete'
      ? { type: 'discrete', on: steps(p.on), off: steps(p.off), onDelayMs: p.on_delay_ms ?? 0 }
      : { type: p.type, on: [], off: [], onDelayMs: 0 }
  return {
    id: d.id,
    name: d.name,
    ...(d.provider.type === 'ble_hid' ? { connectFirst: action('wait_connected', { timeout_ms: WAIT_CONNECTED_MS }) } : {}),
    commands,
    buttons: d.buttons,
    power,
    inputs: (d.inputs ?? []).map((i) => ({ name: i.name, steps: steps(i.steps) })),
    inputDelayMs: d.timing?.input_delay_ms ?? 0,
    keyDelayMs: d.timing?.key_delay_ms ?? 0,
    wakeForInputMs: 0,
  }
}

/** Any device the wizard can use: a profile device from its detail, Home Assistant from its capabilities. */
export function wizardDevice(d: Device, detail: DeviceDetail | undefined, caps: Capabilities | undefined): WizardDevice | null {
  if (d.profile) return detail ? profileDevice(detail) : null
  return caps ? haDevice(d, caps) : null
}
