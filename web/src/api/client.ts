// Typed client for /api/v1 (api/openapi.yaml). Every request the UI makes
// goes through here. Session auth is an HttpOnly, SameSite=Strict cookie the
// browser handles; there's no CSRF token (the hub checks Origin instead).
import type {
  Activity,
  ActivityInput,
  ActivityList,
  ActionParams,
  ApiErrorBody,
  BluetoothStatus,
  EntityDetailsStatus,
  Capabilities,
  ConfigState,
  Device,
  DeviceDetail,
  DeviceRegister,
  DeviceUpdate,
  DiscoverStatus,
  ExecuteResult,
  FirmwareStatus,
  Hub,
  GlobalBindings,
  Integration,
  IntegrationConfig,
  IntegrationConfigUpdate,
  Kind,
  ProfileSummary,
  RemoteConfig,
  RemoteEndpointUpdate,
  RemotePairStatus,
  RemoteStatus,
  RestoreResult,
  Session,
  Status,
  TokenCreated,
  TokenSummary,
  TransitionResult,
  ValidationIssue,
} from './types'

import { API_BASE } from './constants'

export class ApiError extends Error {
  readonly status: number
  readonly code: string
  readonly details: ValidationIssue[]
  readonly body: ApiErrorBody | null
  constructor(status: number, body: ApiErrorBody | null, fallback: string) {
    super(body?.error.message ?? fallback)
    this.status = status
    this.code = body?.error.code ?? (status === 0 ? 'network_error' : 'http_error')
    this.details = body?.error.details ?? []
    this.body = body
  }
}

export const toApiError = (e: unknown): ApiError => (e instanceof ApiError ? e : new ApiError(0, null, e instanceof Error ? e.message : String(e)))

/** Human message for any thrown value; with `details`, appends the hub's per-field messages. */
export function errorMessage(e: unknown, opts: { details?: boolean } = {}): string {
  const err = toApiError(e)
  const extra = opts.details ? err.details.filter((d) => d.message !== err.message) : [] // the hub may repeat its one reason
  return extra.length ? [err.message, ...extra.map((d) => `· ${d.message}`)].join('\n') : err.message
}

/** The hub answered 501: the feature exists in the API but this hub (e.g. hub-sim today) doesn't implement it yet. */
export const isNotImplemented = (e: unknown) => toApiError(e).status === 501

const unauthorizedListeners = new Set<() => void>()
export const onUnauthorized = (fn: () => void) => {
  unauthorizedListeners.add(fn)
  return () => void unauthorizedListeners.delete(fn)
}

const enc = encodeURIComponent

type RequestInit_ = { raw?: BodyInit; headers?: Record<string, string> }

/** Sends one request; throws ApiError for network failures and non-2xx answers. */
async function send(method: string, path: string, body?: unknown, init: RequestInit_ = {}): Promise<Response> {
  const headers: Record<string, string> = { accept: 'application/json', ...init.headers }
  let payload: BodyInit | undefined = init.raw
  if (body !== undefined) {
    headers['content-type'] = 'application/json'
    payload = JSON.stringify(body)
  } else if (init.raw) headers['content-type'] ??= 'application/octet-stream'
  let res: Response
  try {
    res = await fetch(API_BASE + path, { method, headers, body: payload, credentials: 'same-origin' })
  } catch {
    throw new ApiError(0, null, 'Can’t reach Hubbo')
  }
  if (res.ok) return res
  throw failure(path, res.status, res.statusText, await res.text().catch(() => ''))
}

/** The ApiError for a non-2xx answer; tells the app when the session needs re-checking. */
function failure(path: string, status: number, statusText: string, text: string): ApiError {
  let json: unknown = null
  try {
    json = text ? JSON.parse(text) : null
  } catch {
    /* non-JSON error page */
  }
  // 401, or a 503 storage_unavailable (under auth nobody can authenticate while the
  // credentials store is unreadable): re-check the session so the right screen shows.
  const storageDown = status === 503 && (json as ApiErrorBody | null)?.error?.code === 'storage_unavailable'
  if ((status === 401 || storageDown) && !path.startsWith('/auth/')) unauthorizedListeners.forEach((l) => l())
  const errBody = json && typeof json === 'object' && 'error' in json ? (json as ApiErrorBody) : null
  return new ApiError(status, errBody, `${status} ${statusText}`)
}

/**
 * POST /admin/firmware/update with XHR, the only way to see upload progress (fetch can't).
 * The browser sets Content-Length; `onProgress` gets 0..1.
 */
