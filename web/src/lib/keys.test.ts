import { describe, expect, it } from 'vitest'
import { groupKeys, keyGroup, keyLabel, paramValueLabel } from './keys.ts'
import { sentenceCase, titleCase } from './utils.ts'

describe('keyLabel', () => {
  it('names keys for people', () => {
    expect(keyLabel('MENU_ESCAPE')).toBe('Exit')
    expect(keyLabel('KEYPAD_ENTER')).toBe('Enter')
    expect(keyLabel('ARROW_UP')).toBe('Arrow up')
    expect(keyLabel('KEY_A')).toBe('A')
  })
})

describe('key sections and option labels', () => {
  it('sorts keys into sections, in section order, each keeping the list’s order', () => {
    expect(keyGroup('ARROW_UP')).toBe('Navigation')
    expect(keyGroup('KEY_Q')).toBe('Letters')
    expect(keyGroup('KEYPAD_7')).toBe('Numbers')
    expect(keyGroup('F12')).toBe('Function keys')
    expect(keyGroup('PLAY_PAUSE')).toBe('Media')
    expect(keyGroup('COMMA')).toBe('Symbols')
    expect(keyGroup('POWER')).toBe('Other')
    expect(groupKeys(['PLAY_PAUSE', 'KEY_B', 'ARROW_UP', 'KEY_A', 'MUTE'])).toEqual([
      ['Navigation', ['ARROW_UP']],
      ['Letters', ['KEY_B', 'KEY_A']],
      ['Media', ['PLAY_PAUSE', 'MUTE']],
    ])
  })

  it('labels keys by name and other options in title case, keeping capitals and digits', () => {
    expect(paramValueLabel('key', 'AC_HOME')).toBe(keyLabel('AC_HOME'))
    expect(paramValueLabel('sound_output', 'external_arc')).toBe('External Arc')
    expect(titleCase('tv_speaker')).toBe('Tv Speaker')
    expect(titleCase('HDMI 1')).toBe('HDMI 1')
    expect(titleCase('eARC')).toBe('eARC')
  })

  it('puts Title Case labels in sentence case, and leaves the rest as written', () => {
    expect(sentenceCase('Sleep Time')).toBe('Sleep time')
    expect(sentenceCase('Join Sonos group')).toBe('Join Sonos group') // not Title Case: as written
    expect(sentenceCase('Set HEV Cycle State')).toBe('Set HEV Cycle State')
    expect(sentenceCase('LG eARC DTS:X Output')).toBe('LG eARC DTS:X Output')
  })
})
