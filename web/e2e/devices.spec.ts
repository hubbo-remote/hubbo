import { expect, open, SIM_URL, test } from './hub.ts'

test('devices list and command tests with typed params', async ({ page }) => {
  await open(page, '/devices')
  await expect(page.getByRole('link', { name: /Family Room Denon/ })).toBeVisible()
  await expect(page.getByRole('link', { name: /Family Room Apple TV/ })).toBeVisible()

  await page.getByRole('link', { name: /Family Room Denon/ }).click()
  await expect(page.getByRole('heading', { name: 'Family Room Denon' })).toBeVisible()
  await page.getByRole('button', { name: /^Set volume/ }).click()
  await page.getByLabel('Level', { exact: true }).fill('0.4')
  await page.getByRole('button', { name: 'Test' }).click()
  // hub-sim's fake adapters may answer differently from the mock's fixtures.
  await expect(page.getByText(SIM_URL ? /^(Accepted|Timed out|Failed)/ : /^Accepted/)).toBeVisible()

  // Out-of-range input is flagged before anything is sent.
  await page.getByLabel('Level', { exact: true }).fill('3')
  await expect(page.getByText('Level must be at most 1')).toBeVisible()
})

test('a command that times out reports it, not an HTTP error', async ({ page }) => {
  test.skip(!!SIM_URL, 'the timeout case comes from the mock fixtures (devices/execute.timeout.json)')
  await open(page, '/devices/dev_sonos')
  await page.getByRole('button', { name: /^Volume up/ }).click()
  await page.getByRole('button', { name: 'Test' }).click()
  await expect(page.getByText(/^Timed out/)).toBeVisible()
})

test('hold commands are binding-only on the device page', async ({ page }) => {
  await open(page, '/devices/dev_apple_tv')
  const keyDown = page.getByRole('button', { name: /^Key down/ })
  await expect(keyDown).toContainText('Binding only')
  await keyDown.click()
  await expect(page.getByText('can’t be tested on its own')).toBeVisible()
  await expect(page.getByRole('button', { name: 'Test' })).toHaveCount(0)
  // Momentary commands still have a test form.
  await page.getByRole('button', { name: /^Key tap/ }).click()
  await expect(page.getByRole('button', { name: 'Test' })).toBeVisible()
})
