// The hub behind the UI for e2e: the TypeScript mock (inside Vite) or
// hub-sim (HUBBO_API_TARGET). Every test starts from a fresh hub via the
// auto `hub` fixture; a spec file that needs first-run or login sets
// `test.use({ reset: {...} })` and is skipped where the target can't do it.
import { test as base, expect, type Locator, type Page } from '@playwright/test'
import { API_BASE } from '../src/api/constants.ts'
import type { Session } from '../src/api/types.ts'
import { fixture } from '../mock/fixtures.ts'
import type { ResetFlags } from '../mock/handler.ts'

export const SIM_URL = process.env.HUBBO_API_TARGET && process.env.HUBBO_API_TARGET !== 'mock' ? process.env.HUBBO_API_TARGET.replace(/\/$/, '') : null
/** The admin e2e sets up on a fresh hub, and the one a set-up mock has. */
export const ADMIN = fixture<{ username: string; password: string }>('auth/login.request.json')

export const session = async (page: Page): Promise<Session> => (await page.request.get(`${API_BASE}/auth/session`)).json()

/**
 * Fresh hub state, through `page.request` so the page shares the session.
 * Returns false when the target can't be put in that state.
 */
async function resetHub(page: Page, flags: ResetFlags): Promise<boolean> {
  if (!SIM_URL) {
    expect((await page.request.post(`${API_BASE}/__mock/reset`, { data: flags })).status()).toBe(204)
    return true
  }
  // hub-sim keeps the admin across /sim/reset, and under --auth needs a session for it.
  let s = await session(page)
  // Credentials the hub can't read come from how hub-sim was started, not from a reset.
  if (flags.authState) return s.auth_state === flags.authState
  const fresh = !!flags.firstRun && s.setup_required
  if (flags.firstRun && !fresh) return false // already set up
  if (flags.requireAuth && s.auth_mode !== 'enforced') return false // no-auth simulator
  if (flags.startupRejected) return false // hub-sim can't be told to reject a startup
  // Mock-only scenarios: hub-sim's fake HA and simulated remote are driven differently.
  // (A scan that never finds is hub-sim's normal behaviour: only /sim/radio/found answers it.)
  if (flags.discoverFails || flags.discoverTruncated) return false
  // A blocked config comes from seeding hub-sim's --data dir (hub-sim write-record), not from a reset.
  if (flags.configState && s.config_state !== flags.configState) return false
  if (fresh) return true
  if (s.auth_mode === 'enforced' && !s.authenticated) {
    const r = await page.request.post(`${API_BASE}/auth/${s.setup_required ? 'setup' : 'login'}`, { data: ADMIN })
    expect(r.ok(), `sign-in for reset -> ${r.status()}`).toBeTruthy()
    s = await r.json()
  }
  if (flags.configState) {
    // Resetting would replace the seeded config. A blocked spec runs only on its own seed (damaged or not).
    if (flags.configState !== 'blocked') return true
    const damaged = /damaged/i.test((await (await page.request.get(`${API_BASE}/status`)).json()).config?.reason ?? '')
    return damaged === !!flags.configDamaged
  }
  const r = await page.request.post(`${SIM_URL}/sim/reset`)
  expect(r.ok(), `POST /sim/reset -> ${r.status()}`).toBeTruthy()
  if (flags.requireAuth) await page.request.post(`${API_BASE}/auth/logout`)
  return true
}

export const test = base.extend<{ reset: ResetFlags; hub: void }>({
  reset: [{}, { option: true }],
  hub: [
    async ({ page, reset }, use, testInfo) => {
      if (testInfo.project.name === 'noauth') testInfo.skip((await session(page)).auth_mode === 'enforced', 'the noauth project is for hub-sim without auth')
      testInfo.skip(!(await resetHub(page, reset)), 'this hub cannot start in that state (first run: fresh hub-sim --auth; login: --auth; blocked: seeded with hub-sim write-record; storage: --storage-unavailable --auth)')
      await use()
    },
    { auto: true },
  ],
})


/** API tokens need a real admin; the no-auth simulator starts without one, so create it. */
export async function ensureAdmin(page: Page) {
  const s = await session(page)
  if (s.auth_state !== 'fresh') return
  const r = await page.request.post(`${API_BASE}/auth/setup`, { data: ADMIN })
  expect(r.ok(), `setup for tokens -> ${r.status()}`).toBeTruthy()
}

/** hub-sim's in-process fake Home Assistant (never a real one): its URL and the only token it accepts. */
export const FAKE_HA = { url: 'http://homeassistant.local:8123', token: 'fake-ha-token' }

