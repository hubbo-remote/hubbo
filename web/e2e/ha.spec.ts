import type { Page } from '@playwright/test'
import { api, connectHa, discoverHa, expect, expectNotStoredInBrowser, open, removeHaEntity, setHaState, SIM_URL, test } from './hub.ts'

// Never a real Home Assistant: the mock, or hub-sim's in-process fake HA, plays it.
// Entities differ by target: the mock serves api/fixtures (with realistic platforms);
// hub-sim's fake HA names entities by id, all on the `demo` platform.
const T = SIM_URL
  ? { free: 'media_player.denon', freeCommand: 'media_player.turn_on', platform: 'demo' }
  : { free: 'switch.tv_backlight', freeCommand: 'switch.turn_on', platform: 'hue' }

test.beforeEach(async ({ page }) => connectHa(page))

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
  await expect(conn).toContainText('Connected')

  await setHaState(page, 'unreachable')
  await expect(conn).toContainText('Unreachable')
  await expect(conn).toContainText('keeps retrying')

  await setHaState(page, 'connected')
  await expect(conn).toContainText('Connected')

  await setHaState(page, 'auth_failed')
  await expect(conn).toContainText('Token rejected')
  await expect(conn).toContainText('won’t retry until you save a new token')

  if (!SIM_URL) {
    await setHaState(page, 'authenticating')
    await expect(conn).toContainText('Signing in')
  }
})

test('the token is write-only, and changing the URL says it clears the token', async ({ page }) => {
  await open(page, '/integrations')
  const form = page.locator('form').filter({ has: page.getByLabel('Home Assistant URL') })
  await expect(form.getByLabel('Long-lived access token')).toHaveValue('') // never filled from the hub
  await expect(form).toContainText('A token is stored on the hub')

  await form.getByLabel('Home Assistant URL').fill('http://ha-e2e.invalid:8123')
  await expect(page.getByText('Changing the URL clears the stored token')).toBeVisible()
  await form.getByRole('button', { name: 'Save and connect' }).click()
  await expect(form.getByLabel('Long-lived access token')).toBeFocused() // required again: the browser stops the save

  await form.getByLabel('Long-lived access token').fill('e2e-ha-token-not-real')
  await form.getByRole('button', { name: 'Save and connect' }).click()
  await expect(page.getByText('Home Assistant settings saved')).toBeVisible()
  await expect(form.getByLabel('Long-lived access token')).toHaveValue('')
  await expect(form).toContainText('A token is stored on the hub')
  await expectNotStoredInBrowser(page, 'e2e-ha-token-not-real')
  expect(JSON.stringify(await api(page).get('/admin/integrations/ha_main'))).not.toContain('e2e-ha-token-not-real')
})

test('add an HA device: discover, search, preview its commands, register', async ({ page }) => {
  const dialog = await openAddHa(page)
  const list = dialog.getByRole('list', { name: 'Discovered entities' })
  await expect(list).toBeVisible() // discovery started on its own and finished
  await expect(dialog.getByText('7 entities found.')).toBeVisible()
  await expect(list.getByRole('button', { name: /family_room_sonos/ })).toBeDisabled() // already added

  await dialog.getByLabel('Search Home Assistant entities').fill(T.free)
  await expect(list.getByRole('button')).toHaveCount(1)
  await list.getByRole('button', { name: T.free }).click()
  const preview = dialog.getByRole('region', { name: 'Commands this device will have' })
  await expect(preview).toContainText('Built-in commands')
  await expect(preview).toContainText(T.freeCommand)
  await expect(preview).toContainText(T.platform)

  await dialog.getByLabel('Name').fill('E2E device')
  await dialog.getByRole('button', { name: 'Register device' }).click()
  await expect(dialog).toBeHidden()
  await expect(page.getByRole('heading', { name: 'E2E device' })).toBeVisible() // its page, with the stored commands
  await expect(page.getByRole('main')).toContainText(T.freeCommand)
})

test('registering an entity that isn’t in the latest discovery reads clearly', async ({ page }) => {
  const dialog = await openAddHa(page)
  await dialog.getByLabel('Search Home Assistant entities').fill(T.free)
  await dialog.getByRole('button', { name: T.free }).click()
  // Elsewhere, the discovery is cancelled (its candidates dropped) before Register.
  expect((await api(page).del('/admin/integrations/ha_main/discover')).status()).toBe(204)
  await dialog.getByRole('button', { name: 'Register device' }).click()
  await expect(dialog.getByRole('alert')).toContainText('latest discovery doesn’t include this entity')
  await expect(dialog.getByRole('alert')).toContainText('Discover again')
})

test('discovery while Home Assistant is down explains it', async ({ page }) => {
  await setHaState(page, 'unreachable')
  const dialog = await openAddHa(page)
  await expect(dialog.getByRole('alert').filter({ hasText: 'Home Assistant isn’t connected' })).toBeVisible()
})

