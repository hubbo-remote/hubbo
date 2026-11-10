import { connectHa, expect, open, SIM_URL, test } from './hub.ts'

test('devices list and command tests with typed params', async ({ page }) => {
  await open(page, '/devices')
  await expect(page.getByRole('link', { name: /Family Room Denon/ })).toBeVisible()
  await expect(page.getByRole('link', { name: /Family Room Apple TV/ })).toBeVisible()

  await page.getByRole('link', { name: /Family Room Denon/ }).click()
  await expect(page.getByRole('heading', { name: 'Family Room Denon' })).toBeVisible()
  await page.getByRole('button', { name: /^Set volume/ }).click()
  await page.getByLabel('Level', { exact: true }).fill('0.4')
  await page.getByRole('button', { name: 'Test' }).click()
  // hub-sim's fake adapters may answer differently from the mock's fixtures.
  await expect(page.getByText(SIM_URL ? /^(Accepted|Timed out|Failed)/ : /^Accepted/)).toBeVisible()

  // Out-of-range input is flagged before anything is sent.
  await page.getByLabel('Level', { exact: true }).fill('3')
  await expect(page.getByText('Level must be at most 1')).toBeVisible()
})

test('a command that times out reports it, not an HTTP error', async ({ page }) => {
  test.skip(!!SIM_URL, 'the timeout case comes from the mock fixtures (devices/execute.timeout.json)')
  await open(page, '/devices/dev_sonos')
  await page.getByRole('button', { name: /^Volume up/ }).click()
  await page.getByRole('button', { name: 'Test' }).click()
  await expect(page.getByText(/^Timed out/)).toBeVisible()
})

test('hold commands are binding-only on the device page', async ({ page }) => {
  await open(page, '/devices/dev_apple_tv')
  const keyDown = page.getByRole('button', { name: /^Key down/ })
  await expect(keyDown).toContainText('Binding only')
  await keyDown.click()
  await expect(page.getByText('can’t be tested on its own')).toBeVisible()
  await expect(page.getByRole('button', { name: 'Test' })).toHaveCount(0)
  // Momentary commands still have a test form.
  await page.getByRole('button', { name: /^Key tap/ }).click()
  await expect(page.getByRole('button', { name: 'Test' })).toBeVisible()
})

test('the Home Assistant search keeps its box (and focus) while results update; the kind picker shows its arrow', async ({ page }) => {
  await connectHa(page)
  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByRole('button', { name: /Home Assistant/ }).click()
  const search = dialog.getByLabel('Search Home Assistant entities')
  const results = dialog.getByRole('list', { name: 'Discovered entities' })
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
  await expect(dialog.getByLabel('Kind')).toBeVisible()
  await expect(dialog.locator('#dev-kind ~ svg')).toBeVisible() // a picker, not a text field
})