/** The HA client's state as /status reports it. */
const haState = async (page: Page) => ((await (await page.request.get(`${API_BASE}/status`)).json()).integrations as { id: string; state: string }[]).find((i) => i.id === 'ha_main')?.state

/**
 * hub-sim runs on simulated time: after a settings change or a reconnect the HA client waits out
 * its backoff (1 s, doubling) only as sim time passes. Nudge it, then wait for `state`.
 */
async function settleHa(page: Page, state: string) {
  await advance(page, 2_000)
  await expect.poll(() => haState(page), { message: `HA client -> ${state}` }).toBe(state)
}

/** Point ha_main at the fake HA with its token (hub-sim seeds no token). The mock starts connected. */
export async function connectHa(page: Page, token = FAKE_HA.token) {
  if (!SIM_URL) return
  const hub = api(page)
  const cfg = await hub.get('/admin/integrations/ha_main')
  const r = await hub.put('/admin/integrations/ha_main', { revision: cfg.revision, settings: { type: 'home_assistant', url: FAKE_HA.url, access_token: token } })
  expect(r.ok(), `configure HA -> ${r.status()}`).toBeTruthy()
  await settleHa(page, token === FAKE_HA.token ? 'connected' : 'auth_failed')
}

/** POST a test control: hub-sim's /sim/<path> or the mock's /__mock/<path>. */
export async function control(page: Page, path: string) {
  const r = await page.request.post(`${SIM_URL ? `${SIM_URL}/sim` : `${API_BASE}/__mock`}/${path}`)
  expect(r.ok(), `control ${path} -> ${r.status()}`).toBeTruthy()
  return r.status() === 204 ? undefined : r.json()
}

/** hub-sim's fake-HA settings for each state (it can't hold HA in authenticating). */
const SIM_HA_STATE: Partial<Record<HaState, string>> = {
  connecting: 'reachable=1&connect=stall',
  connected: 'reachable=1&connect=ok&auth=ok',
  auth_failed: 'reachable=1&connect=ok&auth=reject',
  unreachable: 'reachable=0',
}
type HaState = 'connected' | 'connecting' | 'unreachable' | 'auth_failed' | 'authenticating'

/** Put the HA client in a state: on hub-sim through its fake HA (/sim/ha); on the mock directly. */
export async function setHaState(page: Page, state: HaState) {
  if (!SIM_URL) return control(page, `ha?state=${state}`)
  const fakeHa = SIM_HA_STATE[state]
  expect(fakeHa, `hub-sim can’t hold HA in ${state}`).toBeDefined()
  await control(page, `ha?${fakeHa}`)
  await settleHa(page, state)
}

/**
 * How the fake HA answers the next connect, after the settings are saved: accept, reject the token,
 * or stall (never answer). hub-sim: /sim/ha; the mock: /__mock/ha, which takes the same parameters.
 */
export const fakeHaNextConnect = (page: Page, how: 'ok' | 'reject' | 'stall') =>
  control(page, `ha?${SIM_HA_STATE[({ ok: 'connected', reject: 'auth_failed', stall: 'connecting' } as const)[how]]}`)

/** Let the HA client connect after a save (hub-sim runs on sim time; the mock answers at once). */
export const letHaConnect = async (page: Page) => {
  if (SIM_URL) await advance(page, 2_000)
}

/** A host bonds in that device's open pairing window, and connects (hub-sim: /sim/ble/bond). */
export const bondHost = (page: Page, deviceId: string) => control(page, `ble/bond?device=${deviceId}`)
/** A paired device's host drops or comes back (hub-sim: /sim/ble/link). */
export const setHostLink = (page: Page, deviceId: string, up: boolean) => control(page, `ble/link?device=${deviceId}&up=${up ? 1 : 0}`)
/** The Bluetooth radio goes down or comes back (hub-sim: /sim/ble/radio). */
export const setBleRadio = (page: Page, ready: boolean) => control(page, `ble/radio?ready=${ready ? 1 : 0}`)

/** Run a discovery and wait for it to finish (register and re-link use the latest one). */
export async function discoverHa(page: Page) {
  const hub = api(page)
  expect((await hub.post('/admin/integrations/ha_main/discover')).status()).toBe(202)
  await expect.poll(async () => (await hub.get('/admin/integrations/ha_main/discover')).state).toBe('done')
}

/** Add a Bluetooth device from a built-in profile through the API; returns its id. */
export async function addBle(page: Page, name: string, profile = 'hubbo:fire-tv'): Promise<string> {
  const r = await api(page).post('/admin/devices', { profile_id: profile, name })
  expect(r.status(), await r.text()).toBe(201)
  return (await r.json()).id
}

