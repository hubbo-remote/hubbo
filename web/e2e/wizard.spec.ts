// The new-activity wizard: devices → watch → volume → inputs → power → review → an ordinary activity.
import { activityIdFromUrl, stepIds } from './editor.ts'
import type { Page } from '@playwright/test'
import { addBle, api, expect, open, test } from './hub.ts'

/** The review's answer row with this label. */
const answer = (page: Page, label: string) => page.getByRole('list', { name: 'Answers' }).getByRole('listitem').filter({ hasText: label })
const reviewSteps = (page: Page, section: 'Start' | 'End') => page.getByRole('list', { name: `${section} steps` }).getByRole('listitem')

test('the wizard builds a starter activity from an HA TV and receiver', async ({ page }) => {
  await open(page, '/')
  await page.getByRole('link', { name: 'New activity' }).first().click()
  const next = page.getByRole('button', { name: 'Continue' })
  await expect(next).toBeDisabled()
  await expect(page.getByRole('heading', { name: 'Included Devices' })).toBeVisible()
  await page.getByLabel('Name', { exact: true }).fill('E2E Movie Night')
  await page.getByLabel('Family Room LG TV').check()
  await page.getByLabel('Family Room Denon').check()
  await next.click()

  await expect(page.getByRole('heading', { name: /What are you watching with/ })).toBeVisible()
  await page.getByLabel('Family Room LG TV').check()
  await next.click()
  await expect(page.getByRole('heading', { name: /What controls volume/ })).toBeVisible()
  await page.getByLabel('Family Room Denon').check()
  await next.click()

  await page.getByRole('combobox', { name: 'Family Room LG TV' }).selectOption('HDMI 1')
  await page.getByRole('combobox', { name: 'Family Room Denon' }).selectOption('Media Player')
  await next.click()
  await page.getByRole('switch', { name: 'Turn off Family Room Denon' }).click()
  await next.click()

  // Review: the answers and the start and end steps, and Change back to a step.
  await expect(page.getByRole('heading', { name: 'E2E Movie Night' })).toBeVisible()
  await expect(answer(page, 'Watching with')).toContainText('Family Room LG TV')
  await expect(answer(page, 'Volume')).toContainText('Family Room Denon')
  await expect(answer(page, 'Inputs')).toContainText('Family Room LG TV → HDMI 1, Family Room Denon → Media Player')
  await expect(reviewSteps(page, 'Start')).toHaveCount(6)
  await expect(reviewSteps(page, 'Start').first()).toContainText('Family Room LG TV')
  await expect(reviewSteps(page, 'End')).toHaveCount(1)
  await page.getByRole('button', { name: 'Change watching with' }).click()
  await expect(page.getByRole('heading', { name: /What are you watching with/ })).toBeVisible()
  for (let i = 0; i < 4; i++) await next.click()
  await page.getByRole('button', { name: 'Create activity' }).click()

  // An ordinary activity, opened in the editor.
  const id = await activityIdFromUrl(page)
  const a = await api(page).get(`/activities/${id}`)
  expect(a.name).toBe('E2E Movie Night')
  expect(stepIds(a.startup)).toEqual([
    'dev_lg_tv media_player.turn_on',
    ' wait',
    'dev_lg_tv media_player.select_source',
    'dev_denon media_player.turn_on',
    ' wait',
    'dev_denon media_player.select_source',
  ])
  expect(a.shutdown.map((s: { device_id: string }) => s.device_id)).toEqual(['dev_lg_tv'])
  expect(a.bindings['volume_up:down'][0]).toMatchObject({ device_id: 'dev_denon', command_id: 'media_player.volume_up' })
  expect(a.bindings['mute:hold'][0]).toMatchObject({ device_id: 'dev_denon', params: { muted: false } })
  expect(a.bindings['play:down'][0]).toMatchObject({ device_id: 'dev_lg_tv', command_id: 'media_player.media_play' }) // HA: one send on press
  // The buttons show in the editor.
  await expect(page.locator('[data-key="volume_up"]')).toHaveAttribute('aria-label', /bound/)
  await expect(page.locator('[data-key="play"]')).toHaveAttribute('aria-label', /bound/)
})

test('ticking devices never disables Continue or reloads the step', async ({ page }) => {
  await open(page, '/activities/new')
  await page.getByLabel('Name', { exact: true }).fill('E2E Ticks')
  await page.getByLabel('Family Room LG TV').check()
  const next = page.getByRole('button', { name: 'Continue' })
  await expect(next).toBeEnabled()
  // Count any disabled moment or device fetch while ticking (a flash is too short to poll for).
  let fetches = 0
  page.on('request', (r) => {
    if (/\/devices\/dev_/.test(r.url())) fetches++
  })
  await next.evaluate((b: HTMLButtonElement) => {
    new MutationObserver(() => {
      if (b.disabled) b.dataset.flashed = '1'
    }).observe(b, { attributes: true, attributeFilter: ['disabled'] })
  })
  for (const d of ['Family Room Denon', 'Family Room Apple TV', 'Family Room Lights']) await page.getByLabel(d).check()
  await page.getByLabel('Family Room Denon').uncheck()
  await expect(next).toBeEnabled()
  await expect(next).not.toHaveAttribute('data-flashed')
  expect(fetches).toBe(0)
})

