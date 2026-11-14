import type { Page } from '@playwright/test'
import { api, connectHa, control, discoverHa, registerHa, expect, expectNotStoredInBrowser, FAKE_HA, fakeHaNextConnect, letHaConnect, open, removeHaEntity, setHaState, SIM_URL, test } from './hub.ts'

// Never a real Home Assistant: the mock, or hub-sim's in-process fake HA, plays it.
// Entities differ by target: the mock serves api/fixtures (with realistic platforms); hub-sim's
// fake HA has realistic players and a light, with integration services, and one free Denon.
const T = SIM_URL
  ? { free: 'media_player.denon', freeCommand: 'media_player.turn_on' }
  : { free: 'switch.tv_backlight', freeCommand: 'switch.turn_on' }

test.beforeEach(async ({ page }) => connectHa(page))

/** The HA settings form on Integrations (the MQTT form has its own Save). */
const haForm = (page: Page) => page.locator('form').filter({ has: page.getByLabel('Access token') })

async function openAddHa(page: Page) {
  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: /Home Assistant/ }).click()
  return dialog
}

test('connection state: each HA client state reads clearly, with what to do about it', async ({ page }) => {
  await open(page, '/integrations')
  const conn = page.getByTestId('ha-connection')
  const state = page.getByTestId('state-ha_main') // the card's header line: the one place the state shows
  await expect(state).toContainText('Connected')
  await expect(conn).not.toContainText('Connected')

  await setHaState(page, 'unreachable')
  await expect(state).toContainText('Unreachable')
  await expect(conn).toContainText('Check the URL')

  await setHaState(page, 'connected')
  await expect(state).toContainText('Connected')

  await setHaState(page, 'auth_failed')
  await expect(state).toContainText('Token rejected')
  await expect(conn).toContainText('Enter a new one')

  if (!SIM_URL) {
    await setHaState(page, 'authenticating')
    await expect(state).toContainText('Signing in')
  }
})

test('saving waits for the connection: a stall says so after a while, then a good save connects', async ({ page }) => {
  await page.clock.install() // runs normally; fastForward skips the wait
  await open(page, '/integrations')
  const conn = page.getByTestId('ha-connection')
  const token = page.getByLabel('Access token')
  const save = haForm(page).getByRole('button', { name: 'Save', exact: true })
  await fakeHaNextConnect(page, 'stall')
  await token.fill(FAKE_HA.token)
  await save.click()
  await expect(conn).toContainText('Saved. Connecting…')
  await page.clock.fastForward(21_000)
  await expect(conn).toContainText('Still connecting')

  await fakeHaNextConnect(page, 'ok')
  await token.fill(FAKE_HA.token)
  await save.click()
  await letHaConnect(page)
  await expect(conn).toContainText('Saved and connected.')
})

test('saving a wrong token while connected never shows the old connection as the result', async ({ page }) => {
  await open(page, '/integrations')
  const conn = page.getByTestId('ha-connection')
  await expect(page.getByTestId('state-ha_main')).toContainText('Connected')
  // Record every result shown, so a brief "Saved and connected" flash (from a pre-save read) is caught.
  await page.evaluate(() => {
    const w = window as unknown as { seen: string[] }
    w.seen = []
    new MutationObserver(() => w.seen.push(document.querySelector('[data-testid="ha-connection"]')?.textContent ?? '')).observe(document.body, { subtree: true, childList: true, characterData: true })
  })
  await fakeHaNextConnect(page, 'reject')
  await page.getByLabel('Access token').fill('e2e-wrong-token')
  await haForm(page).getByRole('button', { name: 'Save', exact: true }).click()
  await letHaConnect(page) // hub-sim: the HA task applies the settings as sim time runs
  await expect(conn).toContainText('The token was rejected')
  expect((await page.evaluate(() => (window as unknown as { seen: string[] }).seen)).filter((t) => t.includes('Saved and connected'))).toEqual([])
})

