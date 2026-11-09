import { expect, test } from './hub.ts'

// hub-sim: seed damaged credentials with `hub-sim write-record --name secrets
// --file api/tools/contract-data/secrets.corrupt.json`, then serve --auth.
test.use({ reset: { authState: 'corrupt' } })

test('damaged login data: clear recovery screen, no setup, no login', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByRole('heading', { name: 'The saved login data is damaged' })).toBeVisible()
  await expect(page.getByText('physical access')).toBeVisible()
  await expect(page.getByRole('heading', { name: 'Set up Hubbo' })).toHaveCount(0)
  await expect(page.getByRole('heading', { name: 'Sign in to Hubbo' })).toHaveCount(0)
})
