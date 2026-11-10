// Read-only checks against a real board serving the UI itself (its CSP, gzip and caching), outside
// the reset harness: nothing here changes the hub. Run on its own:
//   HUBBO_E2E_BASE_URL=http://<board>/ bin/hubbo web e2e --project=board
// With HUBBO_E2E_BOARD_PASSWORD it also signs in (a session, no config change) and opens the main
// pages read-only.
import { expect, test, type Page } from '@playwright/test'

test.skip(!process.env.HUBBO_E2E_BASE_URL, 'only against a board: HUBBO_E2E_BASE_URL=http://<board>/')

const PASSWORD = process.env.HUBBO_E2E_BOARD_PASSWORD
const USER = process.env.HUBBO_E2E_BOARD_USER ?? 'admin'

/** Collect CSP violations and console errors from the first script on. */
async function watch(page: Page) {
  const problems: string[] = []
  await page.addInitScript(() => {
    document.addEventListener('securitypolicyviolation', (e) => console.error(`CSP violation: ${e.violatedDirective} ${e.blockedURI}`))
  })
  page.on('console', (m) => m.type() === 'error' && problems.push(m.text()))
  page.on('pageerror', (e) => problems.push(`page error: ${e.message}`))
  page.on('requestfailed', (r) => problems.push(`request failed: ${r.url()} ${r.failure()?.errorText}`))
  return problems
}

test('the board serves the UI: it loads under its CSP, styled, with nothing blocked', async ({ page }) => {
  const problems = await watch(page)
  const res = await page.goto('/')
  expect(res?.headers()['content-security-policy']).toContain("default-src 'self'")
  await expect(page.getByRole('heading', { name: 'Sign in to Hubbo' }).or(page.getByRole('heading', { name: 'Dashboard' }))).toBeVisible()
  // Tailwind's stylesheet applied (the body isn't the browser's default white-on-nothing).
  expect(await page.evaluate(() => getComputedStyle(document.body).fontFamily)).not.toBe('Times New Roman')
  expect(await page.evaluate(() => getComputedStyle(document.documentElement).getPropertyValue('--background').trim())).not.toBe('')
  expect(problems.filter((p) => !/401|Unauthorized/i.test(p))).toEqual([])
})

test('signed in, the main pages render read-only with nothing blocked', async ({ page }) => {
  test.skip(!PASSWORD, 'set HUBBO_E2E_BOARD_PASSWORD to sign in')
  const problems = await watch(page)
  await page.goto('/')
  await page.getByLabel('Username').fill(USER)
  await page.getByLabel('Password').fill(PASSWORD!)
  await page.getByRole('button', { name: 'Sign in' }).click()
  await expect(page.getByRole('heading', { name: 'Dashboard' })).toBeVisible()
  for (const name of ['Remote', 'Bluetooth', 'Home Assistant', 'Ethernet']) await expect(page.getByText(name, { exact: true }).first()).toBeVisible()
  for (const [hash, heading] of [['/activities', 'Activities'], ['/devices', 'Devices'], ['/integrations', 'Integrations'], ['/settings/diagnostics', 'Settings']]) {
    await page.goto(`/#${hash}`)
    await expect(page.getByRole('heading', { name: heading, level: 1 })).toBeVisible()
  }
  expect(problems).toEqual([])
})
