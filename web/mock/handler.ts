// In-memory /api/v1 for UI development, following api/openapi.yaml and
// seeded from api/fixtures. Includes auth, CSRF, optimistic concurrency,
// validation errors and slow/failing adapters. NOT behavioural truth: that
// is hub-sim running the real C++ core.
import type {
  Activity,
  ActivityDraft,
  Backup,
  Device,
  DeviceRegister,
  DeviceUpdate,
  DiscoverRequest,
  ErrorCode,
  ExecuteResult,
  GlobalBindings,
  IntegrationConfig,
  IntegrationConfigUpdate,
  IntegrationState,
  RemotePairStatus,
  Session,
  Status,
  TokenCreated,
  TokenSummary,
  ValidationIssue,
} from '../src/api/types.ts'
import { validateActivity, validateBindings, type Catalog } from '../src/lib/activity.ts'
import { fixture, loadSeed, type Seed } from './fixtures.ts'
import { activityShape, bindingsShape } from './shape.ts'

export interface MockRequest {
  method: string
  path: string // without /api/v1, without query
  query: URLSearchParams
  headers: Record<string, string | undefined>
  body: unknown
  rawBody?: Uint8Array
}

export interface MockResponse {
  status: number
  body?: unknown
  headers?: Record<string, string>
}

export interface MockOptions {
  /** Base latency in ms; each response adds up to 50% jitter. */
  latencyMs?: number
  /** Start with no admin (first-run wizard). */
  firstRun?: boolean
  /** Require login. Default false: the mock starts signed in as admin. */
  requireAuth?: boolean
  random?: () => number
}

type Role = 'public' | 'setup' | 'token' | 'admin'
type Handler = (req: MockRequest, m: string[]) => MockResponse | Promise<MockResponse>

const DEV_SID = 'dev'
const MAX_BODY = 32 * 1024

const err = (status: number, code: ErrorCode, message: string, extra: Record<string, unknown> = {}): MockResponse => ({ status, body: { error: { code, message, ...extra } } })
const invalid = (details: ValidationIssue[]) => err(422, 'validation_failed', details[0]?.message ?? 'Invalid request', { details: details.slice(0, 32) })
const ok = (body?: unknown, status = 200): MockResponse => ({ status: body === undefined ? 204 : status, body })

type Mutable<T> = { -readonly [K in keyof T]: T[K] }
const mut = <T,>(x: T) => x as Mutable<T>

/** RFC 3339 without milliseconds, like the hub. */
const isoNow = () => new Date().toISOString().replace(/\.\d+Z$/, 'Z')

const slug = (s: string) => s.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '').slice(0, 24) || 'x'
const rand36 = (n: number) => Array.from({ length: n }, () => 'abcdefghijklmnopqrstuvwxyz0123456789'[Math.floor(Math.random() * 36)]).join('')
const cookie = (sid: string) => `hubbo_session=${sid}; Path=/; HttpOnly; SameSite=Strict`

