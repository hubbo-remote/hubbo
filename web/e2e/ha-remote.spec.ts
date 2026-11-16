// A Home Assistant remote is just another device: added from Home Assistant, its own commands, and
// HA's `remote` services through the Home Assistant action. The mock and hub-sim both have it.
import { api, connectHa, expect, open, test } from './hub.ts'

test.skip(({ isMobile }) => isMobile, 'desktop layout: the phone edits a key in a sheet, the same fields')

test.beforeEach(async ({ page }) => connectHa(page))

test('add a remote and bind a button to send one of its commands', async ({ page }) => {
  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: /Home Assistant/ }).click()
  await dialog.getByLabel('Search devices').fill('remote.living_room_tv')
  await dialog.getByRole('button', { name: /remote\.living_room_tv/ }).click()
  await expect(dialog.getByRole('region', { name: 'Commands' })).toContainText('remote.turn_on')
  await expect(dialog.getByLabel('Type').locator('option:checked')).toHaveText('Other')
  await dialog.getByLabel('Name').fill('E2E Remote')
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  await expect(dialog.getByRole('status').filter({ hasText: 'Added E2E Remote' })).toBeVisible()
  await dialog.getByRole('button', { name: 'Done' }).click()
  const devices = (await api(page).get('/devices')) as { devices: { id: string; name: string }[] }
  const id = devices.devices.find((d) => d.name === 'E2E Remote')!.id

  await open(page, '/remote')
  await page.locator('[data-key="info"]').click()
  await page.getByRole('button', { name: /^(Command for|Edit) Info press$/ }).click()
  await page.getByLabel('Device').selectOption({ label: 'E2E Remote' })
  await page.getByLabel('Command', { exact: true }).selectOption({ label: 'Home Assistant action…' })
  await page.getByLabel('Service').selectOption('remote.send_command')
  await expect(page.getByLabel('Data')).toHaveValue('{"command":"Play"}') // HA's example
  await page.getByLabel('Data').fill('{"command": "DPAD_UP"}')
  await page.getByRole('button', { name: 'Apply' }).click()
  await page.getByRole('button', { name: /^Save/ }).click()
  await expect(page.getByText('Saved', { exact: true })).toBeVisible()

  const action = { integration_id: 'ha_main', device_id: id, command_id: 'call_service', params: {}, call: { service: 'remote.send_command', data: { command: 'DPAD_UP' } } }
  expect((await api(page).get('/global-bindings')).bindings['info:down']).toEqual([action])
  await page.reload()
  await page.locator('[data-key="info"]').click()
  await expect(page.getByText('remote.send_command · command: DPAD_UP').first()).toBeVisible()
})
