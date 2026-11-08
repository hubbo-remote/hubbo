// Contract tests: the mock must accept every request fixture in api/fixtures
// and reject every malicious one, so UI built against the mock is built
// against the spec.
import { describe, expect, it } from 'vitest'
import type { Activity, Session } from '../src/api/types.ts'
import { fixture } from './fixtures.ts'
import { createMock, type MockRequest } from './handler.ts'

const req = (method: string, path: string, body?: unknown, headers: Record<string, string> = { 'x-hubbo-csrf': 'csrf-dev' }): MockRequest => {
  const [p, q] = path.split('?')
  return { method, path: p!, query: new URLSearchParams(q ?? ''), headers, body, rawBody: new TextEncoder().encode(JSON.stringify(body ?? '')) }
}

const index = fixture<{ fixtures: { file: string; operation: string; kind: string }[] }>('index.json').fixtures

// operationId → request for the mock (paths from api/openapi.yaml).
const ROUTES: Record<string, [string, string]> = {
  login: ['POST', '/auth/login'],
  executeCommand: ['POST', '/devices/dev_denon/commands/media_player.volume_set/execute'],
  createActivity: ['POST', '/activities'],
  replaceActivity: ['PUT', '/activities/act_watch_apple_tv'],
  configureIntegration: ['PUT', '/admin/integrations/ha_main'],
  registerDevice: ['POST', '/admin/devices'],
  updateDevice: ['PUT', '/admin/devices/dev_sonos'],
  createToken: ['POST', '/admin/tokens'],
  changeAdminPassword: ['PUT', '/admin/account/password'],
  restoreBackup: ['POST', '/admin/restore?dry_run=true'],
}

describe('mock vs api/fixtures', () => {
  for (const f of index.filter((x) => x.kind === 'request' && ROUTES[x.operation])) {
    it(`accepts ${f.file}`, async () => {
      const [method, path] = ROUTES[f.operation]!
      const r = await createMock({ random: () => 0.99 }).handle(req(method, path, fixture(f.file)))
      expect(r.status, JSON.stringify(r.body)).toBeLessThan(300)
    })
  }

  it('accepts the setup request on a fresh hub', async () => {
    const m = createMock({ firstRun: true })
    const r = await m.handle(req('POST', '/auth/setup', fixture('auth/setup.request.json'), {}))
    expect(r.status).toBe(201)
    expect((r.body as Session).csrf_token).toBeTruthy()
    expect(r.headers?.['set-cookie']).toMatch(/HttpOnly; SameSite=Strict/)
  })

  for (const f of index.filter((x) => x.kind === 'request-invalid')) {
    it(`rejects ${f.file}`, async () => {
      const r = await createMock().handle(req('POST', '/activities', fixture(f.file)))
      expect(r.status).toBe(422)
      expect((r.body as { error: { code: string } }).error.code).toBe('validation_failed')
    })
  }

  it('serves fixture-shaped lists', async () => {
    const m = createMock()
    expect((await m.handle(req('GET', '/devices'))).body).toEqual(fixture('devices/list.json'))
    expect((await m.handle(req('GET', '/activities'))).body).toEqual(fixture('activities/list.json'))
    expect((await m.handle(req('GET', '/remote/buttons'))).body).toEqual(fixture('remote/buttons.json'))
  })
})

describe('mock behaviour', () => {
  it('requires X-Hubbo-CSRF on unsafe requests', async () => {
    const r = await createMock().handle(req('POST', '/activities/off', {}, {}))
    expect(r.status).toBe(403)
    expect((r.body as { error: { code: string } }).error.code).toBe('csrf_failed')
  })

  it('requires login when configured', async () => {
    const m = createMock({ requireAuth: true })
    expect((await m.handle(req('GET', '/devices'))).status).toBe(401)
    expect((await m.handle(req('POST', '/auth/login', { username: 'admin', password: 'nope' }))).status).toBe(401)
    expect((await m.handle(req('POST', '/auth/login', fixture('auth/login.request.json')))).status).toBe(200)
  })

  it('enforces optimistic concurrency with current_revision', async () => {
    const m = createMock()
    const a = (await m.handle(req('GET', '/activities/act_play_xbox'))).body as Activity
    const first = await m.handle(req('PUT', '/activities/act_play_xbox', { ...a, name: 'Xbox!' }))
    expect(first.status).toBe(200)
    expect((first.body as Activity).revision).toBe(a.revision + 1)
    const stale = await m.handle(req('PUT', '/activities/act_play_xbox', { ...a, name: 'Stale' }))
    expect(stale.status).toBe(409)
    expect((stale.body as { error: { current_revision: number } }).error.current_revision).toBe(a.revision + 1)
  })

  it('refuses to delete a device that activities use, listing references', async () => {
    const r = await createMock().handle(req('DELETE', '/admin/devices/dev_sonos?revision=3'))
    expect(r.status).toBe(409)
    expect((r.body as { error: { code: string; details: unknown[] } }).error.details.length).toBeGreaterThan(0)
  })

  it('reports adapter timeouts as a result, not an HTTP error', async () => {
    const r = await createMock().handle(req('POST', '/devices/dev_sonos/commands/media_player.volume_up/execute', { params: {} }))
    expect(r.status).toBe(200)
    expect((r.body as { status: string }).status).toBe('timeout')
  })

  it('shows a created token once and never lists it again', async () => {
    const m = createMock()
    const c = (await m.handle(req('POST', '/admin/tokens', { name: 'test' }))).body as { token: string }
    expect(c.token).toMatch(/^hbt_[a-z0-9]{8}_[A-Za-z0-9_-]{43}$/)
    expect(JSON.stringify((await m.handle(req('GET', '/admin/tokens'))).body)).not.toContain(c.token)
  })

  it('never returns the Home Assistant token', async () => {
    const m = createMock()
    const body = { ...fixture<object>('integrations/ha_main.configure.request.json') }
    const r = await m.handle(req('PUT', '/admin/integrations/ha_main', body))
    expect(JSON.stringify(r.body)).not.toContain('eyJ')
    expect(JSON.stringify((await m.handle(req('GET', '/admin/backup'))).body)).not.toContain('eyJ')
  })
})
