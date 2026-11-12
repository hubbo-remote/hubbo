// In-memory /api/v1 for UI development, following api/openapi.yaml and
// seeded from api/fixtures. Includes auth, the Origin check, optimistic concurrency,
// validation errors and slow/failing adapters. NOT behavioural truth: that
// is hub-sim running the real C++ core.
import type {
  Action,
  Activity,
  ActivityInput,
  AuthState,
  Backup,
  Command,
  ConfigState,
  Capabilities,
  Device,
  DeviceDetail,
  DeviceRegister,
  DeviceRegisterProfile,
  DeviceUpdate,
  DiscoverStatus,
  ErrorCode,
  EventKind,
  ButtonId,
  ExecuteResult,
  GlobalBindings,
  IntegrationConfig,
  IntegrationConfigUpdate,
  IntegrationState,
  RemoteConfig,
  RemoteEndpoint,
  RemoteEndpointUpdate,
  RemotePairStatus,
  Session,
  Status,
  TokenCreated,
  TokenSummary,
  TransitionResult,
  ValidationIssue,
  BluetoothStatus,
  EntityDetailsStatus,
  FirmwareStatus,
} from '../src/api/types.ts'
import { BLE_MAX_DEVICES, BLE_PAIR_WINDOW_MAX_S, BLE_PAIR_WINDOW_MIN_S, BLE_PAIR_WINDOW_S, REMOTE_CHANNEL_MAX, REMOTE_SCAN_TIMEOUT_S } from '../src/api/constants.ts'
import { forEachAction, validateActivity, validateBindings, type Catalog } from '../src/lib/activity.ts'
import { addressProblem } from '../src/lib/remote.ts'
import { checkFirmwareImage } from './firmware.ts'
import { fixture, fixtureBytes, loadSeed, type Seed } from './fixtures.ts'
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
  /** 0: drop the connection (the hub is rebooting). */
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
  /** How long an HA discovery runs. Default 1.5 s; e2e uses 0. */
  discoverDelayMs?: number
  /** How long a remote-pairing scan takes to "find" the Hub. Default 6 s; e2e uses 0. */
  pairDelayMs?: number
  /** Hub ms a firmware update or factory reset moves per real ms. Default 1/3 (a 6 s write); e2e uses 0, so only `advance` moves it, like hub-sim. */
  timeScale?: number
  random?: () => number
}

/** What `POST /__mock/reset` may change until the next reset (e2e uses this). */
export type ResetFlags = Pick<MockOptions, 'firstRun' | 'requireAuth'> & {
  /** Start with a saved config the hub can't use (`blocked`) or can't write (`storage_unavailable`). */
  configState?: 'blocked' | 'storage_unavailable'
  /** With `configState: 'blocked'`: the saved record is damaged (no intact slot), so there's nothing to export. */
  configDamaged?: boolean
  /** Activations switch but report the startup as rejected (`queue_full`), like a full action queue. */
  startupRejected?: boolean
  /** HA discovery reports this many entities left out (the hub's 2048-candidate cap). */
  discoverTruncated?: number
  /** HA discovery ends `failed` instead of `done`. */
  discoverFails?: boolean
  /** A remote scan never hears a Hub: it runs out its 120 s and ends `timeout`. */
  scanNeverFinds?: boolean
  /** The credentials store couldn't be read (or held damaged credentials) at boot. */
  authState?: 'unavailable'
}

type Role = 'public' | 'setup' | 'token' | 'admin'
type Handler = (req: MockRequest, m: string[]) => MockResponse | Promise<MockResponse>

const DEV_SID = 'dev'
/** Like hub-sim's SimulatedFirmwareUpdate: a write takes 2 s, then rebooting 1 s (the mock's hub is down for its second half). */
const FW_WRITE_MS = 2000
const FW_REBOOT_MS = 1000
/** The OTA slot (POST /admin/firmware/update x-hubbo-max-body). */
const FW_SLOT_BYTES = 6_553_600
/** A factory reset's restart, like hub-sim's (the mock's hub is down for all of it). */
const ERASE_RESTART_MS = 1000
/** Sessions last 7 days, absolute (or until the hub reboots). */
const SESSION_S = 604_800
/** Requests refused while the config is blocked or unwritable (validate is a dry run, so it isn't one). */
const EDITS = [/^\/activities(\/(?!validate$)[^/]+)?$/, /^\/activities\/[^/]+\/activate$/, /^\/global-bindings$/, /^\/admin\/devices/]
const MAX_BODY = 32 * 1024
/** Like the hub's JSON reader: deeper nesting is malformed (a restore is a file, so it gets the file limit). */
const MAX_JSON_DEPTH = 8
const MAX_FILE_DEPTH = 12
const depth = (v: unknown): number => (v !== null && typeof v === 'object' ? 1 + Math.max(0, ...Object.values(v).map(depth)) : 0)
/** Entity → platform, the integration services Hubbo would offer, and the ones it skips. */
const PLATFORMS: [RegExp, string, Command[], string[]][] = [
  [/denon/, 'denonavr', [{ command_id: 'denonavr.set_dynamic_eq', label: 'Dynamic EQ', effect: 'momentary', params: [{ name: 'dynamic_eq', label: 'Dynamic EQ', type: 'bool', required: true }] }], ['denonavr.get_command']],
  [/sonos/, 'sonos', [{ command_id: 'sonos.set_sleep_timer', label: 'Sleep timer', effect: 'momentary', params: [{ name: 'sleep_time', label: 'Sleep after', type: 'int', required: true, minimum: 0, maximum: 86400, unit: 's' }] }], ['sonos.snapshot', 'sonos.restore', 'sonos.play_queue']],
  [/lg_tv/, 'webostv', [{ command_id: 'webostv.select_sound_output', label: 'Sound output', effect: 'momentary', params: [{ name: 'sound_output', label: 'Output', type: 'enum', required: true, options: ['tv_speaker', 'external_arc', 'external_optical', 'bt_soundbar'] }] }], ['webostv.command']],
  [/xbox/, 'xbox', [], []],
  [/^light\./, 'hue', [], ['hue.activate_scene']],
  [/^switch\./, 'hue', [], []],
  [/kitchen/, 'cast', [], []],
]
const INTEGRATION_SERVICES = new Map(PLATFORMS.flatMap(([, , services]) => services.map((cmd) => [cmd.command_id, cmd] as const)))
/** POST /admin/restore x-hubbo-max-body (api/openapi.yaml). */
const RESTORE_MAX_BODY = 640 * 1024
/** The saved config record format this "firmware" reads (core kConfigFileVersion). */
const CONFIG_SCHEMA = 1
/** The saved record a blocked mock hub holds: from newer firmware, so restoring it here is refused. */
const BLOCKED_RECORD = 'admin/restore.blocked-raw.request.json'
const BLOCKED_RECORD_SCHEMA = fixture<{ schema_version: number }>(BLOCKED_RECORD).schema_version
/** An error response straight from api/fixtures/errors. */
const fixtureError = (status: number, file: string): MockResponse => ({ status, body: fixture(file) })
/** The hub's refusal of fields a body may not carry, at `at` (e.g. `/devices/0`). */
const unknownFields = (keys: string[], at = '') => keys.map((k) => ({ path: `${at}/${k}`, code: 'unknown_field' as const, message: `Unknown field ${k}` }))

