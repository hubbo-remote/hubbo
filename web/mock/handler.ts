// In-memory /api/v1 for UI development, following api/openapi.yaml and
// seeded from api/fixtures. Includes auth, the Origin check, optimistic concurrency,
// validation errors and slow/failing adapters. NOT behavioural truth: that
// is hub-sim running the real C++ core.
import type { operations } from '../src/api/schema.gen.ts'
import type {
  Action,
  MacroRunAccepted,
  MacroRunRequest,
  Activity,
  ActivityInput,
  AuthState,
  Backup,
  Command,
  CustomService,
  HaServiceCall,
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
  SlotId,
  SmartButtons,
  ActivityRun,
  ActivityRuns,
  ActivityRunStep,
  SmartButtonsInput,
  Hub,
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
  FirmwareRelease,
  FirmwareStatus,
} from '../src/api/types.ts'
import { BLE_MAX_DEVICES, BLE_PAIR_WINDOW_MAX_S, BLE_PAIR_WINDOW_MIN_S, BLE_PAIR_WINDOW_S, REMOTE_CHANNEL_MAX, REMOTE_SCAN_TIMEOUT_S } from '../src/api/constants.ts'
import { bindsButton, forEachAction, parseTrigger, triggerKey, validateActivity, validateBindings, type Catalog } from '../src/lib/activity.ts'
import { CALL_DATA_MAX_DEPTH, CALL_SERVICE, depth } from '../src/lib/haCall.ts'
import { addressProblem } from '../src/lib/remote.ts'
import { GUIDE_PATH, loadDocs, sendDoc } from './docs.ts'
import { checkFirmwareImage } from './firmware.ts'
import { fixture, fixtureBytes, loadSeed, type Seed } from './fixtures.ts'
import { activityShape, bindingsShape, DEVICE_ID, macrosShape, smartButtonsShape, unknownFields } from './shape.ts'
import { emptySmartButtons, isSlot, SLOTS, validateSmartButtons } from '../src/lib/smartButtons.ts'
import { failedSteps } from '../src/lib/runs.ts'
import { noActions, slotActions } from './smartActions.ts'

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

