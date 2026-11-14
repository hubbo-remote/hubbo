import type { Page } from '@playwright/test'
import { ADMIN, advance, api, connectHa, control, expect, expectNotStoredInBrowser, FAKE_HA, fakeHaNextConnect, hubAnswersScan, letHaConnect, notAvailable, settleIntegration, SIM_URL, test } from './hub.ts'

// hub-sim: a Hub answers the scan only via /sim/radio/found, and only the fake HA's token connects.
const HA_TOKEN = SIM_URL ? FAKE_HA.token : 'e2e-not-a-real-token'

test.use({ reset: { firstRun: true } })

/** The wizard is on step `n` of 7, titled `title`. */
const step = (page: Page, n: number, title: string) => Promise.all([expect(page.getByText(`Step ${n} of 7`)).toBeVisible(), expect(page.getByRole('heading', { name: title })).toBeVisible()])

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

test('first-run setup: admin, name, network, remote, Home Assistant and MQTT (save shows the result), done', async ({ page }) => {
  // 1. Admin
  await createAdmin(page)

  // 2. Name: the default to start with; spaces at the ends go; blank isn't a name.
  await expect(page.getByText('Step 2 of 7')).toBeVisible()
  const name = page.getByLabel('Hub name')
  await expect(name).toHaveValue('Hubbo 534D') // hub-sim's default, and the mock's
  await expect(name).toHaveAttribute('data-1p-ignore', '') // not a login, though it follows one
  await name.fill('   ')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()
  await expect(page.getByText('Name is required')).toBeVisible()
  await name.fill('  Den Hub  ')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 3. Network (read-only)
  await expect(page.getByText('Step 3 of 7')).toBeVisible()
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 4. Remote pairing via an existing Hub
  await expect(page.getByText('Step 4 of 7')).toBeVisible()
  await page.getByRole('button', { name: 'Scan', exact: true }).click()
  if (SIM_URL) {
    await expect(page.getByRole('progressbar', { name: 'Scan progress' })).toBeVisible()
    await hubAnswersScan(page, 74, 'FC5C97550F')
  }
  await expect(page.getByText('Remote paired').or(notAvailable(page))).toBeVisible()
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 5. Home Assistant (hub-sim: not available yet, or no HA integration while the config is blocked; skip)
  await expect(page.getByText('Step 5 of 7')).toBeVisible()
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
    await expect(page.getByText('Step 5 of 7')).toBeVisible()

    await fakeHaNextConnect(page, 'ok')
    await page.getByLabel('Access token').fill(HA_TOKEN)
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await letHaConnect(page)
    await expect(conn).toContainText('Saved and connected.')
    await expect(page.getByText('Step 5 of 7')).toBeVisible()
    await page.getByRole('button', { name: 'Continue', exact: true }).click()
  } else {
    await page.getByRole('button', { name: 'Skip' }).click()
  }

  // 6. MQTT: the same save → connection result as Home Assistant.
  await expect(page.getByText('Step 6 of 7')).toBeVisible()
  const broker = page.getByLabel('Broker')
  await expect(broker.or(notAvailable(page)).or(page.getByText('MQTT isn’t available.'))).toBeVisible()
  const mqttSupported = await broker.isVisible()
  if (mqttSupported) {
    await broker.fill('mqtt.local')
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await settleIntegration(page, 'mqtt_main', 'connected')
    await expect(page.getByTestId('mqtt-connection')).toContainText('Saved and connected.')
    await page.getByRole('button', { name: 'Continue', exact: true }).click()
  } else {
    await page.getByRole('button', { name: 'Skip' }).click()
  }

  // 7. Done: a summary, nothing about devices or activities; it goes to Activities.
  await expect(page.getByText('Step 7 of 7')).toBeVisible()
  const summary = page.getByLabel('Setup summary')
  await expect(summary).toContainText(/Paired|Not paired/)
  await expect(summary).toContainText('Den Hub')
  if (haSupported) await expect(summary).toContainText(/Home Assistant\s*Connected/)
  if (mqttSupported) await expect(summary).toContainText(/MQTT\s*Connected/)
  await expect(page.getByRole('region', { name: 'Done' })).not.toContainText(/device|activit/i)
  await page.getByRole('button', { name: 'Finish' }).click()
  await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible()
  expect((await api(page).get('/admin/hub')).name).toBe('Den Hub')
  // A new hub's one global button: Off turns everything off.
  expect((await api(page).get('/global-bindings')).bindings).toEqual({ 'off:tap': [{ integration_id: 'system', device_id: null, command_id: 'off', params: {} }] })
  // The tokens typed in step 5 are never shown back or kept in the browser.
  await expectNotStoredInBrowser(page, HA_TOKEN)
  await expectNotStoredInBrowser(page, 'e2e-wrong-token')
})

