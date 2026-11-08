// Typed client for /api/v1 (api/openapi.yaml). Every request the UI makes
// goes through here. Session auth is an HttpOnly cookie the browser handles;
// the CSRF token is held in memory only (never localStorage/sessionStorage)
// and sent as X-Hubbo-CSRF on unsafe requests.
import type {
  Activity,
  ActivityDraft,
  ActivityList,
  ActionParams,
  ApiErrorBody,
  Backup,
  BlePairingStatus,
  Capabilities,
  Device,
  DeviceRegister,
  DeviceUpdate,
  DiscoverRequest,
  DiscoverResult,
  ExecuteResult,
  FirmwareStatus,
  GlobalBindings,
  Integration,
  IntegrationConfig,
  IntegrationConfigUpdate,
  RemotePairStatus,
  RestoreResult,
  Session,
  Status,
  TokenCreated,
  TokenSummary,
  TransitionResult,
  ValidationIssue,
  ValidationResult,
} from './types'

const API_BASE = '/api/v1'
const CSRF_HEADER = 'x-hubbo-csrf'

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
  return opts.details && err.details.length ? [err.message, ...err.details.map((d) => `· ${d.message}`)].join('\n') : err.message
}

let csrfToken: string | null = null
const remember = (s: Session) => {
  csrfToken = s.csrf_token ?? null
  return s
}

const unauthorizedListeners = new Set<() => void>()
export const onUnauthorized = (fn: () => void) => {
  unauthorizedListeners.add(fn)
  return () => void unauthorizedListeners.delete(fn)
}

const enc = encodeURIComponent

async function request<T>(method: string, path: string, body?: unknown, init: { raw?: BodyInit; headers?: Record<string, string> } = {}): Promise<T> {
  const headers: Record<string, string> = { accept: 'application/json', ...init.headers }
  if (method !== 'GET' && csrfToken) headers[CSRF_HEADER] = csrfToken
  let payload: BodyInit | undefined = init.raw
  if (body !== undefined) {
    headers['content-type'] = 'application/json'
    payload = JSON.stringify(body)
  } else if (init.raw) headers['content-type'] = 'application/octet-stream'
  let res: Response
  try {
    res = await fetch(API_BASE + path, { method, headers, body: payload, credentials: 'same-origin' })
  } catch {
    throw new ApiError(0, null, 'Cannot reach the hub')
  }
  if (res.status === 204) return undefined as T
  const text = await res.text()
  let json: unknown = null
  try {
    json = text ? JSON.parse(text) : null
  } catch {
    /* non-JSON error page */
  }
  if (!res.ok) {
    if (res.status === 401 && !path.startsWith('/auth/')) unauthorizedListeners.forEach((l) => l())
    const errBody = json && typeof json === 'object' && 'error' in json ? (json as ApiErrorBody) : null
    throw new ApiError(res.status, errBody, `${res.status} ${res.statusText}`)
  }
  return json as T
}

const get = <T>(p: string) => request<T>('GET', p)
const post = <T>(p: string, b?: unknown) => request<T>('POST', p, b)
const put = <T>(p: string, b: unknown) => request<T>('PUT', p, b)
const del = (p: string) => request<void>('DELETE', p)

/** Strip editor-only fields; PUT accepts id/revision/schema_version, POST does not. */
const activityBody = (a: ActivityDraft) => ({ name: a.name, startup: a.startup, shutdown: a.shutdown, bindings: a.bindings })

