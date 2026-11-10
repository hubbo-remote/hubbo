import { api, expect, open, test } from './hub.ts'

test('start an activity from the home page, then Off', async ({ page }) => {
  const hub = api(page)
  await open(page, '/')
  const xbox = page.getByRole('main').getByRole('button', { name: /^Play Xbox/ })
  await xbox.click()
  await expect(xbox).toHaveAttribute('aria-pressed', 'true')
  await expect.poll(hub.activeActivity).toBe('act_play_xbox')

  // The running tile does nothing on tap: no second activate (it would re-run the startup).
  const activates: string[] = []
  page.on('request', (r) => r.url().includes('/activate') && activates.push(r.url()))
  await expect(xbox).toHaveAttribute('aria-disabled', 'true')
  await xbox.dispatchEvent('click') // a tap: Playwright won't click an aria-disabled button
  expect(activates).toEqual([])

  await page.getByRole('button', { name: /^Off/ }).click()
  await expect(xbox).toHaveAttribute('aria-pressed', 'false')
  await expect.poll(hub.activeActivity).toBeNull()
  await expect(page.getByRole('button', { name: /^Off/ })).toBeDisabled()
})

test.describe('when the hub switches but can\'t queue the startup', () => {
  test.use({ reset: { startupRejected: true } })

  test('says it switched and why the startup didn’t run, with no retry', async ({ page }) => {
    await open(page, '/')
    const xbox = page.getByRole('main').getByRole('button', { name: /^Play Xbox/ })
    await xbox.click()
    await expect(page.getByText('Play Xbox is on, but its startup didn’t run')).toBeVisible()
    await expect(page.getByText('Hubbo was busy. Try again.')).toBeVisible()
    await expect(xbox).toHaveAttribute('aria-pressed', 'true') // it did switch
    await expect(page.getByRole('button', { name: /retry/i })).toHaveCount(0)
    await expect(page.getByText('Couldn’t start')).toHaveCount(0)
  })
})
