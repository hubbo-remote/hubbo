// Request-shape checks the spec expresses as `additionalProperties: false`,
// scalar-only params and refused target-like parameter names. The real hub
// rejects these before validation; so does the mock.
import type { ValidationIssue } from '../src/api/types.ts'
import { LIMITS } from '../src/lib/activity.ts'

const FORBIDDEN_PARAMS = new Set(['entity_id', 'device_id', 'area_id', 'floor_id', 'label_id', 'target', 'data', 'service', 'service_data', 'domain'])
const ACTION_KEYS = new Set(['integration_id', 'device_id', 'command_id', 'params'])

const isObj = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v)

function action(a: unknown, path: string, out: ValidationIssue[]) {
  if (!isObj(a)) return void out.push({ path, code: 'wrong_type', message: 'Action must be an object' })
  for (const k of Object.keys(a)) if (!ACTION_KEYS.has(k)) out.push({ path: `${path}/${k}`, code: 'unknown_field', message: `Unknown field ${k}` })
  for (const k of ACTION_KEYS) if (!(k in a)) out.push({ path: `${path}/${k}`, code: 'required', message: `${k} is required` })
  if (typeof a.device_id === 'string' && !/^dev_[a-z0-9_]{1,28}$/.test(a.device_id)) out.push({ path: `${path}/device_id`, code: 'pattern', message: 'device_id must be a hub device ID (dev_…), not a provider reference' })
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
  return out
}
