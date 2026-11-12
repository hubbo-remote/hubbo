import { connectHa, expect, open, setHaState, SIM_URL, test } from './hub.ts'

test('Status shows the remote, Bluetooth, Home Assistant and Ethernet as they are', async ({ page }) => {
  await connectHa(page)
  await open(page, '/status')
  const tile = (name: string) => page.getByRole('region', { name, exact: true })
  await expect(tile('Remote')).toContainText('Paired')
  await expect(tile('Ethernet')).toContainText(/\d+\.\d+\.\d+\.\d+/)
  await expect(tile('Bluetooth')).toContainText('Ready') // the radio: hosts come and go on their device pages
  await expect(tile('Home Assistant')).toContainText('Connected')
  await expect(page.getByText('Response time')).toBeVisible()
  await expect(page.getByText('Recent errors')).toBeVisible()

  for (const [state, label] of [['unreachable', 'Unreachable'], ['connecting', 'Connecting'], ['auth_failed', 'Token rejected'], ['connected', 'Connected']] as const) {
    await setHaState(page, state)
    await expect(tile('Home Assistant'), `HA ${state}${SIM_URL ? ' (hub-sim)' : ''}`).toContainText(label)
  }
})
