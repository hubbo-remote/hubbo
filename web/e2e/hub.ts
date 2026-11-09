// The hub behind the UI for e2e: the TypeScript mock (inside Vite) or
// hub-sim (HUBBO_API_TARGET). Every test starts from a fresh hub via the
// auto `hub` fixture; a spec file that needs first-run or login sets
// `test.use({ reset: {...} })` and is skipped where the target can't do it.
import { test as base, expect, type Locator, type Page } from '@playwright/test'
import { API_BASE, CSRF_HEADER } from '../src/api/constants.ts'
import type { Session } from '../src/api/types.ts'
import { fixture } from '../mock/fixtures.ts'
import type { ResetFlags } from '../mock/handler.ts'

export const SIM_URL = process.env.HUBBO_API_TARGET && process.env.HUBBO_API_TARGET !== 'mock' ? process.env.HUBBO_API_TARGET.replace(/\/$/, '') : null
export const ADMIN = fixture<{ username: string; password: string }>('auth/login.request.json')

const session = async (page: Page): Promise<Session> => (await page.request.get(`${API_BASE}/auth/session`)).json()
const csrf = (s: Session): Record<string, string> => (s.csrf_token ? { [CSRF_HEADER]: s.csrf_token } : {})

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
  // Credentials the hub can't read or trust come from how hub-sim was started, not from a reset.
  if (flags.authState) return s.auth_state === flags.authState
  const fresh = !!flags.firstRun && s.setup_required
  if (flags.firstRun && !fresh) return false // already set up
  if (flags.requireAuth && s.auth_mode !== 'enforced') return false // no-auth simulator
  if (flags.startupRejected) return false // hub-sim can't be told to reject a startup
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
  const r = await page.request.post(`${SIM_URL}/sim/reset`, { headers: csrf(s) })
  expect(r.ok(), `POST /sim/reset -> ${r.status()}`).toBeTruthy()
  if (flags.requireAuth) await page.request.post(`${API_BASE}/auth/logout`, { headers: csrf(s) })
  return true
}

export const test = base.extend<{ reset: ResetFlags; hub: void }>({
  reset: [{}, { option: true }],
  hub: [
    async ({ page, reset }, use, testInfo) => {
      testInfo.skip(!(await resetHub(page, reset)), 'this hub cannot start in that state (first run: fresh hub-sim --auth; login: --auth; blocked/corrupt: seeded with hub-sim write-record; storage: --storage-unavailable --auth)')
      await use()
    },
    { auto: true },
  ],
})

/** hub-sim with real authentication (`--auth`). */
export const onAuthSim = async (page: Page) => !!SIM_URL && (await session(page)).auth_mode === 'enforced'

/** API tokens need a real admin; the no-auth simulator starts without one, so create it. */
export async function ensureAdmin(page: Page) {
  const s = await session(page)
  if (s.auth_state !== 'fresh') return
  const r = await page.request.post(`${API_BASE}/auth/setup`, { data: ADMIN })
  expect(r.ok(), `setup for tokens -> ${r.status()}`).toBeTruthy()
}

/** Take the hub's config store down or bring it back (hub-sim: POST /sim/storage, an admin host route). */
export async function setStorage(page: Page, health: 'available' | 'unavailable' | 'read_only') {
  const url = SIM_URL ? `${SIM_URL}/sim/storage?health=${health}` : `${API_BASE}/__mock/storage?health=${health}`
  const r = await page.request.post(url, { headers: csrf(await session(page)) })
  expect(r.ok(), `storage ${health} -> ${r.status()}`).toBeTruthy()
}

export async function open(page: Page, hash: string) {
  await page.goto(`/#${hash}`)
  await expect(page.getByRole('navigation', { name: 'Main' }).first()).toBeVisible()
}

export const loginHeading = (page: Page) => page.getByRole('heading', { name: 'Sign in to Hubbo' })
export async function signIn(page: Page, password = ADMIN.password) {
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

/** JSON API calls as the UI makes them (session cookie + CSRF), sharing the page's session. */
export function api(page: Page) {
  let headers: Promise<Record<string, string>> | undefined
  const h = () => (headers ??= session(page).then(csrf))
  const get = async (path: string) => {
    const r = await page.request.get(`${API_BASE}${path}`)
    expect(r.ok(), `GET ${path} -> ${r.status()}`).toBeTruthy()
    return r.json()
  }
  return {
    get,
    put: async (path: string, data: unknown) => page.request.put(`${API_BASE}${path}`, { data, headers: await h() }),
    post: async (path: string, data?: unknown) => page.request.post(`${API_BASE}${path}`, { data, headers: await h() }),
    activeActivity: async (): Promise<string | null> => (await get('/status')).active_activity_id,
  }
}

export { expect }
