import { expect, loginHeading, signIn, test } from './hub.ts'

test.use({ reset: { requireAuth: true } })

test('login, wrong password, sign out', async ({ page }) => {
  await page.goto('/')
  await expect(loginHeading(page)).toBeVisible()

  await signIn(page, 'wrong password')
  await expect(page.getByRole('alert')).toContainText(/wrong username or password/i)

  await signIn(page)
  await expect(page.getByRole('heading', { name: 'Dashboard' })).toBeVisible()
  // The session is an HttpOnly cookie: invisible to page script.
  expect(await page.evaluate(() => document.cookie)).not.toContain('hubbo_session')

  await page.getByRole('button', { name: 'Sign out' }).click()
  await expect(loginHeading(page)).toBeVisible()
  await page.reload()
  await expect(loginHeading(page)).toBeVisible()
})
