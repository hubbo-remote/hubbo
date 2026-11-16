import { configureIntegration, connectHa, control, expect, open, setHaState, settleIntegration, SIM_URL, test } from './hub.ts'

test('Status shows the remote, Bluetooth, Home Assistant and Ethernet as they are', async ({ page }) => {
  await connectHa(page)
  await open(page, '/status')
  const tile = (name: string) => page.getByRole('region', { name, exact: true })
  await expect(tile('Remote')).toContainText('Paired')
  await expect(tile('Remote')).toContainText('Connected') // the dot's label: one-way RF, so paired is healthy, presses or not
  await expect(tile('Ethernet')).toContainText(/\d+\.\d+\.\d+\.\d+/)
  await expect(tile('Bluetooth')).toContainText('Ready') // the radio: hosts come and go on their device pages
  await expect(tile('Home Assistant')).toContainText('Connected')
  await expect(tile('Response time')).toBeVisible()
  if (!SIM_URL) {
    // A board's sub-millisecond presses (the fixture's 0.5 / 0.65 / 0.8 ms) are well inside the 2 / 5 ms targets.
    const latency = tile('Response time')
    await expect(latency.getByText('Good', { exact: true })).toBeVisible()
    await expect(latency.getByText(/^0\.\d\d ms$/).first()).toBeVisible()
  }
  await expect(page.getByText('Recent errors')).toBeVisible()

  for (const [state, label] of [['unreachable', 'Unreachable'], ['connecting', 'Connecting'], ['auth_failed', 'Token rejected'], ['connected', 'Connected']] as const) {
    await setHaState(page, state)
    await expect(tile('Home Assistant'), `HA ${state}${SIM_URL ? ' (hub-sim)' : ''}`).toContainText(label)
  }
})

test('Status shows MQTT beside Home Assistant; off or not set up reads quietly, with a way to set it up', async ({ page }) => {
  const configure = (body: object) => configureIntegration(page, 'mqtt_main', body)
  await open(page, '/status')
  const tile = page.getByRole('region', { name: 'MQTT', exact: true })

  await configure({ enabled: true, settings: { type: 'mqtt', host: 'mqtt.local', port: 1883 } })
  await settleIntegration(page, 'mqtt_main', 'connected')
  await expect(tile).toContainText('Connected')
  await expect(tile).not.toContainText('Connected to the broker') // no line that only repeats the state

  // A problem gets its line.
  await control(page, 'mqtt?auth=ok&reachable=0')
  await settleIntegration(page, 'mqtt_main', 'unreachable')
  await expect(tile).toContainText("Can't reach the broker") // the hub's own words
  await control(page, 'mqtt?reachable=1')

  await configure({ enabled: false })
  await expect(tile).toContainText('Off')
  await expect(tile.getByRole('link', { name: 'Set up a broker' })).toBeVisible()

  await configure({ enabled: true, settings: { type: 'mqtt', host: '' } })
  await expect(tile).toContainText('Not set up')
  await expect(tile.locator('.bg-destructive')).toHaveCount(0) // a grey dot, not an error
  await tile.getByRole('link', { name: 'Set up a broker' }).click()
  await expect(page.getByRole('heading', { name: 'Integrations' })).toBeVisible()
})
