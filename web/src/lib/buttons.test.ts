import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { describe, expect, it } from 'vitest'
import { KEYS, neighbour } from '@/components/remote/layout'
import { fixture } from '../../mock/fixtures.ts'
import { BUTTONS } from './buttons'

// Contract with the core: button names and order come from button.cpp.
const cpp = readFileSync(resolve(__dirname, '../../../core/src/remote/button.cpp'), 'utf8')
const coreIds = [...cpp.matchAll(/\{Button::\w+, kGroup(\w+), 0x[0-9A-Fa-f]+, "([a-z0-9_]+)"\}/g)].map((m) => ({ id: m[2]!, group: m[1] === 'Keypad' ? 'keypad' : 'media_home' }))

describe('button catalog', () => {
  it('matches core/src/remote/button.cpp exactly (names, order, group)', () => {
    expect(coreIds).toHaveLength(48)
    expect(BUTTONS.map((b) => ({ id: b.id, group: b.group }))).toEqual(coreIds)
  })

  it('matches GET /remote/buttons in api/fixtures (names, order, group, label)', () => {
    const api = fixture<{ buttons: { name: string; group: string; label?: string }[] }>('remote/buttons.json').buttons
    expect(BUTTONS.map((b) => b.id)).toEqual(api.map((b) => b.name))
    expect(BUTTONS.map((b) => b.group)).toEqual(api.map((b) => b.group))
  })

  it('draws every button exactly once on the remote', () => {
    const drawn = KEYS.map((k) => k.id).sort()
    expect(drawn).toEqual(BUTTONS.map((b) => b.id).sort())
  })

  it('arrow navigation reaches every key from select', () => {
    const seen = new Set(['select'])
    const queue = ['select']
    while (queue.length) {
      const id = queue.shift()!
      for (const d of ['up', 'down', 'left', 'right'] as const) {
        const n = neighbour(id, d)
        if (n && !seen.has(n)) {
          seen.add(n)
          queue.push(n)
        }
      }
    }
    expect(seen.size).toBe(48)
  })

  it('moves spatially on the d-pad', () => {
    expect(neighbour('select', 'up')).toBe('up')
    expect(neighbour('select', 'left')).toBe('left')
    expect(neighbour('number_5', 'down')).toBe('number_8')
  })
})
