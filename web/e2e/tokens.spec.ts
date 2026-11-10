import { ensureAdmin, expect, expectNotStoredInBrowser, open, test } from './hub.ts'

test('API token is shown exactly once', async ({ page }) => {
  await ensureAdmin(page)
  await open(page, '/settings/tokens')
  await page.getByLabel('Name').fill('E2E agent')
  await page.getByRole('button', { name: 'Create token' }).click()

  const dialog = page.getByRole('dialog')
  await expect(dialog.getByRole('heading', { name: 'Copy your token' })).toBeVisible()
  const token = (await dialog.locator('code').first().textContent())!.trim()
  expect(token).toMatch(/^hbt_[a-z0-9]{8}_[A-Za-z0-9_-]{43}$/)
  await dialog.getByRole('button', { name: 'Done' }).click()

  await expect(page.getByText('E2E agent')).toBeVisible()
  await expectNotStoredInBrowser(page, token)
  await page.reload()
  await expect(page.getByText('E2E agent')).toBeVisible()
  await expectNotStoredInBrowser(page, token)

  await page.getByRole('button', { name: 'Revoke E2E agent' }).click()
  await expect(page.getByText('E2E agent')).toBeHidden()
})
