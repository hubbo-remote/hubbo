import { readFile } from 'node:fs/promises'
import { api, expect, open, test } from './hub.ts'

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
  const hub = api(page)
  expect((await hub.post('/activities/act_play_xbox/activate')).status()).toBe(200)
  await expect.poll(hub.activeActivity).toBe('act_play_xbox')

  await open(page, '/settings/backup')
  const { text } = await downloadBackup(page, 'Download backup')
  // Same configuration, every activity id changed (global bindings that name them too).
  const ids: string[] = JSON.parse(text).activities.map((a: { id: string }) => a.id)
  const renamed = ids.reduce((t, id) => t.replaceAll(`"${id}"`, `"${id}_v2"`), text)

  await page.locator('input[type=file]').setInputFiles({ name: 'renamed.json', mimeType: 'application/json', buffer: Buffer.from(renamed) })
  await expect(page.getByText('Ready to restore')).toBeVisible()
  await page.getByRole('button', { name: 'Restore', exact: true }).click()
  await expect(page.getByText('Backup restored')).toBeVisible()

  expect(await hub.activeActivity()).toBeNull()
  const list = await hub.get('/activities')
  expect(list.active_activity_id).toBeNull()
  expect(list.activities.map((a: { id: string }) => a.id).sort()).toEqual(ids.map((id) => `${id}_v2`).sort())
})
