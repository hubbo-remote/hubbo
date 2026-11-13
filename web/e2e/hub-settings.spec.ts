// Settings → Hub: the hub's name and a restart.
import { advance, api, expect, loginHeading, open, session, test } from './hub.ts'

test('Settings opens on Hub; the name saves trimmed, and a refused one says why', async ({ page }) => {
  await open(page, '/settings')
  await expect(page.getByRole('tab', { name: 'Hub' })).toHaveAttribute('aria-selected', 'true')
  const name = page.getByLabel('Name', { exact: true })
  await expect(name).toHaveValue((await api(page).get('/admin/hub')).name)
  // Not a login: password managers leave it alone (the sign-in fields keep theirs).
  await expect(name).toHaveAttribute('autocomplete', 'off')
  await expect(name).toHaveAttribute('data-1p-ignore', '')
  await expect(name).toHaveAttribute('data-lpignore', 'true')
  await expect(name).toHaveAttribute('data-form-type', 'other')
  const save = page.getByRole('button', { name: 'Save' })
  await expect(save).toBeDisabled()

  await name.fill('x'.repeat(33))
  await save.click()
  await expect(page.getByText('At most 32 characters')).toBeVisible()

  await name.fill('Den\tHub') // the hub's own check, shown at the field
  await save.click()
  await expect(page.getByText('Name must be text without control characters')).toBeVisible()

  await name.fill('  Den Hub  ')
  await save.click()
  await expect(page.getByText('Paired devices may show the old name until they reconnect.')).toBeVisible()
  await expect(name).toHaveValue('Den Hub')
  expect(await api(page).get('/admin/hub')).toMatchObject({ name: 'Den Hub', hostname: 'den-hub' })
})

test('restart asks first, then waits for the hub and opens the app afresh', async ({ page }) => {
  await open(page, '/settings/hub')
  const enforced = (await session(page)).auth_mode === 'enforced'
  await page.getByRole('button', { name: 'Restart…' }).click()
  const dialog = page.getByRole('dialog', { name: 'Restart the hub?' })
  await expect(dialog).toContainText('You’ll need to sign in again.')
  await dialog.getByRole('button', { name: 'Restart', exact: true }).click()
  await expect(dialog.getByText('Restarting…')).toBeVisible()
  await advance(page, 1_000)
  if (!enforced) return await expect(page.getByRole('heading', { name: 'Activities' })).toBeVisible() // a simulator with no sign-in
  await expect(loginHeading(page)).toBeVisible()
  expect((await session(page)).authenticated).toBe(false) // every session ended
})
