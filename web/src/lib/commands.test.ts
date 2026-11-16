import { describe, expect, it } from 'vitest'
import type { Bindings, Action, Command } from '../api/types'
import { loadSeed } from '../../mock/fixtures.ts'
import { actionFor, choiceName, choiceOf, choiceParams, choices, commandName, commandTitle, isFriendly, readButton, toggleLabel, toggleOf, toggleParams, viewKinds, writeLongPress, writeMain } from './commands'

const appleTv = loadSeed().capabilities.get('dev_apple_tv')!.commands
const cmd = (command_id: string, label: string, params: Command['params'] = []): Command => ({ command_id, label, effect: 'momentary', params })
// Shapes from a live Home Assistant (hub-sim): LIFX and Sonos services, and the standard ones.
const flame = cmd('lifx.effect_flame', 'Flame effect', [
  { name: 'speed', label: 'Speed', type: 'int', required: false, minimum: 1, maximum: 25, unit: 's', default: 3 },
  { name: 'power_on', label: 'Power on', type: 'bool', required: false, default: true },
])
const hev = cmd('lifx.set_hev_cycle_state', 'Set HEV cycle state', [
  { name: 'power', label: 'Enable', type: 'bool', required: true },
  { name: 'duration', label: 'Duration', type: 'int', required: false, minimum: 0, maximum: 86400, unit: 's', default: 7200 },
])
const snapshot = cmd('sonos.snapshot', 'Snapshot', [{ name: 'with_group', label: 'With group', type: 'bool', required: false, default: true }])
const setOption = cmd('sonos.set_option', 'Set option', [
  { name: 'night_sound', label: 'Night sound', type: 'bool', required: false },
  { name: 'speech_enhance', label: 'Speech enhance', type: 'bool', required: false },
])
const mute = cmd('media_player.volume_mute', 'Mute', [{ name: 'muted', type: 'bool', required: true }])
const timer = cmd('sonos.set_sleep_timer', 'sonos.set_sleep_timer', [{ name: 'sleep_time', label: 'Sleep Time', type: 'int', required: false, minimum: 0, maximum: 7200, unit: 's' }])
const ha = [cmd('media_player.turn_on', 'Turn on'), mute, flame, hev, snapshot, setOption, timer]
const dev = { integration_id: 'ha_main', device_id: 'dev_x' }
const press = (command: string, extra = {}): Action => ({ integration_id: 'ble', device_id: 'dev_apple_tv', command_id: 'press', params: { command, ...extra } })

describe('names', () => {
  it('reads profile commands and raw HA ids as words, and keeps HA labels in sentence case', () => {
    expect(commandName('DirectionUp')).toBe('Up')
    expect(commandName('PlayPause')).toBe('Play/Pause')
    expect(commandName('FastForward')).toBe('Fast forward')
    expect(commandTitle(timer)).toBe('Set sleep timer')
    expect(commandTitle(cmd('x.y', 'Play Queue'))).toBe('Play queue')
    expect(commandTitle(cmd('x.y', 'Join Sonos group'))).toBe('Join Sonos group')
  })
})

describe('on/off splitting', () => {
  it('splits a required bool and optional bools with no default, but not a modifier bool', () => {
    expect(toggleParams(mute)?.map((p) => p.name)).toEqual(['muted'])
    expect(toggleParams(hev)?.map((p) => p.name)).toEqual(['power'])
    expect(toggleParams(setOption)?.map((p) => p.name)).toEqual(['night_sound', 'speech_enhance'])
    expect(toggleParams(snapshot)).toBeNull()
    expect(toggleParams(flame)).toBeNull()
  })

  it('names each side', () => {
    expect([toggleLabel(mute, 'muted', true), toggleLabel(mute, 'muted', false)]).toEqual(['Mute', 'Unmute'])
    expect(toggleLabel(hev, 'power', true)).toBe('HEV cycle state on')
    expect(toggleLabel(setOption, 'night_sound', false)).toBe('Night sound off')
  })

  it('a required bool carries the command’s other params; an optional-bool split sends only its bool', () => {
    expect(toggleOf({ ...dev, command_id: hev.command_id, params: { power: true, duration: 60 } }, hev)).toEqual(['power', true])
    expect(toggleOf({ ...dev, command_id: setOption.command_id, params: { night_sound: true, speech_enhance: false } }, setOption)).toBeNull()
  })
})

