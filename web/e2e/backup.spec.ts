import { readFile } from 'node:fs/promises'
import { fixture } from '../mock/fixtures.ts'
import { advance, api, connectHa, control, expect, open, session, test } from './hub.ts'

const downloadBackup = async (page: import('@playwright/test').Page, name: string | RegExp) => {
  const [download] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name }).click()])
  const text = await readFile((await download.path())!, 'utf8')
  return { filename: download.suggestedFilename(), text }
}

test('downloads a backup: the hub’s file, under the hub’s filename', async ({ page }) => {
  await open(page, '/settings/backup')
  const { filename, text } = await downloadBackup(page, 'Download backup')
  expect(filename).toMatch(/^hubbo-backup-\d{4}-\d\d-\d\d\.json$/)
  const backup = JSON.parse(text)
  expect(backup.format).toBe('hubbo-backup')
  expect(backup.activities.length).toBeGreaterThan(0)
  await expect(page.getByText('This is the hub’s saved configuration exactly as stored')).toBeHidden() // only for blocked-raw
})

test('a restore whose activities have new ids leaves nothing active, and starts nothing', async ({ page }) => {
  await connectHa(page) // HA with a URL and a token
  const hub = api(page)
  expect((await hub.post('/activities/act_play_xbox/activate')).status()).toBe(200)
  await expect.poll(hub.activeActivity).toBe('act_play_xbox')

  await open(page, '/settings/backup')
  const { text } = await downloadBackup(page, 'Download backup')
  // Same configuration, every activity id changed (global bindings that name them too).
  const ids: string[] = JSON.parse(text).activities.map((a: { id: string }) => a.id)
  const renamed = JSON.parse(ids.reduce((t, id) => t.replaceAll(`"${id}"`, `"${id}_v2"`), text))
  // HA at another address (as after a wipe): the hub keeps a token only for the same URL.
  renamed.integrations.find((i: { id: string }) => i.id === 'ha_main').settings.url = 'http://ha.example:8123'

  await page.locator('input[type=file]').setInputFiles({ name: 'renamed.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(renamed)) })
  await expect(page.getByText('Ready to restore')).toBeVisible()
  await page.getByRole('button', { name: 'Restore', exact: true }).click()
  await expect(page.getByText('Backup restored')).toBeVisible()
  // Backups never carry the HA token: the restored Home Assistant settings need it again.
  const tokenNote = page.getByRole('status').filter({ hasText: 'Enter the Home Assistant token again' })
  await expect(tokenNote).toBeVisible()
  await expect(tokenNote.getByRole('link', { name: 'Integrations' })).toHaveAttribute('href', '#/integrations')

  expect(await hub.activeActivity()).toBeNull()
  const list = await hub.get('/activities')
  expect(list.active_activity_id).toBeNull()
  expect(list.activities.map((a: { id: string }) => a.id).sort()).toEqual(ids.map((id) => `${id}_v2`).sort())
})

test('a hub that couldn’t read storage when it loaded refuses a backup (503): a clear message, no file', async ({ page }) => {
  // Only reachable at boot with storage down (storage-boot.spec), where nobody can sign in; stub that answer here.
  await page.route('**/api/v1/admin/backup', (r) => r.fulfill({ status: 503, json: fixture('errors/backup_storage_unavailable.json') }))
  let saved = false
  page.on('download', () => (saved = true))
  await open(page, '/settings/backup')
  await page.getByRole('button', { name: 'Download backup' }).click()
  await expect(page.getByRole('alert').filter({ hasText: 'The hub can’t read its storage' })).toContainText('No backup was made')
  expect(saved).toBe(false)
})

test('resetting the configuration needs “reset” typed first', async ({ page }) => {
  await open(page, '/settings/backup')
  await page.getByRole('button', { name: 'Reset configuration…' }).click()
  const dialog = page.getByRole('dialog', { name: 'Reset the configuration?' })
  const confirm = dialog.getByRole('button', { name: 'Reset configuration' })
  await expect(confirm).toBeDisabled()
  const word = dialog.getByLabel('Type “reset” to confirm')
  await word.fill('rese')
  await expect(confirm).toBeDisabled()
  await word.fill('reset')
  await confirm.click()
  await expect(page.getByText('Configuration reset')).toBeVisible()
  await open(page, '/devices')
  await expect(page.getByText('No devices yet')).toBeVisible()
})

test('a factory reset erases everything: the hub restarts into setup', async ({ page }) => {
  await open(page, '/settings/backup')
  await page.getByRole('button', { name: 'Factory reset…' }).click()
  const dialog = page.getByRole('dialog', { name: 'Erase everything on the hub?' })
  await expect(dialog).toContainText('forget “Hubbo” in the Apple TV’s Bluetooth settings')
  const confirm = dialog.getByRole('button', { name: 'Erase everything' })
  const word = dialog.getByLabel('Type “erase” to confirm')
  await word.fill('reset') // the configuration reset's word doesn't do
  await expect(confirm).toBeDisabled()
  await word.fill('erase')
  await confirm.click()
  await expect(dialog.getByText('Erasing… the hub is restarting')).toBeVisible()
  await advance(page, 1_000)
  await expect(page.getByRole('heading', { name: 'Set up Hubbo' })).toBeVisible()
  expect(await session(page)).toMatchObject({ authenticated: false, auth_state: 'fresh', setup_required: true })
})

test('a factory reset the hub can’t do erases nothing and says why', async ({ page }) => {
  await control(page, 'factory-reset?ready=0')
  await open(page, '/settings/backup')
  await page.getByRole('button', { name: 'Factory reset…' }).click()
  const dialog = page.getByRole('dialog', { name: 'Erase everything on the hub?' })
  await dialog.getByLabel('Type “erase” to confirm').fill('erase')
  await dialog.getByRole('button', { name: 'Erase everything' }).click()
  await expect(dialog.getByRole('alert').filter({ hasText: 'Nothing was erased' })).toBeVisible()
  await expect(dialog.getByRole('button', { name: 'Cancel' })).toBeEnabled()
  expect((await session(page)).auth_state).not.toBe('fresh')
})
