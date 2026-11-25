// Contract tests: the mock must accept every request fixture in api/fixtures
// and reject every malicious one, so UI built against the mock is built
// against the spec.
import { gunzipSync } from 'node:zlib'
import { describe, expect, it } from 'vitest'
import type { Action, Activity, ActivityRuns, ApiErrorBody, Capabilities, DeviceDetail, DiscoverStatus, ExecuteResult, EntityDetailsStatus, FirmwareStatus, Session, SmartButtons, SmartButtonsInput, Status, TransitionResult } from '../src/api/types.ts'
import { fixture, fixtureBytes } from './fixtures.ts'
import { GUIDE_PATH } from './docs.ts'
import { buildFirmwareImage } from './firmware.ts'
import { createMock, type MockRequest, type MockResponse } from './handler.ts'
import { isPronto } from './profileCheck.ts'
import { activityBody, inOurWords } from '../src/lib/activity.ts'

/** Load an entity's details (POST, then GET: discoverDelayMs is 0 in these tests). */
async function loadDetails(m: ReturnType<typeof createMock>, entityId: string) {
  await m.handle(req('POST', `/admin/integrations/ha_main/entities/${entityId}/details`))
  return (await m.handle(req('GET', `/admin/integrations/ha_main/entities/${entityId}/details`))).body as EntityDetailsStatus
}

const req = (method: string, path: string, body?: unknown, headers: Record<string, string> = {}): MockRequest => {
  const [p, q] = path.split('?')
  return { method, path: p!, query: new URLSearchParams(q ?? ''), headers, body, rawBody: body === undefined ? undefined : new TextEncoder().encode(JSON.stringify(body)) }
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
  setRemoteEndpoint: ['PUT', '/admin/remote/endpoint'],
  replaceSmartButtons: ['PUT', '/smart-buttons'],
  runMacro: ['POST', '/macros/run'],
}

// Requests whose operation needs another target than ROUTES gives.
const FILE_ROUTES: Record<string, [string, string]> = {
  'devices/execute.press.request.json': ['POST', '/devices/dev_apple_tv/commands/press/execute'],
  'devices/execute.ir_press.request.json': ['POST', '/devices/dev_bedroom_tv/commands/press/execute'],
  'integrations/mqtt_main.configure.request.json': ['PUT', '/admin/integrations/mqtt_main'],
}

