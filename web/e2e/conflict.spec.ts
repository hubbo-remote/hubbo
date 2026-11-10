import { api, expect, open, test } from './hub.ts'

/** Someone else saves the activity while this page is editing it. */
async function saveElsewhere(page: Parameters<typeof api>[0], name: string) {
  const hub = api(page)
  const theirs = await hub.get('/activities/act_play_xbox')
  expect((await hub.put('/activities/act_play_xbox', { ...theirs, name })).status()).toBe(200)
  return hub
}

test('saving over a newer revision shows the conflict and can overwrite', async ({ page }) => {
  await open(page, '/activities/act_play_xbox')
  const name = page.getByLabel('Activity name')
  await expect(name).toHaveValue('Play Xbox')
  await name.fill('Play Xbox (mine)')
  const hub = await saveElsewhere(page, 'Play Xbox (theirs)')

  await page.getByRole('button', { name: /^Save/ }).click()
  const dialog = page.getByRole('dialog')
  await expect(dialog.getByRole('heading', { name: 'Changed somewhere else' })).toBeVisible()
  await expect(dialog).toContainText('Play Xbox (theirs)')

  await dialog.getByRole('button', { name: 'Save mine' }).click()
  await expect(dialog).toBeHidden()
  await expect(page.getByRole('main').getByText('Saved', { exact: true })).toBeVisible()
  expect((await hub.get('/activities/act_play_xbox')).name).toBe('Play Xbox (mine)')
})

test('conflict: discard mine and load theirs', async ({ page }) => {
  await open(page, '/activities/act_play_xbox')
  await page.getByLabel('Activity name').fill('Mine')
  await saveElsewhere(page, 'Theirs')
  await page.getByRole('button', { name: /^Save/ }).click()
  await page.getByRole('dialog').getByRole('button', { name: 'Load theirs' }).click()
  await expect(page.getByLabel('Activity name')).toHaveValue('Theirs')
})

test('a tab left open across a restore gets the conflict dialog, not a silent overwrite', async ({ page }) => {
  await open(page, '/activities/act_play_xbox')
  await page.getByLabel('Activity name').fill('Edited before the restore')

  // Elsewhere: restore a backup. Every object gets a new revision.
  const hub = api(page)
  const backup = await hub.get('/admin/backup')
  // Backups may omit revisions (or carry 0); the hub stamps everything with a new generation.
  for (const a of backup.activities) delete a.revision
  for (const d of backup.devices) d.revision = 0
  expect((await hub.post('/admin/restore?dry_run=false', backup)).status()).toBe(200)

  await page.getByRole('button', { name: /^Save/ }).click()
  await expect(page.getByRole('dialog').getByRole('heading', { name: 'Changed somewhere else' })).toBeVisible()
})
