import { describe, expect, it } from 'vitest'
import type { Activity } from '../api/types'
import { fixture, loadSeed } from '../../mock/fixtures.ts'
import { boundEvents, buttonForPath, parseTrigger, validateActivity, validateBindings, waitAction, systemAction, type Catalog } from './activity'

const s = loadSeed()
const cat: Catalog = {
  devices: new Map(s.devices.map((d) => [d.id, d])),
  capabilities: s.capabilities,
  systemCommands: s.integrations.find((i) => i.id === 'system')!.commands!,
  activities: s.activities.map((a) => ({ id: a.id, name: a.name })),
}
const watch = s.activities.find((a) => a.id === 'act_watch_apple_tv')!

describe('activity helpers', () => {
  it('parses trigger keys and error paths', () => {
    expect(parseTrigger('volume_up:down')).toEqual({ button: 'volume_up', event: 'down' })
    expect(parseTrigger('volume_up:double')).toBeNull()
    expect(buttonForPath('/bindings/info:hold/0/params/key')).toBe('info')
    expect(buttonForPath('/startup/2')).toBeNull()
  })

  it('reports bound events in canonical order', () => {
    expect(boundEvents(watch.bindings, 'up')).toEqual(['down', 'up'])
  })

  it('accepts every fixture activity and the global bindings', () => {
    for (const a of s.activities) expect(validateActivity(a, cat).errors).toEqual([])
    for (const f of ['activities/create.request.json', 'activities/replace.request.json']) expect(validateActivity(fixture<Activity>(f), cat).errors).toEqual([])
    const errors: never[] = []
    validateBindings(s.globalBindings.bindings, cat, errors, [])
    expect(errors).toEqual([])
  })

  it('rejects unknown devices, ownership mismatches, bad params, long waits and misplaced transitions', () => {
    const r = validateActivity(
      {
        name: ' ',
        startup: [waitAction(120_000), { integration_id: 'ha_main', device_id: 'dev_nope', command_id: 'x', params: {} }, systemAction('off')],
        shutdown: [{ integration_id: 'ha_main', device_id: 'dev_denon', command_id: 'media_player.volume_set', params: { level: 2 } }],
        bindings: {
          'up:down': [{ integration_id: 'ha_main', device_id: 'dev_apple_tv', command_id: 'key_down', params: { key: 'ARROW_UP' } }],
          'tv:tap': [systemAction('activate', { activity_id: 'act_nope' })],
        },
      },
      cat,
    )
    expect(r.valid).toBe(false)
    expect(r.errors.map((e) => e.code).sort()).toEqual(['integration_mismatch', 'misplaced_transition', 'missing_param', 'param_out_of_range', 'param_out_of_range', 'required', 'unknown_device', 'unpaired_hold'])
  })

  it('holds: down only, released on up; never in sequences', () => {
    const apple = (command_id: string, key = 'ARROW_UP') => ({ integration_id: 'ble', device_id: 'dev_apple_tv', command_id, params: { key } })
    const codes = (bindings: Record<string, ReturnType<typeof apple>[]>, startup: ReturnType<typeof apple>[] = []) => {
      const r = validateActivity({ name: 'x', startup, shutdown: [], bindings }, cat)
      return { errors: r.errors.map((e) => `${e.code}@${e.path}`), warnings: r.warnings.map((w) => w.code) }
    }
    expect(codes({ 'up:down': [apple('key_down')], 'up:up': [apple('key_up')] }).errors).toEqual([])
    expect(codes({ 'up:down': [apple('key_down')] }).errors).toEqual(['unpaired_hold@/bindings/up:down/0/command_id'])
    expect(codes({ 'up:down': [apple('key_down')], 'up:up': [apple('key_up', 'ARROW_DOWN')] }).errors).toEqual(['unpaired_hold@/bindings/up:down/0/command_id'])
    expect(codes({ 'up:tap': [apple('key_down')] }).errors).toEqual(['hold_not_allowed@/bindings/up:tap/0/command_id'])
    expect(codes({}, [apple('key_down')]).errors).toEqual(['hold_not_allowed@/startup/0/command_id'])
    expect(codes({ 'up:down': [apple('key_down')], 'up:up': [apple('key_up')], 'up:hold': [apple('key_tap', 'AC_HOME')] }).warnings).toEqual(['mixed_semantics'])
  })
})
