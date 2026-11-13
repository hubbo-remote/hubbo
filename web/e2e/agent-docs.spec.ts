// What an agent finds with no sign-in, through the dev server: /llms.txt beside the API, the index,
// the guide, and the hint in the web admin's own page.
import { expect, test } from './hub.ts'

test('an agent finds /llms.txt, the API index and the guide without signing in', async ({ page, request }) => {
  const llms = await request.get('/llms.txt')
  expect(llms.status()).toBe(200)
  expect(llms.headers()['content-type']).toMatch(/^text\/markdown/)
  expect(await llms.text()).toContain('(/api/v1/agent.md)')

  const index = await request.get('/api/v1/')
  expect(await index.json()).toMatchObject({ docs: '/api/v1/agent.md', openapi: '/api/v1/openapi.json' })
  expect((await request.get('/api/v1/agent.md')).status()).toBe(200)

  // Any path under /api the API doesn't have points at the guide (a missing id doesn't: mock unit tests).
  for (const path of ['/api', '/api/', '/api/v2', '/api/v1/no-such-thing']) {
    const r = await request.get(path)
    expect(r.status(), path).toBe(404)
    expect((await r.json()).error.docs, path).toBe('/api/v1/agent.md')
  }

  await page.goto('/')
  await expect(page.locator('link[rel="alternate"][href="/llms.txt"]')).toHaveCount(1)
})
