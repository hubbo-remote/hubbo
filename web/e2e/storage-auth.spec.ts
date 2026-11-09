import { api, expect, onAuthSim, open, setStorage, test } from './hub.ts'

// hub-sim --auth only. With the store unreadable nobody can authenticate, so
// every protected request gets 503 storage_unavailable (activate included).
// The UI re-checks the session and shows the storage screen, not a pile of errors.
// Storage can't be brought back from here (POST /sim/storage needs a session),
// so this runs in the `last` project.
test('storage drops mid-session under auth: the storage screen takes over', async ({ page }) => {
  test.skip(!(await onAuthSim(page)), 'hub-sim --auth only')
  await open(page, '/activities')
  await setStorage(page, 'unavailable')

  await expect(page.getByRole('heading', { name: 'Hubbo can’t read its storage' })).toBeVisible() // on the next status poll
  await expect(page.getByRole('navigation', { name: 'Main' })).toHaveCount(0)
  const r = await api(page).post('/activities/act_play_xbox/activate')
  expect([r.status(), (await r.json()).error.code]).toEqual([503, 'storage_unavailable'])
})
