import { activityIdFromUrl, addStep, bind, openNewActivity, saveActivity, stepIds } from './editor.ts'
import { advance, api, control, expect, open, SIM_URL, test } from './hub.ts'

test('build an activity: bindings on the remote, startup and shutdown', async ({ page }) => {
  const id = await openNewActivity(page, 'E2E Movie Night')

  // Held: a key's down, and its up on release (both written for us).
  await bind(page, 'up', 'hold', 'Family Room Apple TV', 'Keyboard key…', 'ARROW_UP')
  // Press and long press on Info.
  await bind(page, 'info', 'press', 'Family Room Apple TV', 'Keyboard key…', 'PLAY_PAUSE')
  await bind(page, 'info', 'long press', 'Family Room Apple TV', 'Keyboard key…', 'AC_HOME')
  // A Home Assistant binding, held: one send on down.
  await bind(page, 'volume_up', 'hold', 'Family Room Denon', 'Volume up')
  await expect(page.locator('[data-key="up"]')).toHaveAttribute('aria-label', /bound: hold$/)
  await expect(page.locator('[data-key="info"]')).toHaveAttribute('aria-label', /bound: press, long press$/)

  await page.getByRole('tab', { name: /Startup/ }).click()
  await addStep(page, 'Family Room LG TV', 'Power on')
  await page.getByRole('button', { name: 'Add wait' }).click()
  await addStep(page, 'Family Room Denon', 'Power on')
  await page.getByRole('button', { name: 'Move step 3 up' }).click()
  await expect(page.getByRole('list', { name: 'Startup steps' }).getByRole('listitem')).toHaveCount(3)

  await page.getByRole('tab', { name: /Shutdown/ }).click()
  await addStep(page, 'Family Room LG TV', 'Power off')

  await page.getByRole('button', { name: 'Check' }).click()
  await expect(page.getByText('No problems found')).toBeVisible()
  await saveActivity(page)

  // What the hub stored.
  const a = await api(page).get(`/activities/${id}`)
  expect(a.name).toBe('E2E Movie Night')
  expect(Object.keys(a.bindings).sort()).toEqual(['info:hold', 'info:tap', 'up:down', 'up:up', 'volume_up:down'])
  expect(a.bindings['up:down'][0]).toMatchObject({ device_id: 'dev_apple_tv', command_id: 'key_down', params: { key: 'ARROW_UP' } })
  expect(a.bindings['up:up'][0]).toMatchObject({ device_id: 'dev_apple_tv', command_id: 'key_up', params: { key: 'ARROW_UP' } })
  expect(a.bindings['info:hold'][0]).toMatchObject({ command_id: 'key_tap', params: { key: 'AC_HOME' } })
  expect(a.startup.map((s: { command_id: string }) => s.command_id)).toEqual(['media_player.turn_on', 'media_player.turn_on', 'wait'])
  expect(a.shutdown).toHaveLength(1)
})

test('errors show only from Save on; Save then saves nothing; they clear live', async ({ page }) => {
  const id = await openNewActivity(page, 'E2E Errors')
  const { revision } = await api(page).get(`/activities/${id}`)
  const name = page.getByLabel('Activity name')
  await name.fill('')
  await expect(name).toHaveAttribute('placeholder', 'Activity name') // a native, muted placeholder
  await expect(page.getByText('Name is required')).toHaveCount(0)
  await expect(name).not.toHaveAttribute('aria-invalid', 'true')

  await page.getByRole('button', { name: /^Save/ }).click()
  await expect(page.getByText('Name is required').first()).toBeVisible()
  await expect(name).toHaveAttribute('aria-invalid', 'true')
  expect((await api(page).get(`/activities/${id}`)).revision).toBe(revision)

  await name.fill('E2E Errors Clear')
  await expect(page.getByText('Name is required')).toHaveCount(0)
})

test('deleting the active activity switches to the next one', async ({ page }) => {
  // act_play_xbox is referenced by the global music button, so remove a fresh one instead.
  const hub = api(page)
  const first = (await hub.get('/activities')).activities[0].id
  const created = await hub.post('/activities', { name: 'E2E Temp', startup: [], shutdown: [], bindings: {} })
  expect(created.status()).toBe(201)
  const temp = await created.json()

  await open(page, '/')
  await page.getByRole('main').getByRole('button', { name: 'E2E Temp', exact: true }).click()
  await expect.poll(hub.activeActivity).toBe(temp.id)

  await page.getByRole('button', { name: 'More for E2E Temp' }).click()
  await page.getByRole('menuitem', { name: 'Delete' }).click()
  const dialog = page.getByRole('dialog')
  await expect(dialog).toContainText('switch to the next activity')
  await dialog.getByRole('button', { name: 'Delete' }).click()
  await expect(page.getByRole('main').getByRole('listitem').filter({ hasText: 'E2E Temp' })).toHaveCount(0)
  // It took the last list position, so the hub wraps to the first activity.
  await expect.poll(hub.activeActivity).toBe(first)
})

test('an activity referenced by a button cannot be deleted', async ({ page }) => {
  await open(page, '/')
  await page.getByRole('button', { name: 'More for Play Xbox' }).click()
  await page.getByRole('menuitem', { name: 'Delete' }).click()
  await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click()
  await expect(page.getByText('Couldn’t delete')).toBeVisible()
  await expect(page.getByText(/Used by global buttons/)).toBeVisible()
  await expect(page.getByRole('main').getByRole('button', { name: 'Play Xbox', exact: true })).toBeVisible()
})

