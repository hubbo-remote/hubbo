// Seeds the mock from api/fixtures, the contract's example data. The mock
// never invents its own devices or activities, so the UI is developed
// against exactly what the spec documents (and what hub-sim must return).
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import type {
  Activity,
  ActivityList,
  Backup,
  Capabilities,
  Device,
  DiscoverCandidate,
  ExecuteResult,
  FirmwareStatus,
  GlobalBindings,
  Integration,
  IntegrationConfig,
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
  devices: Device[]
  capabilities: Map<string, Capabilities>
  integrations: Integration[]
  configs: Map<string, IntegrationConfig>
  activities: Activity[]
  activeActivityId: string | null
  globalBindings: GlobalBindings
  tokens: TokenSummary[]
  /** The entities a discovery finds (fixture: integrations/ha_main.discovery.json). */
  discover: { candidates: DiscoverCandidate[] }
  firmware: FirmwareStatus
  backup: Backup
  /** The remote radio at boot (GET /admin/remote). */
  remote: RemoteConfig
  /** Non-accepted execute results; the mock returns them for their device + command. */
  executeResults: ExecuteResult[]
}

export function loadSeed(): Seed {
  const devices = fixture<{ devices: Device[] }>('devices/list.json').devices
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
      ['system', { id: 'system', type: 'system', name: 'System', enabled: true, settings: { type: 'system' }, revision: 1 }],
    ]),
    activities: acts.activities,
    activeActivityId: null, // a freshly started hub (like hub-sim) has no active activity
    globalBindings: fixture('activities/global_bindings.json'),
    tokens: fixture<{ tokens: TokenSummary[] }>('admin/tokens.json').tokens,
    discover: fixture('integrations/ha_main.discovery.json'),
    firmware: fixture('admin/firmware.json'),
    backup: fixture('admin/backup.json'),
    remote: fixture('admin/remote.json'),
    executeResults: [fixture<ExecuteResult>('devices/execute.timeout.json'), fixture<ExecuteResult>('devices/execute.failed.json')],
  }
}
