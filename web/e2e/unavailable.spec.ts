import { expect, notAvailable, open, stubUnimplemented, test } from './hub.ts'

// Endpoints hub-sim answers with 501 today: calm "not available yet", no error toasts.
test('features the hub does not implement yet show "Not available yet"', async ({ page }) => {
  await stubUnimplemented(page)
  await open(page, '/settings/firmware')
  await expect(notAvailable(page)).toBeVisible()

  await open(page, '/settings/remote')
  await page.getByRole('button', { name: /Start scan|Re-pair/ }).click()
  await expect(notAvailable(page)).toBeVisible()

  await open(page, '/integrations')
  await expect(notAvailable(page)).toHaveCount(2) // Home Assistant and Bluetooth settings
  // An action that hits a 501 gives one neutral notice, however often it's repeated.
  const toggle = page.getByRole('switch', { name: 'Enable Home Assistant' })
  await toggle.click()
  await toggle.click()
  await expect(page.getByText('Not available yet', { exact: true })).toHaveCount(3) // 2 panels + 1 toast

  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: /Home Assistant/ }).click()
  await expect(notAvailable(dialog)).toBeVisible()

  await expect(page.getByText('Something went wrong')).toHaveCount(0)
})
