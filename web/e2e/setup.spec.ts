import type { Page } from '@playwright/test'
import { ADMIN, advance, api, connectHa, control, expect, expectNotStoredInBrowser, FAKE_HA, fakeHaNextConnect, hubAnswersScan, letHaConnect, notAvailable, SIM_URL, test } from './hub.ts'

// hub-sim: a Hub answers the scan only via /sim/radio/found, and only the fake HA's token connects.
const HA_TOKEN = SIM_URL ? FAKE_HA.token : 'e2e-not-a-real-token'

test.use({ reset: { firstRun: true } })

async function createAdmin(page: Page) {
  await page.goto('/')
  await expect(page.getByRole('heading', { name: 'Set up Hubbo' })).toBeVisible()
  await expect(page.getByLabel('Username')).toHaveValue('') // no default: the user picks it
  await page.getByLabel('Username').fill(ADMIN.username)
  await page.getByLabel('Password', { exact: true }).fill(ADMIN.password)
  await page.getByLabel('Confirm password').fill('mismatch')
  // Errors wait for a submit, then follow the fields live.
  await expect(page.getByText('Passwords don’t match')).toHaveCount(0)
  await page.getByRole('button', { name: 'Create account' }).click()
  await expect(page.getByText('Passwords don’t match')).toBeVisible()
  await page.getByLabel('Confirm password').fill(ADMIN.password)
  await expect(page.getByText('Passwords don’t match')).toHaveCount(0)
  await page.getByRole('button', { name: 'Create account' }).click()
}

test('first-run setup: admin, name, network, remote, Home Assistant (save shows the result), done', async ({ page }) => {
  // 1. Admin
  await createAdmin(page)

  // 2. Name: the default to start with; spaces at the ends go; blank isn't a name.
  await expect(page.getByText('Step 2 of 6')).toBeVisible()
  const name = page.getByLabel('Hub name')
  await expect(name).toHaveValue('Hubbo')
  await expect(name).toHaveAttribute('data-1p-ignore', '') // not a login, though it follows one
  await name.fill('   ')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()
  await expect(page.getByText('Name is required')).toBeVisible()
  await name.fill('  Den Hub  ')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 3. Network (read-only)
  await expect(page.getByText('Step 3 of 6')).toBeVisible()
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 4. Remote pairing via an existing Hub
  await expect(page.getByText('Step 4 of 6')).toBeVisible()
  await page.getByRole('button', { name: 'Scan', exact: true }).click()
  if (SIM_URL) {
    await expect(page.getByRole('progressbar', { name: 'Scan progress' })).toBeVisible()
    await hubAnswersScan(page, 74, 'FC5C97550F')
  }
  await expect(page.getByText('Remote paired').or(notAvailable(page))).toBeVisible()
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 5. Home Assistant (hub-sim: not available yet, or no HA integration while the config is blocked; skip)
  await expect(page.getByText('Step 5 of 6')).toBeVisible()
  const haForm = page.getByLabel('URL', { exact: true })
  await expect(haForm.or(notAvailable(page)).or(page.getByText('Home Assistant isn’t available.'))).toBeVisible()
  const haSupported = await haForm.isVisible()
  if (haSupported) {
    // Save isn't Continue: it shows the connection result, and a rejected token leaves the form to fix.
    const conn = page.getByTestId('ha-connection')
    await fakeHaNextConnect(page, 'reject')
    await haForm.fill(FAKE_HA.url) // the mock's, or hub-sim's in-process fake: never a real HA
    await page.getByLabel('Access token').fill('e2e-wrong-token')
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await letHaConnect(page)
    await expect(conn).toContainText('The token was rejected')
    await expect(page.getByText('Step 5 of 6')).toBeVisible()

    await fakeHaNextConnect(page, 'ok')
    await page.getByLabel('Access token').fill(HA_TOKEN)
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await letHaConnect(page)
    await expect(conn).toContainText('Saved and connected.')
    await expect(page.getByText('Step 5 of 6')).toBeVisible()
    await page.getByRole('button', { name: 'Continue', exact: true }).click()
  } else {
    await page.getByRole('button', { name: 'Skip' }).click()
  }

  // 6. Done: a summary, nothing about devices or activities; it goes to Activities.
  await expect(page.getByText('Step 6 of 6')).toBeVisible()
  const summary = page.getByLabel('Setup summary')
  await expect(summary).toContainText(/Paired|Not paired/)
  await expect(summary).toContainText('Den Hub')
  if (haSupported) await expect(summary).toContainText('Connected')
  await expect(page.getByRole('region', { name: 'Done' })).not.toContainText(/device|activit/i)
  await page.getByRole('button', { name: 'Finish' }).click()
  await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible()
  expect((await api(page).get('/admin/hub')).name).toBe('Den Hub')
  // The tokens typed in step 5 are never shown back or kept in the browser.
  await expectNotStoredInBrowser(page, HA_TOKEN)
  await expectNotStoredInBrowser(page, 'e2e-wrong-token')
})

