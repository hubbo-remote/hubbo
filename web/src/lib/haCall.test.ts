import { describe, expect, it } from 'vitest'
import { callDataIssues, callIssues, callLabel, prefill, readCallData } from './haCall.ts'

const codes = (data: unknown) => callDataIssues(data, '/call/data').map((x) => `${x.code} ${x.path}`)

describe('a Home Assistant action’s data', () => {
  it('takes an object, and refuses target keys, templates, deep nesting and too much', () => {
    expect(codes({ button: 'INFO' })).toEqual([])
    expect(codes({})).toEqual([])
    expect(codes([1])).toEqual(['wrong_type /call/data'])
    expect(codes('INFO')).toEqual(['wrong_type /call/data'])
    expect(codes({ entity_id: 'media_player.tv', button: 'INFO' })).toEqual(['forbidden_param /call/data/entity_id'])
    expect(codes({ a: { b: ['{{ states("x") }}'] } })).toEqual(['template_not_supported /call/data/a/b/0'])
    expect(codes({ '{% x %}': 1 })).toEqual(['template_not_supported /call/data/{% x %}'])
    expect(codes({ a: { b: { c: { d: 1 } } } })).toEqual([]) // four levels, the data object being the first
    expect(codes({ a: { b: { c: { d: { e: 1 } } } } })).toEqual(['too_deep /call/data'])
    expect(codes({ x: { y: [{ z: 1 }] } })).toEqual([]) // an array is a level too
    expect(codes({ x: { y: [{ z: [] }] } })).toEqual(['too_deep /call/data'])
    expect(codes({ text: 'x'.repeat(1024) })).toEqual(['too_long /call/data'])
  })

  it('checks the service’s shape: picked, and domain.service', () => {
    expect(callIssues({ service: 'webostv.button', data: {} }, '/call')).toEqual([])
    expect(callIssues({ service: '', data: {} }, '/call').map((x) => x.code)).toEqual(['required'])
    expect(callIssues({ service: 'Button', data: {} }, '/call').map((x) => x.code)).toEqual(['pattern'])
    expect(callIssues({ service: 'a.b', extra: 1 }, '/call').map((x) => x.path)).toEqual(['/call/extra'])
  })

  it('reads the box as you type: text that isn’t JSON stays as typed, and says why', () => {
    expect(readCallData('{"button": "INFO"}')).toEqual({ data: { button: 'INFO' }, error: null })
    expect(readCallData('')).toEqual({ data: {}, error: null })
    expect(readCallData('{"button": ')).toEqual({ data: '{"button": ', error: 'Not valid JSON' })
    expect(readCallData('{"entity_id": "x"}').error).toMatch(/entity_id/)
  })

  it('starts from the required fields with HA’s examples as given, "" without one', () => {
    const svc = { service: 'webostv.command', fields: [{ name: 'command', required: true, example: 'system.launcher/open' }, { name: 'payload', required: false, example: 'x' }, { name: 'id', required: true }] }
    expect(prefill(svc)).toEqual({ command: 'system.launcher/open', id: '' })
    expect(prefill({ service: 'a.b', fields: [{ name: 'list', required: true, example: '[1, 2]' }] })).toEqual({ list: '[1, 2]' }) // never parsed
  })

  it('reads as the service and its data', () => {
    expect(callLabel({ service: 'webostv.button', data: { button: 'INFO' } })).toBe('webostv.button · button: INFO')
    expect(callLabel({ service: 'sonos.play_queue', data: { queue_position: 2, shuffle: true } })).toBe('sonos.play_queue · queue_position: 2, shuffle: true')
    expect(callLabel({ service: 'hue.activate_scene', data: {} })).toBe('hue.activate_scene')
  })
})
