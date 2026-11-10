import { connectHa, expect, open, setBle, setHaState, SIM_URL, test } from './hub.ts'

test('the dashboard shows the remote, Bluetooth, Home Assistant and Ethernet as they are', async ({ page }) => {
  await connectHa(page)
  await open(page, '/')
  const tile = (name: string) => page.getByRole('region', { name, exact: true })
  await expect(tile('Remote')).toContainText('Paired')
  await expect(tile('Ethernet')).toContainText(/\d+\.\d+\.\d+\.\d+/)
  await expect(tile('Bluetooth')).toContainText('Connected')
  await expect(tile('Home Assistant')).toContainText('Connected')
  await expect(page.getByText('RF dispatch latency')).toBeVisible()
  await expect(page.getByText('Recent errors')).toBeVisible()

  await setBle(page, false)
  await expect(tile('Bluetooth')).toContainText('Disconnected')
  await setBle(page, true)
  await expect(tile('Bluetooth')).toContainText('Connected')

  for (const [state, label] of [['unreachable', 'Unreachable'], ['connecting', 'Connecting'], ['auth_failed', 'Token rejected'], ['connected', 'Connected']] as const) {
    await setHaState(page, state)
    await expect(tile('Home Assistant'), `HA ${state}${SIM_URL ? ' (hub-sim)' : ''}`).toContainText(label)
  }
})
