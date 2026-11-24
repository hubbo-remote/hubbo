import { toApiError, errorMessage } from '@/api/client'
import { humanize } from '@/lib/utils'

/** A profile file the user picked: the parsed document (sent as is) and what the dialog shows of it. */
export interface ProfileFile {
  profile: Record<string, unknown>
  manufacturer: string
  model: string
  platform: string
  kind: string | null
}

const str = (v: unknown) => (typeof v === 'string' && v.trim() ? v : null)

/**
 * Read a picked file as a device profile. Only what the dialog needs is checked here (JSON, an
 * object with a manufacturer and model); the hub validates the rest when it's added.
 */
export function readProfileFile(name: string, text: string): ProfileFile | { error: string } {
  let doc: unknown
  try {
    doc = JSON.parse(text)
  } catch {
    return { error: `${name} isn’t a profile file. Pick the .json file you downloaded for the device.` }
  }
  const o = doc && typeof doc === 'object' && !Array.isArray(doc) ? (doc as Record<string, unknown>) : null
  const manufacturer = str(o?.manufacturer)
  const model = str(o?.model)
  if (!o || !manufacturer || !model) return { error: `${name} isn’t a device profile.` }
  return { profile: o, manufacturer, model, platform: str(o.platform) ?? '', kind: str(o.kind) }
}

/** One problem the hub found in the file, in plain words: where (by its JSON Pointer under `/profile`) and what. */
function problem(path: string, code: string, message: string): string {
  const [field, item] = path.split('/').slice(2).map((p) => p.replace(/~1/g, '/').replace(/~0/g, '~'))
  if (field === 'commands' && item) return code === 'invalid_pronto' ? `The code for “${item}” isn’t valid.` : `The “${item}” command: ${message}`
  if (field === 'inputs' && item && /^\d+$/.test(item)) return `Input ${Number(item) + 1}: ${message}`
  return `${field ? humanize(field) : 'The file'}: ${message}`
}

/** The hub's refusal of an imported profile: what's wrong with the file, or the hub's own reason. */
export function profileFileError(e: unknown): string {
  const err = toApiError(e)
  const inFile = err.details.filter((d) => d.path === '/profile' || d.path.startsWith('/profile/'))
  if (!inFile.length) return errorMessage(e, { details: true })
  const lines = inFile.slice(0, 5).map((d) => `· ${problem(d.path, d.code, d.message)}`)
  if (inFile.length > 5) lines.push(`· and ${inFile.length - 5} more`)
  return ['This profile file has a problem, so it wasn’t added.', ...lines].join('\n')
}
