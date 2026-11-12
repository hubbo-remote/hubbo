import { describe, expect, it } from 'vitest'
import type { Capabilities, Device, DeviceDetail } from '../../api/types.ts'
import { bleDevice, buildStarter, haDevice, type ButtonRule, type WizardAnswers, type WizardDevice } from './build.ts'

const rule = (button: ButtonRule['button'], role: ButtonRule['role'], hold: boolean, ...candidates: string[]): ButtonRule => ({ button, role, hold, candidates })
const RULES: ButtonRule[] = [
  rule('up', 'watch', true, 'DirectionUp'),
  rule('select', 'watch', false, 'Select', 'OK', 'Enter'),
  rule('menu', 'watch', true, 'Menu', 'Home'), // held for Control Center, as in profiles/buttons.json
  rule('info', 'watch', false, 'Info'),
  rule('play', 'watch', false, 'Play', 'PlayPause'),
  rule('volume_up', 'volume', true, 'VolumeUp'),
  rule('mute', 'volume', false, 'Mute', 'MuteToggle'),
]

const ha = (id: string, commands: Capabilities['commands']): [Device, Capabilities] => [
  { id, name: id, integration_id: 'ha_main', kind: 'tv', provider: { type: 'home_assistant', entity_id: `media_player.${id}` }, platform: 'home_assistant', availability: 'available', schema_version: 1, revision: 1 },
  { device_id: id, integration_id: 'ha_main', commands },
]
const cmd = (command_id: string, params: Capabilities['commands'][number]['params'] = []) => ({ command_id, label: command_id, effect: 'momentary' as const, params })
const [tvD, tvC] = ha('tv', [
  cmd('media_player.turn_on'),
  cmd('media_player.turn_off'),
  cmd('media_player.volume_up'),
  cmd('media_player.volume_mute', [{ name: 'muted', type: 'bool', required: true }]),
  cmd('media_player.media_play_pause'),
  cmd('media_player.select_source', [{ name: 'source', type: 'enum', required: true, options: ['HDMI 1', 'HDMI 2'] }]),
])
const u = { page: 'consumer' as const, usage: '0x0001' }
// A Bluetooth streamer with power steps (the builder's rules, not any one profile).
const streamer: DeviceDetail = {
  id: 'streamer',
  name: 'Streamer',
  integration_id: 'ble',
  kind: 'streamer',
  provider: { type: 'ble_hid' },
  platform: 'ble_hid',
  manufacturer: 'Test',
  model: 'Streamer',
  profile: { id: 'test:streamer', revision: 1, sha256: '' },
  availability: 'available',
  schema_version: 1,
  revision: 1,
  commands: { DirectionUp: u, DirectionDown: u, Select: u, Home: u, PlayPause: u },
  buttons: { menu: 'Home' },
  power: { type: 'discrete', on: [{ command: 'Home' }], off: [{ command: 'Home', hold_ms: 2000 }, { delay_ms: 800 }, { command: 'DirectionDown' }, { delay_ms: 200 }, { command: 'Select' }], on_delay_ms: 6000 },
  timing: { key_delay_ms: 100 },
}
const devices = [haDevice(tvD, tvC), bleDevice(streamer)]
const answers = (a: Partial<WizardAnswers> = {}): WizardAnswers => ({ name: ' Watch ', devices: ['streamer', 'tv'], watch: 'streamer', volume: 'tv', inputs: {}, power: {}, ...a })
const press = (command_id: string, command: string, extra = {}) => ({ integration_id: 'ble', device_id: 'streamer', command_id, params: { command, ...extra } })
const tv = (command_id: string, params = {}) => ({ integration_id: 'ha_main', device_id: 'tv', command_id, params })
const wait = (duration_ms: number) => ({ integration_id: 'system', device_id: null, command_id: 'wait', params: { duration_ms } })

