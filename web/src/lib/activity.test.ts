import { describe, expect, it } from 'vitest'
import type { Activity } from '../api/types'
import { fixture, loadSeed } from '../../mock/fixtures.ts'
import type { ParamSpec } from '../api/types'
import { bindsButton, buttonForPath, defaultParams, hubboCommands, mostUsedDevice, drawerParams, fanSpeeds, fieldLabel, speedLabel, speedOf, speedOptions, paramLabel, parseTrigger, percentLabel, percentOptions, percentScale, validateActivity, validateBindings, waitAction, systemAction, type Catalog } from './activity'

const s = loadSeed()
const cat: Catalog = {
  devices: new Map(s.devices.map((d) => [d.id, d])),
  capabilities: s.capabilities,
  systemCommands: s.integrations.find((i) => i.id === 'system')!.commands!,
  activities: s.activities.map((a) => ({ id: a.id, name: a.name })),
}

describe('activity helpers', () => {
  it('parses trigger keys and error paths', () => {
    expect(parseTrigger('volume_up:down')).toEqual({ button: 'volume_up', event: 'down' })
    expect(parseTrigger('volume_up:double')).toBeNull()
    expect(buttonForPath('/bindings/info:hold/0/params/key')).toBe('info')
    expect(buttonForPath('/startup/2')).toBeNull()
  })

  it('counts a button as bound only when one of its triggers has an action (the hub’s masking rule)', () => {
    const a = waitAction()
    expect(bindsButton({ 'up:tap': [a] }, 'up')).toBe(true)
    expect(bindsButton({ 'up:hold': [] }, 'up')).toBe(false)
    expect(bindsButton({ 'down:tap': [a] }, 'up')).toBe(false)
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

describe('params as people see them', () => {
  const level: ParamSpec = { name: 'level', type: 'number', required: true, minimum: 0, maximum: 1 }
  const brightness: ParamSpec = { name: 'brightness', type: 'int', required: false, minimum: 0, maximum: 255 }
  const step: ParamSpec = { name: 'step', type: 'int', required: true, minimum: -255, maximum: 255 }

  it('shows volume, light brightness and brightness steps as percents', () => {
    const vol = percentScale('media_player.volume_set', level)!
    expect(percentLabel(vol, 0.35)).toBe('35%')
    expect(percentLabel(vol, 0.335)).toBe('33.5%')
    expect(percentOptions(vol)).toHaveLength(21)
    expect(percentOptions(vol, 0.33)).toContain(0.33) // an off-grid value stays an option

    const bri = percentScale('light.turn_on', brightness)!
    expect(percentOptions(bri).slice(0, 3)).toEqual([0, 13, 26])
    expect(percentOptions(bri).at(-1)).toBe(255)
    expect(percentLabel(bri, 128)).toBe('50%')
    expect(percentOptions(bri, 100)).toContain(100)
    expect(percentLabel(bri, 100)).toBe('39%')

    const st = percentScale('light.brightness_step', step)!
    const opts = percentOptions(st)
    expect(opts).toHaveLength(40)
    expect(opts).not.toContain(0)
    expect([percentLabel(st, opts[0]!), percentLabel(st, 26)]).toEqual(['−100%', '+10%'])

    expect(percentScale('fan.set_percentage', brightness)).toBeNull() // only lights
    expect(percentScale('light.turn_on', { ...brightness, maximum: 100 })).toBeNull()
  })

  it('a required whole-percent param starts at the allowed value nearest 35%, never 0', () => {
    const speed = (step: number, minimum = 0): ParamSpec => ({ name: 'percentage', type: 'int', required: true, minimum, maximum: 100, step, unit: '%' })
    expect(defaultParams([speed(33)], 'x.set_level')).toEqual({ percentage: 33 })
    expect([28, 42]).toContain(defaultParams([speed(14)], 'x.set_level').percentage)
    expect(defaultParams([speed(1)], 'x.set_level')).toEqual({ percentage: 35 })
    expect(defaultParams([speed(100)], 'x.set_level')).toEqual({ percentage: 100 })
  })

  it('picks a fan’s speed from its real speeds, stored as Home Assistant computes them', () => {
    const pct = (step?: number, required = true): ParamSpec => ({ name: 'percentage', type: 'int', required, minimum: 0, maximum: 100, step, unit: '%' })
    expect(fanSpeeds('fan.set_percentage', pct(33))).toBe(3)
    expect(fanSpeeds('fan.turn_on', pct(14, false))).toBe(7)
    expect(fanSpeeds('fan.set_percentage', pct(1))).toBeNull() // no real speeds: a number
    expect(fanSpeeds('fan.set_percentage', pct())).toBeNull()
    expect(fanSpeeds('fan.increase_speed', { ...pct(33), name: 'percentage_step' })).toBeNull()
    expect(fanSpeeds('light.turn_on', pct(33))).toBeNull()
    expect(speedOptions(3)).toEqual([33, 66, 100])
    expect(speedOptions(7)).toEqual([14, 28, 42, 57, 71, 85, 100])
    expect(speedOptions(3, 50)).toEqual([33, 50, 66, 100]) // a stored off-list value stays
    expect([speedOf(50, 3), speedOf(34, 3), speedOf(33, 3), speedOf(100, 3)]).toEqual([2, 2, 1, 3])
    expect(speedLabel(50, 3)).toBe('Speed 2 of 3')
    expect(defaultParams([pct(33)], 'fan.set_percentage')).toEqual({ percentage: 33 }) // speed 1
    expect(defaultParams([pct(14)], 'fan.set_percentage')).toEqual({ percentage: 14 })
    expect(fieldLabel(pct(33), 'fan.set_percentage')).toBe('Percentage')
  })

  it('a new action gets required params only (percents at 35% / +10%)', () => {
    expect(defaultParams([level], 'media_player.volume_set')).toEqual({ level: 0.35 })
    expect(defaultParams([step, { name: 'transition', type: 'number', required: false, default: 1 }], 'light.brightness_step')).toEqual({ step: 26 })
  })
  it('starts a humidity at 45%', () => {
    expect(defaultParams([{ name: 'humidity', type: 'int', required: true, minimum: 30, maximum: 80, unit: '%' }], 'humidifier.set_humidity')).toEqual({ humidity: 45 })
    expect(defaultParams([{ name: 'humidity', type: 'int', required: true, minimum: 50, maximum: 60, unit: '%' }], 'humidifier.set_humidity')).toEqual({ humidity: 50 }) // a narrow range: the nearest it allows
  })

  it('keeps optional params in the drawer, unless one is a command’s only param with no default', () => {
    const queue: ParamSpec = { name: 'queue_position', type: 'int', required: false }
    expect(drawerParams([queue])).toEqual([])
    expect(drawerParams([{ ...queue, default: 0 }]).map((p) => p.name)).toEqual(['queue_position'])
    expect(drawerParams([step, brightness]).map((p) => p.name)).toEqual(['brightness'])
  })

  it('labels a param from the spec, its unit, or its name', () => {
    expect(paramLabel({ name: 'sleep_time', label: 'Sleep Time', type: 'int', required: false })).toBe('Sleep time')
    expect(paramLabel({ name: 'sleep_time', type: 'int', required: false })).toBe('Sleep time')
    expect(fieldLabel({ name: 'speed', label: 'Speed', type: 'int', required: false, unit: 's' }, 'lifx.effect_flame')).toBe('Speed (s)')
    expect(fieldLabel({ name: 'hold_ms', type: 'int', required: false, unit: 'ms' }, 'press')).toBe('Hold (ms)')
    expect(fieldLabel({ name: 'color_temp_kelvin', type: 'int', required: false }, 'light.turn_on')).toBe('Color temperature (K)')
    expect(fieldLabel({ name: 'transition', label: 'Transition', type: 'number', required: false, unit: 'seconds' }, 'light.turn_on')).toBe('Transition (seconds)')
    expect(fieldLabel(brightness, 'light.turn_on')).toBe('Brightness')
  })
})

describe('what a new step or binding starts with', () => {
  it('offers Hubbo’s Switch activity and Off in a binding, and nothing of Hubbo’s in steps', () => {
    expect(hubboCommands(cat, 'binding').map((c) => c.command_id).sort()).toEqual(['activate', 'off'])
    expect(hubboCommands(cat, 'sequence')).toEqual([])
  })

  it('picks the device used most, ties to the first seen, skipping Hubbo’s own', () => {
    const on = (device_id: string | null) => ({ integration_id: 'x', device_id, command_id: 'c', params: {} })
    expect(mostUsedDevice([])).toBeNull()
    expect(mostUsedDevice([on(null), on(null)])).toBeNull()
    expect(mostUsedDevice([on('a'), on('b'), on('b'), on(null), on(null), on(null)])).toBe('b')
    expect(mostUsedDevice([on('a'), on('b')])).toBe('a')
  })
})
