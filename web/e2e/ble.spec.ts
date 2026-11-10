// Pairing the Apple TV over Bluetooth from the Integrations page. A host "bonds" through a test
// control (hub-sim: /sim/ble/bonded; the mock: /__mock/ble/bonded), and sim time runs the window down.
import { advance, api, control, expect, open, test } from './hub.ts'

test('pair an Apple TV, a window that closes with nothing paired, and forget', async ({ page }) => {
  await open(page, '/integrations')
  const pairState = async () => (await api(page).get('/admin/integrations/ble/pair')).state as string
  const dialog = page.getByRole('dialog')
  const startPairing = async () => {
    const pairButton = page.getByRole('button', { name: /^Pair (a different |an )Apple TV$/ })
    const replacing = (await pairButton.textContent())?.includes('different')
    await pairButton.click()
    if (replacing) {
      // Pairing replaces an existing bond: said before it starts.
      await expect(dialog).toContainText('replaces the current bond')
      await dialog.getByRole('button', { name: 'Start pairing' }).click()
    }
    await expect(page.getByText(/Pairing is open · \d+ s left/)).toBeVisible()
    await expect(page.getByText('Pair now from the Apple TV')).toBeVisible()
  }

  // Pair: the Apple TV bonds while the window is open.
  await startPairing()
  await control(page, `ble/bonded?peer=${encodeURIComponent('Living Room')}`)
  await expect(page.getByText('Paired with Living Room')).toBeVisible()
  await expect(page.getByText('Pairing is open')).toHaveCount(0)

  // A window that closes with no bond says so (and stays that way until the next pair or forget).
  await startPairing()
  await advance(page, 125_000)
  await expect(page.getByText('No device paired')).toBeVisible()
  expect(await pairState()).toBe('failed')

  // Forget: confirmed first, with the reminder to remove Hubbo on the Apple TV too.
  await control(page, 'ble/bonded?peer=Den')
  await expect.poll(pairState).toBe('paired')
  await page.reload()
  await expect(page.getByText('Paired with Den')).toBeVisible()
  await page.getByRole('button', { name: 'Forget', exact: true }).click()
  await expect(dialog).toContainText('disconnects it')
  await expect(dialog).toContainText('Remotes and Devices → Bluetooth')
  await dialog.getByRole('button', { name: 'Forget' }).click()
  await expect(page.getByText('Nothing is paired.')).toBeVisible()
  expect(await pairState()).toBe('idle')
})