describe('mock vs api/fixtures', () => {
  for (const f of index.filter((x) => x.kind === 'request' && ROUTES[x.operation])) {
    it(`accepts ${f.file}`, async () => {
      const [method, path] = FILE_ROUTES[f.file] ?? ROUTES[f.operation]!
      const m = createMock({ random: () => 0.99, discoverDelayMs: 0 })
      const body = fixture<{ provider?: { entity_id: string } }>(f.file)
      if (f.operation === 'registerDevice' && body.provider) await loadDetails(m, body.provider.entity_id) // HA devices snapshot the entity's details
      const r = await m.handle(req(method, path, fixture(f.file)))
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
    expect(r.body as Session).toMatchObject({ authenticated: true, username: fixture<{ username: string }>('auth/setup.request.json').username, expires_in_s: 604_800 })
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
  it('a Home Assistant action: its call is checked like the hub, and execute sends it', async () => {
    const m = createMock()
    const lg = { integration_id: 'ha_main', device_id: 'dev_lg_tv', command_id: 'call_service', params: {} }
    const put = async (action: object) => {
      const g = (await m.handle(req('GET', '/global-bindings'))).body as { revision: number }
      return m.handle(req('PUT', '/global-bindings', { revision: g.revision, bindings: { 'info:tap': [action] }, macros: [] }))
    }
    const paths = async (action: object) => errorOf(await put(action)).details?.map((d) => `${d.code} ${d.path}`)
    expect(await paths(lg)).toEqual(['required /bindings/info:tap/0/call'])
    expect(await paths({ ...lg, call: { service: 'webostv.button', data: { entity_id: 'x' } } })).toEqual(['forbidden_param /bindings/info:tap/0/call/data/entity_id'])
    expect(await paths({ ...lg, call: { service: 'webostv.button', data: { a: '{{ x }}' } } })).toEqual(['template_not_supported /bindings/info:tap/0/call/data/a'])
    expect(await paths({ ...lg, command_id: 'media_player.turn_on', call: { service: 'webostv.button', data: {} } })).toEqual(['unknown_field /bindings/info:tap/0/call'])
    expect((await put({ ...lg, call: { service: 'webostv.button', data: { button: 'INFO' } } })).status).toBe(200)
    // Data at its deepest (4 levels) is a 9-level body, which the hub's JSON reader allows.
    expect((await put({ ...lg, call: { service: 'webostv.button', data: { a: { b: { c: {} } } } } })).status).toBe(200)

    const run = (body: object) => m.handle(req('POST', '/devices/dev_lg_tv/commands/call_service/execute', body))
    expect(((await run({ params: {}, call: { service: 'webostv.button', data: { button: 'INFO' } } })).body as ExecuteResult).status).toBe('accepted')
    expect(errorOf(await run({ params: {} })).details?.map((d) => `${d.code} ${d.path}`)).toEqual(['required /call'])
    // Only the device's own platform's services (core's rule), in a config and on execute.
    expect(await paths({ ...lg, call: { service: 'sonos.snapshot', data: {} } })).toEqual(['forbidden_param /bindings/info:tap/0/call/service'])
    expect(errorOf(await run({ params: {}, call: { service: 'sonos.snapshot', data: {} } })).details?.map((d) => `${d.code} ${d.path}`)).toEqual(['forbidden_param /call/service'])
  })

  it('a remote takes `remote` services through the Home Assistant action; other devices refuse them', async () => {
    const m = createMock({ discoverDelayMs: 0 })
    const call = { service: 'remote.send_command', data: { command: 'DPAD_UP' } }
    const run = (id: string) => m.handle(req('POST', `/devices/${id}/commands/call_service/execute`, { params: {}, call }))
    expect(errorOf(await run('dev_lg_tv')).details?.map((d) => `${d.code} ${d.path}`)).toEqual(['forbidden_param /call/service'])
    await loadDetails(m, 'remote.living_room_tv')
    const r = await m.handle(req('POST', '/admin/devices', { integration_id: 'ha_main', provider: { type: 'home_assistant', entity_id: 'remote.living_room_tv' }, name: 'Living Room TV', kind: 'other' }))
    const { id } = r.body as { id: string }
    expect(((await run(id)).body as ExecuteResult).status).toBe('accepted')
    const g = (await m.handle(req('GET', '/global-bindings'))).body as { revision: number }
    const put = await m.handle(req('PUT', '/global-bindings', { revision: g.revision, macros: [], bindings: { 'info:tap': [{ integration_id: 'ha_main', device_id: id, command_id: 'call_service', params: {}, call }] } }))
    expect(put.status).toBe(200)
  })

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

  it('403 csrf_failed for a foreign Origin on unsafe requests; same-origin and no Origin pass', async () => {
    const m = createMock()
    const r = await m.handle(req('POST', '/activities/off', {}, { host: 'hub.local', origin: 'http://evil.example' }))
    expect(r.status).toBe(403)
    expect(errorOf(r).code).toBe('csrf_failed')
    expect((await m.handle(req('POST', '/activities/off', {}, { host: 'hub.local', origin: 'http://hub.local' }))).status).toBe(200)
    expect((await m.handle(req('POST', '/activities/off', {}))).status).toBe(200)
  })

  it('refuses to delete a referenced activity (activity_in_use) and lists the references', async () => {
    const m = createMock()
    const a = (await m.handle(req('GET', '/activities/act_play_xbox'))).body as Activity
    const r = await m.handle(req('DELETE', `/activities/act_play_xbox?revision=${a.revision}`))
    expect(r.status).toBe(409)
    const e = errorOf(r)
    expect(e.code).toBe('activity_in_use')
    expect(e.details?.[0]).toMatchObject({ path: '/global_bindings/bindings/music:tap/0', message: 'Used by global buttons' })
  })

  it('deleting the active activity switches to the one in its place', async () => {
    const m = createMock()
    const created = (await m.handle(req('POST', '/activities', { name: 'Temp', startup: [], shutdown: [], bindings: {}, macros: [] }))).body as Activity
    await m.handle(req('POST', `/activities/${created.id}/activate`))
    expect((await m.handle(req('DELETE', `/activities/${created.id}?revision=${created.revision}`))).status).toBe(204)
    const list = (await m.handle(req('GET', '/activities'))).body as { activities: Activity[]; active_activity_id: string }
    expect(list.active_activity_id).toBe(list.activities[0]!.id)
  })
})

describe('agent docs, like core', () => {
  it('serves the index, the spec as JSON and the guide with no sign-in; gzip only when asked', async () => {
    const m = createMock({ requireAuth: true })
    for (const p of ['', '/']) expect((await m.handle(req('GET', p))).body).toMatchObject({ docs: '/api/v1/agent.md', openapi: '/api/v1/openapi.json' })
    const spec = await m.handle(req('GET', '/openapi.json'))
    expect(JSON.parse(new TextDecoder().decode(spec.body as Uint8Array))).toMatchObject({ openapi: expect.any(String), paths: expect.any(Object) })
    const guide = await m.handle(req('GET', '/agent.md'))
    expect(guide.headers).toMatchObject({ 'content-type': 'text/markdown; charset=utf-8', vary: 'Accept-Encoding' })
    expect(Buffer.from(guide.body as Uint8Array).equals(Buffer.from(fixtureBytes('../../docs/api.md')))).toBe(true)
    expect(guide.headers?.['content-encoding']).toBeUndefined()
    const zipped = await m.handle(req('GET', '/agent.md', undefined, { 'accept-encoding': 'gzip, deflate' }))
    expect(zipped.headers).toMatchObject({ 'content-encoding': 'gzip', vary: 'Accept-Encoding' })
    expect(Buffer.from(gunzipSync(zipped.body as Uint8Array)).equals(Buffer.from(guide.body as Uint8Array))).toBe(true)
    expect((await m.handle(req('GET', '/agent.md', undefined, { 'if-none-match': guide.headers!.etag! }))).status).toBe(304)
  })

  it('points a request with no credentials and an unknown path at the guide; not a wrong password or a missing id', async () => {
    const m = createMock({ requireAuth: true })
    expect(errorOf(await m.handle(req('GET', '/devices'))).docs).toBe(GUIDE_PATH)
    expect(errorOf(await m.handle(req('GET', '/no-such-thing'))).docs).toBe(GUIDE_PATH)
    expect(errorOf(await m.handle(req('POST', '/auth/login', { username: 'x', password: 'wrong' }))).docs).toBeUndefined()
    const open = createMock()
    const missing = await open.handle(req('GET', '/devices/dev_no_such_device'))
    expect(missing.status).toBe(404)
    expect(errorOf(missing).docs).toBeUndefined()
  })
})

describe('hub name, like core', () => {
  const put = (m: ReturnType<typeof createMock>, body: unknown) => m.handle(req('PUT', '/admin/hub', body))
  it('trims spaces, then checks control characters before length', async () => {
    const m = createMock()
    expect((await put(m, { name: '  Den  ' })).body).toMatchObject({ name: 'Den', hostname: 'den' })
    expect((await put(m, { name: ` ${'x'.repeat(32)} ` })).status).toBe(200)
    expect(errorOf(await put(m, { name: '   ' })).details?.[0]?.code).toBe('required')
    expect(errorOf(await put(m, { name: 'x'.repeat(33) })).details?.[0]?.code).toBe('too_long')
    expect(errorOf(await put(m, { name: `\t${'x'.repeat(40)}` })).details?.[0]?.code).toBe('invalid_name') // both wrong: control first
    expect(errorOf(await put(m, { name: 'Den', hostname: 'den' })).details?.[0]?.code).toBe('unknown_field')
  })

  it('derives the hostname lowercasing ASCII only', async () => {
    const m = createMock()
    expect((await put(m, { name: 'Living Room' })).body).toMatchObject({ hostname: 'living-room' })
    expect((await put(m, { name: '\u212A' })).body).toMatchObject({ hostname: 'hubbo' }) // Kelvin sign: not a 'k'
  })

  it('a new hub is called as hub-sim’s; a hostname clash shows until a rename', async () => {
    const m = createMock({ firstRun: true })
    const setup = await m.handle(req('POST', '/auth/setup', fixture('auth/setup.request.json'), {})) // an admin, to read it
    const as = { cookie: setup.headers!['set-cookie']!.split(';')[0]! }
    const hub = async () => (await m.handle(req('GET', '/admin/hub', undefined, as))).body
    expect(await hub()).toMatchObject({ name: 'Hubbo 534D', hostname: 'hubbo-534d', hostname_conflict: false, check_updates: true })
    await m.handle(req('POST', '/__mock/hub?conflict=1'))
    expect(await hub()).toMatchObject({ name: 'Hubbo 534D', hostname: 'hubbo-534d-2', hostname_conflict: true, check_updates: true })
    expect(((await m.handle(req('GET', '/status', undefined, as))).body as Status).network.hostname).toBe('hubbo-534d-2')
    expect((await m.handle(req('PUT', '/admin/hub', { name: 'Den' }, as))).body).toMatchObject({ name: 'Den', hostname: 'den', hostname_conflict: false, check_updates: true })
    expect(errorOf(await m.handle(req('PUT', '/admin/hub', {}, as))).details?.[0]).toMatchObject({ path: '', code: 'required' }) // like core: name or check_updates
    expect((await m.handle(req('PUT', '/admin/hub', { check_updates: false }, as))).body).toMatchObject({ name: 'Den', check_updates: false })
  })

  it('keeps the radio power; a level it doesn’t know is refused and changes nothing', async () => {
    const m = createMock()
    const before = (await put(m, { radio_power: 'low' })).body
    expect(before).toMatchObject({ radio_power: 'low' })
    expect(errorOf(await put(m, { radio_power: 'loud', name: 'Den' })).details?.[0]).toEqual({ path: '/radio_power', code: 'invalid_enum_value', message: 'Radio power must be low, medium, high or max' })
    expect(errorOf(await put(m, { radio_power: 5 })).details?.[0]).toMatchObject({ path: '/radio_power', code: 'wrong_type' })
    expect((await m.handle(req('GET', '/admin/hub'))).body).toEqual(before)
  })
})

const stateOf = async (m: ReturnType<typeof createMock>, id: string) => ((await m.handle(req('GET', '/status'))).body as Status).integrations.find((i) => i.id === id)?.state

describe('MQTT, like core', () => {
  const put = async (m: ReturnType<typeof createMock>, settings: object) => {
    const { revision } = (await m.handle(req('GET', '/admin/integrations/mqtt_main'))).body as { revision: number }
    return m.handle(req('PUT', '/admin/integrations/mqtt_main', { revision, settings: { type: 'mqtt', ...settings } }))
  }
  const state = (m: ReturnType<typeof createMock>) => stateOf(m, 'mqtt_main')
  it('keeps the password write-only: omitted keeps it, null clears it, a new host clears it', async () => {
    const m = createMock()
    expect((await put(m, { username: 'den' })).body).toMatchObject({ settings: { username: 'den', has_password: true } })
    expect(JSON.stringify((await put(m, { password: 'sekrit-1' })).body)).not.toContain('sekrit-1')
    expect((await put(m, { password: null })).body).toMatchObject({ settings: { has_password: false } })
    await put(m, { password: 'sekrit-1' })
    expect((await put(m, { host: 'mqtt.local' })).body).toMatchObject({ settings: { host: 'mqtt.local', has_password: false } })
    expect((await put(m, { host: 'other.local', password: 'sekrit-2' })).body).toMatchObject({ settings: { has_password: true } })
    expect((await put(m, { host: 'fd00::11' })).status).toBe(200) // IPv6 has several colons
    for (const [bad, path] of [[{ port: 0 }, '/settings/port'], [{ host: 'a b' }, '/settings/host'], [{ host: '192.168.1.5:1883' }, '/settings/host'], [{ password: '' }, '/settings/password'], [{ access_token: 'x' }, '/settings/access_token']] as const) {
      expect(errorOf(await put(m, bad)).details?.[0]?.path).toBe(path)
    }
  })
  it('asks for the password again for any restored broker with a host, in core’s order', async () => {
    const m = createMock()
    type B = { integrations: { type: string; enabled: boolean; settings: { host?: string; username?: string } }[] }
    const backup = (await m.handle(req('GET', '/admin/backup'))).body as B
    const requires = async () => ((await m.handle(req('POST', '/admin/restore?dry_run=true', backup))).body as { requires: string[] }).requires
    const mqtt = backup.integrations.find((i) => i.type === 'mqtt')!
    Object.assign(mqtt, { enabled: false, settings: { ...mqtt.settings, username: '' } })
    expect(await requires()).toEqual(['home_assistant_token', 'mqtt_password', 'ble_pairing', 'api_tokens'])
    mqtt.settings.host = ''
    expect(await requires()).not.toContain('mqtt_password')
  })
  it('reports the broker as hub-sim’s fake one does', async () => {
    const m = createMock()
    expect(await state(m)).toBe('connected')
    await m.handle(req('POST', '/__mock/mqtt?auth=reject'))
    expect(await state(m)).toBe('auth_failed')
    await m.handle(req('POST', '/__mock/mqtt?auth=ok&reachable=0'))
    expect(await state(m)).toBe('unreachable')
    expect((await m.handle(req('POST', '/__mock/mqtt?reachable=2'))).status).toBe(422)
    await put(m, { host: '' })
    expect(await state(m)).toBe('not_configured')
  })
})

describe('mock behaviour', () => {
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
    const put = await m.handle(req('PUT', '/global-bindings', { bindings: {}, macros: [], revision: 1 }))
    expect([put.status, errorOf(put).code]).toEqual([409, 'config_blocked'])
    expect((await m.handle(req('POST', '/activities/validate', { name: 'x', startup: [], shutdown: [], bindings: {}, macros: [] }))).status).toBe(200)
    const reset = await m.handle(req('POST', '/admin/reset'))
    expect((reset.body as { state: string }).state).toBe('ok')
    expect(((await m.handle(req('GET', '/devices'))).body as { devices: unknown[] }).devices).toEqual([])
    expect(((await m.handle(req('GET', '/admin/tokens'))).body as { tokens: unknown[] }).tokens.length).toBeGreaterThan(0) // credentials and tokens stay
    // Like core: a reset hub's Off button turns everything off.
    expect(((await m.handle(req('GET', '/global-bindings'))).body as { bindings: unknown }).bindings).toEqual({ 'off:tap': [{ integration_id: 'system', device_id: null, command_id: 'off', params: {} }] })
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

  it('nesting: requests allow 10 levels, a restore (a file) 12, like the hub', async () => {
    const m = createMock()
    const nested = (n: number): unknown => (n === 0 ? 1 : { a: nested(n - 1) })
    const raw = (path: string, n: number): MockRequest => ({ ...req('POST', path), headers: { 'content-type': 'application/json' }, rawBody: new TextEncoder().encode(JSON.stringify(nested(n))) })
    expect(errorOf(await m.handle(raw('/activities', 10))).code).not.toBe('malformed_json')
    expect(errorOf(await m.handle(raw('/activities', 11))).code).toBe('malformed_json')
    expect(errorOf(await m.handle(raw('/admin/restore?dry_run=true', 12))).code).not.toBe('malformed_json')
    expect(errorOf(await m.handle(raw('/admin/restore?dry_run=true', 13))).code).toBe('malformed_json')
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

  it('a damaged blocked config has nothing to back up (409); storage down at load answers 503, later failures still back up', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/reset', { configState: 'blocked', configDamaged: true }))
    expect(((await m.handle(req('GET', '/status'))).body as Status).config.reason).toMatch(/damaged/)
    const r = await m.handle(req('GET', '/admin/backup'))
    expect([r.status, r.body]).toEqual([409, fixture('errors/backup_blocked_damaged.json')])
    // Storage down at load: nothing trustworthy to export.
    await m.handle(req('POST', '/__mock/reset', { configState: 'storage_unavailable' }))
    const u = await m.handle(req('GET', '/admin/backup'))
    expect([u.status, u.body]).toEqual([503, fixture('errors/backup_storage_unavailable.json')])
    // Storage failing after a good load: the running config is the last acknowledged save, so it's exported.
    await m.handle(req('POST', '/__mock/reset', {}))
    await m.handle(req('POST', '/__mock/storage?health=unavailable'))
    const later = await m.handle(req('GET', '/admin/backup'))
    expect([later.status, later.headers?.['x-hubbo-backup']]).toEqual([200, 'config'])
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

  it('remote: shows the endpoint; sets a valid one; refuses what radio set refuses', async () => {
    const m = createMock()
    expect((await m.handle(req('GET', '/admin/remote'))).body).toEqual(fixture('admin/remote.json'))
    const bad = await m.handle(req('PUT', '/admin/remote/endpoint', { channel: 125, address: 'FC5C975555' }))
    expect(bad.status).toBe(422)
    expect(errorOf(bad).details?.map((d) => d.path)).toEqual(['/channel', '/address'])
    const good = await m.handle(req('PUT', '/admin/remote/endpoint', { channel: 12, address: '0xa1b2c3d4' }))
    expect(good.body).toMatchObject({ radio: 'listening', endpoint: { channel: 12, address: '00A1B2C3D4' } })
    expect(((await m.handle(req('GET', '/status'))).body as Status).remote.channel).toBe(12)
  })

  it('a remote scan runs 120 s; while it runs the endpoint is locked; stopping it keeps the pairing', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/reset', { scanNeverFinds: true }))
    expect((await m.handle(req('POST', '/admin/remote/pair'))).body).toMatchObject({ state: 'scanning', remaining_s: 120 })
    expect((await m.handle(req('POST', '/admin/remote/pair'))).status).toBe(409)
    expect((await m.handle(req('GET', '/admin/remote'))).body).toMatchObject({ radio: 'scanning' })
    expect((await m.handle(req('PUT', '/admin/remote/endpoint', { channel: 1, address: 'A1B2C3D4E5' }))).status).toBe(409)
    expect((await m.handle(req('DELETE', '/admin/remote/pair'))).status).toBe(204)
    expect((await m.handle(req('GET', '/admin/remote/pair'))).body).toMatchObject({ state: 'stopped', paired: true, channel: 74 })
  })

  it('discovery is async: start, poll until done, filter; cancel drops it', async () => {
    const m = createMock({ discoverDelayMs: 0 })
    expect((await m.handle(req('GET', '/admin/integrations/ha_main/discover'))).body).toEqual({ state: 'idle' })
    expect((await m.handle(req('POST', '/admin/integrations/ha_main/discover'))).status).toBe(202)
    const all = (await m.handle(req('GET', '/admin/integrations/ha_main/discover'))).body as DiscoverStatus
    const fx = fixture<DiscoverStatus>('integrations/ha_main.discovery.json')
    expect(all).toMatchObject({ state: 'done', total: fx.candidates!.length + 2, next_cursor: null }) // plus a free Denon and a YAML-only one
    expect(all.candidates!.map((c) => [c.provider.entity_id, c.registered_device_id])).toEqual([...fx.candidates!.map((c) => [c.provider.entity_id, c.registered_device_id]), ['media_player.denon', null], ['switch.patio_fountain', null]])
    // Candidates preview the domain commands; services, skipped ones and the registry id come with the details.
    const sonos = all.candidates!.find((c) => c.provider.entity_id === 'media_player.family_room_sonos')!
    expect(sonos).toMatchObject({ platform: 'sonos', dropped_options: 0 })
    expect(sonos.command_ids?.every((id) => id.startsWith('media_player.'))).toBe(true)
    expect(sonos.provider).not.toHaveProperty('unique_id')
    const details = await loadDetails(m, 'media_player.family_room_sonos')
    expect(details).toMatchObject({ state: 'done', platform: 'sonos', provider: { unique_id: expect.any(String) } })
    expect(details.command_ids).toContain('sonos.set_sleep_timer')
    expect(details.skipped_services).toContainEqual({ service: 'sonos.get_queue', reason: 'response_only' })
    expect(await loadDetails(m, 'switch.patio_fountain')).not.toHaveProperty('platform') // YAML-only
    const lights = (await m.handle(req('GET', '/admin/integrations/ha_main/discover?domain=light'))).body as DiscoverStatus
    expect(lights.candidates!.map((c) => c.domain)).toEqual(['light'])
    const page = (await m.handle(req('GET', '/admin/integrations/ha_main/discover?limit=2'))).body as DiscoverStatus
    expect([page.candidates!.length, page.next_cursor]).toEqual([2, '2'])
    expect((await m.handle(req('DELETE', '/admin/integrations/ha_main/discover'))).status).toBe(204)
    expect(((await m.handle(req('GET', '/admin/integrations/ha_main/discover'))).body as DiscoverStatus).state).toBe('idle')
  })

  it('discovery while HA is down is 503 integration_unavailable; a failed run reports why', async () => {
    const m = createMock({ discoverDelayMs: 0 })
    await m.handle(req('POST', '/__mock/ha?state=unreachable'))
    expect(errorOf(await m.handle(req('POST', '/admin/integrations/ha_main/discover'))).code).toBe('integration_unavailable')
    await m.handle(req('POST', '/__mock/reset', { discoverFails: true }))
    await m.handle(req('POST', '/admin/integrations/ha_main/discover'))
    expect((await m.handle(req('GET', '/admin/integrations/ha_main/discover'))).body).toMatchObject({ state: 'failed', error: expect.any(String) })
  })

  it('registering one of the newer domains stores its fixture’s commands, typed params and all', async () => {
    const m = createMock({ discoverDelayMs: 0 })
    for (const entityId of ['scene.movie_night', 'script.goodnight', 'cover.office_blinds', 'button.restart_router', 'input_boolean.guest_mode', 'humidifier.bedroom_humidifier', 'select.dryer_cycle', 'remote.living_room_tv']) {
      await loadDetails(m, entityId)
      const r = await m.handle(req('POST', '/admin/devices', { integration_id: 'ha_main', provider: { type: 'home_assistant', entity_id: entityId }, name: entityId, kind: 'other' }))
      expect(r.status).toBe(201)
      const { id } = r.body as { id: string }
      const caps = (await m.handle(req('GET', `/devices/${id}/capabilities`))).body as Capabilities
      expect(caps.commands).toEqual(fixture<Capabilities>(`integrations/ha_main.entities/${entityId}.capabilities.json`).commands)
    }
  })

  it('registering checks the id shape (422), then needs done details for that entity (409), then commands (422); HA connected (503)', async () => {
    const m = createMock({ discoverDelayMs: 0 })
    const backlight = { integration_id: 'ha_main', provider: { type: 'home_assistant', entity_id: 'switch.tv_backlight' }, name: 'Backlight', kind: 'switch' }
    const broadcast = errorOf(await m.handle(req('POST', '/admin/devices', { ...backlight, provider: { type: 'home_assistant', entity_id: 'light.all' } })))
    expect(broadcast.details?.[0]).toEqual(expect.objectContaining({ path: '/provider/entity_id', code: 'pattern' }))
    expect(errorOf(await m.handle(req('POST', '/admin/devices', backlight))).code).toBe('details_required') // no discovery needed, just details
    expect(await loadDetails(m, 'switch.not_in_ha')).toMatchObject({ state: 'failed', error: "switch.not_in_ha isn't in Home Assistant" })
    await loadDetails(m, 'media_player.kitchen_display')
    expect(errorOf(await m.handle(req('POST', '/admin/devices', backlight))).code).toBe('details_required') // another entity loaded last
    const noCommands = await m.handle(req('POST', '/admin/devices', { ...backlight, provider: { type: 'home_assistant', entity_id: 'media_player.kitchen_display' } }))
    expect(errorOf(noCommands).details?.[0]?.code).toBe('unknown_device')
    await loadDetails(m, 'switch.tv_backlight')
    expect((await m.handle(req('POST', '/admin/devices', backlight))).status).toBe(201)
    await m.handle(req('POST', '/__mock/ha?state=unreachable'))
    expect((await m.handle(req('POST', '/admin/devices', { ...backlight, name: 'Again' }))).status).toBe(503)
  })

  it('a device update takes only DeviceUpdate fields (ha_capabilities is never round-tripped)', async () => {
    const m = createMock()
    const d = (await m.handle(req('GET', '/devices/dev_sonos'))).body as { revision: number }
    const r = await m.handle(req('PUT', '/admin/devices/dev_sonos', { name: 'Sonos', revision: d.revision, ha_capabilities: {} }))
    expect([r.status, errorOf(r).details?.[0]?.code]).toEqual([422, 'unknown_field'])
  })

  it('registering stores the entity’s commands, integration services with typed params included', async () => {
    const m = createMock({ discoverDelayMs: 0 })
    await loadDetails(m, 'switch.tv_backlight')
    const d = (await m.handle(req('POST', '/admin/devices', { integration_id: 'ha_main', provider: { type: 'home_assistant', entity_id: 'switch.tv_backlight' }, name: 'Backlight', kind: 'switch' }))).body as { id: string }
    const caps = (await m.handle(req('GET', `/devices/${d.id}/capabilities`))).body as { commands: { command_id: string }[] }
    expect(caps.commands.map((c) => c.command_id).sort()).toEqual(['switch.turn_off', 'switch.turn_on'])
    // Re-linking (here: to the same entity) re-reads it, picking up its integration's typed services.
    const sonos = (await m.handle(req('GET', '/devices/dev_sonos'))).body as { revision: number; provider: object }
    await loadDetails(m, 'media_player.family_room_sonos')
    expect((await m.handle(req('PUT', '/admin/devices/dev_sonos', { provider: sonos.provider, revision: sonos.revision }))).status).toBe(200)
    const caps2 = (await m.handle(req('GET', '/devices/dev_sonos/capabilities'))).body as { commands: { command_id: string; params: { type: string }[] }[] }
    expect(caps2.commands.find((c) => c.command_id === 'sonos.set_sleep_timer')?.params).toMatchObject([{ type: 'int', minimum: 0, maximum: 86400 }])
  })

  it('HA client states: auth_failed sticks until the settings change', async () => {
    const m = createMock()
    const ha = () => stateOf(m, 'ha_main')
    await m.handle(req('POST', '/__mock/ha?state=auth_failed'))
    expect(await ha()).toBe('auth_failed')
    const cfg = (await m.handle(req('GET', '/admin/integrations/ha_main'))).body as { revision: number }
    await m.handle(req('PUT', '/admin/integrations/ha_main', { revision: cfg.revision, settings: { type: 'home_assistant', access_token: 'new-token' } }))
    expect(await ha()).toBe('connected')
  })

  it('a stale device’s commands fail as entity_not_found until it is re-linked', async () => {
    const m = createMock({ discoverDelayMs: 0 })
    await m.handle(req('POST', '/__mock/ha?rename=media_player.family_room_lg_tv&to=media_player.living_room_lg_tv'))
    expect(((await m.handle(req('GET', '/devices/dev_lg_tv'))).body as { availability: string }).availability).toBe('available') // not yet noticed
    await m.handle(req('POST', '/admin/integrations/ha_main/discover'))
    expect(((await m.handle(req('GET', '/admin/integrations/ha_main/discover'))).body as DiscoverStatus).total).toBe(18) // renamed, not removed
    expect(((await m.handle(req('GET', '/devices/dev_lg_tv'))).body as { availability: string }).availability).toBe('stale')
    const exec = await m.handle(req('POST', '/devices/dev_lg_tv/commands/media_player.turn_on/execute', { params: {} }))
    expect([exec.status, exec.body]).toEqual([200, expect.objectContaining({ status: 'failed', message: 'entity_not_found' })])
    const d = (await m.handle(req('GET', '/devices/dev_lg_tv'))).body as { revision: number; provider: object }
    expect((await loadDetails(m, 'media_player.family_room_lg_tv')).state).toBe('failed') // it's gone
    expect(errorOf(await m.handle(req('PUT', '/admin/devices/dev_lg_tv', { provider: d.provider, revision: d.revision }))).code).toBe('details_required')
    await loadDetails(m, 'switch.tv_backlight')
    // Like the hub: a re-link that breaks an activity's steps is refused, with paths by activity index.
    const breaks = errorOf(await m.handle(req('PUT', '/admin/devices/dev_lg_tv', { provider: { type: 'home_assistant', entity_id: 'switch.tv_backlight' }, revision: d.revision })))
    expect(breaks.code).toBe('validation_failed')
    expect(breaks.details?.[0]).toEqual(expect.objectContaining({ path: '/activities/0/startup/0/command_id', code: 'unknown_command' }))
    // A global button on the LG TV: the hub's path is /global_bindings/<trigger>/<i>/… (no /bindings level).
    const g = (await m.handle(req('GET', '/global-bindings'))).body as { revision: number; bindings: Record<string, unknown[]> }
    const lgStep = { integration_id: 'ha_main', device_id: 'dev_lg_tv', command_id: 'media_player.turn_on', params: {} }
    expect((await m.handle(req('PUT', '/global-bindings', { ...g, bindings: { ...g.bindings, 'tv:tap': [lgStep] } }))).status).toBe(200)
    const breaksGlobal = errorOf(await m.handle(req('PUT', '/admin/devices/dev_lg_tv', { provider: { type: 'home_assistant', entity_id: 'switch.tv_backlight' }, revision: d.revision })))
    expect(breaksGlobal.details?.map((x) => x.path)).toContain('/global_bindings/tv:tap/0/command_id')
    await loadDetails(m, 'media_player.living_room_lg_tv')
    const relinked = await m.handle(req('PUT', '/admin/devices/dev_lg_tv', { provider: { type: 'home_assistant', entity_id: 'media_player.living_room_lg_tv' }, revision: d.revision }))
    expect((relinked.body as { availability: string }).availability).toBe('available')
    await loadDetails(m, 'media_player.xbox')
    const taken = await m.handle(req('PUT', '/admin/devices/dev_lg_tv', { provider: { type: 'home_assistant', entity_id: 'media_player.xbox' }, revision: (relinked.body as { revision: number }).revision }))
    expect(errorOf(taken).details?.[0]?.code).toBe('duplicate_id')
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

const BOARD = fixture<FirmwareStatus>('admin/firmware.json').board
const upload = (image: Uint8Array, headers: Record<string, string> = { 'content-type': 'application/octet-stream', 'content-length': String(image.byteLength) }): MockRequest => ({ method: 'POST', path: '/admin/firmware/update', query: new URLSearchParams(), headers, body: undefined, rawBody: image })
const fw = async (m: ReturnType<typeof createMock>) => (await m.handle(req('GET', '/admin/firmware'))).body as FirmwareStatus
const advance = (m: ReturnType<typeof createMock>, ms: number) => m.handle(req('POST', `/__mock/advance?ms=${ms}`))

describe('firmware update', () => {
  it('writes, reboots into the new version and drops every session', async () => {
    const m = createMock({ timeScale: 0 })
    const r = await m.handle(upload(await buildFirmwareImage({ board: BOARD, version: 'v9.0.0' })))
    expect(r.status).toBe(202)
    expect((r.body as FirmwareStatus).update).toEqual({ state: 'writing', progress: 0, error: null })
    await advance(m, 1000)
    expect((await fw(m)).update).toMatchObject({ state: 'writing', progress: 50 })
    expect((await m.handle(upload(await buildFirmwareImage({ board: BOARD })))).status).toBe(409)
    await advance(m, 1000)
    expect((await fw(m)).update).toMatchObject({ state: 'rebooting', progress: 100 })
    await advance(m, 600)
    expect((await m.handle(req('GET', '/status'))).status).toBe(0) // down mid-reboot
    await advance(m, 400)
    expect((await m.handle(req('GET', '/admin/firmware'))).status).toBe(401)
    expect((await m.handle(req('GET', '/auth/session'))).body).toMatchObject({ authenticated: false, auth_state: 'ok' })
    const login = await m.handle(req('POST', '/auth/login', fixture('auth/login.request.json')))
    expect(login.status).toBe(200)
    expect(((await m.handle({ ...req('GET', '/admin/firmware'), headers: { cookie: login.headers!['set-cookie']!.split(';')[0]! } })).body as FirmwareStatus).version).toBe('v9.0.0')
  })

  it('refuses what the hub refuses, before writing anything', async () => {
    const m = createMock({ timeScale: 0 })
    const good = await buildFirmwareImage({ board: BOARD })
    const code = async (r: MockRequest) => {
      const res = await m.handle(r)
      return [res.status, errorOf(res).code]
    }
    expect(await code(upload(new TextEncoder().encode('not an image at all, just some text that is long enough to pass the size floor')))).toEqual([422, 'invalid_image'])
    expect(await code(upload(await buildFirmwareImage({ board: 'other_board' })))).toEqual([422, 'wrong_board'])
    expect(await code(upload(await buildFirmwareImage({ board: BOARD, badHash: true })))).toEqual([422, 'invalid_image'])
    expect(await code(upload(good, { 'content-type': 'application/json', 'content-length': '1' }))).toEqual([415, 'unsupported_media_type'])
    expect(await code(upload(good, { 'content-type': 'application/octet-stream' }))).toEqual([411, 'length_required'])
    expect(await code(upload(new Uint8Array(6_553_601)))).toEqual([413, 'payload_too_large'])
    await m.handle(req('POST', '/__mock/firmware?ready=0'))
    expect(await code(upload(good))).toEqual([503, 'unavailable'])
    expect((await fw(m)).update.state).toBe('idle')
  })

  it('a failed write leaves the running version', async () => {
    const m = createMock({ timeScale: 0 })
    await m.handle(req('POST', '/__mock/firmware?fail=1'))
    expect((await m.handle(upload(await buildFirmwareImage({ board: BOARD, version: 'v9.0.0' })))).status).toBe(202)
    await advance(m, 2000)
    expect(await fw(m)).toMatchObject({ version: fixture<FirmwareStatus>('admin/firmware.json').version, update: { state: 'failed', error: 'write_failed' } })
  })

  it('offers a newer release; Install downloads, writes and reboots into it', async () => {
    const m = createMock({ timeScale: 0 })
    const release = fixture<FirmwareStatus>('admin/firmware.available.json').available!
    expect((await fw(m)).available).toEqual(release) // checked at boot
    expect(errorOf(await m.handle(req('POST', '/admin/firmware/install', { version: '9.9.9' }))).code).toBe('not_available')
    expect(errorOf(await m.handle(req('POST', '/admin/firmware/install', { version: `v${release.version}` }))).details?.[0]).toMatchObject({ path: '/version', code: 'pattern' })
    expect(errorOf(await m.handle(req('POST', '/admin/firmware/install', {}))).details?.[0]).toMatchObject({ path: '/version', code: 'required' })
    // As the browser sends it: raw JSON, parsed by the mock.
    const started = await m.handle({ ...req('POST', '/admin/firmware/install', { version: release.version }), body: undefined, headers: { 'content-type': 'application/json' } })
    expect(started.status).toBe(202)
    expect(started.body).toMatchObject({ rolled_back_from: null, update: { state: 'downloading', progress: 0, error: null } })
    expect(errorOf(await m.handle(req('POST', '/admin/firmware/install', { version: release.version }))).code).toBe('update_in_progress')
    await advance(m, 2000)
    expect((await fw(m)).update.state).toBe('writing')
    await advance(m, 3000) // written, then rebooted: sign in again
    const login = await m.handle(req('POST', '/auth/login', fixture('auth/login.request.json')))
    const after = await m.handle({ ...req('GET', '/admin/firmware'), headers: { cookie: login.headers!['set-cookie']!.split(';')[0]! } })
    // The checks lived in RAM: forgotten.
    expect(after.body).toMatchObject({ version: release.version, available: null, checked_ago_ms: null, check_error: null, update: { state: 'idle' } })
  })

  it('a check reads the feed; a failed one keeps the last release; a rollback is reported', async () => {
    const m = createMock({ timeScale: 0 })
    await m.handle(req('POST', '/__mock/release?version=2.0.0'))
    const checking = await m.handle(req('POST', '/admin/firmware/check'))
    expect(checking.status).toBe(202)
    expect(checking.body).toMatchObject({ checking: true })
    expect(errorOf(await m.handle(req('POST', '/admin/firmware/check'))).code).toBe('busy')
    expect((await fw(m)).checking).toBe(true)
    await advance(m, 1000)
    expect(await fw(m)).toMatchObject({ checking: false, check_error: null, available: { version: '2.0.0' } })
    await m.handle(req('POST', '/__mock/release?fault=bad_signature&version=3.0.0'))
    await m.handle(req('POST', '/admin/firmware/check'))
    await advance(m, 1000)
    expect(await fw(m)).toMatchObject({ check_error: 'bad_signature', available: { version: '2.0.0' } })
    // No release: GitHub's 404, so unreachable (the last release stays).
    await m.handle(req('POST', '/__mock/release?fault=none&none=1'))
    await m.handle(req('POST', '/admin/firmware/check'))
    await advance(m, 1000)
    expect(await fw(m)).toMatchObject({ check_error: 'unreachable', available: { version: '2.0.0' } })
    // Not for this board or layout, or not installable from here: nothing.
    const offered = async (change: Record<string, string> = {}) => {
      const query = new URLSearchParams({ board: BOARD, layout: '1', min_version: '', version: '2.0.0', ...change })
      await m.handle(req('POST', `/__mock/release?${query}`))
      await m.handle(req('POST', '/admin/firmware/check'))
      await advance(m, 1000)
      return (await fw(m)).available?.version ?? null
    }
    expect(await offered()).toBe('2.0.0')
    expect(await offered({ board: 'other' })).toBeNull()
    expect(await offered({ layout: '2' })).toBeNull()
    expect(await offered({ min_version: '1.0.0' })).toBeNull()
    expect(await offered({ version: '0.0.1' })).toBeNull()
    await m.handle(req('POST', '/__mock/release?running=1.4.0&rolled_back_from=1.5.0'))
    expect(await fw(m)).toMatchObject({ version: '1.4.0', rolled_back_from: '1.5.0' })
  })

  it('an upload clears a past rollback only once it’s written; install doesn’t wait on ready', async () => {
    const m = createMock({ timeScale: 0 })
    await m.handle(req('POST', '/__mock/release?rolled_back_from=1.5.0'))
    expect((await m.handle(upload(await buildFirmwareImage({ board: 'other_board' })))).status).toBe(422)
    expect((await fw(m)).rolled_back_from).toBe('1.5.0')
    expect(((await m.handle(upload(await buildFirmwareImage({ board: BOARD })))).body as FirmwareStatus).rolled_back_from).toBe('1.5.0')
    await advance(m, 2000)
    expect((await fw(m)).rolled_back_from).toBeNull()

    const m2 = createMock({ timeScale: 0 })
    await m2.handle(req('POST', '/__mock/firmware?ready=0'))
    expect((await m2.handle(req('POST', '/admin/firmware/install', { version: (await fw(m2)).available!.version }))).status).toBe(202)
  })

  it('a failed download leaves the running version', async () => {
    const m = createMock({ timeScale: 0 })
    const { version } = (await fw(m)).available!
    await m.handle(req('POST', '/__mock/release?download=fail'))
    await m.handle(req('POST', '/admin/firmware/install', { version }))
    await advance(m, 2000)
    expect(await fw(m)).toMatchObject({ version: fixture<FirmwareStatus>('admin/firmware.json').version, update: { state: 'failed', error: 'download_failed' } })
  })
})

describe('contract details', () => {
  it('refuses a factory reset while an update writes or reboots', async () => {
    const m = createMock({ timeScale: 0 })
    await m.handle(upload(await buildFirmwareImage({ board: BOARD })))
    expect((await m.handle(req('POST', '/admin/factory-reset'))).status).toBe(409)
    await advance(m, 2100)
    expect((await fw(m)).update.state).toBe('rebooting')
    expect((await m.handle(req('POST', '/admin/factory-reset'))).status).toBe(409)
  })

  it('the factory reset 503 is the hub’s text', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/factory-reset?ready=0'))
    expect(errorOf(await m.handle(req('POST', '/admin/factory-reset'))).message).toBe("The hub can't do a factory reset right now")
  })

  it('devices never carry ha_capabilities, even after restoring a backup that has them', async () => {
    const m = createMock()
    expect((await m.handle(req('POST', '/admin/restore?dry_run=false', fixture('admin/restore.request.json')))).status).toBe(200)
    for (const d of ((await m.handle(req('GET', '/devices'))).body as { devices: object[] }).devices) expect(d).not.toHaveProperty('ha_capabilities')
    expect((await m.handle(req('GET', '/devices/dev_denon'))).body).not.toHaveProperty('ha_capabilities')
  })
})

describe('Bluetooth devices', () => {
  const bt = async (m: ReturnType<typeof createMock>, id: string) => ((await m.handle(req('GET', `/devices/${id}`))).body as { bluetooth?: { state: string } }).bluetooth?.state
  const add = (m: ReturnType<typeof createMock>, name: string) => m.handle(req('POST', '/admin/devices', { profile_id: 'hubbo:fire-tv', name }))

  it('a new device is not paired; pairing bonds the window’s device; forget and delete drop only its host', async () => {
    const m = createMock()
    expect(await bt(m, 'dev_apple_tv')).toBe('connected') // seeded paired
    const shield = (await add(m, 'Shield')).body as { id: string; bluetooth: { state: string } }
    expect(shield.bluetooth.state).toBe('not_paired')
    expect((await m.handle(req('POST', `/admin/devices/${shield.id}/pair`))).body).toMatchObject({ state: 'pairing', window_s: 120 })
    expect(errorOf(await m.handle(req('POST', '/admin/devices/dev_apple_tv/pair'))).code).toBe('busy') // one window at a time
    await m.handle(req('POST', `/__mock/ble/bond?device=${shield.id}`))
    expect(await bt(m, shield.id)).toBe('connected')
    expect(await bt(m, 'dev_apple_tv')).toBe('connected')
    await m.handle(req('POST', `/__mock/ble/link?device=${shield.id}&up=0`))
    expect(await bt(m, shield.id)).toBe('not_connected')
    expect((await m.handle(req('POST', `/devices/${shield.id}/commands/key_tap/execute`, { params: { key: 'PLAY_PAUSE' } }))).body).toMatchObject({ status: 'failed', message: 'adapter_unavailable' })
    expect((await m.handle(req('DELETE', `/admin/devices/${shield.id}/pairing`))).status).toBe(204)
    expect(await bt(m, shield.id)).toBe('not_paired')
    expect(await bt(m, 'dev_apple_tv')).toBe('connected')
    expect((await m.handle(req('GET', '/devices/dev_denon'))).body).not.toHaveProperty('bluetooth')
  })

  it('pairing the device whose window is open is its status again; reset and delete leave no stale bonds', async () => {
    const m = createMock()
    await m.handle(req('POST', '/admin/devices/dev_apple_tv/pair'))
    const again = await m.handle(req('POST', '/admin/devices/dev_apple_tv/pair'))
    expect([again.status, (again.body as { state: string }).state]).toEqual([202, 'pairing'])
    await m.handle(req('DELETE', '/admin/devices/dev_apple_tv/pair'))
    expect((await m.handle(req('POST', '/admin/reset'))).status).toBe(200)
    // A new device reusing the old id starts unpaired.
    const r = await add(m, 'Family Room Apple TV')
    expect((r.body as { id: string; bluetooth: { state: string } }).bluetooth.state).toBe('not_paired')
  })

  it('a re-pair that’s cancelled or runs out keeps the old host', async () => {
    const m = createMock()
    await m.handle(req('POST', '/admin/devices/dev_apple_tv/pair'))
    expect(await bt(m, 'dev_apple_tv')).toBe('pairing')
    expect((await m.handle(req('DELETE', '/admin/devices/dev_apple_tv/pair'))).status).toBe(204)
    expect(await bt(m, 'dev_apple_tv')).toBe('connected')
    await m.handle(req('POST', '/admin/devices/dev_apple_tv/pair'))
    await advance(m, 125_000)
    expect(await bt(m, 'dev_apple_tv')).toBe('connected')
  })

  it('refusals are the hub’s (api/fixtures)', async () => {
    const m = createMock()
    const both = (r: MockResponse) => [r.status, r.body]
    for (const n of ['B', 'C', 'D']) expect((await add(m, n)).status).toBe(201) // the seed has one
    expect(both(await add(m, 'E'))).toEqual([422, fixture('errors/too_many_bluetooth_devices.json')])
    expect(both(await m.handle(req('POST', '/admin/devices/dev_denon/pair')))).toEqual([404, fixture('errors/not_bluetooth.json')])
    expect((await m.handle(req('POST', '/admin/devices/dev_apple_tv/pair'))).body).toEqual(fixture('admin/device_pair.json'))
    expect(both(await m.handle(req('POST', '/admin/devices/dev_b/pair')))).toEqual([409, fixture('errors/pairing_busy.json')])
    await m.handle(req('POST', '/__mock/ble/radio?ready=0'))
    expect(both(await m.handle(req('POST', '/admin/devices/dev_b/pair')))).toEqual([503, fixture('errors/bluetooth_unavailable.json')])
    expect(await bt(m, 'dev_apple_tv')).toBe('not_connected')
  })

  it('runtime device fields are refused in request bodies', async () => {
    const m = createMock()
    const got = (await m.handle(req('GET', '/devices/dev_apple_tv'))).body as Record<string, unknown>
    const put = await m.handle(req('PUT', '/admin/devices/dev_apple_tv', { name: 'TV', revision: got.revision, availability: 'available' }))
    expect(errorOf(put).details![0]).toMatchObject({ path: '/availability', code: 'unknown_field' })
    const backup = (await m.handle(req('GET', '/admin/backup'))).body as { devices: Record<string, unknown>[] }
    backup.devices[0]!.bluetooth = { state: 'connected' }
    expect(errorOf(await m.handle(req('POST', '/admin/restore?dry_run=true', backup))).details![0]).toMatchObject({ path: '/devices/0/bluetooth', code: 'unknown_field' })
  })

  it('the pair routes are for Bluetooth devices only; a restore keeps bonds only for devices it keeps', async () => {
    const m = createMock()
    expect((await m.handle(req('POST', '/admin/integrations/ble/pair'))).status).toBe(404)
    const backup = (await m.handle(req('GET', '/admin/backup'))).body as { devices: { id: string }[]; activities: unknown[] }
    expect(JSON.stringify(backup)).not.toContain('bluetooth')
    expect((await m.handle(req('POST', '/admin/restore?dry_run=false', backup))).status).toBe(200)
    expect(await bt(m, 'dev_apple_tv')).toBe('connected')
    // A restore without the device drops its bond, so bringing it back starts unpaired.
    const without = { ...backup, devices: backup.devices.filter((d) => d.id !== 'dev_apple_tv'), activities: [] }
    expect((await m.handle(req('POST', '/admin/restore?dry_run=false', without))).status).toBe(200)
    expect((await m.handle(req('POST', '/admin/restore?dry_run=false', backup))).status).toBe(200)
    expect(await bt(m, 'dev_apple_tv')).toBe('not_paired')
  })
})

describe('device profiles', () => {
  const get = async (m: ReturnType<typeof createMock>, path: string) => (await m.handle(req('GET', path))).body
  const caps = async (m: ReturnType<typeof createMock>, id: string) => ((await get(m, `/devices/${id}/capabilities`)) as Capabilities).commands

  it('lists the built-in profiles and kinds, and serves a profile, as the fixtures', async () => {
    const m = createMock()
    expect(await get(m, '/profiles')).toEqual(fixture('profiles/list.json'))
    expect(await get(m, '/kinds')).toEqual(fixture('profiles/kinds.json'))
    expect(await get(m, '/profiles/hubbo%3Aapple-tv')).toEqual(fixture('profiles/apple-tv.json'))
    expect((await m.handle(req('GET', '/profiles/hubbo:roku'))).status).toBe(404)
  })

  it('a backup stores devices without platform, like the fixture', async () => {
    const devices = ((await get(createMock(), '/admin/backup')) as { devices: object[] }).devices
    for (const d of devices) expect(d).not.toHaveProperty('platform')
    expect(fixture<{ devices: object[] }>('admin/backup.json').devices[0]).not.toHaveProperty('platform')
  })

  it('adds a device from a profile, like the fixture; the list is light, the device carries its profile', async () => {
    const m = createMock()
    const r = await m.handle(req('POST', '/admin/devices', fixture('admin/register_profile.request.json')))
    const { revision: _r, ...want } = fixture<Record<string, unknown>>('admin/register_profile.json')
    expect(r.status).toBe(201)
    expect(r.body).toMatchObject(want)
    const listed = ((await get(m, '/devices')) as { devices: Record<string, unknown>[] }).devices.find((d) => d.id === want.id)!
    expect(listed).not.toHaveProperty('commands')
    expect(listed).toMatchObject({ platform: 'ble_hid', profile: want.profile })
    // Its remote commands are the profile's; keys and custom codes are every Bluetooth device's.
    const press = (await caps(m, want.id as string)).find((c) => c.command_id === 'press')!
    expect(press.params.find((p) => p.name === 'command')?.options).toEqual(Object.keys(want.commands as object))
    expect(new Set((await caps(m, want.id as string)).map((c) => c.group))).toEqual(new Set(['remote', 'keys', 'custom', undefined]))
  })

  it('a connected Bluetooth device takes a timed press, answering at key down; one that isn’t fails like the fixture', async () => {
    const m = createMock()
    const press = () => m.handle(req('POST', '/devices/dev_apple_tv/commands/press/execute', { params: { command: 'Home', hold_ms: 2000 } }))
    const ok = (await press()).body as ExecuteResult
    expect(ok.status).toBe('accepted')
    expect(ok.elapsed_ms).toBeLessThan(2000)
    await m.handle(req('POST', '/__mock/ble/link?device=dev_apple_tv&up=0'))
    const { elapsed_ms: _e, ...failed } = fixture<ExecuteResult>('devices/execute.failed.json')
    expect((await press()).body).toMatchObject(failed)
  })

  it('refusals: an unknown profile or kind; wait_connected outside a sequence', async () => {
    const m = createMock()
    const r = await m.handle(req('POST', '/admin/devices', { profile_id: 'hubbo:roku' }))
    expect([r.status, r.body]).toEqual([422, fixture('errors/unknown_profile.json')])
    expect(errorOf(await m.handle(req('POST', '/admin/devices', { profile_id: 'hubbo:fire-tv', kind: 'toaster' }))).details![0]).toMatchObject({ path: '/kind', code: 'unknown_kind' })
    const x = await m.handle(req('POST', '/devices/dev_apple_tv/commands/wait_connected/execute', { params: {} }))
    expect([x.status, x.body]).toEqual([422, fixture('errors/sequence_only.json')])
    const wait = { integration_id: 'ble', device_id: 'dev_apple_tv', command_id: 'wait_connected', params: {} }
    const bad = await m.handle(req('POST', '/activities', { name: 'W', startup: [wait], shutdown: [], bindings: { 'up:tap': [wait] }, macros: [] }))
    expect(errorOf(bad).details!.map((d) => [d.path, d.code])).toEqual([['/bindings/up:tap/0/command_id', 'sequence_only']])
  })
})

describe('infrared devices', () => {
  const file = () => structuredClone(fixture<{ profile: { commands: Record<string, { pronto: string }> } }>('admin/register_ir.request.json'))

  it('imports a profile file: an IR device with the remote group only, and no pairing', async () => {
    const m = createMock()
    const r = await m.handle(req('POST', '/admin/devices', file()))
    expect(r.status).toBe(201)
    const d = r.body as DeviceDetail
    expect({ ...d, revision: 0 }).toEqual({ ...fixture<DeviceDetail>('admin/register_ir.json'), revision: 0 }) // meta dropped; the hash is of the file as sent
    expect(d).not.toHaveProperty('bluetooth')
    const caps = (await m.handle(req('GET', `/devices/${d.id}/capabilities`))).body as Capabilities
    expect(caps.commands.map((c) => c.command_id)).toEqual(['press', 'press_down', 'press_up'])
    expect((await m.handle(req('GET', `/admin/devices/${d.id}/pair`))).status).toBe(404)
    // The emitter down: its commands fail, like the hub's.
    await m.handle(req('POST', '/__mock/ir?ready=0'))
    const x = (await m.handle(req('POST', `/devices/${d.id}/commands/press/execute`, { params: { command: 'PowerOn' } }))).body as ExecuteResult
    expect(x).toMatchObject({ status: 'failed', message: 'adapter_unavailable' })
    expect(((await m.handle(req('GET', '/integrations'))).body as { integrations: { id: string; state: string; detail?: string }[] }).integrations.find((i) => i.id === 'ir')).toMatchObject({ state: 'not_ready', detail: "The IR emitter isn't working" })
  })

  it('refuses a bad code or a document that isn’t a profile, at its place in the file', async () => {
    const m = createMock()
    const bad = file()
    bad.profile.commands.VolumeUp = { pronto: '0000 006D 0001 0000 0000 0010' }
    const r = await m.handle(req('POST', '/admin/devices', bad))
    expect([r.status, r.body]).toEqual([422, fixture('errors/invalid_pronto.json')])
    expect(errorOf(await m.handle(req('POST', '/admin/devices', { profile: { schema_version: 1 } }))).details).toContainEqual(expect.objectContaining({ path: '/profile/id', code: 'required' }))
    const ir = { name: 'Den TV', integration_id: 'ir', kind: 'tv', provider: { type: 'ir' } }
    expect(errorOf(await m.handle(req('POST', '/admin/devices', ir))).details![0]).toMatchObject({ path: '/profile', code: 'required' })
    expect(errorOf(await m.handle(req('POST', '/admin/devices', { profile: [1] }))).details![0]).toMatchObject({ path: '/profile', code: 'wrong_type' })
    const platform = file()
    Object.assign(platform.profile, { platform: 'rf' })
    expect(errorOf(await m.handle(req('POST', '/admin/devices', platform))).details![0]).toMatchObject({ path: '/profile/platform', code: 'wrong_type' })
    const huge = file()
    Object.assign(huge.profile, { manufacturer: 'x'.repeat(256 * 1024) })
    expect(errorOf(await m.handle(req('POST', '/admin/devices', huge))).details).toContainEqual(expect.objectContaining({ path: '/profile', code: 'too_long' }))
  })

  it('takes a Pronto code only as the hub parses it', () => {
    const ok = '0000 006D 0001 0000 0010 0020'
    expect(isPronto(ok)).toBe(true)
    expect(isPronto('0000 006D 0000 0001 0010 0020')).toBe(true) // a repeat section only
    for (const bad of [
      ok.toLowerCase().replace('006d', '006d'), // lowercase hex
      ok.replace('006D', '006d'),
      ok.replace(' ', '  '), // two spaces
      ` ${ok}`, // leading space
      `${ok} `, // trailing space
      ok.replace('0000 006D', '0100 006D'), // not a raw code
      ok.replace('006D', '0000'), // no frequency
      '0000 006D 0000 0000 0010 0020', // no pairs
      '0000 006D 0002 0000 0010 0020', // length doesn't match the header
      ok.replace('0010', '0000'), // a zero duration
      `0000 006D 0201 0000 ${Array(513 * 2).fill('0010').join(' ')}`, // 513 pairs
    ])
      expect(isPronto(bad), bad.slice(0, 40)).toBe(false)
  })
})

describe('smart buttons', () => {
  const get = async (m: ReturnType<typeof createMock>) => (await m.handle(req('GET', '/smart-buttons'))).body as SmartButtons

  it('answers like the fixtures, deriving each slot from its device', async () => {
    const m = createMock({ random: () => 0.99 })
    const sb = await get(m)
    expect(sb).toEqual(fixture('smart_buttons/get.json'))
    const body = fixture<SmartButtonsInput>('smart_buttons/replace.request.json')
    const r = await m.handle(req('PUT', '/smart-buttons', { ...body, revision: sb.revision }))
    expect(r.status).toBe(200)
    expect({ ...(r.body as SmartButtons), revision: 0 }).toEqual({ ...fixture<SmartButtons>('smart_buttons/replace.json'), revision: 0 })
  })

  it('works out a device’s actions without saving, like the fixtures', async () => {
    const m = createMock({ random: () => 0.99 })
    for (const id of ['dev_family_room_lights', 'dev_lg_tv']) {
      const r = await m.handle(req('GET', `/smart-buttons/actions?device_id=${id}`))
      expect([r.status, r.body], id).toEqual([200, fixture(`smart_buttons/actions.${id}.json`)])
    }
    expect((await m.handle(req('GET', '/smart-buttons/actions?device_id=dev_nope'))).status).toBe(404)
    expect(errorOf(await m.handle(req('GET', '/smart-buttons/actions'))).details?.[0]).toMatchObject({ path: '/device_id', code: 'required' })
    expect(errorOf(await m.handle(req('GET', '/smart-buttons/actions?device_id=light.x'))).details?.[0]).toMatchObject({ path: '/device_id', code: 'invalid_id' })
  })

  it('checks revisions, devices and the shape', async () => {
    const m = createMock({ random: () => 0.99 })
    const { slots, revision } = await get(m)
    const stale = await m.handle(req('PUT', '/smart-buttons', { slots, revision: revision - 1 }))
    expect(stale.status).toBe(409)
    expect(errorOf(stale)).toMatchObject({ code: 'revision_conflict', current_revision: revision })
    const gone = await m.handle(req('PUT', '/smart-buttons', { revision, slots: { ...slots, light_2: 'dev_nope' } }))
    expect(errorOf(gone)).toEqual(fixture<ApiErrorBody>('errors/smart_buttons_unknown_device.json').error)
    const extra = await m.handle(req('PUT', '/smart-buttons', { revision, slots: { ...slots, light_3: null } }))
    expect(errorOf(extra).details?.[0]).toMatchObject({ path: '/slots/light_3', code: 'unknown_field' })
    const actions = await m.handle(req('PUT', '/smart-buttons', { revision, slots, actions: {} }))
    expect(errorOf(actions).details?.[0]).toMatchObject({ path: '/actions', code: 'unknown_field' })
  })

  it("selects the pressed slot unless the activity owns that button", async () => {
    const m = createMock({ random: () => 0.99 })
    const slot = async () => ((await m.handle(req('GET', '/status'))).body as Status).selected_slot
    expect(await slot()).toBe('light_1')
    await m.handle(req('POST', '/__mock/remote/switch_2/press'))
    expect(await slot()).toBe('switch_2')
    const acts = (await m.handle(req('GET', '/activities'))).body as { activities: Activity[] }
    const a = acts.activities[0]!
    const full = (await m.handle(req('GET', `/activities/${a.id}`))).body as Activity
    const own = { integration_id: 'system', device_id: null, command_id: 'off', params: {} }
    const { id: _i, revision, schema_version: _s, ...input } = full
    expect((await m.handle(req('PUT', `/activities/${a.id}`, { ...input, revision, bindings: { ...full.bindings, 'light_2:hold': [own] } }))).status).toBe(200)
    await m.handle(req('POST', `/activities/${a.id}/activate`))
    await m.handle(req('POST', '/__mock/remote/light_2/tap'))
    expect(await slot()).toBe('switch_2')
  })

  it('counts as a reference, and goes in backups and resets', async () => {
    const m = createMock({ random: () => 0.99 })
    const lights = (await m.handle(req('GET', '/devices/dev_family_room_lights'))).body as { revision: number }
    const del = await m.handle(req('DELETE', `/admin/devices/dev_family_room_lights?revision=${lights.revision}`))
    expect(del.status).toBe(409)
    expect(errorOf(del).details).toContainEqual(expect.objectContaining({ path: '/smart_buttons/slots/light_1', code: 'referenced' }))
    const backup = (await m.handle(req('GET', '/admin/backup'))).body as { smart_buttons: SmartButtonsInput }
    expect(backup.smart_buttons).toEqual({ slots: fixture<SmartButtons>('smart_buttons/get.json').slots, revision: expect.any(Number) })
    await m.handle(req('POST', '/admin/reset'))
    const reset = await get(m)
    expect(reset.slots.light_1).toBe(null)
    expect(reset.actions.light_1).toEqual({ button: { tap: [], hold: [], release: [] }, plus: { tap: [], hold: [], release: [] }, minus: { tap: [], hold: [], release: [] } })
    const { smart_buttons: _sb, ...old } = backup
    expect((await m.handle(req('POST', '/admin/restore', old))).status).toBe(200)
    expect((await get(m)).slots.light_1).toBe(null)
    expect((await m.handle(req('POST', '/admin/restore', backup))).status).toBe(200)
    expect((await get(m)).slots.light_1).toBe('dev_family_room_lights')
  })
})

describe('activity runs', () => {
  const runsOf = async (m: ReturnType<typeof createMock>) => ((await m.handle(req('GET', '/activity-runs'))).body as ActivityRuns).runs
  const lastRun = async (m: ReturnType<typeof createMock>) => ((await m.handle(req('GET', '/status'))).body as Status).last_run

  it('starts with the fixture’s runs, and /status flags the newest', async () => {
    const m = createMock()
    const ago = (r: ActivityRuns) => r.runs.map((x) => ({ ...x, started_ago_ms: 0 }))
    expect(ago({ runs: await runsOf(m) })).toEqual(ago(fixture<ActivityRuns>('activity_runs/list.json')))
    expect(await lastRun(m)).toEqual(fixture<Status>('status.json').last_run)
  })

  it('keeps each startup and shutdown, a stale device’s step failed, and the rest going', async () => {
    const m = createMock({ discoverDelayMs: 0 })
    await m.handle(req('POST', '/__mock/ha?remove=media_player.family_room_lg_tv'))
    await m.handle(req('POST', '/admin/integrations/ha_main/discover'))
    await m.handle(req('GET', '/admin/integrations/ha_main/discover'))
    await m.handle(req('POST', '/activities/act_watch_apple_tv/activate'))
    const [run] = await runsOf(m)
    expect(run).toMatchObject({ activity_id: 'act_watch_apple_tv', kind: 'startup', outcome: 'completed_with_errors' })
    expect(run!.steps.map((s) => s.result)).toEqual(['failed', 'ok', 'ok', 'ok', 'ok', 'failed', 'ok'])
    expect(run!.steps[0]).toMatchObject({ device_id: 'dev_lg_tv', error: 'entity_not_found' })
    expect(await lastRun(m)).toEqual({ id: 14, activity_id: 'act_watch_apple_tv', kind: 'startup', button: null, trigger: null, outcome: 'completed_with_errors', failed_steps: 2 }) // ids count on from the fixture's newest

    await m.handle(req('POST', '/activities/off'))
    expect((await runsOf(m))[0]).toMatchObject({ activity_id: 'act_watch_apple_tv', kind: 'shutdown' })
    expect(await runsOf(m)).toHaveLength(6) // the fixture's 4 and these 2; at most 8 are kept
  })

  it('runs and logs a marked Press as a macro, with the button and its activity', async () => {
    const m = createMock()
    await m.handle(req('POST', '/activities/act_watch_apple_tv/activate'))
    await m.handle(req('POST', '/__mock/remote/guide/press'))
    const [run] = await runsOf(m)
    expect(run).toMatchObject({ activity_id: 'act_watch_apple_tv', kind: 'macro', button: 'guide', trigger: 'press', outcome: 'completed' })
    expect(run!.steps.map((s) => s.command_id)).toEqual(['press', 'wait', 'press'])
    await m.handle(req('POST', '/__mock/remote/up/press')) // one command: passed through, no run
    expect((await runsOf(m))[0]!.id).toBe(run!.id)
  })

  it('saves a macro built in the editor: its mark goes with it (a dropped mark is macro_required)', async () => {
    const m = createMock()
    const a = (await m.handle(req('GET', '/activities/act_watch_apple_tv'))).body as Activity
    const up: Action = { integration_id: 'ble', device_id: 'dev_apple_tv', command_id: 'press', params: { command: 'DirectionUp' } }
    // As the editor writes it (see lib/commands' tests): a Press macro beside a Long press, on tap and marked.
    const made = { bindings: { ...a.bindings, 'switch_2:tap': [up, up, up, up], 'switch_2:hold': [up] }, macros: [...a.macros, 'switch_2:tap'] }
    const r = await m.handle(req('PUT', `/activities/${a.id}`, { ...activityBody({ ...a, ...made }), revision: a.revision }))
    expect(r.status, JSON.stringify(r.body)).toBe(200)
    expect((r.body as Activity).macros).toEqual(made.macros)
    const unmarked = await m.handle(req('PUT', `/activities/${a.id}`, { ...activityBody({ ...a, ...made, macros: a.macros }), revision: (r.body as Activity).revision }))
    expect(errorOf(unmarked).details!.map((d) => [d.path, d.code])).toEqual([['/bindings/switch_2:tap', 'macro_required']])
    expect(inOurWords(errorOf(unmarked).details![0]!, [], []).message).toBe('Switch 2: make this a macro to run more than one command')
  })

  it('skips a step HA already shows, records a late reply as no_reply, and neither is a failure (state checks)', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/ha?state=media_player.family_room_lg_tv:on&attr=media_player.family_room_sonos:source=TV'))
    await m.handle(req('POST', '/__mock/ha?slow=media_player.family_room_lg_tv:5000'))
    await m.handle(req('POST', '/activities/act_watch_apple_tv/activate'))
    const [run] = await runsOf(m)
    expect(run!.steps.map((s) => s.result)).toEqual(['skipped', 'skipped', 'ok', 'ok', 'ok', 'no_reply', 'ok'])
    expect(run!.steps[0]).toMatchObject({ error: 'already_set' })
    expect(await lastRun(m)).toMatchObject({ outcome: 'completed', failed_steps: 0 })

    // What was sent is now true (power, source), so a second startup skips it; the light's fields aren't known, so it runs.
    await m.handle(req('POST', '/__mock/ha?slow=none'))
    await m.handle(req('POST', '/activities/act_watch_apple_tv/activate'))
    expect((await runsOf(m))[0]!.steps.map((s) => s.result)).toEqual(['skipped', 'skipped', 'ok', 'ok', 'ok', 'skipped', 'ok'])

    // Off turns the TV off, and an off player has no source: the next startup powers it on and sets the input again.
    await m.handle(req('POST', '/activities/off'))
    await m.handle(req('POST', '/activities/act_watch_apple_tv/activate'))
    expect((await runsOf(m))[0]!.steps.map((s) => s.result)).toEqual(['ok', 'skipped', 'ok', 'ok', 'ok', 'ok', 'ok'])
  })

  it('never checks a Home Assistant action (call_service), like the hub', async () => {
    const m = createMock()
    await m.handle(req('POST', '/__mock/ha?state=media_player.family_room_lg_tv:on&attr=media_player.family_room_lg_tv:button=%22INFO%22'))
    const a = (await (await m.handle(req('GET', '/activities/act_watch_apple_tv'))).body) as Activity
    const call = { integration_id: 'ha_main', device_id: 'dev_lg_tv', command_id: 'call_service', params: {}, call: { service: 'webostv.button', data: { button: 'INFO' } } }
    const put = await m.handle(req('PUT', '/activities/act_watch_apple_tv', { ...a, startup: [call] }))
    expect(put.status, JSON.stringify(put.body)).toBe(200)
    await m.handle(req('POST', '/activities/act_watch_apple_tv/activate'))
    expect((await runsOf(m))[0]!.steps.map((s) => s.result)).toEqual(['ok'])
  })
})