const err = (status: number, code: ErrorCode, message: string, extra: Record<string, unknown> = {}): MockResponse => ({ status, body: { error: { code, message, ...extra } } })
const invalid = (details: ValidationIssue[], message = details[0]?.message ?? 'Invalid request') => err(422, 'validation_failed', message, { details: details.slice(0, 32) })
const nameRequired = () => invalid([{ path: '/name', code: 'required', message: 'Name is required' }])
const deviceLimit = () => err(409, 'limit_reached', 'At most 64 devices')
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
  /** The seeded Bluetooth device's commands, the shape every Bluetooth device's capabilities take. */
  let bleTemplate: Command[] = []
  let adminCreated: boolean
  let username: string
  let password: string
  let sessions: Set<string>
  let pairing: RemotePairStatus & { startedAt?: number }
  /** The Hub endpoint the radio listens on (GET /admin/remote). */
  let endpoint: RemoteEndpoint | null
  /** Simulated HA client state (POST /__mock/ha); cleared when its settings change, like the real client. */
  let haOverride: IntegrationState | null
  /** Bonded Bluetooth hosts by device id, and whether each is connected (like hub-sim's fake hosts). */
  let bleHosts = new Map<string, { connected: boolean }>()
  /** Like hub-sim's /sim/ble/radio?ready=0: the radio is down (no host connects, pairing is refused). */
  let bleRadioDown = false
  /** The one pairing window (POST /admin/devices/{id}/pair). When it ends unbonded, the device keeps its old host, if any. */
  let blePair: { device: string; endsAt: number } | null = null
  /** Like hub-sim's /sim/ha?auth=reject and ?connect=stall: how the fake HA answers the next connect (after a save). */
  let haNextConnect: 'ok' | 'reject' | 'stall' = 'ok'
  /** A firmware update in hub time: `advancedMs` from /__mock/advance plus scaled real time since `startedAt`. */
  let fw: { state: 'idle' } | { state: 'writing'; startedAt: number; advancedMs: number; version: string } | { state: 'failed'; error: string }
  /** Like hub-sim's POST /sim/firmware: writes fail; the hub can't take an update. Mock-only: the new image doesn't take (the old one boots again). */
  let fwFail: boolean
  let fwReady: boolean
  let fwRevert: boolean
  /** A factory reset in progress, in hub time like `fw`. */
  let erase: { startedAt: number; advancedMs: number } | null
  /** Like hub-sim's POST /sim/factory-reset?ready=0: the hub can't erase (503). */
  let eraseReady: boolean
  /** Entities HA has forgotten (POST /__mock/ha?remove=): their devices go stale at the next discovery. */
  let removedEntities: Set<string>
  /** Each HA entity's own command variants (e.g. its source list), from the fixture device on it. */
  let entityCommands: Map<string, Command[]>
  /** What only an entity's details carry (§3b): its integration's services, the skipped ones, its registry id. */
  let entityExtras: Map<string, { services: string[]; skipped: string[]; uniqueId?: string }>
  /** The last details request (one at a time); dropped when the HA settings change. */
  let details: { entityId: string; startedAt: number; state: 'running' | 'settled' } | null
  /** A renamed entity's old id (POST /__mock/ha?rename=): like the hub's unique_id match, its details name the device. */
  let renamedFrom: Map<string, string>
  /** On-demand HA discovery (POST/GET/DELETE …/discover). */
  let discovery: { state: DiscoverStatus['state']; startedAt?: number; elapsedMs?: number }
  let transitions: number // TransitionResult.generation
  let lastEventAt: number
  /** The last remote event (POST /__mock/remote/…); the fixture's until then. */
  let lastEvent: { button: ButtonId; kind: EventKind } | null
  // The config generation: every committed change bumps it, and the changed
  // object's revision is that generation (api/openapi.yaml ConfigState).
  let configGen: number
  let config: ConfigState
  /** Storage couldn't be read when the config loaded (boot with storage down), as opposed to failing later. */
  let loadUnreadable = false
  /** While blocked: the saved config record's bytes, exactly as stored (null when it's damaged). */
  let blockedRecord: Uint8Array | null = null
  const commit = () => {
    configGen++
    config = { state: 'ok', generation: configGen } // the first save also ends `fresh`
    return configGen
  }

  /** `flags` override the start-up options until the next reset (e2e tests use this). */
  let mode: ResetFlags = opts
  function reset(flags: ResetFlags = {}) {
    mode = { ...flags, firstRun: flags.firstRun ?? opts.firstRun, requireAuth: flags.requireAuth ?? opts.requireAuth }
    db = loadSeed()
    for (const d of db.devices) delete d.bluetooth // runtime status, computed by live()
    bleTemplate = [...db.capabilities.values()].find((c) => c.integration_id === 'ble')?.commands ?? []
    adminCreated = !mode.firstRun
    // A set-up mock hub's admin is the login fixture's (the user picked the name; there's no default).
    ;({ username, password } = fixture<{ username: string; password: string }>('auth/login.request.json'))
    // No sessions where none could exist: login required, first run, or credentials the hub can't read.
    sessions = new Set(mode.requireAuth || mode.firstRun || mode.authState ? [] : [DEV_SID])
    // From admin/remote.json; a first-run hub has no endpoint yet.
    pairing = mode.firstRun ? { state: 'idle', paired: false } : { ...db.remote.pairing }
    endpoint = mode.firstRun ? null : db.remote.endpoint
    haOverride = null
    // Like hub-sim: every seeded Bluetooth device starts paired and connected.
    bleHosts = new Map(db.devices.filter((d) => d.provider.type === 'ble_hid').map((d) => [d.id, { connected: true }]))
    blePair = null
    bleRadioDown = false
    haNextConnect = 'ok'
    fw = { state: 'idle' }
    fwFail = false
    fwReady = true
    fwRevert = false
    erase = null
    eraseReady = true
    discovery = { state: 'idle' }
    // The fixture's entities come from HA's `demo` platform, which has no services of its own. Give them
    // realistic platforms, with integration services and skipped ones, so the UI shows those too.
    // Candidates preview only the domain commands; the services come with the entity's details.
    entityExtras = new Map()
    renamedFrom = new Map()
    details = null
    // A free Denon with integration services, the same entity hub-sim's fake HA has.
    if (!haCandidate('media_player.denon')) db.discover.candidates.push({ provider: { type: 'home_assistant', entity_id: 'media_player.denon' }, name: 'Denon AVR-X3700H', domain: 'media_player', supported: true, command_ids: ['media_player.turn_on', 'media_player.turn_off', 'media_player.volume_set', 'media_player.select_source'], dropped_options: 0, registered_device_id: null })
    for (const c of db.discover.candidates) {
      const [, platform = 'unknown', services = [], skipped = []] = PLATFORMS.find(([re]) => re.test(c.provider.entity_id)) ?? []
      entityExtras.set(c.provider.entity_id, { services: services.map((s) => s.command_id), skipped, uniqueId: c.provider.unique_id ?? `${platform}-${c.provider.entity_id.split('.')[1]}` })
      delete c.provider.unique_id // details carry it
      Object.assign(c, { platform, dropped_options: 0 })
    }
    // A YAML-only entity (no registry entry, so no platform or unique_id), like hub-sim's fake HA.
    if (!haCandidate('switch.patio_fountain')) db.discover.candidates.push({ provider: { type: 'home_assistant', entity_id: 'switch.patio_fountain' }, name: 'Patio Fountain', domain: 'switch', supported: true, command_ids: ['switch.turn_on', 'switch.turn_off'], dropped_options: 0, registered_device_id: null })
    removedEntities = new Set()
    entityCommands = new Map(db.discover.candidates.flatMap((c) => (c.registered_device_id ? [[c.provider.entity_id, db.capabilities.get(c.registered_device_id)?.commands ?? []]] : [])))
    transitions = 17
    lastEventAt = Date.now() - 5230
    lastEvent = null
    configGen = Math.max(1, db.globalBindings.revision, ...db.activities.map((a) => a.revision), ...db.devices.map((d) => d.revision), ...[...db.configs.values()].map((c) => c.revision))
    config = { state: 'ok', generation: configGen }
    if (mode.firstRun) {
      db.tokens = []
      factoryDefaults()
      configGen = 0
      config = { state: 'fresh', generation: 0 }
    }
    blockedRecord = null
    if (mode.configState === 'blocked' && mode.configDamaged) config = { state: 'blocked', generation: configGen + 1, reason: 'Saved config is damaged (no intact copy)' }
    else if (mode.configState === 'blocked') {
      config = { state: 'blocked', generation: configGen + 1, reason: `config.json schema_version ${BLOCKED_RECORD_SCHEMA} is newer than this firmware (${CONFIG_SCHEMA})` }
      blockedRecord = fixtureBytes(BLOCKED_RECORD)
    }
    loadUnreadable = mode.configState === 'storage_unavailable'
    if (loadUnreadable) config = { state: 'storage_unavailable', generation: configGen, reason: 'Storage could not be mounted' }
    if (mode.configState) db.activeActivityId = null
  }

  /** Factory defaults: no devices, activities or global bindings; built-in integrations unconfigured. Credentials and tokens stay. */
  function factoryDefaults() {
    db.devices = []
    db.capabilities.clear()
    db.activities = []
    db.activeActivityId = null
    db.globalBindings = { bindings: {}, revision: configGen }
    db.discover.candidates.forEach((c) => (c.registered_device_id = null))
    const ha = db.configs.get('ha_main')!
    ha.settings = { type: 'home_assistant', url: 'http://homeassistant.local:8123', verify_tls: false, has_access_token: false }
  }

  /** Restore and reset stamp every object with one new generation, so every stale client gets 409. */
  function stampAll() {
    const g = commit()
    blockedRecord = null
    db.activities = db.activities.map((a) => ({ ...a, revision: g }))
    db.devices = db.devices.map((d) => ({ ...d, revision: g }))
    db.globalBindings = { ...db.globalBindings, revision: g }
    for (const c of db.configs.values()) c.revision = g
    // Restore and reset never start (or stop) an activity: the active one stays only if its id survived.
    if (!db.activities.some((a) => a.id === db.activeActivityId)) db.activeActivityId = null
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
    if (c.settings.type === 'home_assistant') return c.settings.has_access_token ? (haOverride ?? 'connected') : 'not_configured'
    // ble is the radio: working (connected) while enabled, `error` when it's down.
    if (c.settings.type === 'ble_hid') return bleRadioDown ? 'error' : 'connected'
    return 'connected'
  }

  function pairStatus(): RemotePairStatus {
    if (pairing.state === 'scanning') {
      const ms = Date.now() - (pairing.startedAt ?? 0)
      if (!mode.scanNeverFinds && ms >= (opts.pairDelayMs ?? 6000)) {
        endpoint = { channel: 74, address: db.remote.endpoint?.address ?? 'FC5C97550F' }
        pairing = { state: 'found', paired: true, channel: 74, message: 'Learned the Hub address. Press a remote button to confirm.' }
      } else if (ms >= REMOTE_SCAN_TIMEOUT_S * 1000) pairing = { state: 'timeout', paired: !!endpoint, ...(endpoint ? { channel: endpoint.channel } : {}), message: `No Hub answered in ${REMOTE_SCAN_TIMEOUT_S} s` }
      else pairing.remaining_s = Math.max(0, REMOTE_SCAN_TIMEOUT_S - Math.floor(ms / 1000))
    }
    const { startedAt: _s, ...rest } = pairing
    return rest
  }

  /** A media_player, light or switch entity id; never a broadcast like `light.all`. */
  function haEntityIdOk(id: string) {
    return id.length <= 96 && /^(media_player|light|switch)\.[a-z0-9_]+$/.test(id) && !id.endsWith('.all')
  }

  /** An entity HA has now (whatever the latest discovery saw). */
  function haCandidate(entityId: string) {
    return db.discover.candidates.find((x) => x.provider.entity_id === entityId)
  }

  /** The details of the last requested entity (§3b); `idle` for any other. They settle after the discovery delay. */
  function entityDetails(entityId: string): EntityDetailsStatus {
    if (!details || details.entityId !== entityId) return { state: 'idle' }
    if (details.state === 'running' && Date.now() - details.startedAt < (opts.discoverDelayMs ?? 1500)) return { state: 'running', entity_id: entityId }
    details.state = 'settled'
    const c = haCandidate(entityId)
    if (!c) return { state: 'failed', entity_id: entityId, error: `${entityId} isn't in Home Assistant` }
    const x = entityExtras.get(entityId)
    return {
      state: 'done',
      entity_id: entityId,
      provider: { type: 'home_assistant', entity_id: entityId, ...(x?.uniqueId ? { unique_id: x.uniqueId } : {}) },
      name: c.name,
      domain: c.domain,
      ...(c.platform ? { platform: c.platform } : {}),
      supported: c.supported,
      command_ids: [...(c.command_ids ?? []), ...(x?.services ?? [])],
      dropped_options: c.dropped_options ?? 0,
      skipped_services: x?.skipped ?? [],
      // The list matches by entity_id only; the details also find a device on the renamed entity.
      registered_device_id: c.registered_device_id ?? db.devices.find((d) => d.provider.type === 'home_assistant' && d.provider.entity_id === renamedFrom.get(entityId))?.id ?? null,
    }
  }

  /**
   * The entity a register or re-link (by `selfId`) would snapshot: HA connected, its details loaded
   * (done, for this entity), with commands to offer and no other device on it. Else the error.
   */
  type EntityWithCommands = EntityDetailsStatus & { command_ids: string[] }
  function haEntity(entityId: string, selfId?: string): EntityWithCommands | MockResponse {
    // Like the hub: the id's shape first, then the details, then the commands.
    if (!haEntityIdOk(entityId)) return invalid([{ path: '/provider/entity_id', code: 'pattern', message: 'Not a media_player, light or switch entity id' }])
    if (integrationState('ha_main') !== 'connected') return err(503, 'integration_unavailable', 'Home Assistant is not connected')
    const d = entityDetails(entityId)
    if (d.state !== 'done') return err(409, 'details_required', `Load ${entityId}'s details first (POST …/entities/${entityId}/details)`)
    if (!d.supported || !d.command_ids?.length) return invalid([{ path: '/provider/entity_id', code: 'unknown_device', message: `Hubbo offers no commands for ${entityId}` }])
    if (d.registered_device_id && d.registered_device_id !== selfId) return invalid([{ path: '/provider/entity_id', code: 'duplicate_id', message: `Already registered as ${d.registered_device_id}; re-link that device instead` }])
    return d as EntityWithCommands
  }

  function remoteConfig(): RemoteConfig {
    const p = pairStatus()
    return { radio: p.state === 'scanning' ? 'scanning' : endpoint ? 'listening' : 'idle', endpoint, pairing: p }
  }

  /** A running discovery finishes once its delay is up. */
  function settleDiscovery() {
    if (discovery.state === 'running' && Date.now() - (discovery.startedAt ?? 0) >= (opts.discoverDelayMs ?? 1500)) {
      discovery = { state: mode.discoverFails ? 'failed' : 'done', elapsedMs: Date.now() - (discovery.startedAt ?? Date.now()) }
      // Like hub-sim: a finished discovery is when the hub notices registered entities HA no longer has.
      if (discovery.state === 'done') for (const d of db.devices) if (d.provider.type === 'home_assistant' && removedEntities.has(d.provider.entity_id)) d.availability = 'stale'
    }
  }

  function discoverStatus(q: URLSearchParams): MockResponse {
    settleDiscovery()
    if (discovery.state === 'failed') return ok(fixture<DiscoverStatus>('integrations/ha_main.discovery_failed.json'))
    if (discovery.state !== 'done') return ok({ state: discovery.state } satisfies DiscoverStatus)
    const text = q.get('q')?.toLowerCase() ?? ''
    const domain = q.get('domain')
    if (domain && !['media_player', 'light', 'switch'].includes(domain)) return invalid([{ path: '/domain', code: 'invalid_enum_value', message: 'domain must be media_player, light or switch' }])
    const limit = Number(q.get('limit') ?? 50)
    if (!Number.isInteger(limit) || limit < 1 || limit > 100) return invalid([{ path: '/limit', code: 'param_out_of_range', message: 'limit must be 1 to 100' }])
    const offset = Number(q.get('cursor') ?? 0) || 0
    const all = db.discover.candidates.filter((c) => (!text || c.name.toLowerCase().includes(text) || c.provider.entity_id.includes(text)) && (!domain || c.domain === domain))
    const page = all.slice(offset, offset + limit)
    return ok({ state: 'done', elapsed_ms: discovery.elapsedMs ?? 0, total: db.discover.candidates.length, truncated: mode.discoverTruncated ?? 0, candidates: page, next_cursor: offset + limit < all.length ? String(offset + limit) : null } satisfies DiscoverStatus)
  }

  /** What registering an HA entity stores: its allowlisted built-ins, plus its integration's own services (design §3). */
  function haCommands(c: { entity_id?: string; command_ids?: string[] }): Command[] {
    const all = new Map<string, Command>()
    for (const caps of db.capabilities.values()) for (const cmd of caps.commands) if (cmd.command_id.includes('.') && !all.has(cmd.command_id)) all.set(cmd.command_id, cmd)
    // The entity's own variant of a shared command wins (e.g. its source list), whichever device it's linked to.
    for (const cmd of entityCommands.get(c.entity_id!) ?? []) all.set(cmd.command_id, cmd)
    // A built-in the fixtures never show (e.g. switch.turn_on): a plain momentary command.
    const plain = (id: string): Command => ({ command_id: id, label: id.split('.').pop()!.replace(/_/g, ' ').replace(/^./, (x) => x.toUpperCase()), effect: 'momentary', params: [] })
    return (c.command_ids ?? []).map((id) => all.get(id) ?? INTEGRATION_SERVICES.get(id) ?? plain(id))
  }

  /** Like the hub: drop pairings (and a window) whose device is no longer in config. */
  function pruneBonds() {
    const ble = new Set(db.devices.filter((d) => d.provider.type === 'ble_hid').map((d) => d.id))
    for (const id of bleHosts.keys()) if (!ble.has(id)) bleHosts.delete(id)
    if (blePair && !ble.has(blePair.device)) blePair = null
  }

  /** The open pairing window, or null once it has run out. */
  function bleWindow() {
    if (blePair && blePair.endsAt <= Date.now()) blePair = null
    return blePair
  }

  /** A Bluetooth device's status (core's per-device status function). */
  function bleStatus(id: string): BluetoothStatus {
    const w = bleWindow()
    if (w?.device === id) return { state: 'pairing', window_s: Math.ceil((w.endsAt - Date.now()) / 1000) }
    const h = bleHosts.get(id)
    return { state: !h ? 'not_paired' : h.connected && !bleRadioDown ? 'connected' : 'not_connected' }
  }

  /** A device as the API shows it: Bluetooth devices carry their live status and availability. */
  function live(d: DeviceDetail): DeviceDetail {
    if (d.provider.type !== 'ble_hid') return d
    const bluetooth = bleStatus(d.id)
    return { ...d, availability: bluetooth.state === 'connected' ? 'available' : 'unavailable', bluetooth }
  }

  /** The light form GET /devices lists: no profile content. */
  function light({ commands: _c, buttons: _b, power: _p, timing: _t, inputs: _i, channels: _ch, ...d }: DeviceDetail): Device {
    return d
  }

  /**
   * A Bluetooth device's capabilities, like the hub's: the seeded Bluetooth device's commands (all
   * keys, custom codes, wait_connected) with the remote group's `command` choices from its profile.
   */
  function bleCaps(d: DeviceDetail): Capabilities {
    const names = Object.keys(d.commands ?? {})
    const commands = bleTemplate.map((c) =>
      c.group === 'remote' ? { ...c, params: c.params.map((p) => (p.name === 'command' ? { ...p, options: names } : p)) } : c,
    )
    return { device_id: d.id, integration_id: d.integration_id, commands }
  }

  /** A new device's 201, in the detail form. */
  const created = (d: DeviceDetail): MockResponse => ({ status: 201, body: live(d), headers: { location: `/api/v1/devices/${d.id}` } })

  function newDeviceId(name: string) {
    let id = `dev_${slug(name)}`
    while (db.devices.some((d) => d.id === id)) id = `dev_${slug(name).slice(0, 20)}_${rand36(3)}`
    return id
  }

  /** POST /admin/devices {profile_id, name?, kind?}: the hub copies its profile (without `meta`) into the device. */
  function addFromProfile(b: DeviceRegisterProfile): MockResponse {
    const extra = Object.keys(b).filter((k) => !['profile_id', 'name', 'kind'].includes(k))
    if (extra.length) return invalid(unknownFields(extra))
    const p = db.profiles.find((x) => x.id === b.profile_id)
    if (!p) return invalid([{ path: '/profile_id', code: 'unknown_profile', message: `There's no profile ${b.profile_id}` }])
    if (b.name !== undefined && !b.name.trim()) return nameRequired()
    if (!kindKnown(b.kind)) return unknownKind()
    if (db.devices.length >= 64) return deviceLimit()
    if (db.devices.filter((d) => d.provider.type === 'ble_hid').length >= BLE_MAX_DEVICES) return fixtureError(422, 'errors/too_many_bluetooth_devices.json')
    const { schema_version: _v, id: profileId, revision: profileRevision, source: _s, manufacturer, model, kind, platform: _p, ...content } = p
    const name = (b.name ?? model).trim()
    const d: DeviceDetail = {
      id: newDeviceId(name),
      name,
      integration_id: 'ble',
      kind: b.kind ?? kind,
      provider: { type: 'ble_hid' },
      platform: 'ble_hid',
      manufacturer,
      model,
      profile: { id: profileId, revision: profileRevision, sha256: profileSha(profileId) },
      availability: 'available',
      schema_version: 1,
      revision: commit(),
      ...content,
    }
    db.devices.push(d)
    db.capabilities.set(d.id, bleCaps(d))
    return created(d)
  }

  /** A built-in profile's digest, as GET /profiles lists it. */
  const profileSha = (id: string) => db.profileList.find((x) => x.id === id)?.sha256 ?? ''

  const kindKnown = (k: string | undefined) => k === undefined || db.kinds.some((x) => x.id === k)
  const unknownKind = () => invalid([{ path: '/kind', code: 'unknown_kind', message: 'Not a device kind (see GET /kinds)' }])

  /** 404 unless `id` is a Bluetooth device, in the hub's words; null when it is one. */
  function notBle(id: string): MockResponse | null {
    const d = db.devices.find((x) => x.id === id)
    if (!d) return err(404, 'not_found', 'No such device')
    return d.provider.type === 'ble_hid' ? null : err(404, 'not_found', `${d.name} isn't a Bluetooth device`)
  }

  function integrations() {
    return db.integrations.map((i) => {
      const c = db.configs.get(i.id)
      const state = integrationState(i.id)
      const detail = state === 'not_configured' ? 'Not set up' : i.type === 'ble_hid' ? (state === 'error' ? "Bluetooth isn't working" : undefined) : i.detail
      return { ...i, name: c?.name ?? i.name, enabled: c?.enabled ?? i.enabled, state, detail, device_count: db.devices.filter((d) => d.integration_id === i.id).length }
    })
  }

  function sessionId(req: MockRequest): string | null {
    const m = /(?:^|;\s*)hubbo_session=([^;]+)/.exec(req.headers.cookie ?? '')
    const sid = m?.[1] ?? (sessions.has(DEV_SID) ? DEV_SID : null)
    return sid && sessions.has(sid) ? sid : null
  }

  /** What the hub knows about its credentials; only `fresh` allows setup ("can't read" is never "doesn't exist"). */
  const authState = (): AuthState => mode.authState ?? (adminCreated ? 'ok' : 'fresh')

  function sessionBody(sid: string | null): Session {
    const common = { auth_mode: 'enforced', config_state: config.state, auth_state: authState() } as const
    return sid
      ? { authenticated: true, role: 'admin', username, expires_in_s: SESSION_S, setup_required: false, ...common }
      : { authenticated: false, role: null, setup_required: authState() === 'fresh', ...common }
  }

  function newSession(): [string, Session] {
    const sid = rand36(24)
    sessions.add(sid)
    sessions.delete(DEV_SID)
    return [sid, sessionBody(sid)]
  }

  const hubMs = (t: { startedAt: number; advancedMs: number }) => t.advancedMs + (opts.timeScale ?? 1 / 3) * (Date.now() - t.startedAt)
  /** Hub ms since the running update started. */
  const firmwareMs = () => (fw.state === 'writing' ? hubMs(fw) : 0)

  /** True while a factory reset restarts the hub; at its end everything is gone and the hub is fresh. */
  function erasing(): boolean {
    if (!erase) return false
    if (hubMs(erase) < ERASE_RESTART_MS) return true
    reset({ firstRun: true, requireAuth: mode.requireAuth }) // no admin, sessions, tokens, remote or config
    bleHosts = new Map() // erased with everything else
    return false
  }

  /**
   * Where a firmware update is, in hub time. The reboot's end is applied here: the new version
   * runs and every session is gone (they live in RAM), like the board.
   */
  function firmwarePhase(): 'idle' | 'writing' | 'rebooting' | 'down' | 'failed' {
    if (fw.state !== 'writing') return fw.state
    const ms = firmwareMs()
    if (ms < FW_WRITE_MS) return 'writing'
    if (fwFail) {
      fw = { state: 'failed', error: 'write_failed' }
      return 'failed'
    }
    if (ms < FW_WRITE_MS + FW_REBOOT_MS / 2) return 'rebooting'
    if (ms < FW_WRITE_MS + FW_REBOOT_MS) return 'down'
    if (!fwRevert) {
      db.firmware = { ...db.firmware, version: fw.version }
      db.status.hubbo.version = fw.version
    }
    sessions.clear()
    fw = { state: 'idle' }
    return 'idle'
  }

  function firmwareStatus(): FirmwareStatus {
    const phase = firmwarePhase()
    const state = phase === 'down' ? 'rebooting' : phase // (handle() answers nothing while down)
    const progress = state === 'writing' ? Math.floor((firmwareMs() * 100) / FW_WRITE_MS) : state === 'rebooting' ? 100 : 0
    return { ...db.firmware, update: { state, progress, error: fw.state === 'failed' ? fw.error : null } }
  }

  function status(): Status {
    const s = structuredClone(db.status)
    const jitter = (v: number) => Math.round(v * (0.85 + rand() * 0.3) * 10) / 10
    s.uptime_s = db.status.uptime_s + Math.round((Date.now() - bootedAt) / 1000)
    s.time = isoNow()
    s.active_activity_id = db.activeActivityId
    s.config = config
    s.auth_state = authState()
    s.integrations = integrations().map(({ id, type, state }) => ({ id, type, state }))
    s.remote.paired = pairing.paired
    if (endpoint) s.remote.channel = endpoint.channel
    s.remote.last_packet_age_ms = Date.now() - lastEventAt
    if (lastEvent) s.remote.last_event = { ...lastEvent, age_ms: Date.now() - lastEventAt }
    else if (s.remote.last_event) s.remote.last_event.age_ms = Date.now() - lastEventAt
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
    const v = validateActivity(body as ActivityInput, catalog())
    return v.valid ? null : invalid(v.errors)
  }

  /** Every place an action matching `hit` is used, in the hub's error-detail form. */
  function references(hit: (a: Action) => boolean, skipActivity?: string): ValidationIssue[] {
    const out: ValidationIssue[] = []
    for (const a of db.activities) {
      if (a.id === skipActivity) continue
      forEachAction(a, (x, rel) => hit(x) && out.push({ path: `/activities/${a.id}${rel}`, code: 'referenced', message: `Used by ${a.name}` }))
    }
    forEachAction(db.globalBindings, (x, rel) => hit(x) && out.push({ path: `/global_bindings${rel}`, code: 'referenced', message: 'Used by global buttons' }))
    return out
  }

  const revisionOf = (req: MockRequest) => Number(req.query.get('revision'))
  const requireRevision = (req: MockRequest) =>
    req.query.has('revision') ? null : err(422, 'validation_failed', 'Send ?revision=N (the revision you last read)', { details: [{ path: '/revision', code: 'required', message: 'Required field is missing' }] })

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
      username = b.username!
      password = b.password!
      const [sid, s] = newSession()
      return { status: 201, body: s, headers: { 'set-cookie': cookie(sid) } }
    }],
    ['POST', /^\/auth\/login$/, 'public', (req) => {
      if (!adminCreated) return err(409, 'setup_required', 'Create the admin account first')
      const b = req.body as { username?: string; password?: string }
      if (b?.username !== username || b.password !== password) return err(401, 'invalid_credentials', 'Wrong username or password')
      const [sid, s] = newSession()
      return { status: 200, body: s, headers: { 'set-cookie': cookie(sid) } }
    }],
    ['POST', /^\/auth\/logout$/, 'public', (req) => {
      const sid = sessionId(req)
      if (sid) sessions.delete(sid)
      return { status: 204, headers: { 'set-cookie': 'hubbo_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict' } }
    }],

    ['GET', /^\/status$/, 'token', () => ok(status())],
    ['GET', /^\/remote\/status$/, 'token', () => ok(status().remote)],
    ['GET', /^\/remote\/buttons$/, 'token', () => ok(db.buttons)],

    // profiles
    ['GET', /^\/profiles$/, 'admin', () => ok({ profiles: db.profileList })],
    ['GET', /^\/profiles\/([^/]+)$/, 'admin', (_r, m) => {
      const p = db.profiles.find((x) => x.id === m[1])
      return p ? ok(p) : err(404, 'not_found', `There's no profile ${m[1]}`)
    }],
    ['GET', /^\/kinds$/, 'admin', () => ok({ kinds: db.kinds })],

    // devices
    ['GET', /^\/devices$/, 'token', () => ok({ devices: db.devices.map((d) => light(live(d))) })],
    ['GET', /^\/devices\/([^/]+)$/, 'token', (_r, m) => {
      const d = db.devices.find((x) => x.id === m[1])
      return d ? ok(live(d)) : err(404, 'not_found', 'No such device')
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
      if (db.capabilities.get(d.id)?.commands.find((c) => c.command_id === a.command_id)?.effect === 'wait') return fixtureError(422, 'errors/sequence_only.json')
      const shape = activityShape({ name: 'x', startup: [a], shutdown: [], bindings: {} }, ['name', 'startup', 'shutdown', 'bindings'])
      const v = shape.length ? shape : validateActivity({ name: 'x', startup: [a], shutdown: [], bindings: {} }, catalog()).errors
      if (v.length) return invalid(v.map((e) => ({ ...e, path: e.path.replace(/^\/startup\/0/, '') })))
      if (integrationState(d.integration_id) !== 'connected') return err(503, 'integration_unavailable', `${d.integration_id} is not connected`)
      // Like the hub: a key for a host that isn't connected is sent nowhere.
      if (d.provider.type === 'ble_hid' && bleStatus(d.id).state !== 'connected') return ok({ status: 'failed', device_id: d.id, command_id: a.command_id, elapsed_ms: 0, message: 'adapter_unavailable' } satisfies ExecuteResult)
      // A stale device's entity is gone from HA: the call is made and fails (decided 2026-11-09, like hub-sim).
      if (d.availability === 'stale') return ok({ status: 'failed', device_id: d.id, command_id: a.command_id, elapsed_ms: 1, message: 'entity_not_found' } satisfies ExecuteResult)
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
      const b = req.body as ActivityInput
      let id = `act_${slug(b.name)}`
      while (db.activities.some((a) => a.id === id)) id = `act_${slug(b.name).slice(0, 20)}_${rand36(3)}`
      const act: Activity = { id, name: b.name, startup: b.startup, shutdown: b.shutdown, bindings: b.bindings, schema_version: 1, revision: commit() }
      db.activities.push(act)
      return { status: 201, body: act, headers: { location: `/api/v1/activities/${id}` } }
    }],
    ['POST', /^\/activities\/validate$/, 'token', (req) => {
      const shape = activityShape(req.body, ['id', 'name', 'startup', 'shutdown', 'bindings', 'schema_version', 'revision'])
      if (shape.length) return ok({ valid: false, errors: shape })
      const v = validateActivity(req.body as ActivityInput, catalog())
      return ok({ valid: v.valid, errors: v.errors })
    }],
    ['POST', /^\/activities\/off$/, 'token', () => {
      const cur = db.activities.find((a) => a.id === db.activeActivityId)
      db.activeActivityId = null
      const none = { status: 'none', queued_actions: 0 } as const
      const r: TransitionResult = { switched: !!cur, active_activity_id: null, previous_activity_id: cur?.id ?? null, generation: ++transitions, startup: none, shutdown: cur ? { status: 'queued', queued_actions: cur.shutdown.length } : none }
      return ok(r)
    }],
    ['GET', /^\/activities\/([^/]+)$/, 'token', (_r, m) => {
      const a = db.activities.find((x) => x.id === m[1])
      return a ? ok(a) : err(404, 'not_found', 'No such activity')
    }],
    ['PUT', /^\/activities\/([^/]+)$/, 'token', (req, m) => {
      const i = db.activities.findIndex((x) => x.id === m[1])
      if (i < 0) return err(404, 'not_found', 'No such activity')
      const cur = db.activities[i]!
      const b = req.body as ActivityInput & { id?: string; revision?: number }
      if (b?.revision === undefined) return invalid([{ path: '/revision', code: 'required', message: 'revision is required' }])
      if (b.id !== undefined && b.id !== cur.id) return invalid([{ path: '/id', code: 'id_mismatch', message: 'id does not match the URL' }])
      if (b.revision !== cur.revision) return err(409, 'revision_conflict', `Activity changed since revision ${b.revision}`, { current_revision: cur.revision })
      const bad = checkActivity(req.body, ['id', 'name', 'startup', 'shutdown', 'bindings', 'schema_version', 'revision'])
      if (bad) return bad
      const next: Activity = { ...cur, name: b.name, startup: b.startup, shutdown: b.shutdown, bindings: b.bindings, revision: commit() }
      db.activities[i] = next
      return ok(next)
    }],
    ['DELETE', /^\/activities\/([^/]+)$/, 'token', (req, m) => {
      const i = db.activities.findIndex((x) => x.id === m[1])
      if (i < 0) return err(404, 'not_found', 'No such activity')
      const missing = requireRevision(req)
      if (missing) return missing
      if (revisionOf(req) !== db.activities[i]!.revision) return err(409, 'revision_conflict', 'Activity changed since you loaded it', { current_revision: db.activities[i]!.revision })
      // Referenced by a system.activate binding elsewhere: refuse, listing where (as the hub does).
      const refs = references((x) => x.command_id === 'activate' && x.params.activity_id === m[1], m[1])
      if (refs.length) return err(409, 'activity_in_use', 'The activity is used elsewhere; remove those references first', { details: refs.slice(0, 32) })
      db.activities.splice(i, 1)
      // Deleting the active activity switches to the one now in its place (wrapping) and runs its startup.
      if (db.activeActivityId === m[1]) db.activeActivityId = db.activities.length ? db.activities[i % db.activities.length]!.id : null
      return ok()
    }],
    ['POST', /^\/activities\/([^/]+)\/activate$/, 'token', (_r, m) => {
      const a = db.activities.find((x) => x.id === m[1])
      if (!a) return err(404, 'not_found', 'No such activity')
      const prev = db.activeActivityId
      db.activeActivityId = a.id
      const startup = mode.startupRejected ? ({ status: 'rejected', queued_actions: 0, reason: 'queue_full' } as const) : ({ status: 'queued', queued_actions: a.startup.length } as const)
      const r: TransitionResult = { switched: true, active_activity_id: a.id, previous_activity_id: prev, generation: ++transitions, startup, shutdown: { status: 'none', queued_actions: 0 } }
      return ok(r)
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
      db.globalBindings = { bindings: b.bindings, revision: commit() }
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
        if (s.access_token !== undefined) mut(next.settings).has_access_token = s.access_token !== null
      } else if (s?.type === 'ble_hid' && next.settings.type === 'ble_hid') {
        if (s.device_name !== undefined) next.settings.device_name = s.device_name
      } else if (s) return invalid([{ path: '/settings/type', code: 'wrong_type', message: `Settings type must be ${next.settings.type}` }])
      next.revision = commit()
      db.configs.set(c.id, next)
      if (next.settings.type === 'home_assistant') {
        details = null
        haOverride = next.settings.has_access_token ? ({ ok: null, reject: 'auth_failed', stall: 'connecting' } as const)[haNextConnect] : null // the client reconnects with the new settings
      }
      return ok(next)
    }],
    ['POST', /^\/admin\/integrations\/([^/]+)\/discover$/, 'admin', (_r, m) => {
      if (m[1] !== 'ha_main') return err(404, 'not_found', 'This integration has nothing to discover')
      if (integrationState('ha_main') !== 'connected') return err(503, 'integration_unavailable', 'Home Assistant is not connected')
      if (discovery.state !== 'running') discovery = { state: 'running', startedAt: Date.now() }
      return ok(fixture<DiscoverStatus>('integrations/ha_main.discover.json'), 202)
    }],
    ['GET', /^\/admin\/integrations\/([^/]+)\/discover$/, 'admin', (req, m) => (m[1] !== 'ha_main' ? err(404, 'not_found', 'This integration has nothing to discover') : discoverStatus(req.query))],
    ['DELETE', /^\/admin\/integrations\/([^/]+)\/discover$/, 'admin', (_r, m) => {
      if (m[1] !== 'ha_main') return err(404, 'not_found', 'This integration has nothing to discover')
      discovery = { state: 'idle' }
      return ok()
    }],
    ['POST', /^\/admin\/integrations\/([^/]+)\/entities\/([^/]+)\/details$/, 'admin', (_r, m) => {
      if (m[1] !== 'ha_main') return err(404, 'not_found', 'Only Home Assistant has entities')
      const entityId = decodeURIComponent(m[2]!)
      if (!haEntityIdOk(entityId)) return invalid([{ path: '/entity_id', code: 'pattern', message: 'Not a media_player, light or switch entity id' }])
      if (integrationState('ha_main') !== 'connected') return err(503, 'integration_unavailable', 'Home Assistant is not connected')
      // One at a time: this replaces any earlier request.
      details = { entityId, startedAt: Date.now(), state: 'running' }
      return ok({ state: 'running', entity_id: entityId } satisfies EntityDetailsStatus, 202)
    }],
    ['GET', /^\/admin\/integrations\/([^/]+)\/entities\/([^/]+)\/details$/, 'admin', (_r, m) => {
      if (m[1] !== 'ha_main') return err(404, 'not_found', 'Only Home Assistant has entities')
      return ok(entityDetails(decodeURIComponent(m[2]!)))
    }],
    // Bluetooth pairing, per device: one window at a time; its first new bond becomes that device's host.
    ['GET', /^\/admin\/devices\/([^/]+)\/pair$/, 'admin', (_r, m) => (notBle(m[1]!) ?? ok(bleStatus(m[1]!)))],
    ['POST', /^\/admin\/devices\/([^/]+)\/pair$/, 'admin', (req, m) => {
      const no = notBle(m[1]!)
      if (no) return no
      const raw = req.query.get('window_s')
      const windowS = raw === null ? BLE_PAIR_WINDOW_S : /^\d+$/.test(raw) ? Number(raw) : NaN
      if (!(windowS >= BLE_PAIR_WINDOW_MIN_S && windowS <= BLE_PAIR_WINDOW_MAX_S)) return invalid([{ path: '/window_s', code: 'param_out_of_range', message: `window_s must be ${BLE_PAIR_WINDOW_MIN_S} to ${BLE_PAIR_WINDOW_MAX_S}` }])
      if (integrationState('ble') !== 'connected') return fixtureError(503, 'errors/bluetooth_unavailable.json')
      const open = bleWindow()
      if (open?.device === m[1]) return ok(bleStatus(m[1]!), 202) // already its window
      if (open) return fixtureError(409, 'errors/pairing_busy.json')
      blePair = { device: m[1]!, endsAt: Date.now() + windowS * 1000 }
      return ok(bleStatus(m[1]!), 202)
    }],
    ['DELETE', /^\/admin\/devices\/([^/]+)\/pair$/, 'admin', (_r, m) => {
      const no = notBle(m[1]!)
      if (no) return no
      if (blePair?.device === m[1]) blePair = null // the old host, if any, stays
      return { status: 204 }
    }],
    ['DELETE', /^\/admin\/devices\/([^/]+)\/pairing$/, 'admin', (_r, m) => {
      const no = notBle(m[1]!)
      if (no) return no
      bleHosts.delete(m[1]!)
      return { status: 204 }
    }],

    ['POST', /^\/admin\/devices$/, 'admin', (req) => {
      if (req.body && typeof req.body === 'object' && 'profile_id' in req.body) return addFromProfile(req.body as DeviceRegisterProfile)
      const b = req.body as Extract<DeviceRegister, { provider: unknown }>
      // Like the hub: only DeviceRegister's fields (a client-sent `ha_capabilities` is refused).
      const extra = Object.keys(b ?? {}).filter((k) => !['name', 'integration_id', 'kind', 'provider'].includes(k))
      if (extra.length) return invalid(unknownFields(extra))
      if (!b?.name?.trim()) return nameRequired()
      if (!kindKnown(b.kind)) return unknownKind()
      if (db.devices.length >= 64) return deviceLimit()
      const p = b.provider
      // Only Home Assistant entities come this way; Bluetooth devices come from a profile.
      if (p?.type !== 'home_assistant') return invalid([{ path: '/provider', code: 'integration_mismatch', message: 'Provider does not belong to that integration' }])
      if (integrationState('ha_main') === 'connected' && b.integration_id !== 'ha_main') return invalid([{ path: '/integration_id', code: 'integration_mismatch', message: 'Home Assistant entities belong to ha_main' }])
      const hc = haEntity(p.entity_id)
      if ('status' in hc) return hc
      const d: DeviceDetail = { id: newDeviceId(b.name), name: b.name.trim(), integration_id: b.integration_id, kind: b.kind ?? 'other', provider: p, platform: 'home_assistant', availability: 'available', schema_version: 1, revision: commit() }
      db.devices.push(d)
      const cand = db.discover.candidates.find((x) => x.provider.entity_id === p.entity_id)
      if (cand) cand.registered_device_id = d.id
      db.capabilities.set(d.id, { device_id: d.id, integration_id: d.integration_id, commands: haCommands(hc) })
      return created(d)
    }],
    ['PUT', /^\/admin\/devices\/([^/]+)$/, 'admin', (req, m) => {
      const i = db.devices.findIndex((x) => x.id === m[1])
      if (i < 0) return err(404, 'not_found', 'No such device')
      const d = db.devices[i]!
      const b = req.body as DeviceUpdate
      // Like the hub: only DeviceUpdate's fields (e.g. `ha_capabilities` is never accepted back).
      const extra = Object.keys(b ?? {}).filter((k) => !['name', 'kind', 'provider', 'revision'].includes(k))
      if (extra.length) return invalid(unknownFields(extra))
      if (b?.revision !== d.revision) return err(409, 'revision_conflict', `Device changed since revision ${b?.revision}`, { current_revision: d.revision })
      if (b.name !== undefined && !b.name.trim()) return nameRequired()
      if (!kindKnown(b.kind)) return unknownKind()
      let availability = d.availability
      if (b.provider) {
        // Re-link: HA re-reads the entity (even an unchanged provider) and replaces the stored capabilities.
        if (b.provider.type !== d.provider.type) return invalid([{ path: '/provider/type', code: 'integration_mismatch', message: 'A re-link must stay in the same integration' }])
        const p = b.provider
        if (p.type === 'home_assistant') {
          const c = haEntity(p.entity_id, d.id)
          if ('status' in c) return c
          // Like the hub: the whole config must stay valid with the entity's commands (paths by activity index).
          const commands = haCommands(c)
          const cat = catalog()
          cat.capabilities = new Map(cat.capabilities).set(d.id, { device_id: d.id, integration_id: d.integration_id, commands })
          const broken = db.activities.flatMap((a, ai) => validateActivity(a, cat).errors.map((x) => ({ ...x, path: `/activities/${ai}${x.path}` })))
          const global: ValidationIssue[] = []
          validateBindings(db.globalBindings.bindings, cat, global, [])
          // The hub's shape: /global_bindings/<trigger>/<i>/… (no /bindings level).
          broken.push(...global.map((x) => ({ ...x, path: `/global_bindings${x.path.replace(/^\/bindings/, '')}` })))
          if (broken.length) return invalid(broken, 'The device is invalid')
          db.discover.candidates.forEach((x) => x.registered_device_id === d.id && (x.registered_device_id = null))
          const cand = haCandidate(p.entity_id)
          if (cand) cand.registered_device_id = d.id
          db.capabilities.set(d.id, { device_id: d.id, integration_id: d.integration_id, commands })
          availability = 'available'
        }
      }
      db.devices[i] = { ...d, availability, ...(b.name ? { name: b.name.trim() } : {}), ...(b.kind ? { kind: b.kind } : {}), ...(b.provider ? { provider: b.provider } : {}), revision: commit() }
      return ok(live(db.devices[i]!))
    }],
    ['DELETE', /^\/admin\/devices\/([^/]+)$/, 'admin', (req, m) => {
      const d = db.devices.find((x) => x.id === m[1])
      if (!d) return err(404, 'not_found', 'No such device')
      const missing = requireRevision(req)
      if (missing) return missing
      if (revisionOf(req) !== d.revision) return err(409, 'revision_conflict', 'Device changed since you loaded it', { current_revision: d.revision })
      const refs = references((x) => x.device_id === d.id)
      if (refs.length) return err(409, 'device_in_use', 'The device is used elsewhere; remove those references first', { details: refs.slice(0, 32) })
      db.devices = db.devices.filter((x) => x.id !== d.id)
      pruneBonds() // deleting a device forgets its host
      db.capabilities.delete(d.id)
      db.discover.candidates.forEach((c) => c.registered_device_id === d.id && (c.registered_device_id = null))
      return ok()
    }],

    // remote pairing
    ['GET', /^\/admin\/remote\/pair$/, 'admin', () => ok(pairStatus())],
    ['POST', /^\/admin\/remote\/pair$/, 'admin', () => {
      if (pairStatus().state === 'scanning') return err(409, 'busy', 'A scan is already running')
      pairing = { state: 'scanning', paired: !!endpoint, remaining_s: REMOTE_SCAN_TIMEOUT_S, channel: 0, message: 'Press the pair button on the Harmony Hub', startedAt: Date.now() }
      return ok(pairStatus(), 202)
    }],
    ['DELETE', /^\/admin\/remote\/pair$/, 'admin', () => {
      if (pairStatus().state === 'scanning') pairing = { state: 'stopped', paired: !!endpoint, ...(endpoint ? { channel: endpoint.channel } : {}) }
      return ok()
    }],
    ['GET', /^\/admin\/remote$/, 'admin', () => ok(remoteConfig())],
    ['PUT', /^\/admin\/remote\/endpoint$/, 'admin', (req) => {
      const b = (req.body ?? {}) as Partial<RemoteEndpointUpdate>
      const issues: ValidationIssue[] = []
      if (typeof b.channel !== 'number' || !Number.isInteger(b.channel) || b.channel < 0 || b.channel > REMOTE_CHANNEL_MAX) issues.push({ path: '/channel', code: 'param_out_of_range', message: `Channel must be 0 to ${REMOTE_CHANNEL_MAX}` })
      // Like the console's `radio set`: up to 40 bits (no byte separators), then the shared address rules.
      const hex = typeof b.address === 'string' && /^(0[xX])?[0-9A-Fa-f]{1,10}$/.test(b.address) ? b.address.replace(/^0x/i, '').toUpperCase().padStart(10, '0') : null
      if (!hex) issues.push({ path: '/address', code: 'pattern', message: 'Address must be up to 10 hex digits (40 bits)' })
      else if (addressProblem(hex)) issues.push({ path: '/address', code: 'pattern', message: addressProblem(hex)! })
      if (issues.length) return invalid(issues)
      if (pairStatus().state === 'scanning') return err(409, 'busy', 'A scan is running; stop it first')
      endpoint = { channel: b.channel!, address: hex! }
      pairing = { state: 'idle', paired: true, channel: endpoint.channel }
      return ok(remoteConfig())
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
      if (!name) return nameRequired()
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
      const date = isoNow().slice(0, 10)
      // Never a backup that might be empty or out of date.
      // Only a hub that couldn't read storage when it loaded has nothing trustworthy to export. Storage that
      // failed after a good load or commit still backs up the running config: the last acknowledged save.
      if (config.state === 'storage_unavailable' && loadUnreadable) return fixtureError(503, 'errors/backup_storage_unavailable.json')
      if (config.state === 'blocked') {
        // Nothing runs, so export the saved record exactly as stored, clearly marked.
        if (!blockedRecord) return fixtureError(409, 'errors/backup_blocked_damaged.json')
        return {
          status: 200,
          body: blockedRecord,
          headers: { 'content-type': 'application/octet-stream', 'x-hubbo-backup': 'blocked-raw', 'content-disposition': `attachment; filename="hubbo-backup-BLOCKED-${date}.json"` },
        }
      }
      const b: Backup = {
        ...db.backup,
        created_at: isoNow(),
        devices: db.devices.map(({ availability: _a, platform: _p, ...stored }) => stored), // StoredDevice: no live state, and platform is provider.type
        activities: db.activities,
        global_bindings: db.globalBindings,
        integrations: [...db.configs.values()].map((c) => ({
          id: c.id,
          type: c.type,
          name: c.name,
          enabled: c.enabled,
          settings: c.settings.type === 'home_assistant' ? { url: c.settings.url, verify_tls: c.settings.verify_tls } : c.settings.type === 'ble_hid' ? { device_name: c.settings.device_name } : {},
        })),
      }
      return { status: 200, body: b, headers: { 'x-hubbo-backup': 'config', 'content-disposition': `attachment; filename="hubbo-backup-${date}.json"` } }
    }],
    ['POST', /^\/admin\/restore$/, 'admin', (req) => {
      // A Backup, or a saved config record exported while blocked (blocked-raw). Only the top-level `format` decides.
      const body = req.body as { format?: unknown; schema_version?: unknown; format_version?: unknown } | null
      const record = body?.format === 'hubbo-config'
      if (record) {
        const v = body.schema_version
        if (typeof v !== 'number' || !Number.isInteger(v) || v < 0) return invalid([{ path: '/schema_version', code: 'wrong_type', message: 'schema_version must be a whole number' }])
        if (v !== CONFIG_SCHEMA) return fixtureError(422, 'errors/restore_unsupported_schema.json')
      } else {
        if (body?.format !== 'hubbo-backup') return invalid([{ path: '/format', code: 'wrong_type', message: 'This is not a Hubbo backup file' }])
        if (typeof body.format_version === 'number' && body.format_version > 1) return invalid([{ path: '/format_version', code: 'unsupported_schema', message: 'This backup is from newer firmware; update first' }])
        if (body.format_version !== 1) return invalid([{ path: '/format_version', code: 'wrong_type', message: `Backup format ${String(body.format_version)} is not supported` }])
      }
      const b = req.body as Partial<Backup>
      if (!Array.isArray(b.devices) || !Array.isArray(b.activities)) return invalid([{ path: '', code: 'required', message: 'Backup is missing devices or activities' }])
      // Runtime device fields are never stored, so a backup can't carry them (like the hub).
      const runtime = b.devices.flatMap((d, i) => unknownFields(['availability', 'bluetooth'].filter((k) => k in d), `/devices/${i}`))
      if (runtime.length) return invalid(runtime)
      const dry = req.query.get('dry_run') === 'true'
      const result = { dry_run: dry, devices: b.devices.length, activities: b.activities.length, integrations: b.integrations?.length ?? 0, migrated_from: null, requires: ['home_assistant_token', 'ble_pairing', 'api_tokens'], warnings: [] }
      if (!dry) {
        // Revisions in the backup may be missing or 0; stampAll() replaces them all.
        // A backup's StoredDevice carries ha_capabilities; a Device (what the API returns) never does.
        db.devices = b.devices.map(({ ha_capabilities: _, ...d }) => ({ ...d, platform: d.provider.type, availability: 'unknown' as const, revision: 0 }))
        // A Bluetooth device's commands come from the profile content it carries.
        for (const d of db.devices) if (d.provider.type === 'ble_hid') db.capabilities.set(d.id, bleCaps(d))
        pruneBonds() // a restored device keeps its bond on this hub; others are dropped
        db.activities = b.activities.map((a) => ({ ...a, schema_version: 1 as const, revision: 0 }))
        db.globalBindings = { bindings: b.global_bindings?.bindings ?? {}, revision: 0 }
        // Integration settings come back without secrets (a backup never has the HA token). Like the
        // hub, the stored token survives only when the URL stays the same.
        for (const i of b.integrations ?? []) {
          const c = db.configs.get(i.id)
          if (!c || c.settings.type !== 'home_assistant' || i.type !== 'home_assistant') continue
          const s = (i.settings ?? {}) as { url?: string; verify_tls?: boolean }
          const url = s.url ?? ''
          db.configs.set(i.id, { ...c, name: i.name ?? c.name, enabled: i.enabled ?? c.enabled, settings: { type: 'home_assistant', url, verify_tls: s.verify_tls ?? false, has_access_token: c.settings.has_access_token && url === c.settings.url } })
          haOverride = null // the client reconnects with the restored settings
        }
        stampAll()
      }
      return ok(result)
    }],
    ['POST', /^\/admin\/reset$/, 'admin', () => {
      factoryDefaults()
      pruneBonds()
      stampAll()
      return ok(config)
    }],
    ['POST', /^\/admin\/factory-reset$/, 'admin', () => {
      const update = firmwarePhase()
      if (update === 'writing' || update === 'rebooting') return fixtureError(409, 'errors/factory_reset_during_update.json')
      if (!eraseReady) return err(503, 'unavailable', "The hub can't do a factory reset right now")
      erase = { startedAt: Date.now(), advancedMs: 0 }
      return { status: 202 }
    }],
    ['GET', /^\/admin\/firmware$/, 'admin', () => ok(firmwareStatus())],
    // In the hub's order: size and type (the router), Content-Length, busy, ready, then the image checks.
    ['POST', /^\/admin\/firmware\/update$/, 'admin', async (req) => {
      const image = req.rawBody ?? new Uint8Array()
      if (image.byteLength > FW_SLOT_BYTES) return err(413, 'payload_too_large', 'The image is larger than the update slot')
      if (!/^application\/octet-stream\b/i.test(req.headers['content-type'] ?? '')) return err(415, 'unsupported_media_type', 'Send the image as application/octet-stream')
      if (req.headers['content-length'] === undefined) return fixtureError(411, 'errors/length_required.json')
      if (fw.state === 'writing') return fixtureError(409, 'errors/update_in_progress.json')
      if (!fwReady) return err(503, 'unavailable', 'The hub can’t take an update right now')
      const checked = await checkFirmwareImage(image, db.firmware.board)
      if ('code' in checked) return err(422, checked.code, checked.message)
      fw = { state: 'writing', startedAt: Date.now(), advancedMs: 0, version: checked.version }
      return ok(firmwareStatus(), 202)
    }],

    // Mock-only controls (never in the real API).
    // Like hub-sim's POST /sim/firmware: writes fail (?fail=1) or the hub can't take an update (?ready=0).
    // Mock-only ?revert=1: the reboot comes back on the old image (a reset mid-write, or a new image that crashes).
    ['POST', /^\/__mock\/factory-reset$/, 'public', (req) => {
      if (req.query.has('ready')) eraseReady = req.query.get('ready') === '1'
      return ok({ ready: eraseReady })
    }],
    ['POST', /^\/__mock\/firmware$/, 'public', (req) => {
      if (req.query.has('fail')) fwFail = req.query.get('fail') === '1'
      if (req.query.has('ready')) fwReady = req.query.get('ready') === '1'
      if (req.query.has('revert')) fwRevert = req.query.get('revert') === '1'
      return ok({ fail: fwFail, ready: fwReady, revert: fwRevert })
    }],
    ['POST', /^\/__mock\/reset$/, 'public', (req) => (reset((req.body ?? {}) as ResetFlags), ok())],
    // Like hub-sim's /sim/radio/found and /sim/advance: a Hub answers the scan; time moves on.
    ['POST', /^\/__mock\/radio\/found$/, 'public', (req) => {
      const channel = Number(req.query.get('channel'))
      const address = req.query.get('address')?.toUpperCase() ?? ''
      if (!Number.isInteger(channel) || channel < 0 || channel > REMOTE_CHANNEL_MAX || !/^[0-9A-F]{10}$/.test(address)) return invalid([{ path: '/channel', code: 'wrong_type', message: 'need channel=0..124 and a 10-digit hex address' }])
      if (pairStatus().state !== 'scanning') return err(409, 'busy', 'No scan is running')
      endpoint = { channel, address }
      pairing = { state: 'found', paired: true, channel }
      return ok()
    }],
    ['POST', /^\/__mock\/advance$/, 'public', (req) => {
      const ms = Number(req.query.get('ms'))
      if (!(ms >= 0)) return invalid([{ path: '/ms', code: 'wrong_type', message: 'ms must be a number' }])
      if (pairing.startedAt !== undefined) pairing.startedAt -= ms
      if (blePair) blePair.endsAt -= ms
      if (details) details.startedAt -= ms
      if (discovery.startedAt !== undefined) discovery.startedAt -= ms
      if (fw.state === 'writing') fw.advancedMs += ms
      if (erase) erase.advancedMs += ms
      return ok()
    }],
    // The HA client's state, as the fake HA server would drive it (cleared by the next settings change).
    // Like hub-sim's /sim/ha: ?remove=<entity_id> (HA forgets it), ?auth=reject|ok and
    // ?connect=stall|ok (how the next connect after a save goes; reachable= is accepted and ignored), or ?state=<client state>.
    // Mock-only: ?rename=<entity_id>&to=<new id>, an entity renamed in HA (its old id is gone).
    ['POST', /^\/__mock\/ha$/, 'public', (req) => {
      const remove = req.query.get('remove') ?? req.query.get('rename')
      if (remove) {
        const to = req.query.get('to')
        removedEntities.add(remove)
        if (to) {
          // The same entity under a new id: its commands, services and device link move with it.
          const own = entityCommands.get(remove)
          if (own) entityCommands.set(to, own)
          const extras = entityExtras.get(remove)
          if (extras) entityExtras.set(to, extras)
          renamedFrom.set(to, remove)
        }
        db.discover.candidates = db.discover.candidates.flatMap((c) => (c.provider.entity_id !== remove ? [c] : to ? [{ ...c, provider: { ...c.provider, entity_id: to }, registered_device_id: null }] : []))
        return ok()
      }
      const auth = req.query.get('auth')
      const connect = req.query.get('connect')
      if (auth || connect) {
        haNextConnect = connect === 'stall' ? 'stall' : auth === 'reject' ? 'reject' : 'ok'
        return ok()
      }
      const st = req.query.get('state') as IntegrationState | null
      if (!st || !['connecting', 'authenticating', 'connected', 'auth_failed', 'unreachable'].includes(st)) return invalid([{ path: '/state', code: 'wrong_type', message: 'state must be connecting, authenticating, connected, auth_failed or unreachable' }])
      haOverride = st === 'connected' ? null : st
      return ok()
    }],
    // Like hub-sim's /sim/remote/<button>/<press|release|tap>, but it only records the packet (the
    // mock doesn't run bindings): "last press" becomes now.
    ['POST', /^\/__mock\/remote\/([a-z0-9_]+)\/(press|release|tap)$/, 'public', (_req, m) => {
      const action = m[2]
      lastEvent = { button: m[1] as ButtonId, kind: action === 'press' ? 'down' : action === 'release' ? 'up' : 'tap' }
      lastEventAt = Date.now()
      return ok()
    }],
    // Like hub-sim's fake Bluetooth hosts (/sim/ble/...): a host bonds in that device's open window
    // and connects; a paired host drops or comes back; the radio goes down or comes back.
    ['POST', /^\/__mock\/ble\/bond$/, 'public', (req) => {
      const id = req.query.get('device') ?? ''
      if (bleWindow()?.device !== id) return err(409, 'busy', 'That device has no pairing window open')
      bleHosts.set(id, { connected: true })
      blePair = null
      return ok()
    }],
    ['POST', /^\/__mock\/ble\/link$/, 'public', (req) => {
      const h = bleHosts.get(req.query.get('device') ?? '')
      const up = req.query.get('up')
      if (!h) return err(404, 'not_found', 'That device has no host')
      if (up !== '0' && up !== '1') return invalid([{ path: '/up', code: 'wrong_type', message: 'up must be 0 or 1' }])
      h.connected = up === '1'
      return ok({ up: h.connected })
    }],
    ['POST', /^\/__mock\/ble\/radio$/, 'public', (req) => {
      const v = req.query.get('ready')
      if (v !== '0' && v !== '1') return invalid([{ path: '/ready', code: 'wrong_type', message: 'ready must be 0 or 1' }])
      bleRadioDown = v === '0'
      if (bleRadioDown) blePair = null
      return ok({ ready: !bleRadioDown })
    }],
    // Like hub-sim's POST /sim/storage?health=…: the store goes down or comes back at runtime.
    ['POST', /^\/__mock\/storage$/, 'public', (req) => {
      const health = req.query.get('health')
      if (health === 'available') config = { state: 'ok', generation: configGen }
      else if (health === 'unavailable' || health === 'read_only') config = { state: 'storage_unavailable', generation: configGen, reason: health === 'read_only' ? 'Storage is read-only' : 'Storage could not be written' }
      else return invalid([{ path: '/health', code: 'wrong_type', message: 'health must be available, unavailable or read_only' }])
      return ok()
    }],
  ]

  async function handle(req: MockRequest): Promise<MockResponse> {
    const base = opts.latencyMs ?? 0
    if (base > 0) await new Promise((r) => setTimeout(r, base * (0.5 + rand())))
    if ((firmwarePhase() === 'down' || erasing()) && !req.path.startsWith('/__mock/')) return { status: 0 }
    // Per-route limits (x-hubbo-max-body): restore takes a whole config; firmware images are streamed.
    const limit = req.path.startsWith('/admin/restore') ? RESTORE_MAX_BODY : MAX_BODY
    const firmware = req.path.startsWith('/admin/firmware')
    if (req.rawBody && req.rawBody.byteLength > limit && !firmware) {
      return err(413, 'payload_too_large', `Request bodies are limited to ${limit / 1024} KiB`)
    }
    // The Node adapter leaves parsing to here, after the size check (unit tests pass `body` already parsed).
    if (req.body === undefined && req.rawBody?.byteLength && !firmware) {
      if (!/^application\/json\b/i.test(req.headers['content-type'] ?? '')) return err(415, 'unsupported_media_type', 'Send the body as application/json')
      try {
        req = { ...req, body: JSON.parse(new TextDecoder().decode(req.rawBody)) }
      } catch {
        return err(400, 'malformed_json', 'Body is not valid JSON')
      }
      const max = req.path === '/admin/restore' ? MAX_FILE_DEPTH : MAX_JSON_DEPTH
      if (depth(req.body) > max) return err(400, 'malformed_json', `JSON nesting is limited to ${max} levels`)
    }
    const hit = routes.find(([method, re]) => method === req.method && re.test(req.path))
    if (!hit) {
      const known = routes.some(([, re]) => re.test(req.path))
      return known ? err(405, 'method_not_allowed', `${req.method} is not allowed here`) : err(404, 'not_found', `No route for ${req.method} ${req.path}`)
    }
    const [, re, role, fn] = hit
    const sid = sessionId(req)
    // Credentials the hub can't read: no logins, no setup, no authenticated requests.
    if (authState() === 'unavailable' && (role !== 'public' || /^\/auth\/login$/.test(req.path))) {
      return err(503, 'storage_unavailable', 'The saved login data can’t be read; restart the hub')
    }
    if (role === 'setup' && adminCreated) return err(409, 'setup_already_done', 'An admin account already exists')
    // Like the hub: a browser Origin on an unsafe request must be this hub (with SameSite=Strict
    // and JSON-only bodies, the whole cross-site defence; there's no CSRF token).
    const origin = req.headers.origin
    if (req.method !== 'GET' && origin && req.headers.host && new URL(origin).host !== req.headers.host) {
      return err(403, 'csrf_failed', 'Cross-site request refused')
    }
    if (role === 'token' || role === 'admin') {
      if (!adminCreated) return err(401, 'unauthenticated', 'Finish first-run setup first') // like the hub: setup_required is only a session field
      if (!sid) return err(401, 'unauthenticated', 'Sign in to continue')
    }
    // Config state: a blocked config refuses every edit (and switching, since nothing is compiled);
    // unwritable storage refuses edits. Restore and reset are the way out.
    if (req.method !== 'GET' && EDITS.some((e) => e.test(req.path))) {
      if (config.state === 'blocked') return err(409, 'config_blocked', 'The saved configuration can’t be used; restore a backup or reset')
      if (config.state === 'storage_unavailable' && !/\/(activate|off)$/.test(req.path)) return err(503, 'storage_unavailable', 'The configuration store can’t be written')
    }
    const m = re.exec(req.path)!.map((s) => (s === undefined ? '' : decodeURIComponent(s)))
    return fn(req, m)
  }

  return { handle, reset }
}
