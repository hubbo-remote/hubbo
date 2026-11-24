// An uploaded profile (POST /admin/devices {profile}), checked like the hub for the parts the UI
// relies on: an object, a platform the hub runs, a manufacturer and model, commands, and (for
// infrared) well-formed codes. The hub checks every field; the mock only what its tests need.
import { createHash } from 'node:crypto'
import type { ValidationIssue } from '../src/api/types.ts'
import { isObject } from '../src/lib/haCall.ts'

const PLATFORMS = ['ble_hid', 'ir']

/** Pairs a section may have (core's pronto::kMaxPairs). */
const MAX_PAIRS = 512
/** The most a profile may take, minified without `meta` (core's limit). */
const MAX_PROFILE_BYTES = 256 * 1024

/**
 * A raw Pronto code as core's pronto::parse takes it: uppercase 4-digit hex words split by single
 * spaces, a 0000 header with a nonzero frequency, at most 512 pairs per section and at least one in
 * all, a length that matches the header, no zero durations.
 */
export function isPronto(code: unknown): boolean {
  if (typeof code !== 'string' || !/^[0-9A-F]{4}( [0-9A-F]{4})*$/.test(code)) return false
  const n = code.split(' ').map((w) => parseInt(w, 16))
  if (n.length < 6) return false
  const [head, freq, once, repeat] = n as [number, number, number, number]
  return head === 0 && freq !== 0 && once <= MAX_PAIRS && repeat <= MAX_PAIRS && once + repeat > 0 && n.length === 4 + 2 * (once + repeat) && !n.slice(4).includes(0)
}

export function profileProblems(p: unknown): ValidationIssue[] {
  if (!isObject(p)) return [{ path: '/profile', code: 'wrong_type', message: 'profile must be an object' }]
  const out: ValidationIssue[] = []
  for (const k of ['id', 'revision', 'source', 'manufacturer', 'model', 'kind', 'platform', 'commands'] as const) {
    if (p[k] === undefined || p[k] === '') out.push({ path: `/profile/${k}`, code: 'required', message: `${k} is required` })
  }
  if (p.platform !== undefined && !PLATFORMS.includes(p.platform as string)) out.push({ path: '/profile/platform', code: 'wrong_type', message: 'platform must be ble_hid or ir' })
  const { meta: _m, ...kept } = p
  if (Buffer.byteLength(JSON.stringify(kept)) > MAX_PROFILE_BYTES) out.push({ path: '/profile', code: 'too_long', message: 'The profile is too large' })
  const commands = p.commands
  if (commands === undefined) return out
  if (!isObject(commands) || !Object.keys(commands).length) {
    out.push({ path: '/profile/commands', code: 'required', message: 'commands must have at least one command' })
  } else if (p.platform === 'ir') {
    const bad = (name: string, k: string) => out.push({ path: `/profile/commands/${name}/${k}`, code: 'invalid_pronto', message: 'Not a raw Pronto code' })
    for (const [name, c] of Object.entries(commands)) {
      const o = isObject(c) ? c : {}
      if (!isPronto(o.pronto)) bad(name, 'pronto')
      if (o.pronto_toggle !== undefined && !isPronto(o.pronto_toggle)) bad(name, 'pronto_toggle')
    }
  }
  return out
}

/** JSON with sorted keys and no spaces, as Python's `json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=False)`. */
function canonical(v: unknown): string {
  if (Array.isArray(v)) return `[${v.map(canonical).join(',')}]`
  if (isObject(v)) return `{${Object.keys(v).sort().map((k) => `${JSON.stringify(k)}:${canonical(v[k])}`).join(',')}}`
  return JSON.stringify(v)
}

/** The spec's hash of a profile document as sent (meta included): SHA-256 of its canonical JSON. */
export const profileSha = (p: unknown) => createHash('sha256').update(canonical(p)).digest('hex')