test('the token is write-only, and changing the URL says it clears the token', async ({ page }) => {
  await open(page, '/integrations')
  const form = haForm(page)
  await expect(form.getByLabel('Access token')).toHaveValue('') // never filled from the hub
  await expect(form).toContainText('Leave blank to keep the saved token')

  await form.getByLabel('URL', { exact: true }).fill('http://ha-e2e.invalid:8123')
  await expect(page.getByText('Enter the token again for the new URL.')).toBeVisible()
  await form.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(form.getByLabel('Access token')).toBeFocused() // required again: the browser stops the save

  await form.getByLabel('Access token').fill('e2e-ha-token-not-real')
  await form.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(form.getByLabel('Access token')).toHaveValue('') // cleared once the save lands
  await expect(form).toContainText('Leave blank to keep the saved token')
  await expectNotStoredInBrowser(page, 'e2e-ha-token-not-real')
  expect(JSON.stringify(await api(page).get('/admin/integrations/ha_main'))).not.toContain('e2e-ha-token-not-real')
})

test('add an HA device: discover, search, preview its commands, register; the dialog stays for the next', async ({ page }) => {
  const dialog = await openAddHa(page)
  const list = dialog.getByRole('list', { name: 'Devices found' })
  await expect(list).toBeVisible() // discovery started on its own and finished
  await expect(list.getByRole('button').first()).toBeVisible()
  await expect(list.getByRole('button', { name: /family_room_sonos/ })).toHaveCount(0) // already added: not offered

  await dialog.getByLabel('Search devices').fill(T.free)
  await expect(list.getByRole('button')).toHaveCount(1)
  await list.getByRole('button', { name: T.free }).click()
  const preview = dialog.getByRole('region', { name: 'Commands' })
  await expect(preview).toContainText(T.freeCommand)

  await expect(dialog.getByRole('button', { name: 'Cancel' })).toBeVisible()
  await dialog.getByLabel('Name').fill('E2E device')
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  // Still open, back on the entities with the search kept; the one just added is gone from them.
  await expect(dialog.getByRole('status').filter({ hasText: 'Added E2E device' })).toBeVisible()
  await expect(dialog.getByLabel('Search devices')).toHaveValue(T.free)
  await expect(dialog.getByText('Nothing matches.')).toBeVisible()
  await dialog.getByRole('button', { name: 'Done' }).click()
  await expect(dialog).toBeHidden()
  await page.getByRole('link', { name: /E2E device/ }).click() // already in the list
  await expect(page.getByRole('heading', { name: 'E2E device' })).toBeVisible() // its page, with the stored commands
  await expect(page.getByRole('group', { name: /^(Turn|Power) on$/ }).first().getByRole('button', { name: 'Test' })).toBeVisible()
})

test('registering after another entity’s details were loaded elsewhere reads clearly (409 details_required)', async ({ page }) => {
  const dialog = await openAddHa(page)
  await dialog.getByLabel('Search devices').fill(T.free)
  await dialog.getByRole('button', { name: T.free }).click()
  await expect(dialog.getByRole('region', { name: 'Commands' })).toBeVisible()
  // Elsewhere (another tab), another entity's details are loaded before Register: the hub keeps one.
  expect((await api(page).post('/admin/integrations/ha_main/entities/switch.patio_fountain/details')).status()).toBe(202)
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  await expect(dialog.getByRole('alert')).toContainText('Something changed. Try again.')
})

test('discovery while Home Assistant is down explains it', async ({ page }) => {
  await setHaState(page, 'unreachable')
  const dialog = await openAddHa(page)
  await expect(dialog.getByRole('alert').filter({ hasText: 'Home Assistant isn’t connected' })).toBeVisible()
})

test('the add flow previews integration services, leaves out what it can’t use, and marks unsupported entities', async ({ page }) => {
  // The free Denon AVR, on both targets; the mock also has an unsupported entity.
  const P = { search: 'media_player.denon', name: /Denon AVR-X3700H/, service: 'denonavr.set_dynamic_eq', skipped: 'denonavr.get_command' }

  const dialog = await openAddHa(page)
  if (!SIM_URL) {
    await dialog.getByLabel('Search devices').fill('kitchen')
    await expect(dialog.getByText('Nothing matches.')).toBeVisible() // an unsupported entity isn't offered
  }

  await dialog.getByLabel('Search devices').fill(P.search)
  await dialog.getByRole('button', { name: P.name }).click()
  const preview = dialog.getByRole('region', { name: 'Commands' })
  await expect(preview).toContainText('media_player.volume_set')
  await expect(preview).toContainText(P.service)
  await expect(preview).not.toContainText(P.skipped)
})