describe('choices', () => {
  it('lists a Bluetooth device’s commands by name, keys and codes under More', () => {
    const list = choices(appleTv, 'binding')
    expect(list[0]).toEqual({ id: 'press:DirectionUp', label: 'Up' })
    expect(list.filter((c) => c.more).map((c) => c.label)).toEqual(['Keyboard key…', 'Custom code…'])
    expect(list.some((c) => c.id.includes('wait_connected'))).toBe(false) // a wait only in sequences
    expect(choices(appleTv, 'sequence').some((c) => c.label === 'Wait until connected')).toBe(true)
    expect(choiceName(list.find((c) => c.id === 'key')!)).toBe('Keyboard key')
  })

  it('splits toggles, puts an integration’s own services under More', () => {
    const list = choices(ha, 'test', 'media_player')
    expect(list.filter((c) => !c.more).map((c) => c.label)).toEqual(['Turn on', 'Mute', 'Unmute'])
    expect(list.filter((c) => c.more).map((c) => c.label)).toEqual(['Flame effect', 'HEV cycle state on', 'HEV cycle state off', 'Snapshot', 'Night sound on', 'Night sound off', 'Speech enhance on', 'Speech enhance off', 'Set sleep timer'])
  })

  it('a new action sends required params only; modifiers wait to be changed', () => {
    expect(actionFor('cmd:lifx.effect_flame', dev, ha).params).toEqual({})
    expect(actionFor('set:lifx.set_hev_cycle_state:power:false', dev, ha).params).toEqual({ power: false })
    expect(actionFor('press:Select', { integration_id: 'ble', device_id: 'dev_apple_tv' }, appleTv).params).toEqual({ command: 'Select' })
    expect(actionFor('key', { integration_id: 'ble', device_id: 'dev_apple_tv' }, appleTv)).toMatchObject({ command_id: 'key_tap' })
  })

  it('asks for a toggle’s other params, a press’s timed hold only in steps', () => {
    expect(choiceParams('set:lifx.set_hev_cycle_state:power:true', ha, 'test').map((p) => p.name)).toEqual(['duration'])
    expect(choiceParams('set:sonos.set_option:night_sound:true', ha, 'test')).toEqual([])
    expect(choiceParams('press:Select', appleTv, 'binding')).toEqual([])
    expect(choiceParams('press:Select', appleTv, 'sequence').map((p) => p.name)).toEqual(['hold_ms'])
  })

  it('round-trips an action to its choice, and leaves what it can’t show to the catalog picker', () => {
    for (const id of ['press:Home', 'key', 'cmd:lifx.effect_flame', 'set:media_player.volume_mute:muted:false', 'set:lifx.set_hev_cycle_state:power:true']) {
      const commands = id.startsWith('cmd:') || id.startsWith('set:') ? ha : appleTv
      const a = actionFor(id, dev, commands)
      expect(isFriendly(a, commands), id).toBe(true)
      expect(choiceOf(a, commands)).toBe(id)
    }
    expect(isFriendly({ ...press('Home'), command_id: 'press_down' }, appleTv)).toBe(false)
  })
})

