import { api, connectHa, expect, open, SIM_URL, test } from './hub.ts'

test('devices list and command tests with typed params', async ({ page }) => {
  await connectHa(page) // hub-sim starts with Home Assistant not yet connected
  await open(page, '/devices')
  await expect(page.getByRole('link', { name: /Family Room Denon/ })).toBeVisible()
  await expect(page.getByRole('link', { name: /Family Room Apple TV/ })).toBeVisible()
  // One list, by name; each row's icon says its platform and whether it's live.
  const rows = page.locator('main a[href^="#/devices/"]')
  const names = await rows.evaluateAll((as) => as.map((a) => a.querySelector('p')?.textContent ?? ''))
  expect(names).toEqual([...names].sort((a, b) => a.localeCompare(b)))
  await expect(page.locator('a[href="#/devices/dev_apple_tv"]').getByRole('img', { name: 'Bluetooth, connected' })).toBeVisible()
  await expect(page.locator('a[href="#/devices/dev_denon"]').getByRole('img', { name: 'Home Assistant, available' })).toBeVisible()
  await expect(page.getByRole('heading', { name: 'Home Assistant' })).toHaveCount(0)

  await page.getByRole('link', { name: /Family Room Denon/ }).click()
  await expect(page.getByRole('heading', { name: 'Family Room Denon' })).toBeVisible()
  // Each command is a row: its name, what it needs (volume as a percent), Test.
  const volume = page.getByRole('group', { name: 'Set volume', exact: true })
  await expect(volume.getByLabel('Level', { exact: true })).toHaveValue('0.35')
  await volume.getByLabel('Level', { exact: true }).selectOption({ label: '40%' })
  await volume.getByRole('button', { name: 'Test' }).click()
  // hub-sim's fake adapters may answer differently from the mock's fixtures.
  await expect(volume.getByText(SIM_URL ? /^(Sent|Timed out|Failed)/ : /^Sent/)).toBeVisible()
  await expect(page.getByRole('group', { name: 'Unmute', exact: true }).getByRole('button', { name: 'Test' })).toBeVisible() // a bool splits on/off
})

test('a command that times out reports it, not an HTTP error', async ({ page }) => {
  test.skip(!!SIM_URL, 'the timeout case comes from the mock fixtures (devices/execute.timeout.json)')
  await open(page, '/devices/dev_sonos')
  const row = page.getByRole('group', { name: 'Volume up', exact: true })
  await row.getByRole('button', { name: 'Test' }).click()
  await expect(row.getByText(/^Timed out/)).toBeVisible()
})

test('a Bluetooth device lists its commands by name; holds are for buttons only', async ({ page }) => {
  await open(page, '/devices/dev_apple_tv')
  await expect(page.getByRole('group', { name: 'Up', exact: true }).getByRole('button', { name: 'Test' })).toBeVisible()
  const key = page.getByRole('group', { name: 'Keyboard key', exact: true })
  await expect(key.getByLabel('Key', { exact: true })).toBeVisible()
  await expect(page.getByText(/^Key (down|up)/)).toHaveCount(0) // no hold or release to test on its own
})

test('the Home Assistant search keeps its box (and focus) while results update; the type picker shows its arrow', async ({ page }) => {
  await connectHa(page)
  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: /Home Assistant/ }).click()
  const search = dialog.getByLabel('Search devices')
  const results = dialog.getByRole('list', { name: 'Devices found' })
  await expect(results).toBeVisible()
  await search.click()
  const box = await search.elementHandle()
  // Letter by letter, waiting out each letter's (debounced) search, so every letter runs a new one
  // (the iPhone keyboard closed when the box was swapped for a loading state).
  let typed = ''
  for (const ch of 'media') {
    const q = (typed += ch)
    const searched = page.waitForResponse((r) => new URL(r.url()).searchParams.get('q') === q)
    await page.keyboard.type(ch)
    await searched
    expect(await box!.evaluate((e) => e.isConnected && e === document.activeElement), `focus after "${ch}"`).toBe(true)
  }
  await expect(search).toHaveValue('media')
  await expect(results.getByRole('button').first()).toBeVisible()

  const free = SIM_URL ? 'media_player.denon' : 'switch.tv_backlight'
  await search.fill(free)
  await results.getByRole('button', { name: free }).click()
  await expect(dialog.getByLabel('Type')).toBeVisible()
  await expect(dialog.locator('#dev-kind ~ svg')).toBeVisible() // a picker, not a text field
})

test('changing a device’s type saves it, even right after a rename', async ({ page }) => {
  await open(page, '/devices/dev_denon')
  const type = page.getByLabel('Type', { exact: true })
  await expect(type).toBeEnabled() // the kinds have loaded
  const before = await type.inputValue()
  const other = (await type.locator('option').evaluateAll((os) => os.map((o) => (o as HTMLOptionElement).value))).find((v) => v !== before)!
  // A rename straight before: the two save one after the other, each on the latest revision.
  await page.getByLabel('Name', { exact: true }).fill('Den receiver')
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await type.selectOption(other)
  await expect(page.getByText('Type changed', { exact: true })).toBeVisible()
  await expect(page.getByText('Renamed', { exact: true })).toBeVisible()
  expect(await api(page).get('/devices/dev_denon')).toMatchObject({ name: 'Den receiver', kind: other })
  await page.reload()
  await expect(page.getByLabel('Type', { exact: true })).toHaveValue(other)
})
