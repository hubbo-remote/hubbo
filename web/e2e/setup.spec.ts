import { ADMIN, advance, expect, expectNotStoredInBrowser, FAKE_HA, hubAnswersScan, notAvailable, SIM_URL, test } from './hub.ts'

// hub-sim: a Hub answers the scan only via /sim/radio/found, and only the fake HA's token connects.
const HA_TOKEN = SIM_URL ? FAKE_HA.token : 'e2e-not-a-real-token'
const ENTITY = SIM_URL ? 'media_player.denon' : 'switch.tv_backlight'

test.use({ reset: { firstRun: true } })

test('first-run setup: admin, network, remote, Home Assistant, first device', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByRole('heading', { name: 'Set up Hubbo' })).toBeVisible()

  // 1. Admin
  await page.getByLabel('Username').fill(ADMIN.username)
  await page.getByLabel('Password', { exact: true }).fill(ADMIN.password)
  await page.getByLabel('Confirm password').fill('mismatch')
  await expect(page.getByText('Passwords don’t match')).toBeVisible()
  await page.getByLabel('Confirm password').fill(ADMIN.password)
  await page.getByRole('button', { name: 'Create account' }).click()

  // 2. Network (read-only)
  await expect(page.getByText('Step 2 of 5')).toBeVisible()
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 3. Remote pairing via an existing Hub
  await expect(page.getByText('Step 3 of 5')).toBeVisible()
  await page.getByRole('button', { name: 'Scan from Hub' }).click()
  if (SIM_URL) {
    await expect(page.getByRole('progressbar', { name: 'Scan progress' })).toBeVisible()
    await hubAnswersScan(page, 74, 'FC5C97550F')
  }
  await expect(page.getByText('Remote paired').or(notAvailable(page))).toBeVisible()
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 4. Home Assistant (hub-sim: not available yet, or no HA integration while the config is blocked; skip)
  await expect(page.getByText('Step 4 of 5')).toBeVisible()
  const haForm = page.getByLabel('Home Assistant URL')
  await expect(haForm.or(notAvailable(page)).or(page.getByText('This hub has no Home Assistant integration.'))).toBeVisible()
  const haSupported = await haForm.isVisible()
  if (haSupported) {
    await haForm.fill(FAKE_HA.url) // the mock's, or hub-sim's in-process fake: never a real HA
    await page.getByLabel('Long-lived access token').fill(HA_TOKEN)
    await page.getByRole('button', { name: 'Save and connect' }).click()
    if (SIM_URL) await advance(page, 2_000) // sim time: let the HA client connect
  } else {
    await page.getByRole('button', { name: 'Skip' }).click()
  }

  // 5. Devices
  await expect(page.getByText('Step 5 of 5')).toBeVisible()
  await page.getByRole('button', { name: 'Add device' }).click()
  const dialog = page.getByRole('dialog')
  const haOption = dialog.getByRole('button', { name: /Home Assistant/ })
  // Built-ins only (e.g. while the config is blocked) means there's no Home Assistant to pick from.
  await expect(dialog.getByRole('button', { name: /Home Assistant|Bluetooth/ }).first().or(dialog.getByText('No integrations are enabled'))).toBeVisible()
  if (await haOption.isVisible()) await haOption.click()
  if (haSupported) {
    await dialog.getByLabel('Search Home Assistant entities').fill(ENTITY)
    await dialog.getByRole('button', { name: ENTITY }).click()
    await dialog.getByRole('button', { name: 'Register device' }).click()
    await expect(dialog).toBeHidden()
    await expect(page.getByText(ENTITY).first()).toBeVisible() // hub-sim names entities by id
  } else {
    if (await haOption.count()) await expect(notAvailable(dialog)).toBeVisible()
    await dialog.getByRole('button', { name: 'Cancel' }).click()
  }

  await page.getByRole('button', { name: 'Finish' }).click()
  await expect(page.getByRole('heading', { name: 'Dashboard' })).toBeVisible()
  // The token typed in step 4 is never shown back or kept in the browser.
  await expectNotStoredInBrowser(page, HA_TOKEN)
})
