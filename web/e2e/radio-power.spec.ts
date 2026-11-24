import { api, expect, open, test } from './hub.ts'

// Settings → Remote: the remote radio's transmit power, saved on change.

test('the radio transmit power saves on change and stays after a reload', async ({ page }) => {
  await open(page, '/settings/remote')
  const power = page.getByLabel('Radio transmit power')
  await expect(power).toHaveValue('max')
  await expect(page.getByText('Lower this if your radio module has an amplifier and an external antenna.')).toBeVisible()
  await power.selectOption({ label: 'Low' })
  await expect.poll(async () => (await api(page).get('/admin/hub')).radio_power).toBe('low')
  await page.reload()
  await expect(page.getByLabel('Radio transmit power')).toHaveValue('low')
})

test('at phone width the chevron sits inside the select, at its right edge', async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 })
  await open(page, '/settings/remote')
  const power = page.getByLabel('Radio transmit power')
  const box = (await power.boundingBox())!
  const chevron = (await power.locator('xpath=following-sibling::*[local-name()="svg"]').boundingBox())!
  expect(chevron.x).toBeGreaterThan(box.x + box.width / 2)
  expect(chevron.x + chevron.width).toBeLessThanOrEqual(box.x + box.width)
})
