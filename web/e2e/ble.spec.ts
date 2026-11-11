// Bluetooth devices: each pairs on its own page. A host bonds through a test control
// (hub-sim: /sim/ble/bond; the mock: /__mock/ble/bond), and sim time runs a window down.
import type { Page } from '@playwright/test'
import { addBle, advance, api, bondHost, expect, open, setBleRadio, setHostLink, test } from './hub.ts'

/** Add a Bluetooth device through the Devices page's dialog; returns the dialog. */
const addInDialog = async (page: Page, name: string) => {
  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).first().click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: /Bluetooth/ }).click()
  await dialog.getByLabel('Name').fill(name)
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  return dialog
}
const state = (page: Page) => page.getByTestId('bluetooth-state')
const pairStatus = async (page: Page, id: string) => (await api(page).get(`/admin/devices/${id}/pair`)).state as string

test('add a Bluetooth device, pair it, pair again and cancel, then forget', async ({ page }) => {
  const dialog = await addInDialog(page, 'Shield') // the confirms below reuse the dialog role
  await expect(dialog).toBeHidden()
  await expect(page.getByRole('heading', { name: 'Shield' })).toBeVisible()
  const id = new URL(page.url()).hash.split('/').pop()!
  await expect(state(page)).toHaveText('Not paired')

  await page.getByRole('button', { name: 'Pair', exact: true }).click()
  await expect(state(page)).toHaveText(/Ready to pair · \d+ s$/)
  await expect(page.getByText('open Bluetooth settings')).toBeVisible()
  await bondHost(page, id)
  await expect(state(page)).toHaveText('Connected')
  expect(await pairStatus(page, 'dev_apple_tv')).toBe('connected') // the other device keeps its host

  // Pairing again and cancelling keeps the host.
  await page.getByRole('button', { name: 'Pair again' }).click()
  await expect(dialog).toContainText('This replaces its current pairing.')
  await dialog.getByRole('button', { name: 'Pair', exact: true }).click()
  await expect(state(page)).toContainText('Ready to pair')
  await page.getByRole('button', { name: 'Cancel', exact: true }).click()
  await expect(state(page)).toHaveText('Connected')

  await page.getByRole('button', { name: 'Forget', exact: true }).click()
  await expect(dialog).toContainText('from its Bluetooth settings')
  await dialog.getByRole('button', { name: 'Forget' }).click()
  await expect(state(page)).toHaveText('Not paired')
  expect(await pairStatus(page, 'dev_apple_tv')).toBe('connected')
})

test('a window that ends with nothing paired says so', async ({ page }) => {
  const id = await addBle(page, 'Fire TV')
  await open(page, `/devices/${id}`)
  await page.getByRole('button', { name: 'Pair', exact: true }).click()
  await expect(state(page)).toContainText('Ready to pair')
  await advance(page, 125_000)
  await expect(page.getByText('Nothing paired. Try again.')).toBeVisible()
  await expect(state(page)).toHaveText('Not paired')
})

test('the device list shows each device’s status; a host that drops is Not connected', async ({ page }) => {
  const id = await addBle(page, 'Shield')
  await open(page, '/devices')
  const row = (name: string) => page.getByRole('link', { name: new RegExp(`^${name}`) })
  await expect(row('Family Room Apple TV')).toContainText('Connected')
  await expect(row('Shield')).toContainText('Not paired')
  await setHostLink(page, 'dev_apple_tv', false)
  await page.reload()
  await expect(row('Family Room Apple TV')).toContainText('Not connected')
  // A key for a host that isn't connected goes nowhere; Shield is untouched.
  const r = await api(page).post('/devices/dev_apple_tv/commands/key_tap/execute', { params: { key: 'PLAY_PAUSE' } })
  expect(await r.json()).toMatchObject({ status: 'failed', message: 'adapter_unavailable' })
  expect(await pairStatus(page, id)).toBe('not_paired')
})

test('a fifth Bluetooth device is refused with the hub’s message', async ({ page }) => {
  for (const n of ['Shield', 'Fire TV', 'Roku']) await addBle(page, n) // the seed has one
  await expect(await addInDialog(page, 'One too many')).toContainText('You can add up to 4 Bluetooth devices')
})

test('the Bluetooth card shows only the radio', async ({ page }) => {
  await open(page, '/integrations')
  const card = page.getByTestId('state-ble')
  await expect(card).toHaveText('Connected')
  await expect(page.getByRole('button', { name: /pair/i })).toHaveCount(0)
  await setBleRadio(page, false)
  await expect(card).toHaveText("Bluetooth isn't working")
  await setBleRadio(page, true)
  await expect(card).toHaveText('Connected')

  await page.getByRole('switch', { name: 'Enable Bluetooth' }).click()
  await expect(card).toHaveText('Disabled')
  await page.getByRole('switch', { name: 'Enable Bluetooth' }).click()
  await expect(card).toHaveText('Connected')
})