describe('a button as Press / Long press', () => {
  const none = { bindings: {}, macros: [] }
  const B = (bindings: Bindings, macros: string[] = []) => ({ bindings, macros })
  const view = (v: Partial<ReturnType<typeof readButton>>) => ({ press: [], pressMacro: false, pressKey: null, longPress: [], longMacro: false, raw: [], ...v })

  it('passes Press straight through, and makes it a tap while a long press is bound', () => {
    const mirror = writeMain(none, 'up', press('DirectionUp', { hold_ms: 800 }))
    expect(mirror).toEqual(B({
      'up:down': [{ ...press('DirectionUp'), command_id: 'press_down' }],
      'up:up': [{ ...press('DirectionUp'), command_id: 'press_up' }],
    }))
    expect(readButton(mirror, 'up')).toEqual(view({ press: [press('DirectionUp')] }))
    const ha = writeMain(none, 'volume_up', { ...dev, command_id: 'media_player.volume_up', params: {} })
    expect(Object.keys(ha.bindings)).toEqual(['volume_up:down']) // no down/up: one send, on press
    expect(readButton(ha, 'volume_up').press[0]?.command_id).toBe('media_player.volume_up')

    const both = writeLongPress(writeMain(none, 'select', press('Select')), 'select', press('Home'))
    expect(both).toEqual(B({ 'select:tap': [press('Select')], 'select:hold': [press('Home')] }))
    expect(viewKinds(readButton(both, 'select'))).toEqual(['tap', 'hold'])
    expect(Object.keys(writeLongPress(both, 'select', null).bindings)).toEqual(['select:down', 'select:up']) // back to the mirror
    expect(writeMain(both, 'select', null)).toEqual(B({ 'select:hold': [press('Home')] })) // long press kept
  })

  it('makes Press a tap when the long press was bound first, and Hubbo’s own commands always a tap', () => {
    const held = writeMain(writeLongPress(none, 'info', press('Home')), 'info', press('Info'))
    expect(held).toEqual(B({ 'info:hold': [press('Home')], 'info:tap': [press('Info')] }))
    const off = { integration_id: 'system', device_id: null, command_id: 'off', params: {} }
    expect(writeMain(none, 'tv', off)).toEqual(B({ 'tv:tap': [off] }))
    expect(readButton(B({ 'tv:tap': [off] }), 'tv').press).toEqual([off])
  })

  it('keeps a timed profile command a tap, with or without a long press', () => {
    const timed = (a: Action) => a.params.command === 'ControlCenter'
    const cc = press('ControlCenter')
    expect(writeMain(none, 'menu', cc, { timed })).toEqual(B({ 'menu:tap': [cc] }))
    expect(readButton(B({ 'menu:tap': [cc] }), 'menu')).toEqual(view({ press: [cc] }))
    const both = writeLongPress(writeMain(none, 'menu', cc, { timed }), 'menu', press('Home'), { timed })
    expect(writeLongPress(both, 'menu', null, { timed })).toEqual(B({ 'menu:tap': [cc] }))
  })

  it('writes a macro as its list, marked, at any length: on down, or on the tap with a long press', () => {
    const one = writeMain(none, 'play', [press('Home')], { macro: true })
    expect(one).toEqual(B({ 'play:down': [press('Home')] }, ['play:down']))
    expect(readButton(one, 'play')).toEqual(view({ press: [press('Home')], pressMacro: true, pressKey: 'play:down' }))
    const two = [press('Home'), press('Select')]
    const m = writeMain(none, 'play', two, { macro: true })
    const both = writeLongPress(m, 'play', [press('Info'), press('Back')], { macro: true })
    expect(both).toEqual(B({ 'play:tap': two, 'play:hold': [press('Info'), press('Back')] }, ['play:hold', 'play:tap']))
    expect(readButton(both, 'play')).toEqual(view({ press: two, pressMacro: true, pressKey: 'play:tap', longPress: [press('Info'), press('Back')], longMacro: true }))
    expect(writeLongPress(both, 'play', null)).toEqual(B({ 'play:down': two }, ['play:down']))
    expect(writeMain(m, 'play', null)).toEqual(none) // cleared: the mark goes too
  })

  it('moves a macro’s mark with its list when a Long press comes or goes, so a saved macro is always marked', () => {
    const up = press('DirectionUp')
    const m = writeMain(B({ 'tv:tap': [press('Home')] }, []), 'switch_2', [up, up, up, up], { macro: true })
    const held = writeLongPress(m, 'switch_2', up)
    expect(held.macros).toEqual(['switch_2:tap'])
    expect(writeLongPress(held, 'switch_2', null).macros).toEqual(['switch_2:down'])
    expect(writeMain(held, 'switch_2', [...held.bindings['switch_2:tap']!, up], { macro: true }).macros).toEqual(['switch_2:tap'])
  })

  it('leaves triggers it can’t fold as raw', () => {
    const odd = B({ 'up:down': [press('DirectionUp')], 'up:tap': [press('Select')] })
    expect(readButton(odd, 'up')).toEqual(view({ raw: ['down', 'tap'] }))
    const mismatched = B({ 'up:down': [{ ...press('DirectionUp'), command_id: 'press_down' }], 'up:up': [{ ...press('DirectionDown'), command_id: 'press_up' }] })
    expect(readButton(mismatched, 'up').raw).toEqual(['down', 'up'])
  })
})
