// Pairing a device over Bluetooth from the Integrations page. A host "bonds" through a test
// control (hub-sim: /sim/ble/bonded; the mock: /__mock/ble/bonded), and sim time runs the window down.
import { advance, api, control, expect, open, test } from './hub.ts'

test('pair a device, a window that closes with nothing paired, and forget', async ({ page }) => {
  await open(page, '/integrations')
  const pairState = async () => (await api(page).get('/admin/integrations/ble/pair')).state as string
  const dialog = page.getByRole('dialog')
  const startPairing = async () => {
    const pairButton = page.getByRole('button', { name: /^Pair (another|a) device$/ })
    const replacing = (await pairButton.textContent())?.includes('another')
    await pairButton.click()
    if (replacing) {
      // Pairing replaces an existing bond: said before it starts.
      await expect(dialog).toContainText('This replaces')
      await dialog.getByRole('button', { name: 'Start pairing' }).click()
    }
    await expect(page.getByText(/^\d+ s left$/)).toBeVisible()
    await expect(page.getByText('open Bluetooth settings')).toBeVisible()
  }

  // Pair: the device bonds while the window is open.
  await startPairing()
  await control(page, `ble/bonded?peer=${encodeURIComponent('Living Room')}`)
  await expect(page.getByTestId('state-ble')).toHaveText('Connected to Living Room')
  await expect(page.getByText(/s left$/)).toHaveCount(0)

  // A window that closes with no bond says so (and stays that way until the next pair or forget).
  await startPairing()
  await advance(page, 125_000)
  await expect(page.getByText('Nothing paired. Try again.')).toBeVisible()
  expect(await pairState()).toBe('failed')

  // Forget: confirmed first, with the reminder to remove Hubbo on the device too.
  await control(page, 'ble/bonded?peer=Den')
  await expect.poll(pairState).toBe('paired')
  await page.reload()
  await expect(page.getByTestId('state-ble')).toHaveText('Connected to Den')
  await page.getByRole('button', { name: 'Forget', exact: true }).click()
  await expect(dialog).toContainText('from its Bluetooth settings')
  await dialog.getByRole('button', { name: 'Forget' }).click()
  await expect(page.getByTestId('state-ble')).toHaveText('Not paired')
  expect(await pairState()).toBe('idle')
})

test('turned off, the card says so and nothing else', async ({ page }) => {
  await open(page, '/integrations')
  const state = page.getByTestId('state-ble')
  await expect(state).toHaveText(/^Connected to /)
  await page.getByRole('switch', { name: 'Enable Bluetooth' }).click()
  await expect(state).toHaveText('Disabled')
  await page.getByRole('switch', { name: 'Enable Bluetooth' }).click()
  await expect(state).toHaveText(/^Connected to /)
})