function uploadFirmware(file: Blob, onProgress: (fraction: number) => void): Promise<FirmwareStatus> {
  const path = '/admin/firmware/update'
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest()
    xhr.open('POST', API_BASE + path)
    xhr.setRequestHeader('content-type', 'application/octet-stream')
    xhr.setRequestHeader('accept', 'application/json')
    xhr.upload.onprogress = (e) => e.lengthComputable && onProgress(e.loaded / e.total)
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        try {
          return resolve(JSON.parse(xhr.responseText) as FirmwareStatus)
        } catch {
          return reject(new ApiError(xhr.status, null, 'Hubbo sent an answer that couldn’t be read'))
        }
      }
      reject(failure(path, xhr.status, xhr.statusText, xhr.responseText))
    }
    xhr.onerror = () => reject(new ApiError(0, null, 'Can’t reach Hubbo'))
    xhr.send(file)
  })
}

async function request<T>(method: string, path: string, body?: unknown, init: RequestInit_ = {}): Promise<T> {
  const res = await send(method, path, body, init)
  if (res.status === 204) return undefined as T
  const text = await res.text()
  try {
    return (text ? JSON.parse(text) : null) as T
  } catch {
    return null as T
  }
}

/** What `GET /admin/backup` sent (`X-Hubbo-Backup`). */
export type BackupKind = 'config' | 'blocked-raw'
export interface BackupDownload {
  kind: BackupKind
  /** The body exactly as the hub sent it: never parsed or re-serialized. */
  blob: Blob
  filename: string
}

/** `filename="…"` from Content-Disposition, reduced to a safe base name. */
export function dispositionFilename(header: string | null): string | null {
  const m = header ? /filename\s*=\s*(?:"([^"]*)"|([^;\s]+))/i.exec(header) : null
  const base = [...((m?.[1] ?? m?.[2])?.split(/[\\/]/).pop() ?? '')].filter((c) => c >= ' ').join('').trim()
  return base && base !== '.' && base !== '..' ? base : null
}

async function downloadBackup(): Promise<BackupDownload> {
  const res = await send('GET', '/admin/backup', undefined, { headers: { accept: 'application/json, application/octet-stream' } })
  const kind = res.headers.get('x-hubbo-backup')
  if (kind !== 'config' && kind !== 'blocked-raw') throw new ApiError(res.status, null, 'The backup couldn’t be saved. Try again.')
  const blob = await res.blob()
  if (blob.size === 0) throw new ApiError(res.status, null, 'The backup was empty. Try again.')
  const date = new Date().toISOString().slice(0, 10)
  const filename = dispositionFilename(res.headers.get('content-disposition')) ?? (kind === 'blocked-raw' ? `hubbo-backup-BLOCKED-${date}.json` : `hubbo-backup-${date}.json`)
  return { kind, blob, filename }
}

const get = <T>(p: string) => request<T>('GET', p)
const post = <T>(p: string, b?: unknown) => request<T>('POST', p, b)
const put = <T>(p: string, b: unknown) => request<T>('PUT', p, b)
const del = (p: string) => request<void>('DELETE', p)

/** Strip editor-only fields; PUT accepts id/revision/schema_version, POST does not. */
const activityBody = (a: ActivityInput) => ({ name: a.name, startup: a.startup, shutdown: a.shutdown, bindings: a.bindings })

