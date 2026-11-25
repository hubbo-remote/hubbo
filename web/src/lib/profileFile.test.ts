import { describe, expect, it } from 'vitest'
import { ApiError } from '@/api/client'
import type { ApiErrorBody } from '@/api/types'
import { profileFileError, readProfileFile } from './profileFile'

const refusal = (details: { path: string; code: string; message: string }[]) =>
  new ApiError(422, { error: { code: 'validation_failed', message: 'Invalid profile', details } } as ApiErrorBody, 'x')

describe('profile files', () => {
  it('reads a profile: what the dialog shows, and the text as is', () => {
    const text = JSON.stringify({ manufacturer: 'Test', model: 'NEC TV', platform: 'ir', kind: 'tv', commands: {} })
    expect(readProfileFile('tv.json', text)).toEqual({ text, manufacturer: 'Test', model: 'NEC TV', platform: 'ir', kind: 'tv' })
  })

  it('says so for a file that isn’t JSON, or isn’t a profile', () => {
    expect(readProfileFile('photo.jpg', '\xff\xd8')).toEqual({ error: expect.stringContaining('photo.jpg isn’t a profile file') })
    expect(readProfileFile('list.json', '[1, 2]')).toEqual({ error: 'list.json isn’t a device profile.' })
    expect(readProfileFile('x.json', '{"model": "TV"}')).toEqual({ error: 'x.json isn’t a device profile.' })
  })

  it('names what’s wrong in the file, in plain words', () => {
    const msg = profileFileError(
      refusal([
        { path: '/profile/commands/VolumeUp/pronto', code: 'invalid_pronto', message: 'zero duration' },
        { path: '/profile/power', code: 'required', message: 'type is required' },
        { path: '/profile/inputs/1/name', code: 'too_long', message: 'At most 32 characters' },
      ]),
    )
    expect(msg).toBe(
      ['This profile file has a problem, so it wasn’t added.', '· The code for “VolumeUp” isn’t valid.', '· Power: type is required', '· Input 2: At most 32 characters'].join('\n'),
    )
  })

  it('shows the hub’s own reason for a refusal that isn’t about the file', () => {
    expect(profileFileError(refusal([{ path: '/integration_id', code: 'too_many_devices', message: 'At most 4 Bluetooth devices' }]))).toBe('Invalid profile\n· At most 4 Bluetooth devices')
  })
})
