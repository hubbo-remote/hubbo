import { readFile } from 'node:fs/promises'
import { api, expect, open, setStorage, test } from './hub.ts'

// Never leave the store down for the specs after this one (resets don't touch storage health).
test.afterEach(async ({ page }) => setStorage(page, 'available'))

test('storage goes down: banner, edits disabled, switching still works; it comes back: edits work again', async ({ page }) => {
  // The credentials are read once at boot, so a session (under --auth too) outlives the store going down.
  const hub = api(page)
  await open(page, '/')
  await setStorage(page, 'unavailable')

  const banner = page.getByRole('alert').filter({ hasText: 'Hubbo can’t save changes' })
  await expect(banner).toBeVisible() // appears with the next status poll, no reload needed
  await expect(page.getByRole('button', { name: 'New activity' })).toBeDisabled()
  const before = await hub.get('/activities/act_play_xbox')
  const refused = await hub.put('/activities/act_play_xbox', { ...before, name: 'Not saved' })
  expect(refused.status()).toBe(503)
  expect((await refused.json()).error.code).toBe('storage_unavailable')

  // Switching doesn't need the store.
  await page.getByRole('main').getByRole('button', { name: 'Play Xbox', exact: true }).click()
  await expect.poll(hub.activeActivity).toBe('act_play_xbox')

  await setStorage(page, 'available')
  await expect(banner).toBeHidden()
  await expect(page.getByRole('link', { name: 'New activity' })).toBeVisible()
  const saved = await hub.put('/activities/act_play_xbox', { ...before, name: 'Saved again' })
  expect(saved.status()).toBe(200)
})

test('storage goes down during the activity wizard: Create is disabled until it comes back', async ({ page }) => {
  await open(page, '/activities/new')
  await page.getByLabel('Name', { exact: true }).fill('E2E Paused')
  await page.getByLabel('Family Room Lights').check()
  const next = page.getByRole('button', { name: 'Continue' })
  await next.click() // power
  await next.click() // review
  const create = page.getByRole('button', { name: 'Create activity' })
  await expect(create).toBeEnabled()

  await setStorage(page, 'unavailable')
  await expect(create).toBeDisabled() // with the next status poll
  await expect(create).toHaveAttribute('title', 'Changes are paused')
  await setStorage(page, 'available')
  await expect(create).toBeEnabled()
  await expect(create).not.toHaveAttribute('title')
})

test('storage failing after a good load: a backup still exports the running config (the last acknowledged save)', async ({ page }) => {
  await open(page, '/status/backups')
  await setStorage(page, 'unavailable')
  await expect(page.getByText('Backups still work while storage is down.')).toBeVisible()
  const [download] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Download backup' }).click()])
  expect(download.suggestedFilename()).toMatch(/^hubbo-backup-\d{4}-\d\d-\d\d\.json$/)
  expect(JSON.parse(await readFile((await download.path())!, 'utf8')).format).toBe('hubbo-backup')
  await expect(page.getByText('Backup downloaded')).toBeVisible()
  await expect(page.getByRole('alert').filter({ hasText: /Nothing to download|Can’t read storage/ })).toHaveCount(0)
})
