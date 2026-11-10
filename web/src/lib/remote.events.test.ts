import { describe, expect, it } from 'vitest'
import { isNewEvent } from './remote.ts'

describe('isNewEvent', () => {
  const ev = (button: string, kind: 'down' | 'up' | 'tap' | 'hold', age_ms: number) => ({ button, kind, age_ms }) as never
  it('an event that only aged is the same one', () => expect(isNewEvent(ev('up', 'tap', 1000), ev('up', 'tap', 1500))).toBe(false))
  it('a younger event is a new one, even the same button', () => expect(isNewEvent(ev('up', 'tap', 1500), ev('up', 'tap', 20))).toBe(true))
  it('a different button or kind is a new one (a hub clock that stands still)', () => {
    expect(isNewEvent(ev('up', 'tap', 0), ev('down', 'tap', 0))).toBe(true)
    expect(isNewEvent(ev('up', 'down', 0), ev('up', 'up', 0))).toBe(true)
  })
  it('the first event ever is new; no event is not', () => {
    expect(isNewEvent(null, ev('up', 'tap', 0))).toBe(true)
    expect(isNewEvent(ev('up', 'tap', 0), null)).toBe(false)
  })
})
