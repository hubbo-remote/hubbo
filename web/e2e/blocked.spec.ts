import { readFile } from 'node:fs/promises'
import { expect, open, test } from './hub.ts'

test.use({ reset: { configState: 'blocked' } })

const RAW_NOTE = 'This is the hub’s saved configuration exactly as stored, exported while the hub is blocked. It may only be restorable on firmware that can read it.'

// Runs before the configuration reset below: on hub-sim (seeded with `hub-sim write-record`) the blocked state is real and a reset ends it.
test('blocked: download the saved configuration raw; restoring it here needs newer firmware', async ({ page }) => {
  await open(page, '/activities')
  const banner = page.getByRole('alert').filter({ hasText: 'The saved configuration can’t be used' })
  await expect(banner).toContainText('download the saved configuration (raw)')
  await banner.getByRole('link', { name: 'Download raw, restore or reset…' }).click()

  await expect(page.getByText(RAW_NOTE)).toBeVisible() // before the download
  const [download] = await Promise.all([page.waitForEvent('download'), page.getByRole('button', { name: 'Download the saved configuration (raw)' }).click()])
  const filename = download.suggestedFilename()
  expect(filename).toMatch(/^hubbo-backup-BLOCKED-\d{4}-\d\d-\d\d\.json$/)
  const path = (await download.path())!
  const raw = await readFile(path)
  expect(raw.length).toBeGreaterThan(0)
  expect(JSON.parse(raw.toString('utf8')).format).toBe('hubbo-config') // the stored record, not a Backup
  await expect(page.getByText(`Saved as ${filename}`)).toBeVisible() // and after
  await expect(page.getByText(RAW_NOTE)).toBeVisible()

  // The record is from newer firmware: this hub refuses it, and nothing changes.
  await page.locator('input[type=file]').setInputFiles(path)
  const refused = page.getByRole('alert').filter({ hasText: 'This file was made by newer firmware; update the hub first' })
  await expect(refused).toBeVisible()
  await expect(page.getByRole('button', { name: 'Restore', exact: true })).toBeDisabled()
  await expect(banner).toBeVisible()
})

test('a blocked config: persistent banner, edits disabled, resetting the configuration is the way out', async ({ page }) => {
  await open(page, '/activities')
  const banner = page.getByRole('alert').filter({ hasText: 'The saved configuration can’t be used' })
  await expect(banner).toBeVisible()
  await expect(banner).toContainText(/newer/i) // the hub's reason
  await expect(page.getByRole('button', { name: 'New activity' })).toBeDisabled()

  await open(page, '/devices')
  await expect(banner).toBeVisible() // persistent across pages
  const addButtons = page.getByRole('button', { name: /Add (device|your first device)/ })
  await expect(addButtons.first()).toBeVisible()
  for (const b of await addButtons.all()) await expect(b).toBeDisabled()

  await banner.getByRole('link', { name: 'Download raw, restore or reset…' }).click()
  await page.getByRole('button', { name: 'Reset configuration…' }).click()
  const dialog = page.getByRole('dialog')
  await expect(dialog).toContainText('can’t be undone')
  await dialog.getByLabel('Type “reset” to confirm').fill('reset')
  await dialog.getByRole('button', { name: 'Reset configuration' }).click()
  await expect(banner).toBeHidden()

  await open(page, '/devices')
  await expect(page.getByText('No devices yet')).toBeVisible()
  await expect(page.getByRole('button', { name: 'Add your first device' })).toBeEnabled()
})

test.describe('damaged', () => {
  test.use({ reset: { configState: 'blocked', configDamaged: true } })

  test('a damaged config has nothing intact to download: a clear 409, never an empty backup', async ({ page }) => {
    let saved = false
    page.on('download', () => (saved = true))
    await open(page, '/settings/backup')
    await page.getByRole('button', { name: 'Download the saved configuration (raw)' }).click()
    await expect(page.getByRole('alert').filter({ hasText: 'Nothing intact to download' })).toContainText('damaged')
    expect(saved).toBe(false)
  })
})