/** Register an HA entity as a device through the API: its details first (§3b), then POST /admin/devices. */
export async function registerHa(page: Page, entityId: string, name: string, kind = 'other') {
  const hub = api(page)
  const path = `/admin/integrations/ha_main/entities/${entityId}/details`
  expect((await hub.post(path)).status()).toBe(202)
  await expect.poll(async () => (await hub.get(path)).state).toBe('done')
  const r = await hub.post('/admin/devices', { integration_id: 'ha_main', provider: { type: 'home_assistant', entity_id: entityId }, name, kind })
  expect(r.status(), await r.text()).toBe(201)
  return (await r.json()) as { id: string }
}

/**
 * HA forgets an entity, and a discovery runs so the hub notices: its device goes stale.
 * `renamedTo` (mock only): the entity comes back under a new id, as after a rename in HA.
 */
export async function removeHaEntity(page: Page, entityId: string, renamedTo?: string) {
  expect(!SIM_URL || !renamedTo, 'hub-sim can’t rename an entity').toBeTruthy()
  await control(page, renamedTo ? `ha?rename=${encodeURIComponent(entityId)}&to=${encodeURIComponent(renamedTo)}` : `ha?remove=${encodeURIComponent(entityId)}`)
  await discoverHa(page)
}

/** A Hub answers the running remote scan (hub-sim: /sim/radio/found). */
export const hubAnswersScan = (page: Page, channel: number, address: string) => control(page, `radio/found?channel=${channel}&address=${address}`)

/** Move the hub's clock forward (scan timeouts). */
export const advance = (page: Page, ms: number) => control(page, `advance?ms=${ms}`)

/** Take the hub's config store down or bring it back (hub-sim: POST /sim/storage, an admin host route). */
export const setStorage = (page: Page, health: 'available' | 'unavailable' | 'read_only') => control(page, `storage?health=${health}`)

/** Pick a firmware file in Status → Firmware. */
export async function chooseFirmware(page: Page, image: Uint8Array | string) {
  await page.getByLabel('Firmware file').setInputFiles({ name: 'firmware.bin', mimeType: 'application/octet-stream', buffer: Buffer.from(image) })
}

export async function open(page: Page, hash: string) {
  await page.goto(`/#${hash}`)
  await expect(page.getByRole('navigation', { name: 'Main' }).first()).toBeVisible()
}

export const loginHeading = (page: Page) => page.getByRole('heading', { name: 'Sign in to Hubbo' })
export async function signIn(page: Page, password = ADMIN.password) {
  await page.getByLabel('Username').fill(ADMIN.username)
  await page.getByLabel('Password').fill(password)
  await page.getByRole('button', { name: 'Sign in' }).click()
}

/** The calm 501 panel (NotAvailable). */
export const notAvailable = (scope: Page | Locator) => scope.getByRole('status').filter({ hasText: 'Not available yet' })

/** On the mock, answer 501 where hub-sim does today; on hub-sim the 501s are real. */
export async function stubUnimplemented(page: Page) {
  if (SIM_URL) return
  const notImplemented = { status: 501, json: { error: { code: 'not_implemented', message: 'Not implemented yet' } } }
  for (const path of ['admin/firmware**', 'admin/integrations/**', 'admin/remote/pair']) await page.route(`**${API_BASE}/${path}`, (r) => r.fulfill(notImplemented))
}

/** A secret must not be on the page or in browser storage. */
export async function expectNotStoredInBrowser(page: Page, secret: string) {
  expect(await page.content()).not.toContain(secret)
  expect(await page.evaluate(() => JSON.stringify({ ...localStorage, ...sessionStorage }))).not.toContain(secret)
}

/** JSON API calls as the UI makes them (the session cookie), sharing the page's session. */
export function api(page: Page) {
  const get = async (path: string) => {
    const r = await page.request.get(`${API_BASE}${path}`)
    expect(r.ok(), `GET ${path} -> ${r.status()}`).toBeTruthy()
    return r.json()
  }
  return {
    get,
    put: async (path: string, data: unknown) => page.request.put(`${API_BASE}${path}`, { data }),
    post: async (path: string, data?: unknown) => page.request.post(`${API_BASE}${path}`, { data }),
    del: async (path: string) => page.request.delete(`${API_BASE}${path}`),
    activeActivity: async (): Promise<string | null> => (await get('/status')).active_activity_id,
  }
}

export { expect }
