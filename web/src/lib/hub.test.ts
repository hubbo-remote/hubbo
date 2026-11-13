import { describe, expect, it } from 'vitest'
import { hubNameProblem, trimHubName } from './hub'

describe('hub name', () => {
  it('trims spaces like the hub (only spaces), then checks 1–32 characters', () => {
    expect(trimHubName('  Den  ')).toBe('Den')
    expect(trimHubName('\tDen')).toBe('\tDen') // a control character: the hub refuses it, not us
    expect(hubNameProblem('  Den  ')).toBeNull()
    expect(hubNameProblem('   ')).toBe('Name is required')
    expect(hubNameProblem(` ${'x'.repeat(32)} `)).toBeNull()
    expect(hubNameProblem('x'.repeat(33))).toBe('At most 32 characters')
    expect(hubNameProblem('🎮'.repeat(32))).toBeNull() // characters, not UTF-16 units
  })
})