test('an answer whose step was skipped has no Change link', async ({ page }) => {
  await open(page, '/activities/new')
  await page.getByLabel('Name', { exact: true }).fill('E2E Lights')
  await page.getByLabel('Family Room Lights').check()
  const next = page.getByRole('button', { name: 'Continue' })
  await next.click() // no watch, volume or inputs: straight to power
  await expect(page.getByRole('heading', { name: 'Power' })).toBeVisible()
  await next.click()
  await expect(answer(page, 'Watching with')).toContainText('None')
  await expect(answer(page, 'Volume')).toContainText('None')
  await expect(answer(page, 'Inputs')).toHaveCount(0)
  await expect(page.getByRole('button', { name: /^Change (watching with|volume|inputs)$/ })).toHaveCount(0)
  await page.getByRole('button', { name: 'Change start' }).click()
  await expect(page.getByRole('heading', { name: 'Power' })).toBeVisible()
})

test('with an Apple TV: it gets the remote, and no power steps', async ({ page }) => {
  await open(page, '/activities/new')
  const next = page.getByRole('button', { name: 'Continue' })
  await page.getByLabel('Name', { exact: true }).fill('E2E Watch Apple TV')
  for (const d of ['Family Room LG TV', 'Family Room Denon', 'Family Room Apple TV']) await page.getByLabel(d).check()
  await next.click()
  await page.getByLabel('Family Room Apple TV').check() // watch
  await next.click()
  await page.getByLabel('Family Room Denon').check() // volume
  await next.click()
  await page.getByRole('combobox', { name: 'Family Room LG TV' }).selectOption('HDMI 1')
  await next.click()
  // Power: the TV and receiver, not the Apple TV (its profile has no power steps).
  await expect(page.getByRole('switch', { name: 'Turn on Family Room LG TV' })).toBeVisible()
  await expect(page.getByRole('switch', { name: /Family Room Apple TV/ })).toHaveCount(0)
  await next.click()

  await expect(answer(page, 'Watching with')).toContainText('Family Room Apple TV')
  await expect(answer(page, 'Inputs')).toContainText('Family Room LG TV → HDMI 1')
  await expect(reviewSteps(page, 'Start').filter({ hasText: 'Family Room Apple TV' })).toHaveCount(0) // nothing to send it, so no wait to connect
  await page.getByRole('button', { name: 'Create activity' }).click()

  const a = await api(page).get(`/activities/${await activityIdFromUrl(page)}`)
  expect(a.bindings['up:down'][0]).toMatchObject({ device_id: 'dev_apple_tv', command_id: 'press_down', params: { command: 'DirectionUp' } })
  expect(a.bindings['up:up'][0]).toMatchObject({ command_id: 'press_up', params: { command: 'DirectionUp' } })
  expect(a.bindings['select:down'][0]).toMatchObject({ command_id: 'press_down', params: { command: 'Select' } }) // every button passes through
  expect(a.bindings['select:up'][0]).toMatchObject({ command_id: 'press_up', params: { command: 'Select' } })
  // Menu: the profile's override, Home, as a down/up pair (held for Control Center).
  expect(a.bindings['menu:down'][0]).toMatchObject({ command_id: 'press_down', params: { command: 'Home' } })
  expect(a.bindings['menu:up'][0]).toMatchObject({ command_id: 'press_up', params: { command: 'Home' } })
  expect(a.bindings['menu:tap']).toBeUndefined()
  expect(a.bindings['volume_up:down'][0]).toMatchObject({ device_id: 'dev_denon' })
  expect([...a.startup, ...a.shutdown].filter((s: { device_id: string | null }) => s.device_id === 'dev_apple_tv')).toEqual([])
  expect(stepIds(a.startup)[0]).toBe('dev_lg_tv media_player.turn_on')
})

test('a Bluetooth device with power steps: startup waits for it to connect, then wakes it', async ({ page }) => {
  const id = await addBle(page, 'E2E Fire TV')
  await open(page, '/activities/new')
  await page.getByLabel('Name', { exact: true }).fill('E2E Fire TV night')
  await page.getByLabel('E2E Fire TV').check()
  const next = page.getByRole('button', { name: 'Continue' })
  const create = page.getByRole('button', { name: 'Create activity' })
  while (!(await create.isVisible())) await next.click() // its defaults on every step
  await expect(reviewSteps(page, 'Start').first()).toContainText('Wait until connected')
  await create.click()

  const a = await api(page).get(`/activities/${await activityIdFromUrl(page)}`)
  const steps = stepIds(a.startup)
  expect(steps[0]).toBe(`${id} wait_connected`)
  expect(steps.filter((s) => s.startsWith(id))).toContain(`${id} press`)
  expect(stepIds(a.shutdown).some((s) => s.startsWith(id))).toBe(true)
})