describe('buildStarter', () => {
  it('fills watch buttons from the watch device and volume from the volume device', () => {
    const { activity, filled } = buildStarter(devices, answers(), RULES)
    expect(activity.name).toBe('Watch')
    expect(activity.bindings).toEqual({
      'up:down': [press('press_down', 'DirectionUp')],
      'up:up': [press('press_up', 'DirectionUp')],
      'select:tap': [press('press', 'Select')],
      'menu:down': [press('press_down', 'Home')], // the profile's override, held
      'menu:up': [press('press_up', 'Home')],
      'play:tap': [press('press', 'PlayPause')], // the second candidate
      'volume_up:down': [tv('media_player.volume_up')], // HA can't repeat: one send on press
      'mute:tap': [tv('media_player.volume_mute', { muted: true })],
      'mute:hold': [tv('media_player.volume_mute', { muted: false })],
    })
    expect(filled.map((f) => f.button)).not.toContain('info') // no candidate: left empty
  })

  it('a hold button with no press form gets one send on down', () => {
    const tap = tv('media_player.menu')
    const d: WizardDevice = { ...haDevice(tvD, tvC), commands: new Map([['menu', { tap }]]) }
    const { activity } = buildStarter([d], answers({ devices: ['tv'], watch: 'tv', volume: null }), RULES)
    expect(activity.bindings).toEqual({ 'menu:down': [tap] })
  })

  it('a profile override of null leaves the button empty, and matching ignores case', () => {
    const d = bleDevice({ ...streamer, commands: { select: u, Home: u }, buttons: { menu: null } })
    const { activity } = buildStarter([d], answers({ devices: ['streamer'], volume: null }), RULES)
    expect(Object.keys(activity.bindings)).toEqual(['select:tap'])
  })

  it('startup: HA first, power on, a wake wait before the input; Bluetooth waits to connect, with key delays', () => {
    const { activity } = buildStarter(devices, answers({ inputs: { tv: 'HDMI 2' } }), RULES)
    expect(activity.startup).toEqual([
      tv('media_player.turn_on'),
      wait(3000),
      tv('media_player.select_source', { source: 'HDMI 2' }),
      { integration_id: 'ble', device_id: 'streamer', command_id: 'wait_connected', params: { timeout_ms: 15000 } },
      wait(100),
      press('press', 'Home'),
      wait(6000),
    ])
    expect(activity.shutdown).toEqual([tv('media_player.turn_off'), press('press', 'Home', { hold_ms: 2000 }), wait(800), press('press', 'DirectionDown'), wait(200), press('press', 'Select')])
  })

  it('no wake wait without an input; power answers turn steps off', () => {
    const { activity } = buildStarter(devices, answers({ power: { tv: { on: true, off: false }, streamer: { on: false, off: false } } }), RULES)
    expect(activity.startup).toEqual([tv('media_player.turn_on')])
    expect(activity.shutdown).toEqual([])
  })

  it('a Bluetooth device waits to connect before its first step, whatever it is, and only once', () => {
    const connect = { integration_id: 'ble', device_id: 'streamer', command_id: 'wait_connected', params: { timeout_ms: 15000 } }
    const withInput = bleDevice({ ...streamer, inputs: [{ name: 'Plex', steps: [{ command: 'Home' }] }] })
    // Power on off, an input still switched: the wait comes before the input.
    const off = buildStarter([withInput], answers({ devices: ['streamer'], power: { streamer: { on: false, off: false } }, inputs: { streamer: 'Plex' } }), RULES)
    expect(off.activity.startup).toEqual([connect, wait(100), press('press', 'Home')])
    // Power on and an input: one wait, first.
    const both = buildStarter([withInput], answers({ devices: ['streamer'], inputs: { streamer: 'Plex' } }), RULES)
    expect(both.activity.startup.filter((a) => a.command_id === 'wait_connected')).toHaveLength(1)
    expect(both.activity.startup[0]).toEqual(connect)
    // No power steps (like the Apple TV profile) and no input: no steps, so no wait.
    const none = buildStarter([bleDevice({ ...streamer, power: { type: 'none' } })], answers({ devices: ['streamer'] }), RULES).activity
    expect([none.startup, none.shutdown]).toEqual([[], []])
  })

  it('toggle-only power gets no step', () => {
    const [d, c] = ha('amp', [cmd('media_player.toggle'), cmd('media_player.volume_up')])
    const amp = haDevice(d, c)
    const s = buildStarter([amp], answers({ devices: ['amp'], watch: null, volume: 'amp' }), RULES)
    expect([s.activity.startup, s.activity.shutdown, amp.power.type]).toEqual([[], [], 'toggle'])
  })

  it('ignores roles given to devices that weren’t picked', () => {
    const { activity } = buildStarter(devices, answers({ devices: ['tv'] }), RULES)
    expect(Object.keys(activity.bindings).some((k) => k.startsWith('select'))).toBe(false)
  })
})
