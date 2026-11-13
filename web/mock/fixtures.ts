// Seeds the mock from api/fixtures, the contract's example data. The mock
// never invents its own devices or activities, so the UI is developed
// against exactly what the spec documents (and what hub-sim must return).
import { readdirSync, readFileSync } from 'node:fs'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import type {
  Activity,
  ActivityList,
  Backup,
  Capabilities,
  DeviceDetail,
  DiscoverCandidate,
  ExecuteResult,
  FirmwareStatus,
  GlobalBindings,
  Integration,
  IntegrationConfig,
  Kind,
  Profile,
  ProfileSummary,
  RemoteButtons,
  RemoteConfig,
  Status,
  TokenSummary,
} from '../src/api/types.ts'

export const FIXTURES_DIR = process.env.HUBBO_FIXTURES_DIR ?? fileURLToPath(new URL('../../api/fixtures/', import.meta.url))

export function fixture<T>(rel: string): T {
  return JSON.parse(readFileSync(join(FIXTURES_DIR, rel), 'utf8')) as T
}

/** A fixture's bytes, untouched (the mock serves some bodies exactly as stored). */
export const fixtureBytes = (rel: string): Uint8Array => new Uint8Array(readFileSync(join(FIXTURES_DIR, rel)))

export interface Seed {
  status: Status
  buttons: RemoteButtons
  /** Stored in the detail form (GET /devices/{id}); the list strips the profile content. */
  devices: DeviceDetail[]
  capabilities: Map<string, Capabilities>
  integrations: Integration[]
  configs: Map<string, IntegrationConfig>
  activities: Activity[]
  activeActivityId: string | null
  globalBindings: GlobalBindings
  tokens: TokenSummary[]
  /** The entities a discovery finds (fixture: integrations/ha_main.discovery.json). */
  discover: { candidates: DiscoverCandidate[] }
  /** What registering a discovered entity stores, by entity id (fixtures: integrations/ha_main.entities/). */
  entityCapabilities: Map<string, Capabilities>
  firmware: FirmwareStatus
  backup: Backup
  /** The remote radio at boot (GET /admin/remote). */
  remote: RemoteConfig
  /** The built-in profiles (GET /profiles, /profiles/{id}) and the device kinds. */
  profiles: Profile[]
  profileList: ProfileSummary[]
  kinds: Kind[]
  /** Non-accepted execute results; the mock returns them for their device + command. */
  executeResults: ExecuteResult[]
}

const ENTITIES = 'integrations/ha_main.entities'
const CAPS = '.capabilities.json'

export function loadSeed(): Seed {
  const devices = fixture<{ devices: { id: string }[] }>('devices/list.json').devices.map((d) => fixture<DeviceDetail>(`devices/${d.id}.json`))
  // Profile files are named by the id's slug: hubbo:apple-tv → profiles/apple-tv.json.
  const profileList = fixture<{ profiles: ProfileSummary[] }>('profiles/list.json').profiles
  const profiles = profileList.map((p) => fixture<Profile>(`profiles/${p.id.split(':')[1]}.json`))
  const acts = fixture<ActivityList>('activities/list.json')
  return {
    status: fixture('status.json'),
    buttons: fixture('remote/buttons.json'),
    devices,
    capabilities: new Map(devices.map((d) => [d.id, fixture<Capabilities>(`devices/${d.id}.capabilities.json`)])),
    integrations: fixture<{ integrations: Integration[] }>('integrations/list.json').integrations,
    configs: new Map<string, IntegrationConfig>([
      ['ha_main', fixture<IntegrationConfig>('integrations/ha_main.config.json')],
      ['ble', fixture<IntegrationConfig>('integrations/ble.config.json')],
      ['mqtt_main', fixture<IntegrationConfig>('integrations/mqtt_main.config.json')],
      ['system', { id: 'system', type: 'system', name: 'System', enabled: true, settings: { type: 'system' }, revision: 1 }],
    ]),
    activities: acts.activities,
    activeActivityId: null, // a freshly started hub (like hub-sim) has no active activity
    globalBindings: fixture('activities/global_bindings.json'),
    tokens: fixture<{ tokens: TokenSummary[] }>('admin/tokens.json').tokens,
    discover: fixture('integrations/ha_main.discovery.json'),
    entityCapabilities: new Map(
      readdirSync(join(FIXTURES_DIR, ENTITIES))
        .filter((f) => f.endsWith(CAPS))
        .map((f) => [f.slice(0, -CAPS.length), fixture<Capabilities>(`${ENTITIES}/${f}`)]),
    ),
    firmware: fixture('admin/firmware.json'),
    backup: fixture('admin/backup.json'),
    remote: fixture('admin/remote.json'),
    profiles,
    profileList,
    kinds: fixture<{ kinds: Kind[] }>('profiles/kinds.json').kinds,
    // execute.failed.json (a Bluetooth host that isn't connected) isn't canned: the mock answers it from
    // the host's state, so the fixture device's commands work while it's connected.
    executeResults: [fixture<ExecuteResult>('devices/execute.timeout.json')],
  }
}
