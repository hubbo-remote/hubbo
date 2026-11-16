import type { Page } from '@playwright/test'
import { api, control, expect, expectNotStoredInBrowser, open, settleIntegration, test } from './hub.ts'

// MQTT: the broker Hubbo shows up through in Home Assistant. Never a real broker: hub-sim's fake
// one, or the mock's state. The broker traffic itself is the contract's job; this is the form.

const PASSWORD = 'e2e-mqtt-password'

const settle = (page: Page, state: string) => settleIntegration(page, 'mqtt_main', state)

/** The MQTT form on Integrations. */
async function openCard(page: Page) {
  await open(page, '/integrations')
  return page.locator('form').filter({ has: page.getByLabel('Broker') })
}

test('setting up the broker connects, and the password is write-only', async ({ page }) => {
  const card = await openCard(page)
  await card.getByLabel('Broker').fill('mqtt.local')
  await expect(card.getByLabel('Port')).toHaveValue('1883')
  await card.getByLabel('Username').fill('hubbo')
  await card.getByLabel('Password').fill(PASSWORD)
  await card.getByRole('button', { name: 'Save' }).click()
  await settle(page, 'connected')
  await expect(page.getByTestId('mqtt-connection')).toContainText('Saved and connected.')
  await expect(page.getByTestId('state-mqtt_main')).toContainText('Connected')

  // Never read back: the field is empty, the hub says only that one is saved.
  await page.reload()
  await expect(card.getByLabel('Password')).toHaveValue('')
  await expect(card.getByText('Leave blank to keep the saved password.')).toBeVisible()
  const cfg = await api(page).get('/admin/integrations/mqtt_main')
  expect(cfg.settings).toMatchObject({ type: 'mqtt', host: 'mqtt.local', port: 1883, username: 'hubbo', has_password: true })
  expect(JSON.stringify(cfg)).not.toContain(PASSWORD)
  await expectNotStoredInBrowser(page, PASSWORD)

  // Removing it is its own action, and clears anything typed.
  await card.getByLabel('Password').fill('typed-then-removed')
  await card.getByRole('button', { name: 'Remove password' }).click()
  await expect(card.getByLabel('Password')).toHaveValue('')
  await expect(card.getByText('Leave blank to keep the saved password.')).toBeHidden()
  expect((await api(page).get('/admin/integrations/mqtt_main')).settings.has_password).toBe(false)
})

test('a broker that rejects the login, or can’t be reached, says so', async ({ page }) => {
  const card = await openCard(page)
  await card.getByLabel('Broker').fill('mqtt.local')
  await card.getByLabel('Password').fill(PASSWORD)
  await control(page, 'mqtt?auth=reject')
  await card.getByRole('button', { name: 'Save' }).click()
  await settle(page, 'auth_failed')
  await expect(page.getByTestId('mqtt-connection')).toContainText('The broker rejected the username or password.')
  await expect(page.getByTestId('state-mqtt_main')).toContainText('Password rejected')

  await control(page, 'mqtt?auth=ok&reachable=0')
  await settle(page, 'unreachable')
  await expect(page.getByTestId('mqtt-connection')).toContainText('Can’t reach the broker.')
  await control(page, 'mqtt?reachable=1')
  await settle(page, 'connected')
})
