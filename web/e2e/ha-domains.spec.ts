import { api, connectHa, expect, open, registerHa, test } from './hub.ts'
import { addStep, openNewActivity, saveActivity } from './editor.ts'

// The newer Home Assistant domains (scenes, covers, humidifiers, selects…), with made-up entities that
// the mock's fixtures and hub-sim's fake HA both have.

test.beforeEach(async ({ page }) => connectHa(page))

test('adding a cover starts on the type the hub suggests, and its position is a percent', async ({ page }) => {
  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: /Home Assistant/ }).click()
  await dialog.getByLabel('Search devices').fill('office_blinds')
  await dialog.getByRole('button', { name: /office_blinds/ }).click()
  await expect(dialog.getByRole('region', { name: 'Commands' })).toContainText('cover.set_cover_position')
  await expect(dialog.getByLabel('Type').locator('option:checked')).toHaveText('Cover')
  await dialog.getByLabel('Name').fill('E2E Blinds')
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  await expect(page.getByRole('heading', { name: 'E2E Blinds' })).toBeVisible()

  const position = page.getByRole('group', { name: 'Set position', exact: true })
  await expect(position.getByLabel('Position (%)')).toHaveValue('35')
  await position.getByLabel('Position (%)').fill('50')
  await position.getByRole('button', { name: 'Test' }).click()
  await expect(position.getByText(/^(Sent|Timed out|Failed)/)).toBeVisible()
  await expect(page.getByRole('group', { name: 'Open', exact: true }).getByRole('button', { name: 'Test' })).toBeVisible()
})

test('a humidifier and a select offer their modes and options; humidity starts at 45%', async ({ page }) => {
  const humidifier = await registerHa(page, 'humidifier.bedroom_humidifier', 'E2E Humidifier', 'climate')
  const dryer = await registerHa(page, 'select.dryer_cycle', 'E2E Dryer')

  await open(page, `/devices/${humidifier.id}`)
  await expect(page.getByRole('group', { name: 'Set humidity', exact: true }).getByLabel('Humidity (%)')).toHaveValue('45')
  const mode = page.getByRole('group', { name: 'Set mode', exact: true }).getByLabel('Mode', { exact: true })
  await expect(mode.locator('option')).toHaveText(['Normal', 'Eco', 'Sleep'])

  await open(page, `/devices/${dryer.id}`)
  const option = page.getByRole('group', { name: 'Choose', exact: true }).getByLabel('Option', { exact: true })
  await expect(option.locator('option')).toHaveText(['Normal', 'Delicate', 'Towels'])
  await expect(page.getByRole('group', { name: 'Next', exact: true }).getByRole('button', { name: 'Test' })).toBeVisible()
})

test('a step sets a cover’s position and reads it as a percent', async ({ page }) => {
  await registerHa(page, 'cover.office_blinds', 'E2E Blinds', 'cover')
  const id = await openNewActivity(page, 'E2E Blinds down')
  await page.getByRole('tab', { name: /Startup/ }).click()
  await addStep(page, 'E2E Blinds', 'Set position', { 'Position (%)': '20' })
  await expect(page.getByRole('list', { name: 'Startup steps' }).getByRole('listitem').first()).toContainText('Set position 20%')
  await addStep(page, 'E2E Blinds', 'Stop')
  await saveActivity(page)
  const startup = (await api(page).get(`/activities/${id}`)).startup as { command_id: string; params: object }[]
  expect(startup.map((s) => [s.command_id, s.params])).toEqual([
    ['cover.set_cover_position', { position: 20 }],
    ['cover.stop_cover', {}],
  ])
})
