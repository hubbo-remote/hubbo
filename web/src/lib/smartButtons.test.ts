import { describe, expect, it } from 'vitest'
import type { Action, Device, SmartButtons } from '@/api/types'
import type { Catalog } from './activity'
import { emptySmartButtons, pickedDevices, smartOf, validateSmartButtons, withoutSmartButtons } from './smartButtons'

const off: Action = { integration_id: 'system', device_id: null, command_id: 'off', params: {} }

describe('withoutSmartButtons', () => {
  const none = emptySmartButtons(1).slots
  const b = { 'light_1:tap': [off], 'light_1:down': [off], 'light_1:up': [off], 'plus:hold': [off], 'minus:tap': [off], 'light_2:tap': [off], 'off:tap': [off] }
  it('drops a slot getting its first device, and + and − with the first device anywhere; keeps the rest', () => {
    expect(withoutSmartButtons(b, none, { ...none, light_1: 'dev_x' })).toEqual({ 'light_2:tap': [off], 'off:tap': [off] })
  })
  it('leaves + and − once a device was already saved, and a slot that already had one', () => {
    const saved = { ...none, light_1: 'dev_x' }
    expect(withoutSmartButtons(b, saved, { ...saved, light_2: 'dev_y' })).toEqual({ 'light_1:tap': [off], 'light_1:down': [off], 'light_1:up': [off], 'plus:hold': [off], 'minus:tap': [off], 'off:tap': [off] })
    expect(withoutSmartButtons(b, saved, { ...saved, light_1: 'dev_z' })).toBe(b)
  })
  it('changes nothing for a pick undone before saving', () => {
    expect(withoutSmartButtons(b, none, none)).toBe(b)
  })
})

describe('smartOf', () => {
  it('reads the hub’s slots and actions; nothing before they load', () => {
    expect(smartOf(undefined)).toBeUndefined()
    const none = { button: { tap: [], hold: [], release: [] }, plus: { tap: [], hold: [], release: [] }, minus: { tap: [], hold: [], release: [] } }
    const lit = { ...none, button: { tap: [off], hold: [], release: [] } }
    const sb: SmartButtons = { ...emptySmartButtons(3), slots: { ...emptySmartButtons(3).slots, light_1: 'dev_x' }, actions: { light_1: lit, light_2: none, switch_1: none, switch_2: none } }
    const s = smartOf(sb)!
    expect(s.slots.light_1).toBe('dev_x')
    expect(s.actions('light_1')).toBe(lit)

    // A draft: a device just picked takes its actions from `picked` (null while they load).
    const draft = { ...sb.slots, light_2: 'dev_y', switch_1: 'dev_z' }
    expect(pickedDevices(sb, draft)).toEqual(['dev_y', 'dev_z'])
    expect(pickedDevices(sb, sb.slots)).toEqual([])
    const d = smartOf(sb, draft, new Map([['dev_y', lit]]))!
    expect(d.actions('light_1')).toBe(lit)
    expect(d.actions('light_2')).toBe(lit)
    expect(d.actions('switch_1')).toBeNull()
  })
})

describe('validateSmartButtons', () => {
  it('flags a slot whose device isn’t on the hub, like the hub', () => {
    const cat = { devices: new Map([['dev_x', { id: 'dev_x' } as Device]]) } as Catalog
    const slots = { ...emptySmartButtons(1).slots, light_1: 'dev_x', switch_2: 'dev_gone' }
    expect(validateSmartButtons({ slots }, cat).errors).toEqual([{ path: '/slots/switch_2', code: 'unknown_device', message: 'No device with that id' }])
  })
})
