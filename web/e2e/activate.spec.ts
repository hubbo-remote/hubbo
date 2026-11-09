import { api, expect, open, test } from './hub.ts'

test('activate an activity from the dashboard, then Off', async ({ page }) => {
  const hub = api(page)
  await open(page, '/')
  const xbox = page.getByRole('main').getByRole('button', { name: /^Play Xbox/ })
  await xbox.click()
  await expect(xbox).toHaveAttribute('aria-pressed', 'true')
  await expect.poll(hub.activeActivity).toBe('act_play_xbox')

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
    await expect(page.getByText('Switched to Play Xbox, but its startup didn’t run')).toBeVisible()
    await expect(page.getByText(/the action queue was full/)).toBeVisible()
    await expect(xbox).toHaveAttribute('aria-pressed', 'true') // it did switch
    await expect(page.getByRole('button', { name: /retry/i })).toHaveCount(0)
    await expect(page.getByText('Could not start')).toHaveCount(0)
  })
})
