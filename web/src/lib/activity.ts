// Pure helpers for activities, triggers and validation. Imported by the UI
// and by the mock server, so relative imports only (no `@/` alias).
// Error codes and limits mirror api/openapi.yaml (x-hubbo-limits).
import type {
  Action,
  ActionParams,
  ActivityInput,
  Bindings,
  ButtonId,
  Capabilities,
  Command,
  Device,
  EventKind,
  LocalValidation,
  ParamSpec,
  ParamValue,
  TriggerKey,
  ValidationIssue,
} from '../api/types.ts'
import { CALL_SERVICE, callIssues } from './haCall.ts'
import { buttonLabel, isButtonId } from './buttons.ts'
import { humanize, sentenceCase } from './utils.ts'

export const EVENT_ORDER: readonly EventKind[] = ['down', 'up', 'tap', 'hold']
export const LIMITS = { maxSequence: 32, maxBindingActions: 32, nameMax: 64, paramStringMax: 32 } as const
export const SYSTEM = 'system'

export const triggerKey = (button: string, event: EventKind): TriggerKey => `${button}:${event}`

export function parseTrigger(key: string): { button: ButtonId; event: EventKind } | null {
  const i = key.lastIndexOf(':')
  if (i < 0) return null
  const event = key.slice(i + 1)
  if (!(EVENT_ORDER as readonly string[]).includes(event)) return null
  return { button: key.slice(0, i) as ButtonId, event: event as EventKind }
}

/** Event kinds with at least one action for this button. */
function boundEvents(bindings: Bindings, button: ButtonId): EventKind[] {
  return EVENT_ORDER.filter((e) => (bindings[triggerKey(button, e)]?.length ?? 0) > 0)
}

/**
 * The hub's masking rule: a set binds a button when any of its triggers has an action. Such a button
 * masks every global binding for it; any other button falls through to the globals.
 */
export const bindsButton = (bindings: Bindings, button: ButtonId) => boundEvents(bindings, button).length > 0

/** Visit every action of an activity (or a bindings set) with its JSON-pointer path, e.g. `/bindings/up:down/0`. */
export function forEachAction(doc: { startup?: Action[]; shutdown?: Action[]; bindings: Bindings }, fn: (a: Action, path: string) => void) {
  doc.startup?.forEach((a, i) => fn(a, `/startup/${i}`))
  doc.shutdown?.forEach((a, i) => fn(a, `/shutdown/${i}`))
  for (const [k, list] of Object.entries(doc.bindings)) list.forEach((a, i) => fn(a, `/bindings/${k}/${i}`))
}

/** Button that a validation path points at, if any. */
export function buttonForPath(path: string): ButtonId | null {
  const m = /^\/bindings\/([^/]+)/.exec(path)
  return m?.[1] ? (parseTrigger(decodeURIComponent(m[1]))?.button ?? null) : null
}

export const systemAction = (command_id: string, params: ActionParams = {}): Action => ({ integration_id: SYSTEM, device_id: null, command_id, params })
export const waitAction = (duration_ms = 500): Action => systemAction('wait', { duration_ms })
export const isSystem = (a: Action) => a.integration_id === SYSTEM && a.device_id === null
export const isWait = (a: Action) => isSystem(a) && a.command_id === 'wait'

export type Placement = 'sequence' | 'binding-last' | 'binding-mid'
/** Where a system command may go: activate/off only as the last action of a binding (spec rule). */
export const systemCommandAllowed = (commandId: string, where: Placement) => where === 'binding-last' || (commandId !== 'activate' && commandId !== 'off')

/** Hubbo's own commands a picker offers as the "Hubbo" device: Switch activity and Off in a binding; none in steps (waits have Add wait). */
export const hubboCommands = (cat: Catalog, context: 'sequence' | 'binding') =>
  cat.systemCommands.filter((c) => c.command_id !== 'wait' && systemCommandAllowed(c.command_id, context === 'sequence' ? 'sequence' : 'binding-last'))

/** The device these actions use most (ties go to the first seen), for a new step's or binding's default. */
export function mostUsedDevice(actions: Iterable<Action>): string | null {
  const n = new Map<string, number>()
  for (const a of actions) if (a.device_id) n.set(a.device_id, (n.get(a.device_id) ?? 0) + 1)
  let best: string | null = null
  for (const [d, k] of n) if (best === null || k > n.get(best)!) best = d
  return best
}

const idSets = new WeakMap<Catalog['activities'], ReadonlySet<string>>()
const activityIds = (cat: Catalog) => {
  let s = idSets.get(cat.activities)
  if (!s) idSets.set(cat.activities, (s = new Set(cat.activities.map((x) => x.id))))
  return s
}

