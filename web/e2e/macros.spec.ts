import { api, expect, test } from './hub.ts'
import { addStep, openNewActivity, saveActivity } from './editor.ts'

test('a macro built in the editor saves marked, and runs', async ({ page }) => {
  const id = await openNewActivity(page, 'E2E Macro')
  await page.locator('[data-key="play"]').click()
  await page.getByRole('button', { name: 'Macro for Play press' }).click()
  await addStep(page, 'Family Room Denon', 'Volume up', {}, { noun: 'command', opened: true }) // the add dialog opens with the row
  await addStep(page, 'Family Room Denon', 'Volume down', {}, { noun: 'command' })
  await saveActivity(page)
  await expect(page.getByRole('alert')).toHaveCount(0)
  const saved = (await api(page).get(`/activities/${id}`)) as { bindings: Record<string, unknown[]>; macros: string[]; revision: number }
  expect(saved.macros).toEqual(['play:down'])
  expect(saved.bindings['play:down']).toHaveLength(2)

  await page.getByRole('button', { name: 'Run Play press' }).click()
  await expect(page.getByRole('button', { name: 'Run Play press' })).toBeEnabled()
  expect((await api(page).del(`/activities/${id}?revision=${saved.revision}`)).status()).toBe(204)
})
