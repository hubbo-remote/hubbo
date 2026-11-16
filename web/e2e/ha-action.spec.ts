// A Home Assistant action: one of an HA device's own services (picked, never typed) with JSON data,
// checked as you type, tested, and bound like any command. The LG TV offers webostv.button.
import { api, control, expect, open, SIM_URL, test } from './hub.ts'

test.skip(({ isMobile }) => isMobile, 'desktop layout: the phone edits a key in a sheet, the same fields')

test('bind a Home Assistant action: pick the service, fix the data as you type, test it, save', async ({ page }) => {
  await open(page, '/remote')
  await page.locator('[data-key="info"]').click()
  await page.getByRole('button', { name: /^(Command for|Edit) Info press$/ }).click()
  await page.getByLabel('Device').selectOption({ label: 'Family Room LG TV' })
  await page.getByLabel('Command', { exact: true }).selectOption({ label: 'Home Assistant action…' })

  // Several services: one is picked, never typed; its required fields start from HA's example.
  const service = page.getByLabel('Service')
  const data = page.getByLabel('Data')
  const test_ = page.getByRole('button', { name: 'Test' })
  await expect(service.locator('option:checked')).toHaveText('Choose…')
  await expect(test_).toHaveCount(0)
  await service.selectOption('webostv.button')
  await expect(data).toHaveValue('{"button":"LEFT"}')
  await expect(page.getByRole('list', { name: 'webostv.button fields' })).toContainText('button (required)')

  // Checked as you type.
  await data.fill('{"button": ')
  await expect(page.getByText('Not valid JSON')).toBeVisible()
  await expect(test_).toHaveCount(0)
  await data.fill('{"entity_id": "media_player.other", "button": "INFO"}')
  await expect(page.getByText('Leave out entity_id')).toBeVisible()
  await data.fill('{"button": "INFO"}')
  await expect(page.getByText('Not valid JSON')).toHaveCount(0)
  await expect(page.getByText('Leave out entity_id')).toHaveCount(0)

  // Test sends the call as it stands.
  const sent = page.waitForRequest((r) => r.method() === 'POST' && r.url().endsWith('/devices/dev_lg_tv/commands/call_service/execute'))
  await test_.click()
  expect((await sent).postDataJSON()).toEqual({ params: {}, call: { service: 'webostv.button', data: { button: 'INFO' } } })
  await expect(page.getByText('Sent', { exact: true })).toBeVisible()
  // hub-sim's fake HA got it for the TV's own entity, with the data as given.
  if (SIM_URL) expect((await control(page, 'ha')).last_call).toMatchObject({ service: 'webostv.button', data: { button: 'INFO' } })

  await page.getByRole('button', { name: 'Apply' }).click()
  await expect(page.getByText('webostv.button · button: INFO').first()).toBeVisible()
  await page.getByRole('button', { name: /^Save/ }).click()
  await expect(page.getByText('Saved', { exact: true })).toBeVisible()
  const g = await api(page).get('/global-bindings')
  expect(g.bindings['info:down']).toEqual([{ integration_id: 'ha_main', device_id: 'dev_lg_tv', command_id: 'call_service', params: {}, call: { service: 'webostv.button', data: { button: 'INFO' } } }])
})