export const api = {
  // auth
  session: async () => remember(await get<Session>('/auth/session')),
  login: async (username: string, password: string) => remember(await post<Session>('/auth/login', { username, password })),
  logout: () => post<void>('/auth/logout'),
  setupAdmin: async (username: string, password: string) => remember(await post<Session>('/auth/setup', { username, password })),
  changePassword: (current_password: string, new_password: string) => put<void>('/admin/account/password', { current_password, new_password }),

  // status & remote
  status: () => get<Status>('/status'),

  // devices
  devices: async () => (await get<{ devices: Device[] }>('/devices')).devices,
  device: (id: string) => get<Device>(`/devices/${enc(id)}`),
  capabilities: (id: string) => get<Capabilities>(`/devices/${enc(id)}/capabilities`),
  execute: (id: string, command: string, params: ActionParams) => post<ExecuteResult>(`/devices/${enc(id)}/commands/${enc(command)}/execute`, { params }),
  registerDevice: (b: DeviceRegister) => post<Device>('/admin/devices', b),
  updateDevice: (id: string, b: DeviceUpdate) => put<Device>(`/admin/devices/${enc(id)}`, b),
  deleteDevice: (id: string, revision: number) => del(`/admin/devices/${enc(id)}?revision=${revision}`),

  // activities
  activities: () => get<ActivityList>('/activities'),
  activity: (id: string) => get<Activity>(`/activities/${enc(id)}`),
  createActivity: (a: ActivityDraft) => post<Activity>('/activities', activityBody(a)),
  replaceActivity: (id: string, a: ActivityDraft, revision: number) => put<Activity>(`/activities/${enc(id)}`, { ...activityBody(a), revision }),
  deleteActivity: (id: string, revision: number) => del(`/activities/${enc(id)}?revision=${revision}`),
  validateActivity: (a: ActivityDraft) => post<ValidationResult>('/activities/validate', activityBody(a)),
  activate: (id: string) => post<TransitionResult>(`/activities/${enc(id)}/activate`),
  off: () => post<TransitionResult>('/activities/off'),
  globalBindings: () => get<GlobalBindings>('/global-bindings'),
  replaceGlobalBindings: (g: GlobalBindings) => put<GlobalBindings>('/global-bindings', g),

  // integrations
  integrations: async () => (await get<{ integrations: Integration[] }>('/integrations')).integrations,
  integrationConfig: (id: string) => get<IntegrationConfig>(`/admin/integrations/${enc(id)}`),
  configureIntegration: (id: string, b: IntegrationConfigUpdate) => put<IntegrationConfig>(`/admin/integrations/${enc(id)}`, b),
  discover: (id: string, b: Partial<DiscoverRequest> = {}) => post<DiscoverResult>(`/admin/integrations/${enc(id)}/discover`, b),
  blePairStart: (id: string) => post<BlePairingStatus>(`/admin/integrations/${enc(id)}/pair`),
  bleForget: (id: string) => del(`/admin/integrations/${enc(id)}/pair`),

  // remote pairing
  remotePairStatus: () => get<RemotePairStatus>('/admin/remote/pair'),
  remotePairStart: () => post<RemotePairStatus>('/admin/remote/pair'),
  remotePairCancel: () => del('/admin/remote/pair'),

  // admin
  tokens: async () => (await get<{ tokens: TokenSummary[] }>('/admin/tokens')).tokens,
  createToken: (name: string) => post<TokenCreated>('/admin/tokens', { name }),
  revokeToken: (id: string) => del(`/admin/tokens/${enc(id)}`),
  backup: () => get<Backup>('/admin/backup'),
  restore: (backup: unknown, dryRun: boolean) => post<RestoreResult>(`/admin/restore?dry_run=${dryRun}`, backup),
  firmware: () => get<FirmwareStatus>('/admin/firmware'),
  uploadFirmware: async (file: File) => {
    const sha = await sha256Hex(file)
    return request<FirmwareStatus>('POST', '/admin/firmware/update?reboot=true', undefined, { raw: file, headers: sha ? { 'x-hubbo-image-sha256': sha } : {} })
  },
}

async function sha256Hex(file: Blob): Promise<string | null> {
  // crypto.subtle only exists in secure contexts (HTTPS or localhost).
  if (!globalThis.crypto?.subtle) return null
  const d = await crypto.subtle.digest('SHA-256', await file.arrayBuffer())
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, '0')).join('')
}
