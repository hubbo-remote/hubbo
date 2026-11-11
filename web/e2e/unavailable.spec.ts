import { expect, notAvailable, open, SIM_URL, stubUnimplemented, test } from './hub.ts'

// Endpoints a hub answers with 501: calm "not available yet", no error toasts. hub-sim
// implements every route now, so the 501s are stubbed on the mock only.
test('features the hub does not implement yet show "Not available yet"', async ({ page }) => {
  test.skip(!!SIM_URL, 'hub-sim answers no 501s')
  await stubUnimplemented(page)
  await open(page, '/status/firmware')
  await expect(notAvailable(page)).toBeVisible()

  await open(page, '/settings/remote')
  await page.getByRole('button', { name: /^(Scan|Pair again)$/ }).click()
  await expect(notAvailable(page)).toBeVisible()

  await open(page, '/integrations')
  await expect(notAvailable(page)).toHaveCount(1) // Home Assistant's settings (Bluetooth has none)
  // An action that hits a 501 gives one neutral notice, however often it's repeated.
  const toggle = page.getByRole('switch', { name: 'Enable Home Assistant' })
  await toggle.click()
  await toggle.click()
  await expect(page.getByText('Not available yet', { exact: true })).toHaveCount(2) // the panel + 1 toast

  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: /Home Assistant/ }).click()
  await expect(notAvailable(dialog)).toBeVisible()

  await expect(page.getByText('Something went wrong')).toHaveCount(0)
})