/**
 * A number param picked and shown as a percent: a 0–1 number (HA's volume level), a light's 0–255
 * brightness, or a light's ±255 brightness step. `full` is its 100% value; a signed scale runs
 * −100%…+100% with no 0.
 */
export type PercentScale = { full: number; signed: boolean }
export function percentScale(commandId: string, p: ParamSpec): PercentScale | null {
  if (p.type === 'number' && p.minimum === 0 && p.maximum === 1) return { full: 1, signed: false }
  if (!commandId.startsWith('light.') || p.type !== 'int') return null
  if (p.name === 'brightness' && p.minimum === 0 && p.maximum === 255) return { full: 255, signed: false }
  if (p.name === 'step' && p.minimum === -255 && p.maximum === 255) return { full: 255, signed: true }
  return null
}
/** A 0–1 level with no spec to go by (an action read without its catalog). */
export const LEVEL_SCALE: PercentScale = { full: 1, signed: false }
/** `grid`, plus `current` if it's off it (so a stored value shows and isn't changed). */
const withCurrent = (grid: number[], current?: number) => (current === undefined || grid.some((g) => Math.abs(g - current) < 1e-9) ? grid : [...grid, current].sort((a, b) => a - b))
/** The stored value for a whole percent (255 scales store whole numbers). */
const fromPercent = (s: PercentScale, pct: number) => (s.full === 1 ? pct / 100 : Math.round((pct * s.full) / 100))
/** A scale's starting value when the param has no default: 35% (a sensible volume), +10% for a step. */
export const percentDefault = (s: PercentScale) => fromPercent(s, s.signed ? 10 : 35)
/** Stored values for 0%–100% (or −100%…+100%, no 0) in 5% steps, plus `current` if it's off that grid. */
export function percentOptions(s: PercentScale, current?: number): number[] {
  const pcts = s.signed ? [...Array.from({ length: 20 }, (_, i) => -100 + i * 5), ...Array.from({ length: 20 }, (_, i) => 5 + i * 5)] : Array.from({ length: 21 }, (_, i) => i * 5)
  return withCurrent(
    pcts.map((pct) => fromPercent(s, pct)),
    current,
  )
}
/** 0.35 → "35%", 128 of 255 → "50%", a step of 26 → "+10%" (no float noise: 0.335 → "33.5%"). */
export function percentLabel(s: PercentScale, v: number): string {
  const pct = s.full === 1 ? Math.round(v * 1000) / 10 : Math.round((v * 100) / s.full)
  return `${s.signed && pct > 0 ? '+' : ''}${pct < 0 ? `−${-pct}` : pct}%`
}

/**
 * A fan's speeds: a `fan.*` percentage with a step over 1 is picked as "Speed n of N", N = 100 / step
 * (33 → 3, 14 → 7). Speed n is stored as Home Assistant computes it, floor(n × 100 / N); null otherwise.
 */
export function fanSpeeds(commandId: string, p: ParamSpec): number | null {
  return commandId.startsWith('fan.') && p.name === 'percentage' && p.type === 'int' && p.unit === '%' && (p.step ?? 0) > 1 ? Math.round(100 / p.step!) : null
}
const speedPercent = (n: number, count: number) => Math.floor((n * 100) / count)
/** The speed a stored percentage runs at (HA rounds up: 50% of 3 speeds is speed 2); 0 is off. */
export const speedOf = (value: number, count: number) => Math.min(count, Math.ceil((value * count) / 100))
/** Stored values for speeds 1..N, plus `current` if it's off that list (so it shows and isn't changed). */
export function speedOptions(count: number, current?: number): number[] {
  return withCurrent(
    Array.from({ length: count }, (_, i) => speedPercent(i + 1, count)),
    current,
  )
}
const speedText = (value: number, count: number) => `${speedOf(value, count)} of ${count}`
export const speedLabel = (value: number, count: number) => `Speed ${speedText(value, count)}`

/**
 * A number param picked from a list (a percent, a fan speed), or null for a plain number: its stored
 * options, how one reads in a picker, and how it reads after the command's name in a summary.
 */
