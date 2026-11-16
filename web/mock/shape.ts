// Request-shape checks the spec expresses as `additionalProperties: false`,
// scalar-only params and refused target-like parameter names. The real hub
// rejects these before validation; so does the mock.
import type { ValidationIssue } from '../src/api/types.ts'
import { LIMITS } from '../src/lib/activity.ts'
import { SLOTS } from '../src/lib/smartButtons.ts'
import { isObject as isObj, TARGET_KEYS } from '../src/lib/haCall.ts'

/** api/openapi.yaml DeviceId. */
export const DEVICE_ID = /^dev_[a-z0-9_]{1,28}$/
/** The hub's refusal of fields a body may not carry, at `at` (e.g. `/devices/0`). */
export const unknownFields = (keys: string[], at = '') => keys.map((k) => ({ path: `${at}/${k}`, code: 'unknown_field' as const, message: `Unknown field ${k}` }))

const FORBIDDEN_PARAMS = new Set([...TARGET_KEYS, 'target', 'data', 'service', 'service_data', 'domain'])
const ACTION_KEYS = ['integration_id', 'device_id', 'command_id', 'params']
/** `call` is optional: only a Home Assistant action has one (its rules are checkAction's). */
const ACTION_FIELDS = new Set([...ACTION_KEYS, 'call'])

function action(a: unknown, path: string, out: ValidationIssue[]) {
  if (!isObj(a)) return void out.push({ path, code: 'wrong_type', message: 'Action must be an object' })
  for (const k of Object.keys(a)) if (!ACTION_FIELDS.has(k)) out.push({ path: `${path}/${k}`, code: 'unknown_field', message: `Unknown field ${k}` })
  for (const k of ACTION_KEYS) if (!(k in a)) out.push({ path: `${path}/${k}`, code: 'required', message: `${k} is required` })
  if (typeof a.device_id === 'string' && !DEVICE_ID.test(a.device_id)) out.push({ path: `${path}/device_id`, code: 'pattern', message: 'device_id must be a hub device ID (dev_…), not a provider reference' })
  if (!isObj(a.params)) return void out.push({ path: `${path}/params`, code: 'wrong_type', message: 'params must be an object' })
  for (const [k, v] of Object.entries(a.params)) {
    if (FORBIDDEN_PARAMS.has(k)) out.push({ path: `${path}/params/${k}`, code: 'forbidden_param', message: `${k} is never accepted as a parameter` })
    else if (!['string', 'number', 'boolean'].includes(typeof v)) out.push({ path: `${path}/params/${k}`, code: 'wrong_type', message: `${k} must be a scalar` })
    else if (typeof v === 'string' && v.length > LIMITS.paramStringMax) out.push({ path: `${path}/params/${k}`, code: 'too_long', message: `${k} is too long` })
  }
}

export function bindingsShape(b: unknown, path: string, out: ValidationIssue[]) {
  if (!isObj(b)) return void out.push({ path, code: 'wrong_type', message: 'bindings must be an object' })
  for (const [k, list] of Object.entries(b)) {
    if (!Array.isArray(list) || list.length === 0) out.push({ path: `${path}/${k}`, code: 'wrong_type', message: 'A binding is a non-empty array of actions' })
    else list.forEach((a, i) => action(a, `${path}/${k}/${i}`, out))
  }
}

/** `macros` is optional on input (missing is none). */
export function macrosShape(m: unknown, path: string, out: ValidationIssue[]) {
  if (m === undefined) return
  if (!Array.isArray(m)) return void out.push({ path, code: 'wrong_type', message: 'macros must be an array of triggers' })
  m.forEach((k, i) => typeof k !== 'string' && out.push({ path: `${path}/${i}`, code: 'wrong_type', message: 'A macro is a trigger' }))
}

export function activityShape(body: unknown, allowed: readonly string[]): ValidationIssue[] {
  const out: ValidationIssue[] = []
  if (!isObj(body)) return [{ path: '', code: 'wrong_type', message: 'Body must be an object' }]
  for (const k of Object.keys(body)) if (!allowed.includes(k)) out.push({ path: `/${k}`, code: 'unknown_field', message: `Unknown field ${k}` })
  for (const k of ['name', 'startup', 'shutdown', 'bindings']) if (!(k in body)) out.push({ path: `/${k}`, code: 'required', message: `${k} is required` })
  if (typeof body.name !== 'string') out.push({ path: '/name', code: 'wrong_type', message: 'name must be a string' })
  for (const seq of ['startup', 'shutdown']) {
    const s = body[seq]
    if (!Array.isArray(s)) out.push({ path: `/${seq}`, code: 'wrong_type', message: `${seq} must be an array` })
    else s.forEach((a, i) => action(a, `/${seq}/${i}`, out))
  }
  bindingsShape(body.bindings, '/bindings', out)
  macrosShape(body.macros, '/macros', out)
  return out
}

/** api/openapi.yaml SmartButtonsInput: the four slots, each a device id or null, and the revision. */
export function smartButtonsShape(body: unknown): ValidationIssue[] {
  if (!isObj(body)) return [{ path: '', code: 'wrong_type', message: 'Body must be an object' }]
  const out: ValidationIssue[] = []
  const known = (o: Record<string, unknown>, keys: readonly string[], path: string) => {
    out.push(...unknownFields(Object.keys(o).filter((k) => !keys.includes(k)), path))
    for (const k of keys) if (!(k in o)) out.push({ path: `${path}/${k}`, code: 'required', message: `${k} is required` })
  }
  known(body, ['slots', 'revision'], '')
  if (!isObj(body.slots)) return out.length ? out : [{ path: '/slots', code: 'wrong_type', message: 'slots must be an object' }]
  known(body.slots, SLOTS, '/slots')
  for (const id of SLOTS) {
    const d = body.slots[id]
    if (d !== null && d !== undefined && (typeof d !== 'string' || !DEVICE_ID.test(d))) out.push({ path: `/slots/${id}`, code: 'pattern', message: 'A slot is a hub device ID (dev_…) or null' })
  }
  return out
}
