import { advance, api, expect, hubAnswersScan, open, SIM_URL, test } from './hub.ts'

// On hub-sim a scan never finds a Hub by itself (only /sim/radio/found answers it);
// on the mock this flag does the same.
test.use({ reset: { scanNeverFinds: true } })
// A reset doesn't stop a scan on hub-sim (sim time stands still), so make sure none is left running.
test.beforeEach(async ({ page }) => void (await api(page).del('/admin/remote/pair')))

const dl = (page: import('@playwright/test').Page) => page.locator('dl[aria-label="Current remote endpoint"]')
const scanButton = (page: import('@playwright/test').Page) => page.getByRole('button', { name: /Scan from Hub|Re-pair/ })

test('remote endpoint: shows the current one; manual entry is validated and takes effect', async ({ page }) => {
  await open(page, '/settings/remote')
  // hub-sim keeps whatever endpoint an earlier scan or test set (a reset doesn't touch the radio).
  if (!SIM_URL) await expect(dl(page)).toContainText('FC:5C:97:55:0F')
  await expect(dl(page)).toContainText('Radio')

  await page.getByLabel('Channel', { exact: true }).fill('125')
  await page.getByLabel('Address', { exact: true }).fill('FC5C975555')
  await page.getByLabel('Address', { exact: true }).blur()
  await expect(page.getByText('A whole number from 0 to 124')).toBeVisible()
  await expect(page.getByText('can’t end in 55 or AA')).toBeVisible()
  await expect(page.getByRole('button', { name: 'Use this endpoint' })).toBeDisabled()

  await page.getByLabel('Channel', { exact: true }).fill('12')
  await page.getByLabel('Address', { exact: true }).fill('a1:b2:c3:d4:e5')
  await page.getByRole('button', { name: 'Use this endpoint' }).click()
  await expect(page.getByText('Listening on the new endpoint')).toBeVisible()
  await expect(dl(page)).toContainText('12')
  await expect(dl(page)).toContainText('A1:B2:C3:D4:E5')
  await expect(dl(page)).toContainText('Listening')
})

test('scan from Hub: progress toward the 120 s timeout; cancel keeps the pairing; it survives a reload', async ({ page }) => {
  await open(page, '/settings/remote')
  await scanButton(page).click()
  const bar = page.getByRole('progressbar', { name: 'Scan progress' })
  await expect(bar).toBeVisible()
  await expect(bar).toHaveAttribute('aria-valuemax', '120')
  await expect(page.getByText(/1[12]\ds left/)).toBeVisible()
  await expect(page.getByRole('button', { name: 'Use this endpoint' })).toBeDisabled() // locked while scanning

  await page.reload() // the scan keeps running on the hub; the page picks it up
  await expect(bar).toBeVisible()

  await page.getByRole('button', { name: 'Cancel' }).click()
  await expect(page.getByText('Scan stopped')).toBeVisible()
  await expect(bar).toBeHidden()
})

test('scan from Hub: a Hub answers; its endpoint is shown and used', async ({ page }) => {
  await open(page, '/settings/remote')
  await scanButton(page).click()
  await expect(page.getByRole('progressbar', { name: 'Scan progress' })).toBeVisible()
  await hubAnswersScan(page, 33, '5A5B5C5D5E')
  await expect(page.getByText('Remote paired')).toBeVisible()
  await expect(dl(page)).toContainText('33')
  await expect(dl(page)).toContainText('5A:5B:5C:5D:5E')
})

test('scan from Hub: no Hub in 120 s is a clear timeout', async ({ page }) => {
  await open(page, '/settings/remote')
  await scanButton(page).click()
  await expect(page.getByRole('progressbar', { name: 'Scan progress' })).toBeVisible()
  await advance(page, 121_000)
  await expect(page.getByText('No Hub found')).toBeVisible()
  await expect(page.getByText('press its pair button')).toBeVisible()
})