export interface NumberChoices {
  kind: 'percent' | 'speed'
  options: (current?: number) => number[]
  label: (v: number) => string
  /** "Set speed 2 of 3", "Turn on speed 2 of 3", "Set volume 35%". */
  phrase: (v: number, commandTitle: string) => string
}
export function numberChoices(commandId: string, p: ParamSpec): NumberChoices | null {
  const speeds = fanSpeeds(commandId, p)
  if (speeds) return { kind: 'speed', options: (c) => speedOptions(speeds, c), label: (v) => speedLabel(v, speeds), phrase: (v, title) => `${/speed$/i.test(title) ? '' : 'speed '}${speedText(v, speeds)}` }
  const scale = percentScale(commandId, p)
  if (scale) return { kind: 'percent', options: (c) => percentOptions(scale, c), label: (v) => percentLabel(scale, v), phrase: (v) => percentLabel(scale, v) }
  return null
}

/** A whole-percent param's allowed value (from its minimum, in its steps) nearest `target`, never 0 (a fan's 0% is off). */
function nearPercent(p: ParamSpec, target: number): number {
  const min = p.minimum ?? 0, max = p.maximum ?? 100, step = p.step && p.step > 0 ? p.step : 1
  let best: number | undefined
  for (let v = min; v <= max; v += step) if (v !== 0 && (best === undefined || Math.abs(v - target) < Math.abs(best - target))) best = v
  return best ?? min
}

export function defaultParams(params: ParamSpec[], commandId: string): ActionParams {
  const out: ActionParams = {}
  for (const p of params) {
    // Optional params aren't sent unless set (one with a default runs as that default).
    if (!p.required) continue
    const scale = percentScale(commandId, p)
    const speeds = fanSpeeds(commandId, p)
    if (p.default !== undefined) out[p.name] = p.default
    else if (p.type === 'enum') out[p.name] = p.options?.[0] ?? ''
    else if (p.type === 'bool') out[p.name] = false
    else if (scale) out[p.name] = percentDefault(scale)
    else if (speeds) out[p.name] = speedPercent(1, speeds)
    else if (p.type === 'int' && p.unit === '%') out[p.name] = nearPercent(p, p.name === 'humidity' ? 45 : 35)
    else if (p.type === 'int' || p.type === 'number') out[p.name] = p.minimum ?? 0
    // Anything else (e.g. an activity_ref) stays unset until picked: an empty string would look
    // chosen in a select while being invalid.
  }
  return out
}

/**
 * The params ParamForm keeps in its drawer: the optional ones, unless a command's one param is
 * optional with no default (Set timer, Play queue), which it means nothing without.
 */
export function drawerParams(params: ParamSpec[]): ParamSpec[] {
  const sole = params.length === 1 && params[0]!.default === undefined
  return sole ? [] : params.filter((p) => !p.required)
}

/** Fallbacks for HA params whose catalog entry has no label or unit. */
const KNOWN: Record<string, [label: string, unit: string]> = { color_temp_kelvin: ['Color temperature', 'K'], transition: ['Transition', 's'] }

/**
 * A param's label: the spec's, in sentence case; when missing (or just the name), a readable form of
 * the name, less a unit suffix the unit already says (`sleep_time` → "Sleep time", `hold_ms` → "Hold").
 */
export const paramLabel = (p: ParamSpec) => (p.label && p.label !== p.name ? sentenceCase(p.label) : (KNOWN[p.name]?.[0] ?? humanize(p.unit ? p.name.replace(/_(ms|s|sec|seconds|k|kelvin|pct|percent)$/, '') : p.name)))
/** A param's unit: the spec's, else a known one for its name. */
export const paramUnit = (p: ParamSpec) => p.unit ?? KNOWN[p.name]?.[1]
/** A value with its unit: "50%", "300 s". */
export const withUnit = (v: number | string, unit?: string) => (unit ? `${v}${unit === '%' ? '' : ' '}${unit}` : String(v))
/** A field's label with its unit: "Speed (s)", "Color temperature (K)". */
export function fieldLabel(p: ParamSpec, commandId: string): string {
  const unit = numberChoices(commandId, p) ? undefined : paramUnit(p)
  return unit ? `${paramLabel(p)} (${unit})` : paramLabel(p)
}

export function checkParam(p: ParamSpec, v: ParamValue | undefined, activityIds?: ReadonlySet<string>): string | null {
  const label = paramLabel(p)
  if (v === undefined || v === '') return p.required ? `${label} is required` : null
  switch (p.type) {
    case 'int':
    case 'number': {
      if (typeof v !== 'number' || Number.isNaN(v)) return `${label} must be a number`
      if (p.type === 'int' && !Number.isInteger(v)) return `${label} must be a whole number`
      if (p.minimum !== undefined && v < p.minimum) return `${label} must be at least ${p.minimum}`
      if (p.maximum !== undefined && v > p.maximum) return `${label} must be at most ${p.maximum}`
      return null
    }
    case 'bool':
      return typeof v === 'boolean' ? null : `${label} must be on or off`
    case 'enum':
      return p.options?.includes(String(v)) ? null : `${String(v)} is not a valid ${label}`
    case 'activity_ref':
      if (typeof v !== 'string') return `${label} must be an activity`
      return !activityIds || activityIds.has(v) ? null : `Activity ${v} does not exist`
  }
}

