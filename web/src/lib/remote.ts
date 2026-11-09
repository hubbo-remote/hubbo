import { REMOTE_CHANNEL_MAX } from '../api/constants.ts'

/** Channel 0–124, decimal, like the console's `radio set`. */
export function parseChannel(s: string): number | null {
  const t = s.trim()
  if (!/^\d{1,3}$/.test(t)) return null
  const n = Number(t)
  return n <= REMOTE_CHANNEL_MAX ? n : null
}

/**
 * A Hub address (RemoteEndpointUpdate): up to 40 bits of hex, `0x` optional; `:`, `-` or
 * spaces between bytes are allowed for convenience (`FC:5C:97:55:0F`). Same rules as the
 * console's `radio set`: not all zeros or all ones, and the low byte isn't `55` or `AA`.
 * Returns the address as 10 upper-case digits, or why it's invalid.
 */
export function checkAddress(s: string): { address: string } | { error: string } {
  const t = s.trim().replace(/^0x/i, '').replace(/[\s:-]/g, '')
  if (!/^[0-9a-f]{1,10}$/i.test(t)) return { error: 'Up to 10 hex digits (5 bytes), e.g. FC5C97550F or FC:5C:97:55:0F' }
  const a = t.toUpperCase().padStart(10, '0')
  const error = addressProblem(a)
  return error ? { error } : { address: a }
}

/** Why a 10-digit upper-case address can't be used (the console's `radio set` rules), or null. */
export function addressProblem(a: string): string | null {
  if (a === '0000000000' || a === 'FFFFFFFFFF') return 'An address can’t be all zeros or all ones'
  if (a.endsWith('55') || a.endsWith('AA')) return 'An address can’t end in 55 or AA (the radio can’t lock onto it)'
  return null
}

/** `FC5C97550F` → `FC:5C:97:55:0F`. */
export const formatAddress = (a: string) => a.match(/../g)?.join(':') ?? a
