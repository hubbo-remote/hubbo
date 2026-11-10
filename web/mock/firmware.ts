// Fake Hubbo firmware images in the real ESP app .bin layout (core
// tests/firmware_image_util.h image()), and the mock's small subset of the
// hub's checks (core validateFirmwareImage). Used by the mock, its unit tests and e2e.
import { readMarker } from '../src/lib/firmware.ts'

const sha256 = async (b: Uint8Array) => new Uint8Array(await crypto.subtle.digest('SHA-256', b as Uint8Array<ArrayBuffer>))

export interface ImageSpec {
  board: string
  version?: string
  badHash?: boolean
}

/** Header, segment header, app desc magic, the marker, padding, then the appended SHA-256. */
export async function buildFirmwareImage({ board, version = 'v9.9.9-e2e', badHash }: ImageSpec): Promise<Uint8Array> {
  const marker = `HUBBO-IMAGE/1 board=${board} layout=1 version=${version}\0`
  const body = new Uint8Array(Math.max(512, 32 + 256 + marker.length))
  body[0] = 0xe9
  body[12] = 9 // ESP32-S3
  body[23] = 1 // hash appended
  body.set([0x32, 0x54, 0xcd, 0xab], 32) // app-desc magic
  body.set(new TextEncoder().encode(marker), 32 + 256)
  const digest = await sha256(body)
  if (badHash) digest[0]! ^= 1
  const out = new Uint8Array(body.length + 32)
  out.set(body)
  out.set(digest, body.length)
  return out
}

const INVALID = "This isn't a Hubbo update file"

/** Like the hub: the first failed check's 422, or null for an installable image (the version it carries). */
export async function checkFirmwareImage(bytes: Uint8Array, board: string): Promise<{ code: 'invalid_image' | 'wrong_board'; message: string } | { version: string }> {
  if (bytes.length < 68 || bytes[0] !== 0xe9) return { code: 'invalid_image', message: INVALID }
  const marker = readMarker(bytes)
  if (!marker) return { code: 'invalid_image', message: INVALID }
  if (marker.board !== board) return { code: 'wrong_board', message: 'This update is for a different device' }
  const digest = await sha256(bytes.subarray(0, bytes.length - 32))
  if (bytes[23] !== 1 || digest.some((b, i) => b !== bytes[bytes.length - 32 + i])) return { code: 'invalid_image', message: 'This update file is damaged; download it again' }
  return { version: marker.version }
}