export interface Catalog {
  devices: Map<string, Device>
  capabilities: Map<string, Capabilities>
  /** Deviceless commands of the `system` integration (wait, activate, off). */
  systemCommands: Command[]
  activities: { id: string; name: string }[]
  /** Bluetooth profile commands with their own timed hold (`hold_ms`), by device: a button sends them as one timed press. */
  timed?: ReadonlyMap<string, ReadonlySet<string>>
  /**
   * The service domains each Home Assistant device's calls may use, when known: its snapshot
   * platform (none when YAML-only), and `remote` on a remote. The mock knows them; the editors don't
   * (the picker only offers a device's own services) and leave the check to the hub.
   */
  haServiceDomains?: ReadonlyMap<string, readonly string[]>
}

/** Whether an action is one of those timed profile commands (see `writeMain`). */
export const isTimed = (cat: Catalog) => (a: Action) => a.command_id === 'press' && !!a.device_id && typeof a.params.command === 'string' && !!cat.timed?.get(a.device_id)?.has(a.params.command)

export function commandFor(a: Action, cat: Catalog): Command | undefined {
  if (isSystem(a)) return cat.systemCommands.find((c) => c.command_id === a.command_id)
  return a.device_id ? cat.capabilities.get(a.device_id)?.commands.find((c) => c.command_id === a.command_id) : undefined
}

function checkAction(a: Action, path: string, cat: Catalog, out: ValidationIssue[], where: Placement) {
  // A Home Assistant action carries its call; nothing else takes one.
  if (a.command_id === CALL_SERVICE) {
    if (a.call === undefined) out.push({ path: `${path}/call`, code: 'required', message: 'Pick a Home Assistant action' })
    else {
      const issues = callIssues(a.call, `${path}/call`)
      out.push(...issues)
      const domains = a.device_id ? cat.haServiceDomains?.get(a.device_id) : undefined
      if (!issues.length && domains && !domains.includes(a.call.service.split('.')[0]!)) out.push({ path: `${path}/call/service`, code: 'forbidden_param', message: domains.length ? `Pick a ${domains.join(' or ')} service: the action goes to this device` : 'This device takes no Home Assistant action' })
    }
  } else if (a.call !== undefined) out.push({ path: `${path}/call`, code: 'unknown_field', message: 'Only a Home Assistant action takes a call' })
  let cmd: Command | undefined
  let owner: string
  if (a.integration_id === SYSTEM) {
    if (a.device_id !== null) {
      out.push({ path: `${path}/device_id`, code: 'device_not_allowed', message: 'System commands take no device' })
      return
    }
    if (!systemCommandAllowed(a.command_id, where)) {
      out.push({ path, code: 'misplaced_transition', message: `${a.command_id === 'off' ? 'Off' : 'Switch activity'} can only be the last action of a button binding` })
    }
    owner = 'System'
    if (!cat.systemCommands.length) return // catalog not loaded yet; the hub is authoritative
    cmd = cat.systemCommands.find((c) => c.command_id === a.command_id)
    if (!cmd) {
      out.push({ path: `${path}/command_id`, code: 'unknown_command', message: `System has no command ${a.command_id}` })
      return
    }
  } else {
    const dev = a.device_id ? cat.devices.get(a.device_id) : undefined
    if (!a.device_id) {
      out.push({ path: `${path}/device_id`, code: 'device_required', message: 'Pick a device' })
      return
    }
    if (!dev) {
      out.push({ path: `${path}/device_id`, code: 'unknown_device', message: `${a.device_id} is not registered` })
      return
    }
    if (dev.integration_id !== a.integration_id) {
      out.push({ path: `${path}/integration_id`, code: 'integration_mismatch', message: `${dev.name} belongs to ${dev.integration_id}, not ${a.integration_id}` })
      return
    }
    owner = dev.name
    const caps = cat.capabilities.get(dev.id)
    if (!caps) return // capabilities not loaded yet; the hub is authoritative
    cmd = caps.commands.find((c) => c.command_id === a.command_id)
    if (!cmd) {
      out.push({ path: `${path}/command_id`, code: 'unknown_command', message: `${dev.name} has no command ${a.command_id}` })
      return
    }
  }
  if (!cmd) return
  // A hold (e.g. key_down) needs a release: only a button's down event can pair one (checked in validateBindings).
  if (cmd.effect === 'hold' && where === 'sequence') {
    out.push({ path: `${path}/command_id`, code: 'hold_not_allowed', message: `${cmd.label} holds until released; use the momentary form in sequences` })
    return
  }
  // A wait (wait_connected) holds up what follows: only a startup or shutdown can use one.
  if (cmd.effect === 'wait' && where !== 'sequence') {
    out.push({ path: `${path}/command_id`, code: 'sequence_only', message: `${cmd.label} only works in a startup or shutdown` })
    return
  }
  for (const p of cmd.params) {
    const err = checkParam(p, a.params[p.name], cat.activities.length ? activityIds(cat) : undefined)
    if (err) out.push({ path: `${path}/params/${p.name}`, code: p.type === 'int' || p.type === 'number' ? 'param_out_of_range' : 'missing_param', message: `${owner}: ${err}` })
  }
  for (const k of Object.keys(a.params)) {
    if (!cmd.params.some((p) => p.name === k)) out.push({ path: `${path}/params/${k}`, code: 'unknown_param', message: `${cmd.label} does not take ${k}` })
  }
}

