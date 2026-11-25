// Add device → search the catalog: a manufacturer, then a model, then a name. The catalog is read
// by the browser; here it's the small fixture catalog the dev server serves (web/mock/catalog/), so
// nothing reaches the internet. Failures are made by answering the catalog's requests in the test.
import { API_BASE } from '../src/api/constants.ts'
import { api, expect, open, openAddDevice, pickModel, SIM_URL, test } from './hub.ts'

const CATALOG = '**/mock/catalog/'

/** A profile's command names in the order its JSON text has them (a parsed object puts digits first). */
const commandOrder = (json: string) => {
  const at = json.indexOf('"commands":')
  let depth = 0
  const names: string[] = []
  // The keys one level inside "commands".
  for (const m of json.slice(at + 11).matchAll(/"((?:[^"\\]|\\.)*)"\s*:|[{}]/g)) {
    if (m[0] === '{') depth++
    else if (m[0] === '}') {
      if (--depth === 0) break
    } else if (depth === 1) names.push(m[1]!)
  }
  return names
}
const EVOKE_ORDER = ['1', '2', '3', '4', 'DirectionLeft', 'DirectionRight', 'PowerToggle', 'Select', 'VolumeDown', 'VolumeUp', '5']

test('install an infrared device from the catalog, its commands in the profile’s order', async ({ page }) => {
  const dialog = await openAddDevice(page)
  const brands = dialog.getByRole('searchbox', { name: 'Search manufacturers' })
  await expect(brands).toBeVisible()
  await expect(dialog.getByRole('button', { name: /^(Denon|Google|Pure Digital|Samsung|Sonos|Sony)/ })).toHaveCount(0) // just the search, until something's typed
  await brands.fill('g')
  await expect(dialog.getByRole('button', { name: /^(Denon|Google|Pure Digital|Samsung|Sonos|Sony)/ })).toHaveText([/^Google/, /^Pure Digital/, /^Samsung/]) // prefix, then contains; each alphabetical
  await brands.fill('so')
  await expect(dialog.getByRole('button', { name: /^(Sony|Sonos|Samsung)/ })).toHaveText([/^Sonos/, /^Sony/]) // prefix matches only
  await brands.fill('sam')
  await dialog.getByRole('button', { name: /^Samsung/ }).click()

  // The model list, in model order, narrowed by type or by a search.
  const models = dialog.getByRole('button', { name: / · IR$/ }) // the type, then the integration's name
  await expect(models).toHaveText([/^HW-Q90R/, /^Q-Series/, /^QN55/, /^QN100/])
  await dialog.getByLabel('Type').selectOption('home_theater')
  await expect(models).toHaveText([/^HW-Q90R/])
  await dialog.getByLabel('Type').selectOption('')
  await dialog.getByRole('searchbox', { name: 'Search models' }).fill('q-s')
  await expect(models).toHaveText([/^Q-Series/])
  await dialog.getByRole('searchbox', { name: 'Search models' }).fill('qn')
  await expect(models).toHaveText([/^QN55/, /^QN100/])
  await dialog.getByRole('searchbox', { name: 'Search models' }).fill('55') // model numbers match from their start, never mid-token
  await expect(models).toHaveCount(0)
  await expect(dialog.getByText('No Samsung models start with “55”.')).toBeVisible()

  // Back to the start, and a device whose codeset has a digit command after the named ones.
  await dialog.getByRole('button', { name: 'Back' }).click()
  await pickModel(page, 'Pure Digital', 'Evoke D4')
  await expect(dialog).toContainText('Pure Digital · ') // the manufacturer and its integration
  await expect(dialog.getByLabel('Name')).toHaveValue('Evoke D4')
  await expect(dialog.getByLabel('Type')).toHaveValue('receiver')
  await dialog.getByLabel('Name').fill('Den Radio')
  const sent = page.waitForRequest((r) => r.method() === 'POST' && r.url().endsWith('/admin/devices'))
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  expect(commandOrder((await sent).postData()!)).toEqual(EVOKE_ORDER) // joined with its codeset, on text
  await expect(dialog.getByText('Added Den Radio')).toBeVisible()
  await expect(dialog.getByRole('searchbox', { name: 'Search models' })).toBeVisible() // back at its manufacturer's models, for the next one
  await dialog.getByRole('button', { name: 'Done' }).click()

  const { devices } = (await api(page).get('/devices')) as { devices: { id: string; name: string; integration_id: string }[] }
  const radio = devices.find((d) => d.name === 'Den Radio')!
  expect(radio.integration_id).toBe('ir')
  await expect(page.locator(`a[href="#/devices/${radio.id}"]`)).toContainText('Pure Digital Evoke D4')
  const detail = await page.request.get(`${API_BASE}/devices/${radio.id}`)
  expect((await detail.json()).profile.id).toBe('harmony:275862')
  // The hub keeps that order (the mock stores a parsed object, so only hub-sim can say).
  if (SIM_URL) expect(commandOrder(await detail.text())).toEqual(EVOKE_ORDER)
})

