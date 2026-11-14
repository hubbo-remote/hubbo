// A Home Assistant action on a device: `call_service` with a service picked from the device's own
// (its catalog command's `services`) and JSON data. The hub's rules on the call live here once, for
// the dialog's as-you-type check and the mock. Pure: no React, relative imports.
import type { Action, Command, CustomService, HaServiceCall, ValidationIssue } from '../api/types.ts'

export const CALL_SERVICE = 'call_service'
export const SERVICE_PATTERN = /^[a-z][a-z0-9_]{0,31}\.[a-z][a-z0-9_]{0,63}$/
export const CALL_DATA_MAX_BYTES = 1024
export const CALL_DATA_MAX_DEPTH = 4
/** Hubbo targets the device's own entity: data can't name another target. */
export const TARGET_KEYS = ['entity_id', 'device_id', 'area_id', 'floor_id', 'label_id']

export const isObject = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v)
const pointer = (k: string) => k.replace(/~/g, '~0').replace(/\//g, '~1')
const hasTemplate = (s: string) => s.includes('{{') || s.includes('{%')

/** How deep a JSON value nests: an object or array is a level (the outermost is 1). */
export const depth = (v: unknown): number => (v !== null && typeof v === 'object' ? 1 + Math.max(0, ...Object.values(v).map(depth)) : 0)

/** The first template in a value (keys included), as a path under `at`, or null. */
function templateAt(v: unknown, at: string): string | null {
  if (typeof v === 'string') return hasTemplate(v) ? at : null
  if (Array.isArray(v)) {
    for (let i = 0; i < v.length; i++) {
      const found = templateAt(v[i], `${at}/${i}`)
      if (found) return found
    }
  } else if (isObject(v)) {
    for (const [k, x] of Object.entries(v)) {
      if (hasTemplate(k)) return `${at}/${pointer(k)}`
      const found = templateAt(x, `${at}/${pointer(k)}`)
      if (found) return found
    }
  }
  return null
}

/** The hub's checks on a call's data; `path` is where the data sits (`…/call/data`). */
export function callDataIssues(data: unknown, path: string): ValidationIssue[] {
  if (!isObject(data)) return [{ path, code: 'wrong_type', message: 'Data must be a JSON object, like {"button": "INFO"}' }]
  const out: ValidationIssue[] = []
  for (const k of TARGET_KEYS) if (k in data) out.push({ path: `${path}/${k}`, code: 'forbidden_param', message: `Leave out ${k}: the action always goes to this device` })
  const template = templateAt(data, path)
  if (template) out.push({ path: template, code: 'template_not_supported', message: 'Templates ({{ … }} or {% … %}) aren’t supported' })
  if (depth(data) > CALL_DATA_MAX_DEPTH) out.push({ path, code: 'too_deep', message: `Data can nest at most ${CALL_DATA_MAX_DEPTH} levels` })
  if (new TextEncoder().encode(JSON.stringify(data)).length > CALL_DATA_MAX_BYTES) out.push({ path, code: 'too_long', message: `Data must be ${CALL_DATA_MAX_BYTES} bytes or less` })
  return out
}

/** A call's problems: its service's shape and its data. `path` is the call's (`…/call`). */
export function callIssues(call: unknown, path: string): ValidationIssue[] {
  if (!isObject(call)) return [{ path, code: 'wrong_type', message: 'call must be an object' }]
  const out: ValidationIssue[] = Object.keys(call)
    .filter((k) => k !== 'service' && k !== 'data')
    .map((k) => ({ path: `${path}/${k}`, code: 'unknown_field', message: `Unknown field ${k}` }))
  if (typeof call.service !== 'string' || !call.service) out.push({ path: `${path}/service`, code: typeof call.service === 'string' || call.service === undefined ? 'required' : 'wrong_type', message: 'Pick a service' })
  else if (!SERVICE_PATTERN.test(call.service)) out.push({ path: `${path}/service`, code: 'pattern', message: 'A service looks like webostv.button' })
  if (call.data !== undefined) out.push(...callDataIssues(call.data, `${path}/data`))
  return out
}

/**
 * The data box's text → what the action carries, and why it can't be sent (as you type). Text that
 * isn't JSON is carried as it is, so the action stays invalid (wrong_type) until it's fixed.
 */
export function readCallData(text: string): { data: unknown; error: string | null } {
  let data: unknown
  try {
    data = JSON.parse(text.trim() || '{}')
  } catch {
    return { data: text, error: 'Not valid JSON' }
  }
  return { data, error: callDataIssues(data, '')[0]?.message ?? null }
}

/** A device's services for the action, from its catalog's `call_service`. */
export const customServices = (commands: Command[]): CustomService[] => commands.find((c) => c.command_id === CALL_SERVICE)?.services ?? []

/** A service's starting data: its required fields with HA's examples as given ("" without one). */
export function prefill(s: CustomService | undefined): Record<string, unknown> {
  return Object.fromEntries((s?.fields ?? []).filter((f) => f.required).map((f) => [f.name, f.example ?? '']))
}

/** The data as text for the box: one line when short, else indented; text that wasn't JSON stays as typed. */
export const dataText = (data: unknown) => {
  if (typeof data === 'string') return data
  const one = JSON.stringify(data ?? {})
  return one.length <= 60 ? one : JSON.stringify(data, null, 2)
}

/** How a call reads: "webostv.button · button: INFO". */
export function callLabel(call: HaServiceCall): string {
  const parts = Object.entries(call.data ?? {}).map(([k, v]) => `${k}: ${typeof v === 'string' ? v : JSON.stringify(v)}`)
  return parts.length ? `${call.service} · ${parts.join(', ')}` : call.service
}

/** A Home Assistant action's start on a device: its only service picked, with HA's example data. */
export function startCall(a: Action, services: CustomService[]): Action {
  const only = services.length === 1 ? services[0] : undefined
  return { ...a, params: {}, call: { service: only?.service ?? '', data: prefill(only) } }
}

/** Whether an action's call (if it's a Home Assistant action) could be sent: picked and with usable data. */
export const callReady = (a: Action | null | undefined) => !a || a.command_id !== CALL_SERVICE || callIssues(a.call, '').length === 0
