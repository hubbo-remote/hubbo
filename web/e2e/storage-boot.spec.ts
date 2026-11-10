import { expect, test } from './hub.ts'

// hub-sim: `bin/hubbo sim serve --data DIR --reset --storage-unavailable --auth`.
// The hub can't read its saved login data, so it can't know whether an admin
// exists: an error screen (restart or wipe), never the first-run wizard or a login form.
test.use({ reset: { authState: 'unavailable', configState: 'storage_unavailable' } })

test('storage down at boot: storage error screen, no setup wizard, no login', async ({ page }) => {
  await page.goto('/')
  await expect(page.getByRole('heading', { name: 'Hubbo can’t read its login' })).toBeVisible()
  await expect(page.getByText('Restart Hubbo')).toBeVisible()
  await expect(page.getByRole('heading', { name: 'Set up Hubbo' })).toHaveCount(0)
  await expect(page.getByRole('heading', { name: 'Sign in to Hubbo' })).toHaveCount(0)
  await page.getByRole('button', { name: 'Check again' }).click()
  await expect(page.getByRole('heading', { name: 'Hubbo can’t read its login' })).toBeVisible()
})