test('the wizard resumes from hub state after a reload, and keeps nothing in the browser', async ({ page }) => {
  const step = (n: number, title: string) => Promise.all([expect(page.getByText(`Step ${n} of 6`)).toBeVisible(), expect(page.getByRole('heading', { name: title })).toBeVisible()])
  await createAdmin(page)
  // Hub state can't say Name or Network was seen, so the URL holds them until Continue.
  await step(2, 'Name')
  await expect(page).toHaveURL(/#\/setup\/hub$/)
  await page.reload()
  await step(2, 'Name')
  await page.getByRole('button', { name: 'Continue', exact: true }).click() // the default, kept
  await step(3, 'Network')
  expect((await api(page).get('/admin/hub')).name).toBe('Hubbo')
  await expect(page).toHaveURL(/#\/setup\/network$/)
  await page.reload()
  await step(3, 'Network')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()
  await expect(page).toHaveURL(/#\/setup$/)

  // No endpoint yet: back to the remote. A known endpoint can be typed in instead of scanning.
  // (hub-sim starts paired, so its resume goes past the remote: open that step by its URL.)
  await page.reload()
  if (SIM_URL) await page.goto('/#/setup/remote')
  await step(4, 'Remote')
  await page.getByLabel('Channel').fill('125')
  await page.getByLabel('Address').fill('FC:5C:97:55:0F')
  await expect(page.getByText('A whole number from 0 to 124')).toHaveCount(0) // not before a submit
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(page.getByText('A whole number from 0 to 124')).toBeVisible()
  await page.getByLabel('Channel').fill('74')
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(page.getByText('Saved.', { exact: true })).toBeVisible()
  await expect(page.getByLabel('Channel')).toHaveValue('74') // the fields keep the saved endpoint
  await expect(page.getByLabel('Address')).toHaveValue('FC:5C:97:55:0F')
  await expect(page.getByTestId('press-check')).toContainText('Press any button on the remote')
  await page.waitForResponse((r) => r.url().endsWith('/remote/status')) // its baseline
  await advance(page, 1000)
  await control(page, 'remote/down/tap')
  await expect(page.getByTestId('press-check')).toContainText(/Down (pressed|released)/)

  // Paired, HA has no token: Home Assistant.
  if (SIM_URL) await page.goto('/#/setup') // leave the step opened by URL
  await page.reload()
  await step(5, 'Home Assistant')
  if (SIM_URL) await connectHa(page)
  else {
    await page.getByLabel('Access token').fill(HA_TOKEN)
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await expect(page.getByTestId('ha-connection')).toContainText('Saved and connected')
  }

  // HA set up (devices don't matter): Done.
  await page.reload()
  await step(6, 'Done')
  expect(await page.evaluate(() => Object.keys({ ...localStorage, ...sessionStorage }).filter((k) => k !== 'hubbo.theme'))).toEqual([])
})

test('once finished, the wizard never comes back on its own, even with pairing and HA skipped', async ({ page }) => {
  await createAdmin(page)
  for (let n = 2; n <= 5; n++) {
    await expect(page.getByText(`Step ${n} of 6`)).toBeVisible()
    await page.getByRole('button', { name: 'Skip', exact: true }).click()
  }
  await expect(page.getByText('Step 6 of 6')).toBeVisible()
  await page.getByRole('button', { name: 'Finish' }).click()
  await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible()
  await page.reload()
  await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible()
  await page.goto('/')
  await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible()
  await expect(page.getByRole('heading', { name: 'Set up Hubbo' })).toHaveCount(0)
})