test('picking an entity loads its actions; a failed load says why and can be retried', async ({ page }) => {
  const dialog = await openAddHa(page)
  // The search is debounced, and the previous list shows meanwhile: wait for this search's own result.
  const searched = page.waitForResponse((r) => r.request().method() === 'GET' && r.url().includes('/discover?') && new URL(r.url()).searchParams.get('q') === T.free)
  await dialog.getByLabel('Search devices').fill(T.free)
  await searched
  const entity = dialog.getByRole('button', { name: new RegExp(T.free.replace('.', '\\.')) })
  await expect(entity).toBeVisible()
  // HA forgets it after the list was read: its details can't load.
  await control(page, `ha?remove=${T.free}`)
  await entity.click()
  const failed = dialog.getByRole('alert').filter({ hasText: 'Couldn’t load its commands' })
  await expect(failed).toContainText(`${T.free} isn't in Home Assistant`)
  await expect(dialog.getByRole('button', { name: 'Add device', exact: true })).toBeDisabled()
  const again = page.waitForRequest((r) => r.method() === 'POST' && r.url().includes(`/entities/${T.free}/details`))
  await failed.getByRole('button', { name: 'Retry' }).click()
  await again
  await expect(failed).toBeVisible()
})

test('a YAML-only entity (no registry entry) registers, without integration services', async ({ page }) => {
  const dialog = await openAddHa(page)
  await dialog.getByLabel('Search devices').fill('patio')
  await dialog.getByRole('button', { name: /switch\.patio_fountain/ }).click()
  const preview = dialog.getByRole('region', { name: 'Commands' })
  await expect(preview).toContainText('switch.turn_on')
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  await expect(dialog.getByRole('status').filter({ hasText: /^Added / })).toBeVisible()
  const { devices } = (await api(page).get('/devices')) as { devices: { provider: { entity_id?: string; unique_id?: string } }[] }
  const d = devices.find((x) => x.provider.entity_id === 'switch.patio_fountain')
  expect(d).toBeDefined()
  expect(d!.provider.unique_id).toBeUndefined() // nothing to follow a rename by
})

