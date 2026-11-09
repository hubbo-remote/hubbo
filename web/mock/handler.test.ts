// Contract tests: the mock must accept every request fixture in api/fixtures
// and reject every malicious one, so UI built against the mock is built
// against the spec.
import { describe, expect, it } from 'vitest'
import type { Activity, ApiErrorBody, Session, Status, TransitionResult } from '../src/api/types.ts'
import { fixture } from './fixtures.ts'
import { CSRF_HEADER } from '../src/api/constants.ts'
import { createMock, type MockRequest, type MockResponse } from './handler.ts'

const req = (method: string, path: string, body?: unknown, headers: Record<string, string> = { [CSRF_HEADER]: 'csrf-dev' }): MockRequest => {
  const [p, q] = path.split('?')
  return { method, path: p!, query: new URLSearchParams(q ?? ''), headers, body, rawBody: new TextEncoder().encode(JSON.stringify(body ?? '')) }
}

const errorOf = (r: MockResponse) => (r.body as ApiErrorBody).error

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
      // The blocked-raw record fixture is from newer firmware (schema_version 2): well-formed, but this
      // firmware refuses it exactly as errors/restore_unsupported_schema.json documents.
      if (f.file === 'admin/restore.blocked-raw.request.json') return expect([r.status, r.body]).toEqual([422, fixture('errors/restore_unsupported_schema.json')])
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
      expect(errorOf(r).code).toBe('validation_failed')
    })
  }

  it('serves fixture-shaped lists', async () => {
    const m = createMock()
    expect((await m.handle(req('GET', '/devices'))).body).toEqual(fixture('devices/list.json'))
    // Same activities; a freshly started hub (like hub-sim) has none active.
    expect((await m.handle(req('GET', '/activities'))).body).toEqual({ ...fixture<object>('activities/list.json'), active_activity_id: null })
    expect((await m.handle(req('GET', '/remote/buttons'))).body).toEqual(fixture('remote/buttons.json'))
  })
})

describe('mock matches hub-sim', () => {
  it('405 for a known path with the wrong method', async () => {
    const r = await createMock().handle(req('PATCH', '/devices'))
    expect(r.status).toBe(405)
    expect(errorOf(r).code).toBe('method_not_allowed')
  })

  it('422 when a delete omits ?revision', async () => {
    const m = createMock()
    for (const path of ['/activities/act_play_xbox', '/admin/devices/dev_sonos']) {
      const r = await m.handle(req('DELETE', path))
      expect(r.status).toBe(422)
      expect(errorOf(r).details?.[0]?.path).toBe('/revision')
    }
  })

  it('403 for a foreign Origin on unsafe requests', async () => {
    const r = await createMock().handle(req('POST', '/activities/off', {}, { [CSRF_HEADER]: 'csrf-dev', host: 'hub.local', origin: 'http://evil.example' }))
    expect(r.status).toBe(403)
  })

  it('refuses to delete a referenced activity (activity_in_use) and lists the references', async () => {
    const m = createMock()
    const a = (await m.handle(req('GET', '/activities/act_play_xbox'))).body as Activity
    const r = await m.handle(req('DELETE', `/activities/act_play_xbox?revision=${a.revision}`))
    expect(r.status).toBe(409)
    const e = errorOf(r)
    expect(e.code).toBe('activity_in_use')
    expect(e.details?.[0]).toMatchObject({ path: '/global_bindings/bindings/music:tap/0', message: 'Referenced by the global bindings' })
  })

  it('deleting the active activity switches to the one in its place', async () => {
    const m = createMock()
    const created = (await m.handle(req('POST', '/activities', { name: 'Temp', startup: [], shutdown: [], bindings: {} }))).body as Activity
    await m.handle(req('POST', `/activities/${created.id}/activate`))
    expect((await m.handle(req('DELETE', `/activities/${created.id}?revision=${created.revision}`))).status).toBe(204)
    const list = (await m.handle(req('GET', '/activities'))).body as { activities: Activity[]; active_activity_id: string }
    expect(list.active_activity_id).toBe(list.activities[0]!.id)
  })
})