test('steps reorder by keyboard (Alt+↑/↓) and by drag', async ({ page }) => {
  const id = await openNewActivity(page, 'E2E Reorder')
  await page.getByRole('tab', { name: /Startup/ }).click()
  await addStep(page, 'Family Room LG TV', 'Power on')
  await page.getByRole('button', { name: 'Add wait' }).click()
  await addStep(page, 'Family Room Denon', 'Power on')
  const steps = page.getByRole('list', { name: 'Startup steps' }).getByRole('listitem')

  await steps.nth(2).focus()
  await page.keyboard.press('Alt+ArrowUp')
  await page.keyboard.press('Alt+ArrowUp')
  await page.keyboard.press('Alt+ArrowUp') // already first: nothing
  await expect(steps.nth(0)).toContainText('Family Room Denon')
  await expect(steps.nth(0)).toBeFocused()
  await page.keyboard.press('Alt+ArrowDown')
  await expect(steps.nth(1)).toContainText('Family Room Denon')

  await steps.nth(2).dragTo(steps.nth(0)) // the wait, to the top
  await expect(steps.nth(0)).toContainText('Wait')
  await saveActivity(page)
  const a = await api(page).get(`/activities/${id}`)
  expect(stepIds(a.startup)).toEqual([' wait', 'dev_lg_tv media_player.turn_on', 'dev_denon media_player.turn_on'])
})

test('duplicate asks for the new name; the tile menu works from the keyboard', async ({ page }) => {
  await open(page, '/')
  const more = page.getByRole('button', { name: 'More for Watch Apple TV' })
  await more.click()
  const menu = page.getByRole('menu')
  await expect(menu.getByRole('menuitem', { name: 'Edit' })).toBeFocused()
  await page.keyboard.press('ArrowDown')
  await expect(menu.getByRole('menuitem', { name: 'Duplicate' })).toBeFocused()
  await page.keyboard.press('ArrowDown')
  await expect(menu.getByRole('menuitem', { name: 'Delete' })).toBeFocused()
  await page.keyboard.press('ArrowDown') // wraps
  await expect(menu.getByRole('menuitem', { name: 'Edit' })).toBeFocused()
  await page.keyboard.press('Escape')
  await expect(menu).toBeHidden()
  await expect(more).toBeFocused()

  await more.click()
  await page.keyboard.press('ArrowDown')
  await page.keyboard.press('Enter') // Duplicate
  const dialog = page.getByRole('dialog')
  const name = dialog.getByLabel('Name')
  await expect(name).toHaveValue('Watch Apple TV copy')
  await name.fill('Watch Apple TV (guest room)')
  await dialog.getByRole('button', { name: 'Duplicate' }).click()
  const copy = await api(page).get(`/activities/${await activityIdFromUrl(page)}`)
  const original = await api(page).get('/activities/act_watch_apple_tv')
  expect(copy.name).toBe('Watch Apple TV (guest room)')
  expect([copy.startup, copy.shutdown, copy.bindings]).toEqual([original.startup, original.shutdown, original.bindings])
})

test('closing the duplicate dialog while it saves stays on the list', async ({ page }) => {
  await open(page, '/')
  let release!: () => void
  const held = new Promise<void>((r) => (release = r))
  await page.route('**/api/v1/activities', async (route) => (route.request().method() === 'POST' ? (await held, route.continue()) : route.continue()))
  await page.getByRole('button', { name: 'More for Watch Apple TV' }).click()
  await page.getByRole('menuitem', { name: 'Duplicate' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: 'Duplicate' }).click()
  await page.keyboard.press('Escape')
  await expect(dialog).toBeHidden()
  release()
  await expect(page.getByText('Activity duplicated')).toBeVisible()
  await expect(page.getByText('Watch Apple TV copy')).toBeVisible()
  await expect(page).toHaveURL(/#\/$/)
})

test('with a single activity, a global button switches to it: the picker sets it, Save works, and pressing it starts it', async ({ page }) => {
  // One activity, nothing else bound (the global bindings refer to the others, so clear them first).
  const hub = api(page)
  const g = await hub.get('/global-bindings')
  expect((await hub.put('/global-bindings', { revision: g.revision, bindings: {} })).status()).toBe(200)
  const [only, ...rest] = (await hub.get('/activities')).activities as { id: string; name: string }[]
  for (const a of rest) {
    const { revision } = await hub.get(`/activities/${a.id}`)
    expect((await hub.del(`/activities/${a.id}?revision=${revision}`)).status(), `delete ${a.id}`).toBe(204)
  }
  if (SIM_URL) await hub.post('/activities/off')

  await open(page, '/activities/global')
  await page.locator('[data-key="tv"]').click()
  await page.getByRole('button', { name: /^Bind TV press$/i }).click()
  await page.getByLabel('Device').selectOption({ label: 'Hubbo' })
  await page.getByLabel('Command', { exact: true }).selectOption({ label: 'Switch activity' })
  // Unset shows as unset (never as the first activity, which picking again wouldn't set), and no
  // error is painted before Save.
  const target = page.getByLabel(/^Activity/)
  await expect(target).toHaveValue('')
  await expect(target.locator('option:checked')).toHaveText('Choose…')
  await expect(page.getByText(/is required/)).toHaveCount(0)
  await target.selectOption({ label: only!.name })
  await page.getByRole('button', { name: 'Apply' }).click()
  await page.getByRole('button', { name: 'Save' }).click()
  await expect(page.getByText('Saved', { exact: true })).toBeVisible()
  const saved = await hub.get('/global-bindings')
  expect(saved.bindings['tv:tap']).toEqual([{ integration_id: 'system', device_id: null, command_id: 'activate', params: { activity_id: only!.id } }])

  if (!SIM_URL) return // pressing the remote needs hub-sim
  expect(await hub.activeActivity()).toBeNull()
  await control(page, 'remote/tv/tap')
  await expect.poll(async () => (await advance(page, 500), await hub.activeActivity())).toBe(only!.id)
})