test('a model whose integration is off is shown, but can’t be picked', async ({ page }) => {
  await open(page, '/integrations')
  await page.getByRole('switch', { name: 'Enable Bluetooth' }).click()
  await expect(page.getByTestId('state-ble')).toHaveText('Disabled')
  const dialog = await openAddDevice(page)
  await dialog.getByRole('searchbox', { name: 'Search manufacturers' }).fill('goo')
  await dialog.getByRole('button', { name: /^Google/ }).click()
  const tv = dialog.getByRole('button', { name: /^Android TV \/ Google TV/ })
  await expect(tv).toContainText('Bluetooth is off')
  await expect(tv).toBeDisabled()
  await expect(dialog.getByRole('button', { name: /^Fiber TV Box/ })).toBeEnabled()
})

test('the catalog unreachable, a newer format, a profile the hub refuses: each says so', async ({ page }) => {
  // Unreachable: said in place of the list; a file import is still there.
  await page.route(`${CATALOG}**`, (r) => r.abort())
  const dialog = await openAddDevice(page)
  await expect(dialog.getByText('The device catalog can’t be reached.', { exact: false })).toBeVisible()
  await expect(dialog.getByRole('button', { name: /^Import a profile file/ })).toBeVisible()
  await page.unroute(`${CATALOG}**`)
  await dialog.getByRole('button', { name: 'Retry' }).click()
  await dialog.getByRole('searchbox', { name: 'Search manufacturers' }).fill('sam')
  await expect(dialog.getByRole('button', { name: /^Samsung/ })).toBeVisible()

  // A newer profile format: said as soon as it's picked, and it can't be added.
  await page.route(`${CATALOG}profiles/harmony/275/275862.json`, (r) => r.fulfill({ json: { schema_version: 2, model: 'Evoke D4' } }))
  await pickModel(page, 'Pure Digital', 'Evoke D4')
  await expect(dialog.getByText('This device’s profile uses a newer format')).toBeVisible()
  await expect(dialog.getByRole('button', { name: 'Add device', exact: true })).toBeDisabled()
  await page.unroute(`${CATALOG}profiles/harmony/275/275862.json`)

  // The hub refuses it: its reason, in words.
  await page.route(`${CATALOG}codesets/9a/9aa0916c4acf136c.json`, async (r) => {
    const text = await (await r.fetch()).text()
    await r.fulfill({ body: text.replace(/("VolumeUp": \{\s*"pronto": )"[^"]*"/, '$1"0000 006D 0001 0000 0000 0010"') }) // a zero duration
  })
  await dialog.getByRole('button', { name: 'Retry' }).click()
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  await expect(dialog.getByText('This device’s profile has a problem')).toBeVisible()
  await expect(dialog.getByText('The code for “VolumeUp” isn’t valid.')).toBeVisible()
})