test('the wizard resumes from hub state after a reload, and keeps nothing in the browser', async ({ page }) => {
  await createAdmin(page)
  // Hub state can't say Name or Network was seen, so the URL holds them until Continue.
  await step(page, 2, 'Name')
  await expect(page).toHaveURL(/#\/setup\/hub$/)
  await page.reload()
  await step(page, 2, 'Name')
  await page.getByRole('button', { name: 'Continue', exact: true }).click() // the default, kept
  await step(page, 3, 'Network')
  expect(await api(page).get('/admin/hub')).toMatchObject({ name: 'Hubbo 534D', hostname: 'hubbo-534d' })
  await expect(page).toHaveURL(/#\/setup\/network$/)
  await page.reload()
  await step(page, 3, 'Network')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()
  await expect(page).toHaveURL(/#\/setup$/)

  // No endpoint yet: back to the remote. A known endpoint can be typed in instead of scanning.
  // (hub-sim starts paired, so its resume goes past the remote: open that step by its URL.)
  await page.reload()
  if (SIM_URL) await page.goto('/#/setup/remote')
  await step(page, 4, 'Remote')
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
  await step(page, 5, 'Home Assistant')
  if (SIM_URL) await connectHa(page)
  else {
    await page.getByLabel('Access token').fill(HA_TOKEN)
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await expect(page.getByTestId('ha-connection')).toContainText('Saved and connected')
  }

  // HA set up (devices don't matter), MQTT not yet: MQTT. Continue saves a typed broker.
  await page.reload()
  await step(page, 6, 'MQTT')
  await page.getByLabel('Broker').fill('mqtt.local')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()
  await step(page, 7, 'Done')
  await page.reload()
  await step(page, 7, 'Done')
  expect(await page.evaluate(() => Object.keys({ ...localStorage, ...sessionStorage }).filter((k) => k !== 'hubbo.theme'))).toEqual([])
})

test('once finished, the wizard never comes back on its own, even with pairing and HA skipped', async ({ page }) => {
  await createAdmin(page)
  for (let n = 2; n <= 6; n++) {
    await expect(page.getByText(`Step ${n} of 7`)).toBeVisible()
    await page.getByRole('button', { name: 'Skip', exact: true }).click()
  }
  await expect(page.getByText('Step 7 of 7')).toBeVisible()
  await page.getByRole('button', { name: 'Finish' }).click()
  await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible()
  await page.reload()
  await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible()
  await page.goto('/')
  await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible()
  await expect(page.getByRole('heading', { name: 'Set up Hubbo' })).toHaveCount(0)
})

test('Continue saves what’s typed on the remote, Home Assistant and MQTT steps', async ({ page }) => {
  await createAdmin(page)
  await step(page, 2, 'Name')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()
  await step(page, 3, 'Network')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // Remote: a half-typed endpoint stays on the step and says what's missing.
  await step(page, 4, 'Remote')
  await page.getByLabel('Channel').fill('74')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()
  await expect(page.getByText('Enter the address')).toBeVisible()
  await step(page, 4, 'Remote')
  await page.getByLabel('Address').fill('FC:5C:97:55:0F')
  await page.getByRole('button', { name: 'Continue', exact: true }).click()
  await step(page, 5, 'Home Assistant')
  expect((await api(page).get('/admin/remote')).endpoint).toEqual({ channel: 74, address: 'FC5C97550F' })

  // Home Assistant: saved without waiting for the connection.
  const haForm = page.getByLabel('URL', { exact: true })
  await expect(haForm.or(notAvailable(page)).or(page.getByText('Home Assistant isn’t available.'))).toBeVisible()
  if (await haForm.isVisible()) {
    await haForm.fill(FAKE_HA.url)
    await page.getByLabel('Access token').fill(HA_TOKEN)
    await page.getByRole('button', { name: 'Continue', exact: true }).click()
    await step(page, 6, 'MQTT')
    expect((await api(page).get('/admin/integrations/ha_main')).settings).toMatchObject({ url: FAKE_HA.url, has_access_token: true })
  } else await page.getByRole('button', { name: 'Skip' }).click()

  // MQTT, the same.
  await step(page, 6, 'MQTT')
  const broker = page.getByLabel('Broker')
  await expect(broker.or(notAvailable(page)).or(page.getByText('MQTT isn’t available.'))).toBeVisible()
  if (await broker.isVisible()) {
    await broker.fill('mqtt.local')
    await page.getByRole('button', { name: 'Continue', exact: true }).click()
    await step(page, 7, 'Done')
    expect((await api(page).get('/admin/integrations/mqtt_main')).settings).toMatchObject({ host: 'mqtt.local', port: 1883 })
  }
  await expectNotStoredInBrowser(page, HA_TOKEN)
})