export function createMock(opts: MockOptions = {}) {
  const rand = opts.random ?? Math.random
  const bootedAt = Date.now()
  let db: Seed
  let adminCreated: boolean
  let password: string
  let sessions: Map<string, { csrf: string }>
  let pairing: RemotePairStatus & { startedAt?: number }
  let generation: number
  let lastEventAt: number

  function reset() {
    db = loadSeed()
    adminCreated = !opts.firstRun
    password = fixture<{ password: string }>('auth/login.request.json').password
    sessions = new Map(opts.requireAuth || opts.firstRun ? [] : [[DEV_SID, { csrf: `csrf-${DEV_SID}` }]])
    pairing = { state: 'idle', paired: !opts.firstRun, channel: 74 }
    generation = 17
    lastEventAt = Date.now() - 5230
    if (opts.firstRun) {
      db.devices = []
      db.capabilities.clear()
      db.activities = []
      db.activeActivityId = null
      db.globalBindings = { bindings: {}, revision: 1 }
      db.tokens = []
      db.discover.candidates.forEach((c) => (c.registered_device_id = null))
      const ha = db.configs.get('ha_main')!
      ha.settings = { type: 'home_assistant', url: 'http://homeassistant.local:8123', verify_tls: false, ca_cert_pem: null, has_access_token: false }
    }
  }
  reset()

  // ---------- helpers ----------
  const catalog = (): Catalog => ({
    devices: new Map(db.devices.map((d) => [d.id, d])),
    capabilities: db.capabilities,
    systemCommands: db.integrations.find((i) => i.id === 'system')?.commands ?? [],
    activities: db.activities.map((a) => ({ id: a.id, name: a.name })),
  })

  function integrationState(id: string): IntegrationState {
    const c = db.configs.get(id)
    if (!c) return 'error'
    if (!c.enabled) return 'disabled'
    if (c.settings.type === 'home_assistant') return c.settings.has_access_token ? 'connected' : 'not_configured'
    if (c.settings.type === 'ble_hid') return c.settings.bonded ? 'connected' : 'disconnected'
    return 'connected'
  }

  function integrations() {
    return db.integrations.map((i) => {
      const c = db.configs.get(i.id)
      const state = integrationState(i.id)
      const detail = c?.settings.type === 'ble_hid' ? (c.settings.bonded ? `Bonded to ${c.settings.peer_name ?? 'a host'}` : 'Not bonded') : state === 'not_configured' ? 'Add a URL and access token' : i.detail
      return { ...i, name: c?.name ?? i.name, enabled: c?.enabled ?? i.enabled, state, detail, device_count: db.devices.filter((d) => d.integration_id === i.id).length }
    })
  }

  function sessionId(req: MockRequest): string | null {
    const m = /(?:^|;\s*)hubbo_session=([^;]+)/.exec(req.headers.cookie ?? '')
    const sid = m?.[1] ?? (sessions.has(DEV_SID) ? DEV_SID : null)
    return sid && sessions.has(sid) ? sid : null
  }

  function sessionBody(sid: string | null): Session {
    return sid
      ? { authenticated: true, role: 'admin', username: 'admin', csrf_token: sessions.get(sid)!.csrf, expires_in_s: 28_800, setup_required: false, auth_mode: 'enforced' }
      : { authenticated: false, role: null, setup_required: !adminCreated, auth_mode: 'enforced' }
  }

  function newSession(): [string, Session] {
    const sid = rand36(24)
    sessions.set(sid, { csrf: rand36(32) })
    sessions.delete(DEV_SID)
    return [sid, sessionBody(sid)]
  }

  function status(): Status {
    const s = structuredClone(db.status)
    const jitter = (v: number) => Math.round(v * (0.85 + rand() * 0.3) * 10) / 10
    s.uptime_s = db.status.uptime_s + Math.round((Date.now() - bootedAt) / 1000)
    s.time = isoNow()
    s.active_activity_id = db.activeActivityId
    s.integrations = integrations().map(({ id, type, state }) => ({ id, type, state }))
    s.remote.paired = pairing.paired
    s.remote.last_packet_age_ms = Date.now() - lastEventAt
    if (s.remote.last_event) s.remote.last_event.age_ms = Date.now() - lastEventAt
    const l = s.diagnostics.dispatch_latency
    if (l) s.diagnostics.dispatch_latency = { count: l.count + Math.floor((Date.now() - bootedAt) / 4000), p50_ms: jitter(l.p50_ms), p95_ms: jitter(l.p95_ms), p99_ms: jitter(l.p99_ms) }
    return s
  }

  function logError(source: string, code: string, message: string) {
    const list = db.status.diagnostics.last_errors ?? []
    db.status.diagnostics.last_errors = [{ at: isoNow(), source, code, message }, ...list].slice(0, 16)
  }

  function checkActivity(body: unknown, allowed: readonly string[]): MockResponse | null {
    const shape = activityShape(body, allowed)
    if (shape.length) return invalid(shape)
    const v = validateActivity(body as ActivityDraft, catalog())
    return v.valid ? null : invalid(v.errors)
  }

  const revisionOf = (req: MockRequest) => Number(req.query.get('revision'))

  // ---------- routes ----------
  const routes: [string, RegExp, Role, Handler][] = [
    ['GET', /^\/auth\/session$/, 'public', (req) => ok(sessionBody(sessionId(req)))],
    ['POST', /^\/auth\/setup$/, 'setup', (req) => {
      const b = req.body as { username?: string; password?: string }
      const details: ValidationIssue[] = []
      if (!/^[A-Za-z0-9_.-]{1,32}$/.test(b?.username ?? '')) details.push({ path: '/username', code: 'pattern', message: 'Letters, digits, dot, dash and underscore' })
      if (!b?.password || b.password.length < 10) details.push({ path: '/password', code: 'weak_password', message: 'Use at least 10 characters' })
      if (details.length) return invalid(details)
      adminCreated = true
      password = b.password!
      const [sid, s] = newSession()
      return { status: 201, body: s, headers: { 'set-cookie': cookie(sid) } }
    }],
    ['POST', /^\/auth\/login$/, 'public', (req) => {
      if (!adminCreated) return err(409, 'setup_required', 'Create the admin account first')
      const b = req.body as { username?: string; password?: string }
      if (b?.username !== 'admin' || b.password !== password) return err(401, 'invalid_credentials', 'Wrong username or password')
      const [sid, s] = newSession()
      return { status: 200, body: s, headers: { 'set-cookie': cookie(sid) } }
    }],
    ['POST', /^\/auth\/logout$/, 'public', (req) => {
      const sid = sessionId(req)
      if (sid) sessions.delete(sid)
      return { status: 204, headers: { 'set-cookie': 'hubbo_session=; Max-Age=0; Path=/' } }
    }],

    ['GET', /^\/status$/, 'token', () => ok(status())],
    ['GET', /^\/remote\/status$/, 'token', () => ok(status().remote)],
    ['GET', /^\/remote\/buttons$/, 'token', () => ok(db.buttons)],

    // devices
    ['GET', /^\/devices$/, 'token', () => ok({ devices: db.devices })],
    ['GET', /^\/devices\/([^/]+)$/, 'token', (_r, m) => {
      const d = db.devices.find((x) => x.id === m[1])
      return d ? ok(d) : err(404, 'not_found', 'No such device')
    }],
    ['GET', /^\/devices\/([^/]+)\/capabilities$/, 'token', (_r, m) => {
      const c = db.capabilities.get(m[1]!)
      return c ? ok(c) : err(404, 'not_found', 'No such device')
    }],
    ['POST', /^\/devices\/([^/]+)\/commands\/([^/]+)\/execute$/, 'token', (req, m) => {
      const d = db.devices.find((x) => x.id === m[1])
      if (!d) return err(404, 'not_found', 'No such device')
      const params = (req.body as { params?: Record<string, never> })?.params ?? {}
      const a = { integration_id: d.integration_id, device_id: d.id, command_id: m[2]!, params }
      const shape = activityShape({ name: 'x', startup: [a], shutdown: [], bindings: {} }, ['name', 'startup', 'shutdown', 'bindings'])
      const v = shape.length ? shape : validateActivity({ name: 'x', startup: [a], shutdown: [], bindings: {} }, catalog()).errors
      if (v.length) return invalid(v.map((e) => ({ ...e, path: e.path.replace(/^\/startup\/0/, '') })))
      if (integrationState(d.integration_id) !== 'connected') return err(503, 'integration_unavailable', `${d.integration_id} is not connected`)
      // Error cases come from the contract fixtures: that device + command gets that result.
      const r: ExecuteResult = db.executeResults.find((x) => x.device_id === d.id && x.command_id === a.command_id) ?? {
        status: 'accepted',
        device_id: d.id,
        command_id: a.command_id,
        elapsed_ms: Math.round(d.integration_id === 'ble' ? 3 + rand() * 4 : 40 + rand() * 60),
      }
      if (r.status !== 'accepted') logError(d.integration_id, r.status, `${r.command_id} on ${d.id}: ${r.message}`)
      return ok(r)
    }],

    // activities
    ['GET', /^\/activities$/, 'token', () => ok({ activities: db.activities, active_activity_id: db.activeActivityId })],
    ['POST', /^\/activities$/, 'token', (req) => {
      const bad = checkActivity(req.body, ['name', 'startup', 'shutdown', 'bindings'])
      if (bad) return bad
      if (db.activities.length >= 16) return err(409, 'limit_reached', 'At most 16 activities')
      const b = req.body as ActivityDraft
      let id = `act_${slug(b.name)}`
      while (db.activities.some((a) => a.id === id)) id = `act_${slug(b.name).slice(0, 20)}_${rand36(3)}`
      const act: Activity = { id, name: b.name, startup: b.startup, shutdown: b.shutdown, bindings: b.bindings, schema_version: 1, revision: 1 }
      db.activities.push(act)
      return { status: 201, body: act, headers: { location: `/api/v1/activities/${id}` } }
    }],
    ['POST', /^\/activities\/validate$/, 'token', (req) => {
      const shape = activityShape(req.body, ['id', 'name', 'startup', 'shutdown', 'bindings', 'schema_version', 'revision'])
      if (shape.length) return ok({ valid: false, errors: shape })
      const v = validateActivity(req.body as ActivityDraft, catalog())
      return ok({ valid: v.valid, errors: v.errors })
    }],
    ['POST', /^\/activities\/off$/, 'token', () => {
      const cur = db.activities.find((a) => a.id === db.activeActivityId)
      const prev = db.activeActivityId
      db.activeActivityId = null
      return ok({ active_activity_id: null, previous_activity_id: prev, generation: ++generation, queued_actions: cur?.shutdown.length ?? 0 }, 202)
    }],
    ['GET', /^\/activities\/([^/]+)$/, 'token', (_r, m) => {
      const a = db.activities.find((x) => x.id === m[1])
      return a ? ok(a) : err(404, 'not_found', 'No such activity')
    }],
    ['PUT', /^\/activities\/([^/]+)$/, 'token', (req, m) => {
      const i = db.activities.findIndex((x) => x.id === m[1])
      if (i < 0) return err(404, 'not_found', 'No such activity')
      const cur = db.activities[i]!
      const b = req.body as ActivityDraft & { revision?: number }
      if (b?.revision === undefined) return invalid([{ path: '/revision', code: 'required', message: 'revision is required' }])
      if (b.id !== undefined && b.id !== cur.id) return invalid([{ path: '/id', code: 'id_mismatch', message: 'id does not match the URL' }])
      if (b.revision !== cur.revision) return err(409, 'revision_conflict', `Activity changed since revision ${b.revision}`, { current_revision: cur.revision })
      const bad = checkActivity(req.body, ['id', 'name', 'startup', 'shutdown', 'bindings', 'schema_version', 'revision'])
      if (bad) return bad
      const next: Activity = { ...cur, name: b.name, startup: b.startup, shutdown: b.shutdown, bindings: b.bindings, revision: cur.revision + 1 }
      db.activities[i] = next
      return ok(next)
    }],
    ['DELETE', /^\/activities\/([^/]+)$/, 'token', (req, m) => {
      const i = db.activities.findIndex((x) => x.id === m[1])
      if (i < 0) return err(404, 'not_found', 'No such activity')
      if (revisionOf(req) !== db.activities[i]!.revision) return err(409, 'revision_conflict', 'Activity changed since you loaded it', { current_revision: db.activities[i]!.revision })
      db.activities.splice(i, 1)
      if (db.activeActivityId === m[1]) db.activeActivityId = null
      return ok()
    }],
    ['POST', /^\/activities\/([^/]+)\/activate$/, 'token', (_r, m) => {
      const a = db.activities.find((x) => x.id === m[1])
      if (!a) return err(404, 'not_found', 'No such activity')
      const prev = db.activeActivityId
      db.activeActivityId = a.id
      return ok({ active_activity_id: a.id, previous_activity_id: prev, generation: ++generation, queued_actions: a.startup.length }, 202)
    }],
    ['GET', /^\/global-bindings$/, 'token', () => ok(db.globalBindings)],
    ['PUT', /^\/global-bindings$/, 'token', (req) => {
      const b = req.body as GlobalBindings
      const shape: ValidationIssue[] = []
      bindingsShape(b?.bindings, '/bindings', shape)
      if (shape.length) return invalid(shape)
      if (b.revision !== db.globalBindings.revision) return err(409, 'revision_conflict', `Global bindings changed since revision ${b.revision}`, { current_revision: db.globalBindings.revision })
      const errors: ValidationIssue[] = []
      validateBindings(b.bindings, catalog(), errors, [])
      if (errors.length) return invalid(errors)
      db.globalBindings = { bindings: b.bindings, revision: b.revision + 1 }
      return ok(db.globalBindings)
    }],

    // integrations
    ['GET', /^\/integrations$/, 'token', () => ok({ integrations: integrations() })],
    ['GET', /^\/admin\/integrations\/([^/]+)$/, 'admin', (_r, m) => {
      const c = db.configs.get(m[1]!)
      return c ? ok(c) : err(404, 'not_found', 'No such integration')
    }],
    ['PUT', /^\/admin\/integrations\/([^/]+)$/, 'admin', (req, m) => {
      const c = db.configs.get(m[1]!)
      if (!c) return err(404, 'not_found', 'No such integration')
      const b = req.body as IntegrationConfigUpdate
      if (b?.revision === undefined) return invalid([{ path: '/revision', code: 'required', message: 'revision is required' }])
      if (b.revision !== c.revision) return err(409, 'revision_conflict', `Settings changed since revision ${b.revision}`, { current_revision: c.revision })
      const next: IntegrationConfig = structuredClone(c)
      if (b.name !== undefined) next.name = b.name
      if (b.enabled !== undefined) next.enabled = b.enabled
      const s = b.settings
      if (s?.type === 'home_assistant' && next.settings.type === 'home_assistant') {
        if (s.url !== undefined) {
          if (!/^https?:\/\/[^/@?#\s]+(:[0-9]{1,5})?\/?$/.test(s.url)) return invalid([{ path: '/settings/url', code: 'pattern', message: 'Base URL only, like https://homeassistant.local:8123' }])
          // Changing the URL clears the token unless a new one comes with it.
          if (s.url !== next.settings.url && s.access_token === undefined) mut(next.settings).has_access_token = false
          next.settings.url = s.url
        }
        if (s.verify_tls !== undefined) next.settings.verify_tls = s.verify_tls
        if (s.ca_cert_pem !== undefined) next.settings.ca_cert_pem = s.ca_cert_pem
        if (s.access_token !== undefined) mut(next.settings).has_access_token = s.access_token !== null
      } else if (s?.type === 'ble_hid' && next.settings.type === 'ble_hid') {
        if (s.device_name !== undefined) next.settings.device_name = s.device_name
      } else if (s) return invalid([{ path: '/settings/type', code: 'wrong_type', message: `Settings type must be ${next.settings.type}` }])
      next.revision++
      db.configs.set(c.id, next)
      return ok(next)
    }],
    ['POST', /^\/admin\/integrations\/([^/]+)\/discover$/, 'admin', (req, m) => {
      if (m[1] !== 'ha_main') return err(404, 'not_found', 'This integration has nothing to discover')
      if (integrationState('ha_main') !== 'connected') return err(503, 'integration_unavailable', 'Home Assistant is not connected')
      const b = (req.body ?? {}) as DiscoverRequest
      const q = b.query?.toLowerCase() ?? ''
      const candidates = db.discover.candidates.filter((c) => (!q || c.name.toLowerCase().includes(q) || c.provider.entity_id.includes(q)) && (!b.domains?.length || (b.domains as string[]).includes(c.domain)))
      return ok({ candidates, next_cursor: null })
    }],
    ['POST', /^\/admin\/integrations\/([^/]+)\/pair$/, 'admin', (_r, m) => {
      if (m[1] !== 'ble') return err(404, 'not_found', 'This integration does not pair')
      return ok({ state: 'pairing', window_s: 120 }, 202)
    }],
    ['DELETE', /^\/admin\/integrations\/([^/]+)\/pair$/, 'admin', (_r, m) => {
      const c = db.configs.get(m[1]!)
      if (c?.settings.type !== 'ble_hid') return err(404, 'not_found', 'This integration does not pair')
      const st = mut(c.settings)
      st.bonded = false
      delete st.peer_name
      return ok()
    }],

    ['POST', /^\/admin\/devices$/, 'admin', (req) => {
      const b = req.body as DeviceRegister
      if (!b?.name?.trim()) return invalid([{ path: '/name', code: 'required', message: 'Name is required' }])
      if (db.devices.length >= 64) return err(409, 'limit_reached', 'At most 64 devices')
      const p = b.provider
      if (p?.type === 'home_assistant') {
        const c = db.discover.candidates.find((x) => x.provider.entity_id === p.entity_id)
        if (!c || b.integration_id !== 'ha_main') return invalid([{ path: '/provider/entity_id', code: 'unknown_device', message: 'Pick an entity discovered by Home Assistant' }])
        if (!c.supported) return invalid([{ path: '/provider/entity_id', code: 'unknown_command', message: `Hubbo has no commands for ${c.provider.entity_id}` }])
        if (c.registered_device_id) return invalid([{ path: '/provider/entity_id', code: 'duplicate_id', message: `Already registered as ${c.registered_device_id}` }])
      } else if (b.provider?.type !== 'ble_hid' || b.integration_id !== 'ble') return invalid([{ path: '/provider', code: 'integration_mismatch', message: 'Provider does not belong to that integration' }])
      let id = `dev_${slug(b.name)}`
      while (db.devices.some((d) => d.id === id)) id = `dev_${slug(b.name).slice(0, 20)}_${rand36(3)}`
      const d: Device = { id, name: b.name.trim(), integration_id: b.integration_id, kind: b.kind ?? 'other', provider: b.provider, availability: 'available', schema_version: 1, revision: 1 }
      db.devices.push(d)
      const prov = b.provider
      const cand = prov.type === 'home_assistant' ? db.discover.candidates.find((x) => x.provider.entity_id === prov.entity_id) : undefined
      if (cand) cand.registered_device_id = id
      // Capabilities: reuse a fixture device of the same kind/integration, filtered to the discovered command IDs.
      const like = db.devices.find((x) => x.id !== id && x.integration_id === d.integration_id && x.kind === d.kind) ?? db.devices.find((x) => x.id !== id && x.integration_id === d.integration_id)
      const base = like ? db.capabilities.get(like.id)!.commands : []
      db.capabilities.set(id, { device_id: id, integration_id: d.integration_id, commands: cand?.command_ids ? base.filter((c) => cand.command_ids!.includes(c.command_id)) : base })
      return { status: 201, body: d, headers: { location: `/api/v1/devices/${id}` } }
    }],
    ['PUT', /^\/admin\/devices\/([^/]+)$/, 'admin', (req, m) => {
      const i = db.devices.findIndex((x) => x.id === m[1])
      if (i < 0) return err(404, 'not_found', 'No such device')
      const d = db.devices[i]!
      const b = req.body as DeviceUpdate
      if (b?.revision !== d.revision) return err(409, 'revision_conflict', `Device changed since revision ${b?.revision}`, { current_revision: d.revision })
      if (b.name !== undefined && !b.name.trim()) return invalid([{ path: '/name', code: 'required', message: 'Name is required' }])
      db.devices[i] = { ...d, ...(b.name ? { name: b.name.trim() } : {}), ...(b.kind ? { kind: b.kind } : {}), ...(b.provider ? { provider: b.provider } : {}), revision: d.revision + 1 }
      return ok(db.devices[i])
    }],
    ['DELETE', /^\/admin\/devices\/([^/]+)$/, 'admin', (req, m) => {
      const d = db.devices.find((x) => x.id === m[1])
      if (!d) return err(404, 'not_found', 'No such device')
      if (revisionOf(req) !== d.revision) return err(409, 'revision_conflict', 'Device changed since you loaded it', { current_revision: d.revision })
      const refs: ValidationIssue[] = []
      for (const a of db.activities) {
        a.startup.forEach((x, i) => x.device_id === d.id && refs.push({ path: `/activities/${a.id}/startup/${i}`, code: 'referenced', message: `${a.name} startup step ${i + 1}` }))
        a.shutdown.forEach((x, i) => x.device_id === d.id && refs.push({ path: `/activities/${a.id}/shutdown/${i}`, code: 'referenced', message: `${a.name} shutdown step ${i + 1}` }))
        for (const [k, list] of Object.entries(a.bindings)) list.forEach((x, i) => x.device_id === d.id && refs.push({ path: `/activities/${a.id}/bindings/${k}/${i}`, code: 'referenced', message: `${a.name} ${k}` }))
      }
      for (const [k, list] of Object.entries(db.globalBindings.bindings)) list.forEach((x, i) => x.device_id === d.id && refs.push({ path: `/global_bindings/${k}/${i}`, code: 'referenced', message: `Global ${k}` }))
      if (refs.length) return err(409, 'device_in_use', `${d.id} is used in ${refs.length} place${refs.length === 1 ? '' : 's'}`, { details: refs.slice(0, 32) })
      db.devices = db.devices.filter((x) => x.id !== d.id)
      db.capabilities.delete(d.id)
      db.discover.candidates.forEach((c) => c.registered_device_id === d.id && (c.registered_device_id = null))
      return ok()
    }],

    // remote pairing
    ['GET', /^\/admin\/remote\/pair$/, 'admin', () => {
      if (pairing.state === 'scanning') {
        const s = (Date.now() - (pairing.startedAt ?? 0)) / 1000
        if (s > 6) pairing = { state: 'found', paired: true, channel: 74, message: 'Learned the Hub address. Press a remote button to confirm.' }
        else pairing.remaining_s = Math.max(0, 60 - Math.floor(s))
      }
      const { startedAt: _s, ...rest } = pairing
      return ok(rest)
    }],
    ['POST', /^\/admin\/remote\/pair$/, 'admin', () => {
      pairing = { state: 'scanning', paired: pairing.paired, remaining_s: 60, message: 'Press the pair button on the Harmony Hub', startedAt: Date.now() }
      const { startedAt: _s, ...rest } = pairing
      return ok(rest, 202)
    }],
    ['DELETE', /^\/admin\/remote\/pair$/, 'admin', () => {
      pairing = { state: 'idle', paired: pairing.paired, channel: 74 }
      return ok()
    }],

    // account & tokens
    ['PUT', /^\/admin\/account\/password$/, 'admin', (req) => {
      const b = req.body as { current_password?: string; new_password?: string }
      if (b?.current_password !== password) return err(401, 'invalid_credentials', 'Current password is wrong')
      if (!b.new_password || b.new_password.length < 10) return invalid([{ path: '/new_password', code: 'weak_password', message: 'Use at least 10 characters' }])
      password = b.new_password
      return ok()
    }],
    ['GET', /^\/admin\/tokens$/, 'admin', () => ok({ tokens: db.tokens })],
    ['POST', /^\/admin\/tokens$/, 'admin', (req) => {
      const name = (req.body as { name?: string })?.name?.trim()
      if (!name) return invalid([{ path: '/name', code: 'required', message: 'Name is required' }])
      if (db.tokens.length >= 16) return err(409, 'limit_reached', 'At most 16 tokens')
      const id = rand36(8)
      const secret = Array.from({ length: 43 }, () => 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'[Math.floor(Math.random() * 64)]).join('')
      const t: TokenSummary = { id: `tok_${id}`, name, hint: `hbt_${id}_`, created_at: isoNow(), last_used_at: null }
      db.tokens.push(t)
      const created: TokenCreated = { ...t, token: `hbt_${id}_${secret}` }
      return ok(created, 201)
    }],
    ['DELETE', /^\/admin\/tokens\/([^/]+)$/, 'admin', (_r, m) => {
      if (!db.tokens.some((t) => t.id === m[1])) return err(404, 'not_found', 'No such token')
      db.tokens = db.tokens.filter((t) => t.id !== m[1])
      return ok()
    }],

    // backup & firmware
    ['GET', /^\/admin\/backup$/, 'admin', () => {
      const b: Backup = {
        ...db.backup,
        created_at: isoNow(),
        devices: db.devices,
        activities: db.activities,
        global_bindings: db.globalBindings,
        integrations: [...db.configs.values()].map((c) => ({
          id: c.id,
          type: c.type,
          name: c.name,
          enabled: c.enabled,
          settings: c.settings.type === 'home_assistant' ? { url: c.settings.url, verify_tls: c.settings.verify_tls, ca_cert_pem: c.settings.ca_cert_pem ?? null } : c.settings.type === 'ble_hid' ? { device_name: c.settings.device_name } : {},
        })),
      }
      return { status: 200, body: b, headers: { 'content-disposition': 'attachment; filename="hubbo-backup.json"' } }
    }],
    ['POST', /^\/admin\/restore$/, 'admin', (req) => {
      const b = req.body as Partial<Backup>
      if (b?.format !== 'hubbo-backup') return invalid([{ path: '/format', code: 'wrong_type', message: 'This is not a Hubbo backup file' }])
      if (b.format_version !== 1) return invalid([{ path: '/format_version', code: 'wrong_type', message: `Backup format ${String(b.format_version)} is not supported` }])
      if (!Array.isArray(b.devices) || !Array.isArray(b.activities)) return invalid([{ path: '', code: 'required', message: 'Backup is missing devices or activities' }])
      const dry = req.query.get('dry_run') === 'true'
      const result = { dry_run: dry, devices: b.devices.length, activities: b.activities.length, integrations: b.integrations?.length ?? 0, migrated_from: null, requires: ['home_assistant_token', 'ble_pairing', 'api_tokens'], warnings: [] }
      if (!dry) {
        db.devices = b.devices
        db.activities = b.activities
        if (b.global_bindings) db.globalBindings = b.global_bindings
        db.activeActivityId = null
      }
      return ok(result)
    }],
    ['GET', /^\/admin\/firmware$/, 'admin', () => ok(db.firmware)],
    ['POST', /^\/admin\/firmware\/update$/, 'admin', (req) => {
      const n = req.rawBody?.byteLength ?? 0
      if (n < 1024) return invalid([{ path: '', code: 'wrong_type', message: 'That file is not a signed Hubbo firmware image for this board' }])
      return ok({ ...db.firmware, update_state: 'rebooting', received_bytes: n, message: 'Image verified. Rebooting into the new slot.' }, 202)
    }],

    // Mock-only controls (never in the real API).
    ['POST', /^\/__mock\/reset$/, 'public', () => (reset(), ok())],
    ['POST', /^\/__mock\/touch\/([^/]+)$/, 'public', (_r, m) => {
      const a = db.activities.find((x) => x.id === m[1])
      if (a) db.activities[db.activities.indexOf(a)] = { ...a, name: `${a.name} (edited elsewhere)`, revision: a.revision + 1 }
      return ok()
    }],
  ]

  async function handle(req: MockRequest): Promise<MockResponse> {
    const base = opts.latencyMs ?? 0
    if (base > 0) await new Promise((r) => setTimeout(r, base * (0.5 + rand())))
    if (req.rawBody && req.rawBody.byteLength > MAX_BODY && !req.path.startsWith('/admin/firmware') && !req.path.startsWith('/admin/restore')) {
      return err(413, 'payload_too_large', `Request bodies are limited to ${MAX_BODY / 1024} KiB`)
    }
    const hit = routes.find(([method, re]) => method === req.method && re.test(req.path))
    if (!hit) return err(404, 'not_found', `No route for ${req.method} ${req.path}`)
    const [, re, role, fn] = hit
    const sid = sessionId(req)
    if (role === 'setup' && adminCreated) return err(409, 'setup_already_done', 'An admin account already exists')
    if (role === 'token' || role === 'admin') {
      if (!adminCreated) return err(409, 'setup_required', 'Finish first-run setup')
      if (!sid) return err(401, 'unauthenticated', 'Sign in to continue')
      if (req.method !== 'GET' && req.headers['x-hubbo-csrf'] !== sessions.get(sid)!.csrf) return err(403, 'csrf_failed', 'Missing or wrong X-Hubbo-CSRF header')
    }
    const m = re.exec(req.path)!.map((s) => (s === undefined ? '' : decodeURIComponent(s)))
    return fn(req, m)
  }

  return { handle, reset }
}