export const api = {
  // auth
  session: () => get<Session>('/auth/session'),
  login: (username: string, password: string) => post<Session>('/auth/login', { username, password }),
  logout: () => post<void>('/auth/logout'),
  setupAdmin: (username: string, password: string) => post<Session>('/auth/setup', { username, password }),
  changePassword: (current_password: string, new_password: string) => put<void>('/admin/account/password', { current_password, new_password }),

  // status & remote
  status: () => get<Status>('/status'),

  // devices
  devices: async () => (await get<{ devices: Device[] }>('/devices')).devices,
  /** One device in the detail form: a Bluetooth device carries its profile (commands, power, inputs…). */
  device: (id: string) => get<DeviceDetail>(`/devices/${enc(id)}`),
  capabilities: (id: string) => get<Capabilities>(`/devices/${enc(id)}/capabilities`),
  execute: (id: string, command: string, params: ActionParams) => post<ExecuteResult>(`/devices/${enc(id)}/commands/${enc(command)}/execute`, { params }),
  registerDevice: (b: DeviceRegister) => post<DeviceDetail>('/admin/devices', b),
  profiles: async () => (await get<{ profiles: ProfileSummary[] }>('/profiles')).profiles,
  kinds: async () => (await get<{ kinds: Kind[] }>('/kinds')).kinds,
  updateDevice: (id: string, b: DeviceUpdate) => put<Device>(`/admin/devices/${enc(id)}`, b),
  deleteDevice: (id: string, revision: number) => del(`/admin/devices/${enc(id)}?revision=${revision}`),

  // activities
  activities: () => get<ActivityList>('/activities'),
  activity: (id: string) => get<Activity>(`/activities/${enc(id)}`),
  createActivity: (a: ActivityInput) => post<Activity>('/activities', activityBody(a)),
  replaceActivity: (id: string, a: ActivityInput, revision: number) => put<Activity>(`/activities/${enc(id)}`, { ...activityBody(a), revision }),
  deleteActivity: (id: string, revision: number) => del(`/activities/${enc(id)}?revision=${revision}`),
  activate: (id: string) => post<TransitionResult>(`/activities/${enc(id)}/activate`),
  off: () => post<TransitionResult>('/activities/off'),
  globalBindings: () => get<GlobalBindings>('/global-bindings'),
  replaceGlobalBindings: (g: GlobalBindings) => put<GlobalBindings>('/global-bindings', g),

  // integrations
  integrations: async () => (await get<{ integrations: Integration[] }>('/integrations')).integrations,
  integrationConfig: (id: string) => get<IntegrationConfig>(`/admin/integrations/${enc(id)}`),
  configureIntegration: (id: string, b: IntegrationConfigUpdate) => put<IntegrationConfig>(`/admin/integrations/${enc(id)}`, b),
  /** Discovery runs on the hub's integration task: start it, then poll `discovery` (same as a remote scan). */
  discoverStart: (id: string) => post<DiscoverStatus>(`/admin/integrations/${enc(id)}/discover`),
  discovery: (id: string, f: { q?: string; domain?: string; available?: boolean; limit?: number; cursor?: string } = {}) => {
    // encodeURIComponent, not URLSearchParams: a space must go as %20 (URLSearchParams writes `+`).
    const qs = Object.entries(f)
      .flatMap(([k, v]) => (v === undefined || v === '' ? [] : [`${k}=${encodeURIComponent(String(v))}`]))
      .join('&')
    return get<DiscoverStatus>(`/admin/integrations/${enc(id)}/discover${qs ? `?${qs}` : ''}`)
  },
  discoverCancel: (id: string) => del(`/admin/integrations/${enc(id)}/discover`),
  entityDetailsStart: (integration: string, entityId: string) => post<EntityDetailsStatus>(`/admin/integrations/${enc(integration)}/entities/${enc(entityId)}/details`),
  entityDetails: (integration: string, entityId: string) => get<EntityDetailsStatus>(`/admin/integrations/${enc(integration)}/entities/${enc(entityId)}/details`),
  /** A Bluetooth device's pairing window: its status, open it, cancel it (an existing host stays). */
  pairStatus: (deviceId: string) => get<BluetoothStatus>(`/admin/devices/${enc(deviceId)}/pair`),
  pairStart: (deviceId: string) => post<BluetoothStatus>(`/admin/devices/${enc(deviceId)}/pair`),
  pairCancel: (deviceId: string) => del(`/admin/devices/${enc(deviceId)}/pair`),
  /** Forget a Bluetooth device's host; the device stays, not paired. */
  forgetHost: (deviceId: string) => del(`/admin/devices/${enc(deviceId)}/pairing`),

  // remote pairing
  remotePairStatus: () => get<RemotePairStatus>('/admin/remote/pair'),
  remotePairStart: () => post<RemotePairStatus>('/admin/remote/pair'),
  remotePairCancel: () => del('/admin/remote/pair'),
  remote: () => get<RemoteConfig>('/admin/remote'),
  remoteStatus: () => get<RemoteStatus>('/remote/status'),
  setRemoteEndpoint: (b: RemoteEndpointUpdate) => put<RemoteConfig>('/admin/remote/endpoint', b),

  // admin
  tokens: async () => (await get<{ tokens: TokenSummary[] }>('/admin/tokens')).tokens,
  createToken: (name: string) => post<TokenCreated>('/admin/tokens', { name }),
  revokeToken: (id: string) => del(`/admin/tokens/${enc(id)}`),
  /** The backup download, as an opaque blob: a `Backup`, or (`blocked-raw`) the saved record's raw bytes. */
  downloadBackup,
  /** Sends the file's text exactly as chosen: a `Backup` or a `blocked-raw` record (RestoreRequest). */
  restore: (text: string, dryRun: boolean) => request<RestoreResult>('POST', `/admin/restore?dry_run=${dryRun}`, undefined, { raw: text, headers: { 'content-type': 'application/json' } }),
  /** Devices, activities, global buttons and integration settings go; the login, tokens and pairings stay. */
  resetConfiguration: () => post<ConfigState>('/admin/reset'),
  /** Erases everything (202); the hub restarts into first-run setup. */
  factoryReset: () => post<void>('/admin/factory-reset'),
  firmware: () => get<FirmwareStatus>('/admin/firmware'),
  hub: () => get<Hub>('/admin/hub'),
  renameHub: (name: string) => put<Hub>('/admin/hub', { name }),
  /** Restarts the hub (202); every session ends. 409 `update_in_progress` while an update runs. */
  restart: () => post<void>('/admin/restart'),
  uploadFirmware,
}