/**
 * The release of a hold: same device and params, `<x>_down` released by `<x>_up`
 * (the catalog's only hold today is key_down/key_up; core keeps the mapping as `releasedBy`).
 */
export const releaseFor = (hold: Action): Action => ({ ...hold, command_id: hold.command_id.replace(/_down$/, '_up') })
const isReleaseOf = (r: Action, hold: Action) => r.device_id === hold.device_id && r.command_id === releaseFor(hold).command_id && JSON.stringify(r.params) === JSON.stringify(hold.params)

export function validateBindings(bindings: Bindings, cat: Catalog, errors: ValidationIssue[], warnings: ValidationIssue[]) {
  for (const [key, actions] of Object.entries(bindings)) {
    const t = parseTrigger(key)
    const base = `/bindings/${key}`
    if (!t || !isButtonId(t.button)) {
      errors.push({ path: base, code: 'invalid_trigger', message: `Unknown trigger ${key}` })
      continue
    }
    if (actions.length > LIMITS.maxBindingActions) errors.push({ path: base, code: 'too_many_actions', message: `At most ${LIMITS.maxBindingActions} actions per trigger` })
    actions.forEach((act, i) => {
      checkAction(act, `${base}/${i}`, cat, errors, i === actions.length - 1 ? 'binding-last' : 'binding-mid')
      if (commandFor(act, cat)?.effect !== 'hold') return
      const at = `${base}/${i}/command_id`
      if (t.event !== 'down') errors.push({ path: at, code: 'hold_not_allowed', message: `${buttonLabel(t.button)} ${t.event}: a held command only works on down, released on up` })
      else if (!bindings[triggerKey(t.button, 'up')]?.some((r) => isReleaseOf(r, act))) {
        errors.push({ path: at, code: 'unpaired_hold', message: `${buttonLabel(t.button)}: ${act.command_id} needs its release on the up event` })
      }
    })
  }
  // Advisory: legal, but usually mistakes.
  const buttons = new Set(Object.keys(bindings).map((k) => parseTrigger(k)?.button).filter((b): b is ButtonId => !!b))
  for (const b of buttons) {
    const ev = boundEvents(bindings, b)
    if (ev.includes('down') && (ev.includes('tap') || ev.includes('hold'))) {
      warnings.push({ path: `/bindings/${b}:down`, code: 'mixed_semantics', message: `down fires on every press, before tap or hold is known` })
    }
  }
}

/** Mirrors the hub's checks so the editor can flag problems as you type. The hub is authoritative. */
export function validateActivity(a: ActivityInput, cat: Catalog): LocalValidation {
  const errors: ValidationIssue[] = []
  const warnings: ValidationIssue[] = []
  if (!a.name.trim()) errors.push({ path: '/name', code: 'required', message: 'Name is required' })
  if (a.name.length > LIMITS.nameMax) errors.push({ path: '/name', code: 'too_long', message: `Name must be ${LIMITS.nameMax} characters or fewer` })
  for (const seq of ['startup', 'shutdown'] as const) {
    if (a[seq].length > LIMITS.maxSequence) errors.push({ path: `/${seq}`, code: 'too_many_actions', message: `At most ${LIMITS.maxSequence} steps` })
    a[seq].forEach((act, i) => checkAction(act, `/${seq}/${i}`, cat, errors, 'sequence'))
  }
  validateBindings(a.bindings, cat, errors, warnings)
  return { valid: errors.length === 0, errors, warnings }
}
