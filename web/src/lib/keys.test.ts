import { describe, expect, it } from 'vitest'
import { keyLabel } from './keys.ts'

describe('keyLabel', () => {
  it('names keys for people', () => {
    expect(keyLabel('MENU_ESCAPE')).toBe('Exit')
    expect(keyLabel('KEYPAD_ENTER')).toBe('Enter')
    expect(keyLabel('ARROW_UP')).toBe('Arrow up')
    expect(keyLabel('KEY_A')).toBe('A')
  })
})
