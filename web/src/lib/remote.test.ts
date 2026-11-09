import { describe, expect, it } from 'vitest'
import { checkAddress, formatAddress, parseChannel } from './remote'

describe('remote endpoint input', () => {
  it('channels are decimal 0–124', () => {
    expect(parseChannel('0')).toBe(0)
    expect(parseChannel(' 74 ')).toBe(74)
    expect(parseChannel('124')).toBe(124)
    for (const bad of ['125', '-1', '7.5', '0x10', '', 'abc', '1000']) expect(parseChannel(bad)).toBeNull()
  })
  it('addresses: up to 40 bits of hex, normalized to 10 upper-case digits', () => {
    expect(checkAddress('fc5c97550f')).toEqual({ address: 'FC5C97550F' })
    expect(checkAddress('0xFC5C97550F')).toEqual({ address: 'FC5C97550F' })
    expect(checkAddress('FC:5C:97:55:0F')).toEqual({ address: 'FC5C97550F' })
    expect(checkAddress('1234')).toEqual({ address: '0000001234' })
  })
  it('addresses: the radio set rules', () => {
    for (const bad of ['', '0x', 'FC5C97550F00', 'GC5C97550F', '0', 'ffffffffff', 'FC5C975555', '12345678aa']) expect(checkAddress(bad)).toHaveProperty('error')
  })
  it('formats with colons', () => expect(formatAddress('FC5C97550F')).toBe('FC:5C:97:55:0F'))
})
