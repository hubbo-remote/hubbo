import { describe, expect, it } from 'vitest'
import { deviceModel } from './health'

describe('deviceModel', () => {
  it('says what a profile device is, without repeating the maker', () => {
    expect(deviceModel({ manufacturer: 'Microsoft', model: 'Xbox Series X' })).toBe('Microsoft Xbox Series X')
    expect(deviceModel({ manufacturer: 'Apple', model: 'Apple TV' })).toBe('Apple TV')
    expect(deviceModel({ manufacturer: 'Other', model: 'Generic keyboard' })).toBe('Generic keyboard')
    expect(deviceModel({})).toBe('') // a Home Assistant device
  })
})