test('an entity renamed in HA that is already a device says so, with a way to its device', async ({ page }) => {
  test.skip(!!SIM_URL, 'hub-sim’s fake HA can’t rename an entity')
  // The list matches by entity_id, so the renamed LG TV looks free there; its details name the device.
  await control(page, 'ha?rename=media_player.family_room_lg_tv&to=media_player.living_room_lg_tv')
  const dialog = await openAddHa(page)
  await dialog.getByLabel('Search devices').fill('living_room_lg_tv')
  await dialog.getByRole('button', { name: /living_room_lg_tv/ }).click()
  const added = dialog.getByRole('status').filter({ hasText: 'Already added as Family Room LG TV' })
  await expect(added).toBeVisible()
  await expect(dialog.getByRole('button', { name: 'Add device', exact: true })).toBeDisabled()
  await added.getByRole('link', { name: 'Open it' }).click()
  await expect(dialog).toBeHidden()
  await expect(page).toHaveURL(/#\/devices\/dev_lg_tv$/)
})

test('a stale device is clearly marked, its commands fail, and a re-link fixes it', async ({ page }) => {
  // A re-link must keep the device's activities valid: on the mock, HA renames the LG TV's entity
  // (same commands); hub-sim can't rename, so use a fresh device there.
  let id = 'dev_lg_tv'
  let entity = 'media_player.family_room_lg_tv'
  let target = 'media_player.living_room_lg_tv'
  if (SIM_URL) {
    entity = T.free
    target = 'media_player.limits_test'
    id = (await registerHa(page, entity, 'Stale e2e')).id
  }
  await removeHaEntity(page, entity, SIM_URL ? undefined : target) // HA forgets it; the next discovery notices
  await open(page, '/devices')
  const row = page.locator(`a[href="#/devices/${id}"]`)
  await expect(row.getByRole('img', { name: 'Home Assistant, needs re-link' })).toBeVisible()
  await expect(row.getByText('Needs re-link')).toBeVisible()

  await open(page, `/devices/${id}`)
  const alert = page.getByRole('alert').filter({ hasText: 'Device not found' })
  await expect(alert).toContainText(entity)
  await expect(alert).toContainText('Change the entity')

  await alert.getByRole('button', { name: 'Fix' }).click()
  const dialog = page.getByRole('dialog', { name: /^Replace / })
  await expect(dialog.getByRole('list', { name: 'Devices found' }).getByRole('button').first()).toBeVisible()
  await expect(dialog.getByRole('button', { name: /family_room_sonos/ })).toHaveCount(0) // another device's entity isn't offered
  await dialog.getByLabel('Search devices').fill(target)
  await dialog.getByRole('button', { name: target }).click()
  await expect(page.getByText('Replaced', { exact: true })).toBeVisible()
  await expect(alert).toBeHidden()
})

test('a re-link that would break an activity or a global button is refused, naming them', async ({ page }) => {
  const hub = api(page)
  await discoverHa(page)
  const { id, name: first } = (await hub.get('/activities')).activities[0] as { id: string; name: string }
  // A global button that uses the LG TV the same way the activity's startup does.
  const lgSteps = ((await hub.get(`/activities/${id}`)).startup as { device_id: string | null }[]).filter((x) => x.device_id === 'dev_lg_tv')
  expect(lgSteps.length).toBeGreaterThan(0)
  const global = await hub.get('/global-bindings')
  const put = await hub.put('/global-bindings', { ...global, bindings: { ...global.bindings, 'tv:tap': lgSteps } })
  expect(put.status(), await put.text()).toBe(200)
  const target = SIM_URL ? T.free : 'switch.tv_backlight' // neither offers the LG TV's inputs
  await open(page, '/devices/dev_lg_tv')
  await page.getByRole('button', { name: 'Replace…' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByLabel('Search devices').fill(target)
  await dialog.getByRole('button', { name: target }).click()
  const alert = dialog.getByRole('alert')
  await expect(alert).toContainText('missing commands your activities use')
  await expect(alert).toContainText(`${first}, startup step`)
  await expect(alert).toContainText(/Global buttons, \S+ tap/) // the hub's /global_bindings/<trigger>/… paths
  expect((await hub.get('/devices/dev_lg_tv')).provider.entity_id).toBe('media_player.family_room_lg_tv') // unchanged
})

test('search sends the text URL-encoded (spaces too)', async ({ page }) => {
  const dialog = await openAddHa(page)
  const query = SIM_URL ? 'media_player.denon' : 'Patio Fount' // hub-sim names entities by id (no spaces)
  const sent = page.waitForRequest((r) => r.url().includes('/discover?') && r.url().includes(encodeURIComponent(query)))
  await dialog.getByLabel('Search devices').fill(query)
  expect((await sent).url()).not.toContain('+')
  await expect(dialog.getByRole('list', { name: 'Devices found' }).getByRole('button')).toHaveCount(1)
})

test.describe('a capped discovery', () => {
  // hub-sim's fake HA has far fewer than 2048 entities.
  test.use({ reset: { discoverTruncated: 120 } })

  test('says how many entities were left out', async ({ page }) => {
    const dialog = await openAddHa(page)
    await expect(dialog.getByText('120 more weren’t loaded.')).toBeVisible()
  })
})

test.describe('discovery problems', () => {
  // hub-sim's fake HA can't be told to fail a discovery.
  test.use({ reset: { discoverFails: true } })

  test('a failed discovery says why and can be retried', async ({ page }) => {
    const dialog = await openAddHa(page)
    await expect(dialog.getByRole('alert').filter({ hasText: 'Couldn’t load devices' })).toContainText('didn\'t answer in time')
    await expect(dialog.getByRole('button', { name: 'Try again' })).toBeVisible()
  })
})