/** `a:b:c` → [`a`, `b:c`]: an entity id never holds the separator, a value may. */
const splitOnce = (s: string, sep: string): [string, string] => {
  const i = s.indexOf(sep)
  return i < 0 ? [s, ''] : [s.slice(0, i), s.slice(i + 1)]
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
/** Like hub-sim's SimFirmwareReleases: a check takes 0.5 s; installing a release downloads it for 1 s first. */
const FW_CHECK_MS = 500
const FW_DOWNLOAD_MS = 1000
type CheckError = NonNullable<FirmwareStatus['check_error']>
/** This hub's partition layout (the image marker's `layout`). */
const HUB_LAYOUT = 1
/** The release the mock's feed publishes, until `/__mock/release` changes it: the API fixture's. */
const defaultRelease = () => fixture<FirmwareStatus>('admin/firmware.available.json').available!
/** Like core's firmware_release: a release is a plain X.Y.Z; anything else (a development build) is never older. */
const RELEASE = /^(\d+)\.(\d+)\.(\d+)$/
function isNewer(candidate: string, running: string): boolean {
  const a = RELEASE.exec(candidate), b = RELEASE.exec(running)
  if (!a || !b) return false
  for (let i = 1; i <= 3; i++) if (Number(a[i]) !== Number(b[i])) return Number(a[i]) > Number(b[i])
  return false
}
const FW_REBOOT_MS = 1000
/** The OTA slot (POST /admin/firmware/update x-hubbo-max-body). */
const FW_SLOT_BYTES = 6_553_600
/** A restart (POST /admin/restart, or a factory reset's), like hub-sim's: the mock's hub is down for all of it. */
const RESTART_MS = 1000
/** A new hub's name, as hub-sim's (its fixed MAC suffix; the board's is its own MAC's). */
const DEFAULT_HUB_NAME = 'Hubbo 534D'
/** The skip reasons whose services a Home Assistant action can still send (core's snapshot). */
const CALLABLE_SKIPS = new Set(['unsupported_field', 'too_many_fields', 'too_many_services'])
/** The MQTT bridge's detail lines, by state (core's MqttBridge::detail; none while connected or connecting). */
const MQTT_DETAIL: Partial<Record<IntegrationState, string>> = { auth_failed: 'The broker refused the username or password', unreachable: "Can't reach the broker" }
/** A restored broker that needs its password again (core's rule: it has a host, enabled or not). */
const needsMqttPassword = (i: Backup['integrations'][number]) => i.type === 'mqtt' && !!i.settings?.host
/** The hostname the hub derives from its name (api/openapi.yaml Hub.hostname): ASCII A–Z lowercased only, like core's hostnameFor. */
const hostnameOf = (name: string) => name.replace(/[A-Z]/g, (c) => c.toLowerCase()).replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'hubbo'
/** api/openapi.yaml HubName: trimmed, 1–32 characters, no control characters. */
// eslint-disable-next-line no-control-regex -- control characters are exactly what a hub name can't have
const CONTROL = /[\x00-\x1f\x7f-\x9f]/u
/** Sessions last 7 days, absolute (or until the hub reboots). */
const SESSION_S = 604_800
/** Requests refused while the config is blocked or unwritable (validate is a dry run, so it isn't one). */
const EDITS = [/^\/activities(\/(?!validate$)[^/]+)?$/, /^\/activities\/[^/]+\/activate$/, /^\/global-bindings$/, /^\/smart-buttons$/, /^\/admin\/devices/]
const MAX_BODY = 32 * 1024
/**
 * Like the hub's JSON reader: deeper nesting is malformed (a restore is a file, so it gets the file
 * limit). 10: one past the deepest legal body (an activity's call data, 5 levels down at its deepest), so
 * call data one level too deep reaches validation and gets `too_deep` at its path, like the hub.
 */
const MAX_JSON_DEPTH = 5 + CALL_DATA_MAX_DEPTH + 1
const MAX_FILE_DEPTH = 12
/** The HA entity domains Hubbo offers (api/openapi.yaml), and how the hub's messages list them. */
type HaDomain = NonNullable<NonNullable<operations['getDiscovery']['parameters']['query']>['domain']>
// Every domain of the spec's list, once (the compiler says when they drift).
const DOMAINS: Record<HaDomain, true> = { media_player: true, light: true, switch: true, fan: true, scene: true, script: true, cover: true, button: true, input_boolean: true, humidifier: true, select: true, remote: true }
const HA_DOMAINS: readonly string[] = Object.keys(DOMAINS)
const DOMAIN_LIST = `${HA_DOMAINS.slice(0, -1).join(', ')} or ${HA_DOMAINS.at(-1)}`
/** Entity → platform, the integration services Hubbo would offer, and the ones it skips. */
type Skipped = NonNullable<EntityDetailsStatus['skipped_services']>[number]
const skip = (reason: Skipped['reason'], ...services: string[]): Skipped[] => services.map((service) => ({ service, reason }))
/** HA's field hints for the skipped services a device can still call (its Home Assistant actions). */
const CUSTOM_SERVICES: Record<string, CustomService> = {
  'webostv.button': { service: 'webostv.button', label: 'Button', fields: [{ name: 'button', required: true, example: 'LEFT', description: 'Name of the button to press.' }] },
  'webostv.command': { service: 'webostv.command', label: 'Command', fields: [{ name: 'command', required: true, example: 'system.launcher/open', description: 'Endpoint of the command.' }, { name: 'payload', required: false, description: 'An optional payload to provide to the endpoint.' }] },
  'sonos.play_queue': { service: 'sonos.play_queue', label: 'Play queue', fields: [{ name: 'queue_position', required: false, example: 0, description: 'Position of the song in the queue to start playing from.' }] },
  'hue.activate_scene': { service: 'hue.activate_scene', label: 'Activate Hue scene', fields: [{ name: 'transition', required: false, example: 10, description: 'Transition duration in seconds.' }] },
}
const PLATFORMS: [RegExp, string, Command[], Skipped[]][] = [
  [/denon/, 'denonavr', [{ command_id: 'denonavr.set_dynamic_eq', label: 'Dynamic EQ', effect: 'momentary', params: [{ name: 'dynamic_eq', label: 'Dynamic EQ', type: 'bool', required: true }] }], skip('response_only', 'denonavr.get_command')],
  [/sonos/, 'sonos', [{ command_id: 'sonos.set_sleep_timer', label: 'Sleep timer', effect: 'momentary', params: [{ name: 'sleep_time', label: 'Sleep after', type: 'int', required: true, minimum: 0, maximum: 86400, unit: 's' }] }, { command_id: 'sonos.snapshot', label: 'Snapshot', effect: 'momentary', params: [{ name: 'with_group', label: 'With group', type: 'bool', required: false, default: true }] }, { command_id: 'sonos.restore', label: 'Restore', effect: 'momentary', params: [{ name: 'with_group', label: 'With group', type: 'bool', required: false, default: true }] }], [...skip('response_only', 'sonos.get_queue'), ...skip('unsupported_field', 'sonos.play_queue')]],
  [/lg_tv/, 'webostv', [{ command_id: 'webostv.select_sound_output', label: 'Sound output', effect: 'momentary', params: [{ name: 'sound_output', label: 'Output', type: 'enum', required: true, options: ['tv_speaker', 'external_arc', 'external_optical', 'bt_soundbar'] }] }], skip('unsupported_field', 'webostv.button', 'webostv.command')],
  [/xbox/, 'xbox', [], []],
  [/^light\./, 'hue', [], skip('unsupported_field', 'hue.activate_scene')],
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

const err = (status: number, code: ErrorCode, message: string, extra: Record<string, unknown> = {}): MockResponse => ({ status, body: { error: { code, message, ...extra } } })
const invalid = (details: ValidationIssue[], message = details[0]?.message ?? 'Invalid request') => err(422, 'validation_failed', message, { details: details.slice(0, 32) })
const nameRequired = () => invalid([{ path: '/name', code: 'required', message: 'Name is required' }])
const deviceLimit = () => err(409, 'limit_reached', 'At most 64 devices')
/** Like core: a path under /api the API doesn't have is a 404 pointing at the agent guide. */
export const unknownPath = (what: string) => err(404, 'not_found', `No route for ${what}`, { docs: GUIDE_PATH })
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
  /** The broker, like hub-sim's fake one (POST /__mock/mqtt?reachable=0|1&auth=reject|ok). */
  let broker: { reachable: boolean; auth: 'ok' | 'reject' }
  /** Bonded Bluetooth hosts by device id, and whether each is connected (like hub-sim's fake hosts). */
  let bleHosts = new Map<string, { connected: boolean }>()
  /** Like hub-sim's /sim/ble/radio?ready=0: the radio is down (no host connects, pairing is refused). */
  let bleRadioDown = false
  /** The one pairing window (POST /admin/devices/{id}/pair). When it ends unbonded, the device keeps its old host, if any. */
  let blePair: { device: string; endsAt: number } | null = null
  /** Like hub-sim's /sim/ha?auth=reject and ?connect=stall: how the fake HA answers the next connect (after a save). */
  let haNextConnect: 'ok' | 'reject' | 'stall' = 'ok'
  /** A firmware update in hub time: `advancedMs` from /__mock/advance plus scaled real time since `startedAt`. */
  // A release install downloads first, then writes like an upload.
  let fw: { state: 'idle' } | { state: 'downloading' | 'writing'; startedAt: number; advancedMs: number; version: string } | { state: 'failed'; error: string }
  /**
   * Like hub-sim's POST /sim/release: the release the feed publishes (with its manifest's board, layout and
   * min_version; unpublished = GitHub's 404, so a check is unreachable), or a fault. Mock-only `download=fail`.
   */
  let feed: { release: FirmwareRelease; board: string; layout: number; minVersion: string; published: boolean; fault: CheckError | null; downloadFails: boolean }
  /** The hub's update check: the release it found, when (real ms), and a check running (in hub time). */
  let check: { available: FirmwareRelease | null; error: CheckError | null; checkedAt: number | null; running: { startedAt: number; advancedMs: number } | null }
  /** Hub setting "Check for updates" (`check_updates`). */
  let checkUpdates: boolean
  /** The version a rolled-back update tried to run, until the next update starts. */
  let rolledBackFrom: string | null
  /** Like hub-sim's POST /sim/firmware: writes fail; the hub can't take an update. Mock-only: the new image doesn't take (the old one boots again). */
  let fwFail: boolean
  let fwReady: boolean
  let fwRevert: boolean
  /** A restart in progress, in hub time like `fw`; `onBack` runs as the hub comes back. */
  let restart: { startedAt: number; advancedMs: number; onBack: () => void } | null
  /** Like hub-sim's POST /sim/factory-reset?ready=0: the hub can't erase (503). */
  let eraseReady: boolean
  /** Entities HA has forgotten (POST /__mock/ha?remove=): their devices go stale at the next discovery. */
  let removedEntities: Set<string>
  /**
   * What HA shows for an entity (POST /__mock/ha?state=&attr=, like hub-sim's fake): a sequence step that would change
   * nothing is skipped (state checks). A sent turn_on/turn_off or setter updates it, as HA would.
   */
  let entityStates: Map<string, { state: string; attributes: Record<string, unknown> }>
  /** How late HA answers an entity's service calls (POST /__mock/ha?slow=<entity>:<ms>): past 1 s a step ends `no_reply`. */
  let slowEntities: Map<string, number>
  /** Each HA entity's own command variants (e.g. its source list), from the fixture device on it. */
  let entityCommands: Map<string, Command[]>
  /** What only an entity's details carry (§3b): its integration's services, the skipped ones, its registry id. */
  let entityExtras: Map<string, { platform: string; services: string[]; skipped: Skipped[]; uniqueId?: string }>
  /** The last details request (one at a time); dropped when the HA settings change. */
  let details: { entityId: string; startedAt: number; state: 'running' | 'settled' } | null
  /** A renamed entity's old id (POST /__mock/ha?rename=): like the hub's unique_id match, its details name the device. */
  let renamedFrom: Map<string, string>
  /** On-demand HA discovery (POST/GET/DELETE …/discover). */
  let discovery: { state: DiscoverStatus['state']; startedAt?: number; elapsedMs?: number }
  let transitions: number // TransitionResult.generation
  let lastEventAt: number
  /** The hub's last few startups and shutdowns (RAM only, newest first), with when each started. */
  let runs: { run: ActivityRun; at: number }[]
  /** The last run id handed out (a macro run from the editor gets its id when queued, and is kept once done). */
  let lastRunId = 0
  /** The last remote event (POST /__mock/remote/…); the fixture's until then. */
  let lastEvent: { button: ButtonId; kind: EventKind } | null
  /** The smart button slot + and − act on (RAM only, like the hub): Light 1 after a reboot. */
  let selectedSlot: SlotId
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
    broker = { reachable: true, auth: 'ok' }
    // Like hub-sim: every seeded Bluetooth device starts paired and connected.
    bleHosts = new Map(db.devices.filter((d) => d.provider.type === 'ble_hid').map((d) => [d.id, { connected: true }]))
    blePair = null
    bleRadioDown = false
    haNextConnect = 'ok'
    fw = { state: 'idle' }
    feed = { release: defaultRelease(), board: db.firmware.board, layout: HUB_LAYOUT, minVersion: '', published: true, fault: null, downloadFails: false }
    // A set-up hub checked a while ago (at boot); a new one hasn't yet.
    check = { available: null, error: null, checkedAt: null, running: null }
    if (!mode.firstRun) finishCheck(Date.now() - 3 * 3600_000)
    checkUpdates = true
    rolledBackFrom = null
    fwFail = false
    fwReady = true
    fwRevert = false
    eraseReady = true
    restart = null
    // A new hub has hub-sim's default name; a set-up one has the fixture's name (status.json, admin/hub.json).
    if (mode.firstRun) setHubName(DEFAULT_HUB_NAME)
    discovery = { state: 'idle' }
    // The fixture's entities come from HA's `demo` platform, which has no services of its own. Give them
    // realistic platforms, with integration services and skipped ones, so the UI shows those too.
    // Candidates preview only the domain commands; the services come with the entity's details.
    entityExtras = new Map()
    renamedFrom = new Map()
    details = null
    // A free Denon with integration services, the same entity hub-sim's fake HA has.
    if (!haCandidate('media_player.denon')) db.discover.candidates.push({ provider: { type: 'home_assistant', entity_id: 'media_player.denon' }, name: 'Denon AVR-X3700H', domain: 'media_player', suggested_kind: 'receiver', supported: true, command_ids: ['media_player.turn_on', 'media_player.turn_off', 'media_player.volume_set', 'media_player.select_source'], dropped_options: 0, registered_device_id: null })
    for (const c of db.discover.candidates) {
      const [, platform = c.platform ?? 'unknown', services = [], skipped = []] = PLATFORMS.find(([re]) => re.test(c.provider.entity_id)) ?? []
      entityExtras.set(c.provider.entity_id, { platform, services: services.map((s) => s.command_id), skipped, uniqueId: c.provider.unique_id ?? `${platform}-${c.provider.entity_id.split('.')[1]}` })
      delete c.provider.unique_id // details carry it
      Object.assign(c, { platform, dropped_options: 0 })
    }
    // A YAML-only entity (no registry entry, so no platform or unique_id), like hub-sim's fake HA.
    if (!haCandidate('switch.patio_fountain')) db.discover.candidates.push({ provider: { type: 'home_assistant', entity_id: 'switch.patio_fountain' }, name: 'Patio Fountain', domain: 'switch', suggested_kind: 'switch', supported: true, command_ids: ['switch.turn_on', 'switch.turn_off'], dropped_options: 0, registered_device_id: null })
    removedEntities = new Set()
    entityStates = new Map()
    slowEntities = new Map()
    // A registered entity's commands are its device's; a free one's come from its fixture, when it has one.
    entityCommands = new Map(db.discover.candidates.flatMap((c) => {
      const caps = c.registered_device_id ? db.capabilities.get(c.registered_device_id) : db.entityCapabilities.get(c.provider.entity_id)
      return caps ? [[c.provider.entity_id, caps.commands] as const] : []
    }))
    transitions = 17
    runs = mode.firstRun ? [] : fixture<ActivityRuns>('activity_runs/list.json').runs.map((run) => ({ run, at: Date.now() - run.started_ago_ms }))
    lastRunId = runs[0]?.run.id ?? 0
    lastEventAt = Date.now() - 5230
    lastEvent = null
    selectedSlot = 'light_1'
    configGen = Math.max(1, db.globalBindings.revision, db.smartButtons.revision, ...db.activities.map((a) => a.revision), ...db.devices.map((d) => d.revision), ...[...db.configs.values()].map((c) => c.revision))
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

  /** Factory defaults: no devices or activities, one global button (Off tap → off, like core); built-in integrations unconfigured. Credentials and tokens stay. */
  function factoryDefaults() {
    db.devices = []
    db.capabilities.clear()
    db.activities = []
    db.activeActivityId = null
    db.globalBindings = { bindings: { 'off:tap': [{ integration_id: 'system', device_id: null, command_id: 'off', params: {} }] }, macros: [], revision: configGen }
    db.smartButtons = emptySmartButtons(configGen)
    db.discover.candidates.forEach((c) => (c.registered_device_id = null))
    const ha = db.configs.get('ha_main')!
    ha.settings = { type: 'home_assistant', url: 'http://homeassistant.local:8123', verify_tls: false, has_access_token: false }
    const mqtt = db.configs.get('mqtt_main')!
    Object.assign(mqtt, { enabled: false, settings: { type: 'mqtt', host: '', port: 1883, username: '', has_password: false } })
  }

  /** Restore and reset stamp every object with one new generation, so every stale client gets 409. */
  function stampAll() {
    const g = commit()
    blockedRecord = null
    db.activities = db.activities.map((a) => ({ ...a, revision: g }))
    db.devices = db.devices.map((d) => ({ ...d, revision: g }))
    db.globalBindings = { ...db.globalBindings, revision: g }
    db.smartButtons = { ...db.smartButtons, revision: g }
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
    haServiceDomains: new Map(db.devices.flatMap((d) => (d.provider.type === 'home_assistant' ? [[d.id, serviceDomains(d.provider.entity_id)] as const] : []))),
  })
  /** An entity's Home Assistant action domains (core's serviceOnDevice): its platform, kept after HA forgets it, plus `remote` on a remote. */
  const serviceDomains = (entityId: string) => {
    const domains: string[] = []
    const platform = entityExtras.get(entityId)?.platform
    if (platform) domains.push(platform)
    if (entityId.startsWith('remote.')) domains.push('remote')
    return domains
  }

  function integrationState(id: string): IntegrationState {
    const c = db.configs.get(id)
    if (!c) return 'error'
    if (!c.enabled) return 'disabled'
    if (c.settings.type === 'home_assistant') return c.settings.has_access_token ? (haOverride ?? 'connected') : 'not_configured'
    // ble is the radio: working (connected) while enabled, `error` when it's down.
    if (c.settings.type === 'ble_hid') return bleRadioDown ? 'error' : 'connected'
    if (c.settings.type === 'mqtt') return !c.settings.host ? 'not_configured' : !broker.reachable ? 'unreachable' : broker.auth === 'reject' ? 'auth_failed' : 'connected'
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

  /** An entity id in one of the HA_DOMAINS; never a broadcast like `light.all`. */
  function haEntityIdOk(id: string) {
    const [domain, object = ''] = id.split('.', 2)
    return id.length <= 96 && HA_DOMAINS.includes(domain!) && /^[a-z0-9_]+$/.test(object) && id === `${domain}.${object}` && !id.endsWith('.all')
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
    if (!haEntityIdOk(entityId)) return invalid([{ path: '/provider/entity_id', code: 'pattern', message: `Not a ${DOMAIN_LIST} entity id` }])
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
    if (domain && !HA_DOMAINS.includes(domain)) return invalid([{ path: '/domain', code: 'invalid_enum_value', message: `domain must be ${DOMAIN_LIST}` }])
    const available = q.get('available')
    if (available !== null && available !== 'true' && available !== 'false') return invalid([{ path: '/available', code: 'wrong_type', message: 'available must be true or false' }])
    const limit = Number(q.get('limit') ?? 50)
    if (!Number.isInteger(limit) || limit < 1 || limit > 100) return invalid([{ path: '/limit', code: 'param_out_of_range', message: 'limit must be 1 to 100' }])
    const offset = Number(q.get('cursor') ?? 0) || 0
    const all = db.discover.candidates.filter((c) => (!text || c.name.toLowerCase().includes(text) || c.provider.entity_id.includes(text)) && (!domain || c.domain === domain) && (available !== 'true' || (c.supported && !c.registered_device_id)))
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
    const commands = (c.command_ids ?? []).map((id) => all.get(id) ?? INTEGRATION_SERVICES.get(id) ?? plain(id))
    // Like the hub's snapshot: the services it skipped but can still call, with HA's hints, as one command.
    const services = (entityExtras.get(c.entity_id!)?.skipped ?? [])
      .filter((x) => CALLABLE_SKIPS.has(x.reason))
      .map((x) => CUSTOM_SERVICES[x.service] ?? { service: x.service, fields: [] })
    // Then the entity's own call_service's (a remote's `remote` services from its fixture), once each.
    const own = (all.get(CALL_SERVICE)?.services ?? []).filter((x) => !services.some((y) => y.service === x.service))
    services.push(...own)
    if (services.length) commands.push({ command_id: CALL_SERVICE, label: 'Home Assistant action', effect: 'momentary', group: 'custom', params: [], services })
    return commands
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
      const detail = state === 'not_configured' ? 'Not set up' : i.type === 'ble_hid' ? (state === 'error' ? "Bluetooth isn't working" : undefined) : i.type === 'mqtt' ? MQTT_DETAIL[state] : i.detail
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
  const firmwareMs = () => (fw.state === 'writing' || fw.state === 'downloading' ? hubMs(fw) : 0)
  /** An update is running (a download, the write or the restart): the hub refuses another, and a restart or reset. */
  const updating = () => ['downloading', 'writing', 'rebooting', 'down'].includes(firmwarePhase())
  function setRunning(version: string) {
    db.firmware = { ...db.firmware, version }
    db.status.hubbo.version = version
  }

  /** True while the hub restarts (it answers nothing); as it comes back, `onBack` runs once. */
  function restarting(): boolean {
    if (!restart) return false
    if (hubMs(restart) < RESTART_MS) return true
    const { onBack } = restart
    restart = null
    onBack()
    return false
  }
  const startRestart = (onBack: () => void) => (restart = { startedAt: Date.now(), advancedMs: 0, onBack })

  const hub = (): Hub => {
    const own = hostnameOf(db.status.hubbo.name)
    const hostname = db.status.network.hostname ?? own
    return { name: db.status.hubbo.name, hostname, hostname_conflict: hostname !== own, check_updates: checkUpdates }
  }
  /** A rename (or a fresh hub) advertises the name's own hostname. */
  function setHubName(name: string) {
    db.status.hubbo.name = name
    setHostnameConflict(false)
  }
  /** Mock only (`/__mock/hub?conflict=1`): mDNS found another hub on the name's hostname, so this one advertises another. */
  function setHostnameConflict(on: boolean) {
    db.status.network.hostname = `${hostnameOf(db.status.hubbo.name)}${on ? '-2' : ''}`
  }

  /**
   * Where a firmware update is, in hub time. The reboot's end is applied here: the new version
   * runs and every session is gone (they live in RAM), like the board.
   */
  function firmwarePhase(): 'idle' | 'downloading' | 'writing' | 'rebooting' | 'down' | 'failed' {
    if (fw.state === 'downloading') {
      const ms = hubMs(fw)
      if (ms < FW_DOWNLOAD_MS) return 'downloading'
      if (feed.downloadFails) {
        fw = { state: 'failed', error: 'download_failed' }
        return 'failed'
      }
      // Like hub-sim: the time past the download goes on to the write.
      fw = { state: 'writing', startedAt: Date.now(), advancedMs: ms - FW_DOWNLOAD_MS, version: fw.version }
    }
    if (fw.state !== 'writing') return fw.state
    const ms = firmwareMs()
    if (ms < FW_WRITE_MS) return 'writing'
    if (fwFail) {
      fw = { state: 'failed', error: 'write_failed' }
      return 'failed'
    }
    rolledBackFrom = null // written: a past rollback is old news
    if (ms < FW_WRITE_MS + FW_REBOOT_MS / 2) return 'rebooting'
    if (ms < FW_WRITE_MS + FW_REBOOT_MS) return 'down'
    if (!fwRevert) setRunning(fw.version)
    else rolledBackFrom = fw.version
    // Like the board: the checks live in RAM, so a restart forgets them (a rollback is remembered).
    check = { available: null, error: null, checkedAt: null, running: null }
    sessions.clear()
    fw = { state: 'idle' }
    return 'idle'
  }

  function firmwareStatus(): FirmwareStatus {
    const phase = firmwarePhase()
    const state = phase === 'down' ? 'rebooting' : phase // (handle() answers nothing while down)
    const ms = firmwareMs()
    const progress =
      state === 'downloading' ? Math.floor((ms * 100) / FW_DOWNLOAD_MS)
      : state === 'writing' ? Math.floor((ms * 100) / FW_WRITE_MS)
      : state === 'rebooting' ? 100
      : 0
    if (check.running && hubMs(check.running) >= FW_CHECK_MS) finishCheck(Date.now())
    return {
      version: db.firmware.version,
      board: db.firmware.board,
      update: { state, progress, error: fw.state === 'failed' ? fw.error : null },
      available: check.available,
      checking: !!check.running,
      checked_ago_ms: check.checkedAt === null ? null : Date.now() - check.checkedAt,
      check_error: check.error,
      rolled_back_from: rolledBackFrom,
    }
  }

  /**
   * A check reads the feed, like core: no release (GitHub's 404) or a fault is the check's error and keeps what
   * the last good check found; else the release, if it's for this board and layout, newer, and installable from here.
   */
  function finishCheck(at: number) {
    const { release, board, layout, minVersion, published, fault } = feed
    const running = db.firmware.version
    check.running = null
    check.checkedAt = at
    check.error = published ? fault : 'unreachable'
    if (check.error) return
    const fits = board === db.firmware.board && layout === HUB_LAYOUT && isNewer(release.version, running) && !(minVersion && isNewer(minVersion, running))
    check.available = fits ? release : null
  }

  /**
   * The hub's state check (docs/design/state-checks.md): HA already shows what the step would set. Power: turn_on when
   * on, turn_off when off or standby; every field must equal the attribute of the same name (numbers within a hair;
   * lists and objects never). A step with nothing it can check, or an unknown state, runs.
   */
  function alreadySet(entityId: string, service: string, x: Action): boolean {
    const e = entityStates.get(entityId)
    if (!e || ['unavailable', 'unknown'].includes(e.state)) return false
    const power = service === 'turn_on' ? !['off', 'standby'].includes(e.state) : service === 'turn_off' ? ['off', 'standby'].includes(e.state) : null
    const fields = Object.entries(x.params)
    if (power === false || (power === null && !fields.length)) return false
    return fields.every(([k, v]) => {
      const a = e.attributes[k]
      if (typeof v === 'number') return typeof a === 'number' && Math.abs(a - v) < 1e-6
      return (typeof v === 'string' || typeof v === 'boolean') && a === v
    })
  }

  /** HA after a sent step, like hub-sim's fake: turn_on/turn_off set the power (an off player has no source), select_source the source. */
  function haSent(entityId: string, service: string, x: Action) {
    const e = service === 'select_source' || service === 'turn_on' || service === 'turn_off' ? fakeState(entityId) : null
    if (!e) return
    if (service === 'select_source') e.attributes.source = x.params.source
    else e.state = service === 'turn_on' ? 'on' : 'off'
    if (service === 'turn_off') delete e.attributes.source
  }

  /** An entity's state in the fake HA, made on first use as hub-sim's fake media players start: off. */
  const fakeState = (id: string) => entityStates.get(id) ?? entityStates.set(id, { state: 'off', attributes: {} }).get(id)!

  /** Macros run from the editor still going (the hub queues at most 4). */
  let macrosWaiting = 0

  /** Keep a run, as the hub does. */
  function keepRun(...args: Parameters<typeof runOf>) {
    runs = [{ run: runOf(...args), at: Date.now() }, ...runs].slice(0, 8)
  }

  /**
   * A run as the hub records it: a startup, a shutdown, or a button's macro (`activityId` null for a global one). It
   * runs at once here, and each step keeps going past a failure. A step on a stale device fails like an execute does
   * (entity_not_found); a wait takes its time; the rest are ok.
   */
  function runOf(activityId: string | null, kind: ActivityRun['kind'], actions: Action[], { rejected = false, button = null, trigger = null }: { rejected?: boolean; button?: ButtonId | null; trigger?: ActivityRun['trigger'] } = {}): ActivityRun {
    const steps = actions.map((x, index): ActivityRunStep => {
      const base = { index, device_id: x.device_id, command_id: x.command_id }
      if (rejected) return { ...base, result: 'cancelled', error: 'queue_full', duration_ms: null }
      const d = db.devices.find((dev) => dev.id === x.device_id)
      if (d?.availability === 'stale') return { ...base, result: 'failed', error: 'entity_not_found', duration_ms: 1 }
      const entity = d?.provider.type === 'home_assistant' ? d.provider.entity_id : null
      // Only an HA service step (`<domain>.<service>`) is checked; a `call_service` action never is, like the hub.
      const service = x.command_id.split('.')[1] ?? ''
      if (entity && service && alreadySet(entity, service, x)) return { ...base, result: 'skipped', error: 'already_set', duration_ms: 5 }
      if (entity) haSent(entity, service, x)
      const late = entity ? slowEntities.get(entity) : undefined
      const ms = late ?? (x.command_id === 'wait' ? Number(x.params.duration_ms ?? 0) : 40)
      return ms > 1000 && late !== undefined ? { ...base, result: 'no_reply', error: null, duration_ms: 1000 } : { ...base, result: 'ok', error: null, duration_ms: ms }
    })
    const failed = steps.some((s) => s.result === 'failed')
    const outcome = rejected ? 'rejected' : failed ? 'completed_with_errors' : 'completed'
    return { id: ++lastRunId, activity_id: activityId, kind, button, trigger, started_ago_ms: 0, duration_ms: rejected ? null : steps.reduce((n, s) => n + (s.duration_ms ?? 0), 0), outcome, steps }
  }

  /** GET /smart-buttons: the slots, and what each does with its device (derived like the hub; read-only). */
  function smartButtons(): SmartButtons {
    const actions = Object.fromEntries(
      SLOTS.map((id) => {
        const d = db.devices.find((x) => x.id === db.smartButtons.slots[id])
        return [id, d ? slotActions(d, db.capabilities.get(d.id)?.commands ?? []) : noActions()]
      }),
    ) as SmartButtons['actions']
    return { ...db.smartButtons, actions }
  }

  function status(): Status {
    const s = structuredClone(db.status)
    const jitter = (v: number) => Math.round(v * (0.85 + rand() * 0.3) * 100) / 100 // sub-ms on a real board
    s.uptime_s = db.status.uptime_s + Math.round((Date.now() - bootedAt) / 1000)
    s.time = isoNow()
    s.active_activity_id = db.activeActivityId
    s.selected_slot = selectedSlot
    const last = runs[0]?.run
    s.last_run = last ? { id: last.id, activity_id: last.activity_id, kind: last.kind, button: last.button, trigger: last.trigger, outcome: last.outcome, failed_steps: failedSteps(last) } : null
    s.config = config
    s.auth_state = authState()
    s.integrations = integrations().map(({ id, type, state, detail }) => ({ id, type, state, ...(detail ? { detail } : {}) }))
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

  /** `macros` is optional on input: missing is none. */
  function withMacros<T extends { macros?: string[] }>(b: T): T & { macros: string[] } {
    return { ...b, macros: b.macros ?? [] }
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
  const docs = loadDocs()
  const apiIndex = fixture<Record<string, unknown>>('discovery/index.json')
  const routes: [string, RegExp, Role, Handler][] = [
    // The agent docs (no sign-in): the index, the spec and the guide. /llms.txt is outside /api/v1 (node.ts).
    ['GET', /^\/?$/, 'public', () => ok({ ...apiIndex, version: db.status.hubbo.version })],
    ['GET', /^\/openapi\.json$/, 'public', (req) => sendDoc(req, docs.openapi)],
    ['GET', /^\/agent\.md$/, 'public', (req) => sendDoc(req, docs.guide)],
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
    // The radio's log (dev builds): the mock has no radio, so it's always empty.
    ['GET', /^\/remote\/log$/, 'token', () => ok({ span_ms: 0, dropped: 0, entries: [] })],
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
      const body = req.body as { params?: Record<string, never>; call?: HaServiceCall }
      const a: Action = { integration_id: d.integration_id, device_id: d.id, command_id: m[2]!, params: body?.params ?? {}, ...(body?.call !== undefined ? { call: body.call } : {}) }
      if (db.capabilities.get(d.id)?.commands.find((c) => c.command_id === a.command_id)?.effect === 'wait') return fixtureError(422, 'errors/sequence_only.json')
      const shape = activityShape({ name: 'x', startup: [a], shutdown: [], bindings: {}, macros: [] }, ['name', 'startup', 'shutdown', 'bindings', 'macros'])
      const v = shape.length ? shape : validateActivity({ name: 'x', startup: [a], shutdown: [], bindings: {}, macros: [] }, catalog()).errors
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
      const bad = checkActivity(req.body, ['name', 'startup', 'shutdown', 'bindings', 'macros'])
      if (bad) return bad
      if (db.activities.length >= 16) return err(409, 'limit_reached', 'At most 16 activities')
      const b = req.body as ActivityInput
      let id = `act_${slug(b.name)}`
      while (db.activities.some((a) => a.id === id)) id = `act_${slug(b.name).slice(0, 20)}_${rand36(3)}`
      const act: Activity = { id, name: b.name, startup: b.startup, shutdown: b.shutdown, bindings: b.bindings, macros: b.macros ?? [], schema_version: 1, revision: commit() }
      db.activities.push(act)
      return { status: 201, body: act, headers: { location: `/api/v1/activities/${id}` } }
    }],
    ['POST', /^\/activities\/validate$/, 'token', (req) => {
      const shape = activityShape(req.body, ['id', 'name', 'startup', 'shutdown', 'bindings', 'macros', 'schema_version', 'revision'])
      if (shape.length) return ok({ valid: false, errors: shape })
      const v = validateActivity(req.body as ActivityInput, catalog())
      return ok({ valid: v.valid, errors: v.errors })
    }],
    ['POST', /^\/activities\/off$/, 'token', () => {
      const cur = db.activities.find((a) => a.id === db.activeActivityId)
      db.activeActivityId = null
      if (cur) keepRun(cur.id, 'shutdown', cur.shutdown)
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
      const bad = checkActivity(req.body, ['id', 'name', 'startup', 'shutdown', 'bindings', 'macros', 'schema_version', 'revision'])
      if (bad) return bad
      const next: Activity = { ...cur, name: b.name, startup: b.startup, shutdown: b.shutdown, bindings: b.bindings, macros: b.macros ?? [], revision: commit() }
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
      keepRun(a.id, 'startup', a.startup, { rejected: mode.startupRejected })
      const startup = mode.startupRejected ? ({ status: 'rejected', queued_actions: 0, reason: 'queue_full' } as const) : ({ status: 'queued', queued_actions: a.startup.length } as const)
      const r: TransitionResult = { switched: true, active_activity_id: a.id, previous_activity_id: prev, generation: ++transitions, startup, shutdown: { status: 'none', queued_actions: 0 } }
      return ok(r)
    }],
    // Run a macro as it is on screen (saved or not): checked like a bound one, queued on the Macro lane, and
    // logged once its steps are done (the mock waits out its waits). Body: { actions, trigger, activity_id }.
    ['POST', /^\/macros\/run$/, 'token', (req) => {
      const b = req.body as Partial<Record<keyof MacroRunRequest, unknown>>
      const shape: ValidationIssue[] = []
      if (!Array.isArray(b?.actions) || !b.actions.length || b.actions.length > 32) shape.push({ path: '/actions', code: 'wrong_type', message: 'actions must be 1 to 32 actions' })
      const t = typeof b?.trigger === 'string' ? parseTrigger(b.trigger) : null
      if (!t || t.event === 'up') shape.push({ path: '/trigger', code: 'invalid_trigger', message: 'trigger must be a down, tap or hold trigger' })
      if (b?.activity_id !== null && typeof b?.activity_id !== 'string') shape.push({ path: '/activity_id', code: 'wrong_type', message: 'activity_id must be a string or null' })
      if (shape.length) return invalid(shape)
      const activityId = b.activity_id as string | null
      if (activityId && !db.activities.some((a) => a.id === activityId)) return invalid([{ path: '/activity_id', code: 'unknown_activity', message: 'No such activity' }])
      const key = b.trigger as string
      const actions = b.actions as Action[]
      const errors: ValidationIssue[] = []
      validateBindings({ bindings: { [key]: actions }, macros: [key] }, catalog(), errors, [])
      if (errors.length) return invalid(errors.map((e) => ({ ...e, path: e.path.replace(`/bindings/${key}`, '/actions') })))
      if (macrosWaiting >= 4) return err(503, 'busy', 'Too many macros waiting')
      // Its steps go now; it's kept once they're done (it took `duration_ms`).
      const run = runOf(activityId, 'macro', actions, { button: t!.button, trigger: t!.event === 'hold' ? 'long_press' : 'press' })
      macrosWaiting++
      setTimeout(() => {
        macrosWaiting--
        runs = [{ run, at: Date.now() - run.duration_ms! }, ...runs].slice(0, 8)
      }, run.duration_ms!)
      return { status: 202, body: { run_id: run.id } satisfies MacroRunAccepted }
    }],
    ['GET', /^\/activity-runs$/, 'token', () => ok({ runs: runs.map(({ run, at }) => ({ ...run, started_ago_ms: Date.now() - at })) } satisfies ActivityRuns)],
    ['GET', /^\/global-bindings$/, 'token', () => ok(db.globalBindings)],
    ['GET', /^\/smart-buttons$/, 'token', () => ok(smartButtons())],
    ['GET', /^\/smart-buttons\/actions$/, 'token', (req) => {
      const id = req.query.get('device_id')
      if (!id) return invalid([{ path: '/device_id', code: 'required', message: 'Send ?device_id=' }])
      if (!DEVICE_ID.test(id)) return invalid([{ path: '/device_id', code: 'invalid_id', message: 'device_id must be a hub device ID (dev_…)' }])
      const d = db.devices.find((x) => x.id === id)
      if (!d) return err(404, 'not_found', 'No such device')
      return ok(slotActions(d, db.capabilities.get(d.id)?.commands ?? []))
    }],
    ['PUT', /^\/smart-buttons$/, 'token', (req) => {
      const shape = smartButtonsShape(req.body)
      if (shape.length) return invalid(shape)
      const b = req.body as SmartButtonsInput
      if (b.revision !== db.smartButtons.revision) return err(409, 'revision_conflict', 'Changed since the revision you sent; GET it again and reapply', { current_revision: db.smartButtons.revision })
      const { errors } = validateSmartButtons(b, catalog())
      if (errors.length) return invalid(errors, 'The smart buttons are invalid')
      db.smartButtons = { slots: b.slots, revision: commit() }
      return ok(smartButtons())
    }],
    ['PUT', /^\/global-bindings$/, 'token', (req) => {
      const b = req.body as GlobalBindings
      const shape: ValidationIssue[] = []
      bindingsShape(b?.bindings, '/bindings', shape)
      macrosShape(b?.macros, '/macros', shape)
      if (shape.length) return invalid(shape)
      if (b.revision !== db.globalBindings.revision) return err(409, 'revision_conflict', `Global bindings changed since revision ${b.revision}`, { current_revision: db.globalBindings.revision })
      const errors: ValidationIssue[] = []
      validateBindings(b, catalog(), errors, [])
      if (errors.length) return invalid(errors)
      db.globalBindings = { bindings: b.bindings, macros: b.macros ?? [], revision: commit() }
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
      } else if (s?.type === 'mqtt' && next.settings.type === 'mqtt') {
        const unknown = Object.keys(s).filter((k) => !['type', 'host', 'port', 'username', 'password'].includes(k))
        if (unknown.length) return invalid(unknownFields(unknown, '/settings'))
        if (s.host !== undefined) {
          if (s.host.length > 253) return invalid([{ path: '/settings/host', code: 'too_long', message: 'Up to 253 characters' }])
          // The spec's pattern: no colon, or IPv6's two or more (one is a host:port; the port has its own field).
          if (!/^([A-Za-z0-9._-]*|[A-Za-z0-9._-]*:[A-Za-z0-9._:-]*:[A-Za-z0-9._:-]*)$/.test(s.host)) return invalid([{ path: '/settings/host', code: 'wrong_type', message: 'An IP address or a hostname' }])
          // A new broker clears the password unless a new one comes with it.
          if (s.host !== next.settings.host && s.password === undefined) mut(next.settings).has_password = false
          next.settings.host = s.host
        }
        if (s.port !== undefined) {
          if (!Number.isInteger(s.port) || s.port < 1 || s.port > 65535) return invalid([{ path: '/settings/port', code: 'param_out_of_range', message: 'Port must be 1–65535' }])
          next.settings.port = s.port
        }
        if (s.username !== undefined) next.settings.username = s.username
        if (s.password === '') return invalid([{ path: '/settings/password', code: 'required', message: 'Enter a password, or null to remove it' }])
        if (s.password !== undefined) mut(next.settings).has_password = s.password !== null
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
      if (!haEntityIdOk(entityId)) return invalid([{ path: '/entity_id', code: 'pattern', message: `Not a ${DOMAIN_LIST} entity id` }])
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
          cat.haServiceDomains = new Map(cat.haServiceDomains).set(d.id, serviceDomains(p.entity_id))
          const broken = db.activities.flatMap((a, ai) => validateActivity(a, cat).errors.map((x) => ({ ...x, path: `/activities/${ai}${x.path}` })))
          const global: ValidationIssue[] = []
          validateBindings(db.globalBindings, cat, global, [])
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
      // A slot's own device counts too (core: deleting it would leave the slot pointing at nothing).
      for (const id of SLOTS) if (db.smartButtons.slots[id] === d.id) refs.push({ path: `/smart_buttons/slots/${id}`, code: 'referenced', message: 'Used by smart buttons' })
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
        smart_buttons: db.smartButtons,
        integrations: [...db.configs.values()].map((c) => ({
          id: c.id,
          type: c.type,
          name: c.name,
          enabled: c.enabled,
          // Everything but the type and the secrets' has_* flags (the secrets never leave the hub).
          settings: Object.fromEntries(Object.entries(c.settings).filter(([k]) => k !== 'type' && !k.startsWith('has_'))),
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
      const result = { dry_run: dry, devices: b.devices.length, activities: b.activities.length, integrations: b.integrations?.length ?? 0, migrated_from: null, requires: ['home_assistant_token', ...(b.integrations?.some(needsMqttPassword) ? ['mqtt_password' as const] : []), 'ble_pairing', 'api_tokens'], warnings: [] }
      if (!dry) {
        // Revisions in the backup may be missing or 0; stampAll() replaces them all.
        // A backup's StoredDevice carries ha_capabilities; a Device (what the API returns) never does.
        db.devices = b.devices.map(({ ha_capabilities: _, ...d }) => ({ ...d, platform: d.provider.type, availability: 'unknown' as const, revision: 0 }))
        // A Bluetooth device's commands come from the profile content it carries.
        for (const d of db.devices) if (d.provider.type === 'ble_hid') db.capabilities.set(d.id, bleCaps(d))
        pruneBonds() // a restored device keeps its bond on this hub; others are dropped
        db.activities = b.activities.map((a) => ({ ...withMacros(a), schema_version: 1 as const, revision: 0 }))
        db.globalBindings = { bindings: b.global_bindings?.bindings ?? {}, macros: b.global_bindings?.macros ?? [], revision: 0 }
        db.smartButtons = b.smart_buttons ? { slots: b.smart_buttons.slots, revision: 0 } : emptySmartButtons(0)
        // Integration settings come back without secrets (a backup never has the HA token). Like the
        // hub, the stored token survives only when the URL stays the same.
        for (const i of b.integrations ?? []) {
          const c = db.configs.get(i.id)
          if (c?.settings.type === 'mqtt' && i.type === 'mqtt') {
            const s = (i.settings ?? {}) as { host?: string; port?: number; username?: string }
            const host = s.host ?? ''
            db.configs.set(i.id, { ...c, name: i.name ?? c.name, enabled: i.enabled ?? c.enabled, settings: { type: 'mqtt', host, port: s.port ?? 1883, username: s.username ?? '', has_password: c.settings.has_password && host === c.settings.host } })
            continue
          }
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
      if (updating()) return fixtureError(409, 'errors/factory_reset_during_update.json')
      if (!eraseReady) return err(503, 'unavailable', "The hub can't do a factory reset right now")
      // Back fresh: no admin, sessions, tokens, remote or config; Bluetooth bonds erased with the rest.
      startRestart(() => {
        reset({ firstRun: true, requireAuth: mode.requireAuth })
        bleHosts = new Map()
      })
      return { status: 202 }
    }],
    ['GET', /^\/admin\/hub$/, 'admin', () => ok(hub())],
    ['PUT', /^\/admin\/hub$/, 'admin', (req) => {
      const body = (req.body ?? {}) as Record<string, unknown>
      const extra = Object.keys(body).filter((k) => k !== 'name' && k !== 'check_updates')
      if (extra.length) return invalid(unknownFields(extra)) // hostname is read-only
      if (body.name === undefined && body.check_updates === undefined) return invalid([{ path: '', code: 'required', message: 'Send a name or check_updates' }])
      if (body.check_updates !== undefined && typeof body.check_updates !== 'boolean') return invalid([{ path: '/check_updates', code: 'wrong_type', message: 'check_updates must be true or false' }])
      // Check every field that's there, then apply them.
      let name: string | undefined
      if (body.name !== undefined) {
        if (typeof body.name !== 'string') return invalid([{ path: '/name', code: 'wrong_type', message: 'Name must be text' }])
        name = body.name.replace(/^ +| +$/g, '') // spaces only: a tab or newline is a control character
        if (!name) return invalid([{ path: '/name', code: 'required', message: 'Name is required' }])
        // In core's order: control characters, then length.
        if (CONTROL.test(name)) return invalid([{ path: '/name', code: 'invalid_name', message: 'Name must be text without control characters' }])
        if ([...name].length > 32) return fixtureError(422, 'errors/hub_name_invalid.json')
      }
      if (name !== undefined) setHubName(name)
      if (body.check_updates !== undefined) checkUpdates = body.check_updates as boolean
      return ok(hub())
    }],
    ['POST', /^\/admin\/restart$/, 'admin', () => {
      if (updating()) return fixtureError(409, 'errors/restart_during_update.json')
      startRestart(() => sessions.clear()) // sessions live in RAM
      return { status: 202, body: fixture('admin/restart.json') }
    }],
    ['GET', /^\/admin\/firmware$/, 'admin', () => ok(firmwareStatus())],
    // In the hub's order: size and type (the router), Content-Length, busy, ready, then the image checks.
    ['POST', /^\/admin\/firmware\/update$/, 'admin', async (req) => {
      const image = req.rawBody ?? new Uint8Array()
      if (image.byteLength > FW_SLOT_BYTES) return err(413, 'payload_too_large', 'The image is larger than the update slot')
      if (!/^application\/octet-stream\b/i.test(req.headers['content-type'] ?? '')) return err(415, 'unsupported_media_type', 'Send the image as application/octet-stream')
      if (req.headers['content-length'] === undefined) return fixtureError(411, 'errors/length_required.json')
      if (updating()) return fixtureError(409, 'errors/update_in_progress.json')
      if (!fwReady) return err(503, 'unavailable', 'The hub can’t take an update right now')
      const checked = await checkFirmwareImage(image, db.firmware.board)
      if ('code' in checked) return err(422, checked.code, checked.message)
      fw = { state: 'writing', startedAt: Date.now(), advancedMs: 0, version: checked.version }
      return ok(firmwareStatus(), 202)
    }],
    ['POST', /^\/admin\/firmware\/check$/, 'admin', () => {
      firmwareStatus() // settles a check that's done
      if (check.running) return err(409, 'busy', 'A check is already running')
      check.running = { startedAt: Date.now(), advancedMs: 0 }
      return ok(firmwareStatus(), 202)
    }],
    ['POST', /^\/admin\/firmware\/install$/, 'admin', (req) => {
      const body = (req.body ?? {}) as Record<string, unknown>
      const extra = Object.keys(body).filter((k) => k !== 'version')
      if (extra.length) return invalid(unknownFields(extra))
      if (body.version === undefined) return invalid([{ path: '/version', code: 'required', message: 'version is required' }])
      if (typeof body.version !== 'string') return invalid([{ path: '/version', code: 'wrong_type', message: 'version must be text' }])
      if (!RELEASE.test(body.version)) return invalid([{ path: '/version', code: 'pattern', message: 'version must be X.Y.Z' }])
      if (updating()) return fixtureError(409, 'errors/update_in_progress.json')
      if (check.available?.version !== body.version) return err(409, 'not_available', `${body.version} isn't the available update`)
      fw = { state: 'downloading', startedAt: Date.now(), advancedMs: 0, version: body.version }
      rolledBackFrom = null
      return ok(firmwareStatus(), 202)
    }],

    // Mock-only controls (never in the real API).
    // Like hub-sim's POST /sim/firmware: writes fail (?fail=1) or the hub can't take an update (?ready=0).
    // Mock-only ?revert=1: the reboot comes back on the old image (a reset mid-write, or a new image that crashes).
    ['POST', /^\/__mock\/hub$/, 'public', (req) => {
      if (req.query.has('conflict')) setHostnameConflict(req.query.get('conflict') === '1')
      return ok(hub())
    }],
    ['POST', /^\/__mock\/mqtt$/, 'public', (req) => {
      const reachable = req.query.get('reachable'), auth = req.query.get('auth')
      if ((reachable !== null && reachable !== '0' && reachable !== '1') || (auth !== null && auth !== 'ok' && auth !== 'reject')) return invalid([{ path: '', code: 'invalid_enum_value', message: 'reachable=0|1, auth=ok|reject' }])
      if (reachable !== null) broker.reachable = reachable === '1'
      if (auth) broker.auth = auth as 'ok' | 'reject'
      return ok({})
    }],
    ['POST', /^\/__mock\/factory-reset$/, 'public', (req) => {
      if (req.query.has('ready')) eraseReady = req.query.get('ready') === '1'
      return ok({ ready: eraseReady })
    }],
    // Like hub-sim's POST /sim/release?version=&board=&layout=&min_version=&fault=none|…&none=1&running=&rolled_back_from=:
    // what the feed publishes (any release field publishes it; none=1 withdraws it), the running version and a past
    // rollback. Mock-only ?download=fail: an install's download fails.
    ['POST', /^\/__mock\/release$/, 'public', (req) => {
      const q = req.query
      const bad = (path: string, message: string) => invalid([{ path, code: 'invalid_enum_value', message }])
      const fault = q.get('fault')
      if (fault !== null && !['none', 'unreachable', 'bad_signature', 'bad_manifest'].includes(fault)) return bad('/fault', 'fault=none|unreachable|bad_signature|bad_manifest')
      const version = q.get('version'), layout = q.get('layout')
      if (version !== null && !RELEASE.test(version)) return bad('/version', 'version must be X.Y.Z')
      if (layout !== null && !/^[1-9]\d{0,3}$/.test(layout)) return bad('/layout', 'layout must be 1..9999')
      if (q.has('none') && q.get('none') !== '1') return bad('/none', 'none must be 1')
      if (fault) feed.fault = fault === 'none' ? null : (fault as CheckError)
      if (version !== null) feed.release = { ...feed.release, version }
      if (q.has('board')) feed.board = q.get('board')!
      if (layout !== null) feed.layout = Number(layout)
      if (q.has('min_version')) feed.minVersion = q.get('min_version')!
      if (['version', 'board', 'layout', 'min_version'].some((k) => q.has(k))) feed.published = true
      if (q.has('none')) feed.published = false
      if (q.has('download')) feed.downloadFails = q.get('download') === 'fail'
      const running = q.get('running')
      if (running) setRunning(running)
      if (q.has('rolled_back_from')) rolledBackFrom = q.get('rolled_back_from') || null
      return ok(feed)
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
      if (fw.state === 'writing' || fw.state === 'downloading') fw.advancedMs += ms
      if (restart) restart.advancedMs += ms
      if (check.running) check.running.advancedMs += ms
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
      // State checks, like hub-sim's /sim/ha: state=<entity>:<state>, attr=<entity>:<name>=<JSON or text>, slow=<entity>:<ms>|none.
      const entityState = req.query.get('state')?.includes(':') ? req.query.get('state') : null
      const attrs = req.query.getAll('attr'), slow = req.query.get('slow')
      if (entityState || attrs.length || slow) {
        if (entityState) { const [id, s] = splitOnce(entityState, ':'); fakeState(id).state = s }
        for (const a of attrs) {
          const [id, kv] = splitOnce(a, ':')
          const [name, raw] = splitOnce(kv, '=')
          let v: unknown = raw
          try { v = JSON.parse(raw) } catch { /* plain text */ }
          fakeState(id).attributes[name] = v
        }
        if (slow === 'none') slowEntities.clear()
        else if (slow) { const [id, ms] = splitOnce(slow, ':'); slowEntities.set(id, Number(ms)) }
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
      // A slot's button selects its slot (core: on Down, even when the slot is empty), unless the active activity owns that button.
      const active = db.activities.find((a) => a.id === db.activeActivityId)
      const pressed = m[1]! as ButtonId
      if (action !== 'release' && isSlot(pressed) && !(active && bindsButton(active.bindings, pressed))) selectedSlot = pressed
      // A macro (a Press marked as one, on down or on tap) runs and is logged, like the hub's Macro lane.
      const owner = active && bindsButton(active.bindings, pressed) ? active : null
      const set = owner ?? db.globalBindings
      const key = triggerKey(pressed, lastEvent.kind)
      if (set.macros.includes(key)) keepRun(owner?.id ?? null, 'macro', set.bindings[key] ?? [], { button: pressed, trigger: 'press' })
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
    if ((firmwarePhase() === 'down' || restarting()) && !req.path.startsWith('/__mock/')) return { status: 0 }
    // Per-route limits (x-hubbo-max-body): restore takes a whole config; firmware images are streamed.
    const limit = req.path.startsWith('/admin/restore') ? RESTORE_MAX_BODY : MAX_BODY
    const firmware = req.path === '/admin/firmware/update' // the binary upload
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
      return known ? err(405, 'method_not_allowed', `${req.method} is not allowed here`) : unknownPath(`${req.method} ${req.path}`) // a path the API doesn't have, not a missing id
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
      // Like the hub, a request with no credentials is pointed at the agent guide (a wrong password isn't).
      if (!adminCreated) return err(401, 'unauthenticated', 'Finish first-run setup first', { docs: GUIDE_PATH }) // like the hub: setup_required is only a session field
      if (!sid) return err(401, 'unauthenticated', 'Sign in to continue', { docs: GUIDE_PATH })
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
