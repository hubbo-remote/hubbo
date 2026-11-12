// Phase 4 acceptance (hub-sim only): build "Watch Apple TV" by hand in the editor, then use it
// from the remote. hub-sim records every command it sends (GET /sim/log) and takes button presses
// (POST /sim/remote/<button>/<press|release|tap>); the mock does neither.
import type { Page } from '@playwright/test'
import { addStep, bind, openNewActivity, saveActivity } from './editor.ts'
import { advance, api, connectHa, control, expect, open, SIM_URL, test } from './hub.ts'

test.skip(!SIM_URL, 'needs hub-sim: its command log and simulated remote')

const APPLE_TV = 'Family Room Apple TV'
const DENON = 'Family Room Denon'
const LIGHTS = 'Family Room Lights'
const LG = 'Family Room LG TV'
const [APPLE_TV_ID, DENON_ID, LIGHTS_ID, LG_ID] = ['dev_apple_tv', 'dev_denon', 'dev_family_room_lights', 'dev_lg_tv']

/** Commands hub-sim sent since `since` (sim time), as "device command [params]". */
async function sentSince(page: Page, since: number): Promise<string[]> {
  const log = (await (await page.request.get(`${SIM_URL}/sim/log`)).json()) as { commands: { at_ms: number; text: string }[] }
  // "ha_main dev_denon media_player.turn_on -> pending" → "dev_denon media_player.turn_on"; "system - wait …" → "wait …"
  return log.commands.filter((l) => l.at_ms > since).map((l) => l.text.replace(/ -> \w+$/, '').replace(/^\S+ (- )?/, ''))
}
const simNow = async (page: Page): Promise<number> => (await advance(page, 0)).sim_time_ms
const press = (page: Page, button: string, action: 'press' | 'release' | 'tap') => control(page, `remote/${button}/${action}`)

test('build Watch Apple TV by hand, then use it from the remote', async ({ page }) => {
  test.setTimeout(90_000)
  await connectHa(page)
  const id = await openNewActivity(page, 'E2E Watch Apple TV')

  // Apple TV navigation: key down on press, the matching key up on release (added for us).
  for (const [button, key] of [['up', 'ARROW_UP'], ['down', 'ARROW_DOWN'], ['left', 'ARROW_LEFT'], ['right', 'ARROW_RIGHT'], ['select', 'SELECT'], ['back', 'MENU']] as const) {
    await bind(page, button, 'down', APPLE_TV, 'Key down', key)
    await expect(page.locator(`[data-key="${button}"]`)).toHaveAttribute('aria-label', /bound: down, up/)
  }
  // Volume on the Denon (through Home Assistant), the lights on Light 1 / Light 2.
  await bind(page, 'volume_up', 'down', DENON, 'Volume up')
  await bind(page, 'volume_down', 'down', DENON, 'Volume down')
  await bind(page, 'light_1', 'tap', LIGHTS, 'Turn on')
  await bind(page, 'light_2', 'tap', LIGHTS, 'Turn off')

  // Startup: added out of order, then the Denon moved to the top with the keyboard.
  await page.getByRole('tab', { name: /Startup/ }).click()
  await addStep(page, LG, 'Power on')
  await page.getByRole('button', { name: 'Add wait' }).click()
  await addStep(page, LIGHTS, 'Turn on')
  await addStep(page, DENON, 'Power on')
  const startup = page.getByRole('list', { name: 'Startup steps' }).getByRole('listitem')
  await startup.nth(3).focus()
  for (let i = 0; i < 3; i++) await page.keyboard.press('Alt+ArrowUp')
  await expect(startup.nth(0)).toContainText(DENON)
  await expect(startup.nth(0)).toBeFocused()

  // Shutdown: lights off, then the Denon dragged above it.
  await page.getByRole('tab', { name: /Shutdown/ }).click()
  await addStep(page, LIGHTS, 'Turn off')
  await addStep(page, DENON, 'Power off')
  await addStep(page, LG, 'Power off')
  const shutdown = page.getByRole('list', { name: 'Shutdown steps' }).getByRole('listitem')
  await shutdown.nth(1).dragTo(shutdown.nth(0))
  await expect(shutdown.nth(0)).toContainText(DENON)

  await page.getByRole('button', { name: 'Check' }).click()
  await expect(page.getByText('No problems found')).toBeVisible()
  await saveActivity(page)

  // Start it from the home page; the startup runs in order, waiting out its wait.
  let t = await simNow(page)
  await open(page, '/')
  const tile = page.getByRole('main').getByRole('button', { name: /^E2E Watch Apple TV/ })
  await tile.click()
  await expect.poll(api(page).activeActivity).toBe(id)
  await advance(page, 3_000)
  await expect.poll(() => sentSince(page, t)).toEqual([`${DENON_ID} media_player.turn_on`, `${LG_ID} media_player.turn_on`, 'wait duration_ms=1000', `${LIGHTS_ID} light.turn_on`])

  // The remote: an arrow is a key down/up pair; volume and lights go to Home Assistant.
  t = await simNow(page)
  await press(page, 'up', 'press')
  await press(page, 'up', 'release')
  await press(page, 'select', 'tap')
  await press(page, 'volume_up', 'tap')
  await press(page, 'light_2', 'tap')
  await expect
    .poll(() => sentSince(page, t))
    .toEqual([`${APPLE_TV_ID} key_down key=ARROW_UP`, `${APPLE_TV_ID} key_up key=ARROW_UP`, `${APPLE_TV_ID} key_down key=SELECT`, `${APPLE_TV_ID} key_up key=SELECT`, `${DENON_ID} media_player.volume_up`, `${LIGHTS_ID} light.turn_off`])

  // Off: the shutdown runs, in its order.
  t = await simNow(page)
  await page.getByRole('button', { name: /^Off/ }).click()
  await expect.poll(api(page).activeActivity).toBeNull()
  await advance(page, 1_000)
  await expect.poll(() => sentSince(page, t)).toEqual([`${DENON_ID} media_player.turn_off`, `${LIGHTS_ID} light.turn_off`, `${LG_ID} media_player.turn_off`])
})
