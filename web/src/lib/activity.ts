// Pure helpers for activities, triggers and validation. Imported by the UI
// and by the mock server, so relative imports only (no `@/` alias).
// Error codes and limits mirror api/openapi.yaml (x-hubbo-limits).
import type {
  Action,
  ActionParams,
  ActivityDraft,
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
import { buttonLabel, isButtonId } from './buttons.ts'

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
export function boundEvents(bindings: Bindings, button: ButtonId): EventKind[] {
  return EVENT_ORDER.filter((e) => (bindings[triggerKey(button, e)]?.length ?? 0) > 0)
}

/** Visit every action of an activity (or a bindings set) with its JSON-pointer path, e.g. `/bindings/up:down/0`. */
export function forEachAction(doc: { startup?: Action[]; shutdown?: Action[]; bindings: Bindings }, fn: (a: Action, path: string) => void) {
  doc.startup?.forEach((a, i) => fn(a, `/startup/${i}`))
  doc.shutdown?.forEach((a, i) => fn(a, `/shutdown/${i}`))
  for (const [k, list] of Object.entries(doc.bindings)) list.forEach((a, i) => fn(a, `/bindings/${k}/${i}`))
}

export function bindingCount(bindings: Bindings): number {
  return Object.values(bindings).filter((a) => (a?.length ?? 0) > 0).length
}

/** Button that a validation path points at, if any. */
export function buttonForPath(path: string): ButtonId | null {
  const m = /^\/bindings\/([^/]+)/.exec(path)
  return m?.[1] ? (parseTrigger(decodeURIComponent(m[1]))?.button ?? null) : null
}

export const systemAction = (command_id: string, params: ActionParams = {}): Action => ({ integration_id: SYSTEM, device_id: null, command_id, params })
export const waitAction = (duration_ms = 1000): Action => systemAction('wait', { duration_ms })
export const isSystem = (a: Action) => a.integration_id === SYSTEM && a.device_id === null
export const isWait = (a: Action) => isSystem(a) && a.command_id === 'wait'
export const isTransition = (a: Action) => isSystem(a) && (a.command_id === 'activate' || a.command_id === 'off')

export type Placement = 'sequence' | 'binding-last' | 'binding-mid'
/** Where a system command may go: activate/off only as the last action of a binding (spec rule). */
export const systemCommandAllowed = (commandId: string, where: Placement) => where === 'binding-last' || (commandId !== 'activate' && commandId !== 'off')

const idSets = new WeakMap<Catalog['activities'], ReadonlySet<string>>()
const activityIds = (cat: Catalog) => {
  let s = idSets.get(cat.activities)
  if (!s) idSets.set(cat.activities, (s = new Set(cat.activities.map((x) => x.id))))
  return s
}

export function defaultParams(params: ParamSpec[]): ActionParams {
  const out: ActionParams = {}
  for (const p of params) {
    if (p.default !== undefined) out[p.name] = p.default
    else if (!p.required) continue
    else if (p.type === 'enum') out[p.name] = p.options?.[0] ?? ''
    else if (p.type === 'bool') out[p.name] = false
    else if (p.type === 'int' || p.type === 'number') out[p.name] = p.minimum ?? 0
    else out[p.name] = ''
  }
  return out
}

/** `label` is optional in the spec; fall back to a readable form of the name (`duration_ms` → `Duration ms`). */
export const paramLabel = (p: ParamSpec) => p.label ?? p.name.charAt(0).toUpperCase() + p.name.slice(1).replace(/_/g, ' ')

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
}

export function commandFor(a: Action, cat: Catalog): Command | undefined {
  if (isSystem(a)) return cat.systemCommands.find((c) => c.command_id === a.command_id)
  return a.device_id ? cat.capabilities.get(a.device_id)?.commands.find((c) => c.command_id === a.command_id) : undefined
}

function checkAction(a: Action, path: string, cat: Catalog, out: ValidationIssue[], where: Placement) {
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
export function validateActivity(a: ActivityDraft, cat: Catalog): LocalValidation {
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
