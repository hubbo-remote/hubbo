// Hubbo firmware images (docs/design/phase5-appliance.md §2). The hub is the
// authority on whether an image is installable; this only reads the marker
// every Hubbo image carries, to show which version a chosen file holds.

/** `HUBBO-IMAGE/1 board=<board> layout=<n> version=<v>`, NUL-terminated (core firmware_update_port.h). */
const MARKER = /HUBBO-IMAGE\/1 board=([\x21-\x7e]+) layout=\d{1,4} version=([\x21-\x7e]{1,64})\0/

export interface ImageMarker {
  board: string
  version: string
}

/** The first well-formed marker in an image, or null (not a Hubbo image). */
export function readMarker(bytes: Uint8Array): ImageMarker | null {
  // latin1 keeps one char per byte, so the ASCII marker matches as-is.
  const m = MARKER.exec(new TextDecoder('latin1').decode(bytes))
  return m ? { board: m[1]!, version: m[2]! } : null
}

/** Where Hubbo's releases are published. */
const RELEASES_URL = 'https://github.com/hubbo-remote/hubbo/releases'

/** A release's notes on GitHub (its tag is `v<version>`). */
export const releaseNotesUrl = (version: string) => `${RELEASES_URL}/tag/v${version}`