describe('mock behaviour', () => {
  it('requires X-Hubbo-CSRF on unsafe requests', async () => {
    const r = await createMock().handle(req('POST', '/activities/off', {}, {}))
    expect(r.status).toBe(403)
    expect(errorOf(r).code).toBe('csrf_failed')
  })

  it('requires login when configured', async () => {
    const m = createMock({ requireAuth: true })
    expect((await m.handle(req('GET', '/devices'))).status).toBe(401)
    expect((await m.handle(req('POST', '/auth/login', { username: 'admin', password: 'nope' }))).status).toBe(401)
    expect((await m.handle(req('POST', '/auth/login', fixture('auth/login.request.json')))).status).toBe(200)
  })

  it('revisions are config generations; a stale revision gets 409 with current_revision', async () => {
    const m = createMock()
    const a = (await m.handle(req('GET', '/activities/act_play_xbox'))).body as Activity
    const first = await m.handle(req('PUT', '/activities/act_play_xbox', { ...a, name: 'Xbox!' }))
    expect(first.status).toBe(200)
    const saved = first.body as Activity
    expect(saved.revision).toBeGreaterThan(a.revision)
    expect(((await m.handle(req('GET', '/status'))).body as Status).config).toEqual({ state: 'ok', generation: saved.revision })
    const stale = await m.handle(req('PUT', '/activities/act_play_xbox', { ...a, name: 'Stale' }))
    expect(stale.status).toBe(409)
    expect(errorOf(stale).current_revision).toBe(saved.revision)
  })

  it('a blocked config refuses edits until a factory reset', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/reset', { configState: 'blocked' }))
    const st = (await m.handle(req('GET', '/status'))).body as Status
    expect(st.config.state).toBe('blocked')
    expect(st.config.reason).toMatch(/newer/)
    const put = await m.handle(req('PUT', '/global-bindings', { bindings: {}, revision: 1 }))
    expect([put.status, errorOf(put).code]).toEqual([409, 'config_blocked'])
    expect((await m.handle(req('POST', '/activities/validate', { name: 'x', startup: [], shutdown: [], bindings: {} }))).status).toBe(200)
    const reset = await m.handle(req('POST', '/admin/reset'))
    expect((reset.body as { state: string }).state).toBe('ok')
    expect(((await m.handle(req('GET', '/devices'))).body as { devices: unknown[] }).devices).toEqual([])
    expect(((await m.handle(req('GET', '/admin/tokens'))).body as { tokens: unknown[] }).tokens.length).toBeGreaterThan(0) // credentials and tokens stay
  })

  it('restore accepts missing revisions and stamps every object with one new generation', async () => {
    const m = createMock()
    const backup = (await m.handle(req('GET', '/admin/backup'))).body as { activities: { revision?: number }[] }
    for (const a of backup.activities) delete a.revision
    expect((await m.handle(req('POST', '/admin/restore', backup))).status).toBe(200)
    const list = (await m.handle(req('GET', '/activities'))).body as { activities: Activity[] }
    const gen = ((await m.handle(req('GET', '/status'))).body as Status).config.generation
    expect(new Set(list.activities.map((a) => a.revision))).toEqual(new Set([gen]))
  })

  it('backs up the running config as a Backup, marked config', async () => {
    const r = await createMock().handle(req('GET', '/admin/backup'))
    expect(r.status).toBe(200)
    expect(r.headers?.['x-hubbo-backup']).toBe('config')
    expect(r.headers?.['content-disposition']).toMatch(/^attachment; filename="hubbo-backup-\d{4}-\d\d-\d\d\.json"$/)
    expect((r.body as { format: string }).format).toBe('hubbo-backup')
  })

  it('while blocked, backs up the saved record raw; restoring it on this firmware is unsupported_schema', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/reset', { configState: 'blocked' }))
    const r = await m.handle(req('GET', '/admin/backup'))
    expect(r.status).toBe(200)
    expect(r.headers).toMatchObject({ 'x-hubbo-backup': 'blocked-raw', 'content-type': 'application/octet-stream' })
    expect(r.headers?.['content-disposition']).toMatch(/filename="hubbo-backup-BLOCKED-\d{4}-\d\d-\d\d\.json"/)
    expect(r.body).toBeInstanceOf(Uint8Array)
    const record = JSON.parse(new TextDecoder().decode(r.body as Uint8Array))
    expect(record).toEqual(fixture('admin/restore.blocked-raw.request.json'))
    const back = await m.handle(req('POST', '/admin/restore', record))
    expect([back.status, back.body]).toEqual([422, fixture('errors/restore_unsupported_schema.json')])
    expect(((await m.handle(req('GET', '/status'))).body as Status).config.state).toBe('blocked') // nothing changed
  })

  it('restore takes bodies up to 640 KiB (x-hubbo-max-body) and refuses larger ones', async () => {
    const m = createMock()
    const backup = (await m.handle(req('GET', '/admin/backup'))).body
    const sized = (n: number): MockRequest => ({ ...req('POST', '/admin/restore?dry_run=true', backup), rawBody: new Uint8Array(n) })
    expect((await m.handle(sized(640 * 1024))).status).toBe(200)
    expect(errorOf(await m.handle(sized(640 * 1024 + 1))).code).toBe('payload_too_large')
  })

  it('restores a hubbo-config record this firmware reads', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/reset', { configState: 'blocked' }))
    const record = { ...fixture<object>('admin/restore.blocked-raw.request.json'), schema_version: 1 }
    const r = await m.handle(req('POST', '/admin/restore', record))
    expect(r.status, JSON.stringify(r.body)).toBe(200)
    expect(((await m.handle(req('GET', '/status'))).body as Status).config.state).toBe('ok')
    expect((await m.handle(req('GET', '/admin/backup'))).headers?.['x-hubbo-backup']).toBe('config')
  })

  it('a damaged blocked config has nothing to back up (409); unavailable storage answers 503', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/reset', { configState: 'blocked', configDamaged: true }))
    expect(((await m.handle(req('GET', '/status'))).body as Status).config.reason).toMatch(/damaged/)
    const r = await m.handle(req('GET', '/admin/backup'))
    expect([r.status, r.body]).toEqual([409, fixture('errors/backup_blocked_damaged.json')])
    await m.handle(req('POST', '/__mock/reset', { configState: 'storage_unavailable' }))
    const u = await m.handle(req('GET', '/admin/backup'))
    expect([u.status, u.body]).toEqual([503, fixture('errors/backup_storage_unavailable.json')])
  })

  it('restore and reset never start an activity: the active one stays only if its id survives', async () => {
    const m = createMock()
    const active = async () => ((await m.handle(req('GET', '/status'))).body as Status).active_activity_id
    await m.handle(req('POST', '/activities/act_play_xbox/activate'))
    const backup = (await m.handle(req('GET', '/admin/backup'))).body as { activities: { id: string }[] }
    expect((await m.handle(req('POST', '/admin/restore', backup))).status).toBe(200)
    expect(await active()).toBe('act_play_xbox')
    const renamed = { ...backup, activities: backup.activities.map((a) => ({ ...a, id: `${a.id}_2` })) }
    expect((await m.handle(req('POST', '/admin/restore', renamed))).status).toBe(200)
    expect(await active()).toBeNull()
    await m.handle(req('POST', '/activities/act_play_xbox_2/activate'))
    await m.handle(req('POST', '/admin/reset'))
    expect(await active()).toBeNull()
  })

  it('transitions report each phase; a rejected startup still switched', async () => {
    const m = createMock()
    const ok = (await m.handle(req('POST', '/activities/act_play_xbox/activate'))).body as TransitionResult
    expect(ok).toMatchObject({ switched: true, active_activity_id: 'act_play_xbox', startup: { status: 'queued' }, shutdown: { status: 'none' } })
    await m.handle(req('POST', '/__mock/reset', { startupRejected: true }))
    const r = (await m.handle(req('POST', '/activities/act_play_xbox/activate'))).body as TransitionResult
    expect(r).toMatchObject({ switched: true, startup: { status: 'rejected', reason: 'queue_full' } })
    const off = (await m.handle(req('POST', '/activities/off'))).body as TransitionResult
    expect(off).toMatchObject({ switched: true, active_activity_id: null, shutdown: { status: 'queued' } })
  })

  it('direct execute refuses hold commands', async () => {
    const r = await createMock().handle(req('POST', '/devices/dev_apple_tv/commands/key_down/execute', { params: { key: 'ARROW_UP' } }))
    expect(r.status).toBe(422)
    expect(errorOf(r).details?.[0]).toMatchObject({ path: '/command_id', code: 'hold_not_allowed' })
  })

  it('refuses to delete a device that activities use, listing references', async () => {
    const r = await createMock().handle(req('DELETE', '/admin/devices/dev_sonos?revision=3'))
    expect(r.status).toBe(409)
    expect(errorOf(r).details?.length).toBeGreaterThan(0)
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
