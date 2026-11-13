import { describe, expect, it } from 'vitest'
import type { Action, Command, Device } from '@/api/types'
import type { Catalog } from '@/lib/activity'
import { actionText, commandLabel, paramSummary } from './describe'

const pct = { name: 'percentage', type: 'int', minimum: 0, maximum: 100, step: 33, unit: '%' } as const
const commands: Command[] = [
  { command_id: 'fan.set_percentage', label: 'Set speed', effect: 'momentary', params: [{ ...pct, required: true }] },
  { command_id: 'fan.turn_on', label: 'Turn on', effect: 'momentary', params: [{ ...pct, required: false }] },
  { command_id: 'light.brightness_step', label: 'Change brightness', effect: 'momentary', params: [{ name: 'step', type: 'int', required: true, minimum: -255, maximum: 255 }] },
  { command_id: 'sonos.set_sleep_timer', label: 'Sleep timer', effect: 'momentary', params: [{ name: 'sleep_time', type: 'int', required: true, minimum: 0, maximum: 86400, unit: 's' }] },
  { command_id: 'cover.set_cover_position', label: 'Set position', effect: 'momentary', params: [{ name: 'position', type: 'int', required: true, minimum: 0, maximum: 100, unit: '%' }] },
]
const cat: Catalog = { devices: new Map([['dev_fan', { id: 'dev_fan' } as Device]]), capabilities: new Map([['dev_fan', { device_id: 'dev_fan', integration_id: 'ha_main', commands }]]), systemCommands: [], activities: [] }
const act = (command_id: string, params: Action['params']): Action => ({ integration_id: 'ha_main', device_id: 'dev_fan', command_id, params })
const reads = (a: Action) => `${commandLabel(a, cat)} ${paramSummary(a, cat)}`.trim()

describe('how an action reads', () => {
  it('says a fan speed as one of its speeds, and a brightness step as a signed percent', () => {
    expect(reads(act('fan.set_percentage', { percentage: 66 }))).toBe('Set speed 2 of 3')
    expect(reads(act('fan.set_percentage', { percentage: 50 }))).toBe('Set speed 2 of 3') // off-list: the speed HA runs
    expect(reads(act('fan.turn_on', { percentage: 100 }))).toBe('Turn on speed 3 of 3')
    expect(reads(act('light.brightness_step', { step: 26 }))).toBe('Change brightness +10%')
  })
  it('says a number with its unit', () => {
    expect(reads(act('cover.set_cover_position', { position: 50 }))).toBe('Set position 50%')
    expect(reads(act('sonos.set_sleep_timer', { sleep_time: 300 }))).toBe('Sleep timer 300 s')
  })
})

describe('how a wait reads', () => {
  const wait = (duration_ms: number): Action => ({ integration_id: 'system', device_id: null, command_id: 'wait', params: { duration_ms } })
  it('says milliseconds under a second, else seconds without trailing zeros', () => {
    expect(paramSummary(wait(0))).toBe('0 ms')
    expect(paramSummary(wait(500))).toBe('500 ms')
    expect(paramSummary(wait(1000))).toBe('1 s')
    expect(paramSummary(wait(1500))).toBe('1.5 s')
    expect(paramSummary(wait(2250))).toBe('2.25 s')
    expect(paramSummary(wait(60000))).toBe('60 s')
  })
})

describe('an action as one line', () => {
  it('names the device first, and Hubbo’s own commands without one', () => {
    const fan: Catalog = { ...cat, devices: new Map([['dev_fan', { id: 'dev_fan', name: 'Fan' } as Device]]) }
    expect(actionText(act('fan.set_percentage', { percentage: 66 }), fan)).toBe('Fan → Set speed 2 of 3')
    expect(actionText({ integration_id: 'system', device_id: null, command_id: 'wait', params: { duration_ms: 1500 } }, fan)).toBe('wait 1.5 s')
  })
})
