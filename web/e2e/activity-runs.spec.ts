// Activity runs: a startup whose first step fails keeps going, and Status → Activity Log, the activity's Log tab, its
// Startup steps and the Overview say which step failed and why; a step HA already shows is skipped, and a late reply
// isn't a failure. Mock and hub-sim.
import type { Locator, Page } from '@playwright/test'
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
    macros: [],
  })
  expect(r.status(), await r.text()).toBe(201)
  return ((await r.json()) as { id: string }).id
}

/** The log's newest run (its outer line, not the steps nested in it) and that run's steps. */
function newestRun(page: Page): { run: Locator; steps: Locator } {
  const run = page.getByRole('list', { name: 'Activity log' }).locator(':scope > li').first()
  return { run, steps: run.getByRole('list', { name: 'Steps' }).getByRole('listitem') }
}

test('a failed startup step shows in the log, on the activity and on the Overview', async ({ page }) => {
  const hub = api(page)
  const id = await runsActivity(page, [FREE.command])
  await removeHaEntity(page, FREE.entity) // HA forgets it: its step fails, and the run goes on
  expect((await hub.post(`/activities/${id}/activate`)).status()).toBe(200)
  await advance(page, 1_000)
  await expect.poll(async () => (await hub.get('/status')).last_run).toMatchObject({ activity_id: id, outcome: 'completed_with_errors', failed_steps: 1 })

  // Overview: one line naming it, linking to the activity's own log.
  await open(page, '/status')
  await expect(page.getByText('Runs e2e · 1 step failed')).toBeVisible()
  await page.getByRole('link', { name: 'View log' }).click()
  await expect(page).toHaveURL(new RegExp(`#/activities/${id}/log$`))
  await expect(page.getByRole('tab', { name: 'Log', exact: true, selected: true })).toBeVisible()
  const own = newestRun(page)
  await expect(own.run).toContainText('1 failed')
  await expect(own.run).not.toContainText('Runs e2e') // the page already says which activity
  await expect(own.steps.first()).toContainText('Home Assistant no longer has this device')

  // Status → Activity Log: every activity's runs, each with its name.
  await open(page, '/status/activity')
  const { run, steps } = newestRun(page)
  await expect(run).toContainText('Runs e2e')
  await expect(run).toContainText('start')
  await expect(run).toContainText('1 failed')
  await expect(steps.first()).toContainText('Porch TV')
  await expect(steps.first()).toContainText(/(Turn|Power) on/)
  await expect(steps.first()).toContainText('Home Assistant no longer has this device')
  await expect(steps.nth(1)).toContainText('Wait')

  // The activity's Startup tab (its own URL too) marks the step.
  await open(page, `/activities/${id}/startup`)
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

  await open(page, '/status/activity')
  const { run, steps } = newestRun(page)
  await expect(run).toContainText('Runs e2e')
  await expect(run).toContainText('completed')
  await expect(run).toContainText('1 skipped')
  await expect(steps.first()).toContainText('already on')
  await expect(steps.nth(1)).toContainText('sent, no reply')

  // Neither is a failure: no line on the Overview, no warning on the activity.
  await open(page, '/status')
  await expect(page.getByRole('heading', { name: 'Response time' })).toBeVisible()
  await expect(page.getByRole('link', { name: 'View log' })).toHaveCount(0)
  await open(page, `/activities/${id}`)
  await page.getByRole('tab', { name: /Startup/ }).click()
  await expect(page.getByRole('list', { name: 'Startup steps' })).toBeVisible()
  await expect(page.getByText(/Failed last time/)).toHaveCount(0)
})
