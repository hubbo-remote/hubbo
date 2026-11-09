import { readFile } from 'node:fs/promises'
import { api, expect, onAuthSim, open, setStorage, test } from './hub.ts'

// Never leave the store down for the specs after this one (resets don't touch storage health).
test.afterEach(async ({ page }) => setStorage(page, 'available'))

test('storage goes down: banner, edits disabled, switching still works; it comes back: edits work again', async ({ page }) => {
  // Under --auth nobody can authenticate while storage is down, so the storage screen takes over instead (storage-auth.spec).
  test.skip(await onAuthSim(page), 'hub-sim --auth: covered by storage-auth.spec')
  const hub = api(page)
  await open(page, '/activities')
  await setStorage(page, 'unavailable')

  const banner = page.getByRole('alert').filter({ hasText: 'The hub can’t save changes' })
  await expect(banner).toBeVisible() // appears with the next status poll, no reload needed
  await expect(page.getByRole('button', { name: 'New activity' })).toBeDisabled()
  const before = await hub.get('/activities/act_play_xbox')
  const refused = await hub.put('/activities/act_play_xbox', { ...before, name: 'Not saved' })
  expect(refused.status()).toBe(503)
  expect((await refused.json()).error.code).toBe('storage_unavailable')

  // Switching doesn't need the store.
  await page.getByRole('main').getByRole('listitem').filter({ hasText: 'Play Xbox' }).getByRole('button', { name: 'Start' }).click()
  await expect.poll(hub.activeActivity).toBe('act_play_xbox')

  await setStorage(page, 'available')
  await expect(banner).toBeHidden()
  await expect(page.getByRole('link', { name: 'New activity' })).toBeVisible()
  const saved = await hub.put('/activities/act_play_xbox', { ...before, name: 'Saved again' })
  expect(saved.status()).toBe(200)
})

test('storage failing after a good load: a backup still exports the running config (the last acknowledged save)', async ({ page }) => {
  test.skip(await onAuthSim(page), 'hub-sim --auth: nobody can authenticate while storage is down (storage-auth.spec)')
  await open(page, '/settings/backup')
  await setStorage(page, 'unavailable')
  await expect(page.getByText('A backup still works: it’s the configuration that’s running')).toBeVisible()
  const [download] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Download backup' }).click()])
  expect(download.suggestedFilename()).toMatch(/^hubbo-backup-\d{4}-\d\d-\d\d\.json$/)
  expect(JSON.parse(await readFile((await download.path())!, 'utf8')).format).toBe('hubbo-backup')
  await expect(page.getByText('Backup downloaded')).toBeVisible()
  await expect(page.getByRole('alert').filter({ hasText: /No backup was made|can’t read its storage/ })).toHaveCount(0)
})
