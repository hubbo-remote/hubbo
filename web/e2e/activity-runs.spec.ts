// Activity runs: a startup whose first step fails keeps going, and the dashboard and the activity page say which step
// failed and why; a step HA already shows is skipped, and a late reply isn't a failure. Mock and hub-sim.
import type { Page } from '@playwright/test'
import { advance, api, connectHa, control, expect, open, registerHa, removeHaEntity, SIM_URL, test } from './hub.ts'

// A free entity on each target, registered fresh so HA can forget it.
const FREE = SIM_URL ? { entity: 'media_player.denon', command: 'media_player.turn_on' } : { entity: 'switch.tv_backlight', command: 'switch.turn_on' }

test.beforeEach(async ({ page }) => connectHa(page))

/** 'Runs e2e', whose startup is these commands on a freshly registered Porch TV, then a short wait. */
async function runsActivity(page: Page, commands: string[]) {
  const { id: device } = await registerHa(page, FREE.entity, 'Porch TV')
  const r = await api(page).post('/activities', {
    name: 'Runs e2e',
    startup: [
      ...commands.map((command_id) => ({ integration_id: 'ha_main', device_id: device, command_id, params: {} })),
      { integration_id: 'system', device_id: null, command_id: 'wait', params: { duration_ms: 100 } },
    ],
    shutdown: [],
    bindings: {},
  })
  expect(r.status(), await r.text()).toBe(201)
  return ((await r.json()) as { id: string }).id
}

test('a failed startup step shows on the dashboard and on the activity', async ({ page }) => {
  const hub = api(page)
  const id = await runsActivity(page, [FREE.command])
  await removeHaEntity(page, FREE.entity) // HA forgets it: its step fails, and the run goes on
  expect((await hub.post(`/activities/${id}/activate`)).status()).toBe(200)
  await advance(page, 1_000)
  await expect.poll(async () => (await hub.get('/status')).last_run).toMatchObject({ activity_id: id, outcome: 'completed_with_errors', failed_steps: 1 })

  await open(page, '/')
  const card = page.getByRole('region', { name: 'Recent activity' })
  const line = card.getByRole('button', { name: /^Runs e2e started/ }).first() // newest first; hub-sim's reset keeps the history
  await expect(line).toContainText('1 step failed')
  await line.click()
  const steps = card.getByRole('list', { name: 'Steps' })
  await expect(steps.getByRole('listitem').first()).toContainText(/Porch TV · (Turn|Power) on/)
  await expect(steps.getByRole('listitem').first()).toContainText('Home Assistant no longer has this device')
  await expect(steps.getByRole('listitem').nth(1)).toContainText('Wait')

  await open(page, `/activities/${id}`)
  await page.getByRole('tab', { name: /Startup/ }).click()
  const first = page.getByRole('list', { name: 'Startup steps' }).getByRole('listitem').first()
  await expect(first).toContainText('Failed last time: Home Assistant no longer has this device')
})

test('a step HA already shows is skipped, and a late reply is just "no reply"', async ({ page }) => {
  const hub = api(page)
  const off = FREE.command.replace('turn_on', 'turn_off')
  const id = await runsActivity(page, [FREE.command, off])
  // Already on: turn_on is skipped. HA answers late: turn_off is sent, and no reply comes in 1 s.
  await control(page, `ha?state=${FREE.entity}:on&slow=${FREE.entity}:5000`)
  expect((await hub.post(`/activities/${id}/activate`)).status()).toBe(200)
  await advance(page, 3_000)
  await expect.poll(async () => (await hub.get('/status')).last_run).toMatchObject({ activity_id: id, outcome: 'completed', failed_steps: 0 })

  await open(page, '/')
  const card = page.getByRole('region', { name: 'Recent activity' })
  const line = card.getByRole('button', { name: /^Runs e2e started/ }).first() // newest first; hub-sim's reset keeps the history
  await expect(line).toContainText('all steps ok')
  await line.click()
  const steps = card.getByRole('list', { name: 'Steps' }).getByRole('listitem')
  await expect(steps.first()).toContainText('already on')
  await expect(steps.nth(1)).toContainText('sent, no reply')

  await open(page, `/activities/${id}`)
  await page.getByRole('tab', { name: /Startup/ }).click()
  await expect(page.getByRole('list', { name: 'Startup steps' })).toBeVisible()
  await expect(page.getByText(/Failed last time/)).toHaveCount(0)
})