test('the add flow previews integration services, what was skipped, and unsupported entities', async ({ page }) => {
  test.skip(!!SIM_URL, 'hub-sim’s fake HA has no integration services or unsupported entities')
  // Every fixture entity with integration services is in use: move the Sonos device to another entity to free its own.
  const hub = api(page)
  await discoverHa(page) // a re-link uses the latest discovery
  const sonos = await hub.get('/devices/dev_sonos')
  expect((await hub.put('/admin/devices/dev_sonos', { provider: { type: 'home_assistant', entity_id: 'switch.tv_backlight' }, revision: sonos.revision })).status()).toBe(200)

  const dialog = await openAddHa(page)
  await dialog.getByLabel('Search Home Assistant entities').fill('kitchen')
  await expect(dialog.getByRole('button', { name: /Kitchen Display/ })).toBeDisabled()
  await expect(dialog.getByRole('list', { name: 'Discovered entities' })).toContainText('Not supported')

  await dialog.getByLabel('Search Home Assistant entities').fill('sonos')
  await dialog.getByRole('button', { name: /Family Room Sonos/ }).click()
  const preview = dialog.getByRole('region', { name: 'Commands this device will have' })
  await expect(preview).toContainText('media_player.volume_set')
  await expect(preview.getByText('From the sonos integration')).toBeVisible()
  await expect(preview).toContainText('sonos.set_sleep_timer')
  await expect(preview).toContainText('Not offered')
  await expect(preview).toContainText('sonos.snapshot')
})

test('a stale device is clearly marked, its commands fail, and a re-link fixes it', async ({ page }) => {
  // hub-sim checks a re-link against the activities using the device (their params must still fit),
  // so use a fresh device there; the mock reuses a fixture device.
  const hub = api(page)
  let id = 'dev_lg_tv'
  let entity = 'media_player.family_room_lg_tv'
  let target = T.free
  if (SIM_URL) {
    entity = T.free
    target = 'media_player.limits_test'
    await discoverHa(page)
    const r = await hub.post('/admin/devices', { integration_id: 'ha_main', provider: { type: 'home_assistant', entity_id: entity }, name: 'Stale e2e', kind: 'other' })
    expect(r.status(), await r.text()).toBe(201)
    id = (await r.json()).id
  }
  await removeHaEntity(page, entity) // HA forgets it; the next discovery notices
  await open(page, '/devices')
  await expect(page.getByRole('link').filter({ hasText: entity }).getByText('Needs re-link')).toBeVisible()

  await open(page, `/devices/${id}`)
  const alert = page.getByRole('alert').filter({ hasText: 'This device needs re-linking' })
  await expect(alert).toContainText(entity)
  await expect(alert).toContainText('commands fail')

  await alert.getByRole('button', { name: 'Re-link…' }).click()
  const dialog = page.getByRole('dialog')
  await expect(dialog.getByRole('button', { name: /family_room_sonos|Family Room Sonos/ })).toBeDisabled() // another device's entity
  await dialog.getByLabel('Search Home Assistant entities').fill(target)
  await dialog.getByRole('button', { name: target }).click()
  await expect(page.getByText(`Linked to ${target}`)).toBeVisible()
  await expect(alert).toBeHidden()
})

test('search sends the text URL-encoded (spaces too)', async ({ page }) => {
  const dialog = await openAddHa(page)
  const query = SIM_URL ? 'family_room_sonos' : 'Family Room S' // hub-sim names entities by id (no spaces)
  const sent = page.waitForRequest((r) => r.url().includes('/discover?') && r.url().includes(encodeURIComponent(query)))
  await dialog.getByLabel('Search Home Assistant entities').fill(query)
  expect((await sent).url()).not.toContain('+')
  await expect(dialog.getByRole('list', { name: 'Discovered entities' }).getByRole('button')).toHaveCount(1)
})

test.describe('a capped discovery', () => {
  // hub-sim's fake HA has far fewer than 2048 entities.
  test.use({ reset: { discoverTruncated: 120 } })

  test('says how many entities were left out', async ({ page }) => {
    const dialog = await openAddHa(page)
    await expect(dialog.getByText('120 entities weren’t loaded (more than 2048 supported entities).')).toBeVisible()
  })
})

test.describe('discovery problems', () => {
  // hub-sim's fake HA can't be told to fail a discovery.
  test.use({ reset: { discoverFails: true } })

  test('a failed discovery says why and can be retried', async ({ page }) => {
    const dialog = await openAddHa(page)
    await expect(dialog.getByRole('alert').filter({ hasText: 'Discovery failed' })).toContainText('didn\'t answer in time')
    await expect(dialog.getByRole('button', { name: 'Try again' })).toBeVisible()
  })
})
