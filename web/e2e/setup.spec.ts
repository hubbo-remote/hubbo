import { ADMIN, expect, expectNotStoredInBrowser, notAvailable, test } from './hub.ts'

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

  // 3. Remote pairing via an existing Hub (hub-sim: not available yet)
  await expect(page.getByText('Step 3 of 5')).toBeVisible()
  await page.getByRole('button', { name: 'Start scan' }).click()
  await expect(page.getByText('Remote paired').or(notAvailable(page))).toBeVisible()
  await page.getByRole('button', { name: 'Continue', exact: true }).click()

  // 4. Home Assistant (hub-sim: not available yet, or no HA integration while the config is blocked; skip)
  await expect(page.getByText('Step 4 of 5')).toBeVisible()
  const haForm = page.getByLabel('Home Assistant URL')
  await expect(haForm.or(notAvailable(page)).or(page.getByText('This hub has no Home Assistant integration.'))).toBeVisible()
  const haSupported = await haForm.isVisible()
  if (haSupported) {
    await haForm.fill('http://homeassistant.local:8123')
    await page.getByLabel('Long-lived access token').fill('e2e-not-a-real-token')
    await page.getByRole('button', { name: 'Save and connect' }).click()
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
    await dialog.getByRole('button', { name: /TV Backlight/ }).click()
    await dialog.getByRole('button', { name: 'Register device' }).click()
    await expect(dialog).toBeHidden()
    await expect(page.getByText('switch.tv_backlight')).toBeVisible()
  } else {
    if (await haOption.count()) await expect(notAvailable(dialog)).toBeVisible()
    await dialog.getByRole('button', { name: 'Cancel' }).click()
  }

  await page.getByRole('button', { name: 'Finish' }).click()
  await expect(page.getByRole('heading', { name: 'Dashboard' })).toBeVisible()
  // The token typed in step 4 is never shown back or kept in the browser.
  await expectNotStoredInBrowser(page, 'e2e-not-a-real-token')
})
