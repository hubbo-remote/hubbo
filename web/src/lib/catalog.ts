// The online device catalog (hubbo-profiles' tree: index/, profiles/, codesets/), read straight from
// the browser: the hub makes no request of its own. Same rules as the catalog site's catalog.js:
// short index keys, paths from ids, and the codeset join done on text.

/** Where the catalog lives. Dev and e2e read the fixture catalog in web/mock/catalog/ (.env.development). */
// The firmware's CSP (firmware/scripts/web_assets.py, connect-src) must list this same origin.
export const CATALOG_URL: string = import.meta.env.VITE_HUBBO_CATALOG_URL ?? 'https://catalog.hubbo.tv/'

export interface Brand {
  name: string
  slug: string
  count: number
  /** Devices per kind. */
  kinds: Record<string, number>
  key: string
}
export interface CatalogModel {
  id: string
  model: string
  kind: string
  platform: string
  key: string
}

/** Why the catalog couldn't give a profile: it can't be reached, doesn't have it, or it's in a newer format. */
export class CatalogError extends Error {
  constructor(readonly reason: 'unreachable' | 'missing' | 'newer') {
    super(
      reason === 'newer'
        ? 'This device’s profile uses a newer format than this hub understands. Update Hubbo, then try again.'
        : reason === 'missing'
          ? 'The catalog doesn’t have this device’s profile right now.'
          : 'The device catalog can’t be reached. Check your internet connection and try again, or import a profile file.',
    )
  }
}

async function getText(path: string): Promise<string> {
  const res = await fetch(CATALOG_URL + path).catch(() => {
    throw new CatalogError('unreachable')
  })
  if (!res.ok) throw new CatalogError(res.status === 404 ? 'missing' : 'unreachable')
  return res.text()
}

/**
 * A catalog file as JSON, of the shape `ok` checks. Anything else (a captive portal's 200 HTML
 * page, a null) means the catalog wasn't what answered.
 */
function parse<T>(text: string, ok: (v: unknown) => boolean): T {
  let v: unknown
  try {
    v = JSON.parse(text)
  } catch {
    throw new CatalogError('unreachable')
  }
  if (!ok(v)) throw new CatalogError('unreachable')
  return v as T
}
const isObject = (v: unknown) => !!v && typeof v === 'object' && !Array.isArray(v)

/** Every manufacturer, in the index's order (alphabetical). */
export async function brands(): Promise<Brand[]> {
  const rows = parse<{ n: string; s: string; c: number; k: Record<string, number> }[]>(await getText('index/manufacturers.json'), Array.isArray)
  return rows.map((r) => ({ name: r.n, slug: r.s, count: r.c, kinds: r.k, key: normalize(r.n) }))
}

/** One manufacturer's devices, in model order (numbers in order: "QN55" before "QN100"). */
export async function models(slug: string): Promise<CatalogModel[]> {
  const rows = parse<{ id: string; m: string; k: string; p: string }[]>(await getText(`index/m/${encodeURIComponent(slug)}.json`), Array.isArray)
  const order = new Intl.Collator(undefined, { numeric: true })
  return rows.map((r) => ({ id: r.id, model: r.m, kind: r.k, platform: r.p, key: normalize(r.m) })).sort((a, b) => order.compare(a.model, b.model))
}

/**
 * A profile's path from its id: "hubbo:apple-tv" → profiles/hubbo/apple-tv.json; "harmony:498661"
 * → profiles/harmony/498/498661.json (the number without its last three digits, three wide).
 */
export function profilePath(id: string): string | null {
  const [source, slug, extra] = id.split(':')
  if (extra !== undefined || !/^[a-z]+$/.test(source ?? '') || !/^[a-z0-9.-]{1,48}$/.test(slug ?? '')) return null
  if (source !== 'harmony') return `profiles/${source}/${slug}.json`
  if (!/^\d+$/.test(slug!)) return null
  return `profiles/harmony/${String(Math.floor(Number(slug) / 1000)).padStart(3, '0')}/${slug}.json`
}

/** The profile format versions this web admin can install (the spec's "Versioning"). */
const SCHEMA_VERSIONS = [1]

/**
 * The complete profile's text, as the hub takes it. A converted device names a shared codeset, whose
 * commands go back in its place. Joined on text, like the site: parsing would move "0".."9" first.
 */
export async function profileText(id: string): Promise<string> {
  const path = profilePath(id)
  if (!path) throw new CatalogError('missing')
  const text = await getText(path)
  const stored = parse<{ schema_version?: unknown; codeset?: unknown }>(text, isObject)
  if (!SCHEMA_VERSIONS.includes(stored.schema_version as number)) throw new CatalogError('newer')
  if (!('codeset' in stored)) return text
  // Stored files are indented by two, so the top-level key is the one indented by two (meta can hold its own "codeset").
  const key = `\n  "codeset": ${JSON.stringify(stored.codeset)}`
  if (typeof stored.codeset !== 'string' || !/^[a-z0-9]{2,64}$/.test(stored.codeset) || !text.includes(key)) throw new CatalogError('missing')
  const set = (await getText(`codesets/${stored.codeset.slice(0, 2)}/${stored.codeset}.json`)).trim()
  parse(set, (v) => isObject(v) && isObject((v as { commands?: unknown }).commands))
  const commands = set.slice(1, -1).trim().replace(/^"commands"\s*:\s*/, '')
  return text.replace(key, () => `\n  "commands": ${commands}`) // a function: "$" in the text stays as is
}

/** Lowercase, no accents, letters and digits only: "Bang & Olufsen" → "bangolufsen". */
export function normalize(text: string): string {
  return text
    .normalize('NFKD')
    .replace(/[̀-ͯ]/g, '')
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '')
}

/** The items whose key starts with the query, then those that contain it, each in their own order. */
export function search<T extends { key: string }>(items: T[], query: string): T[] {
  const q = normalize(query)
  if (!q) return items
  const starts: T[] = []
  const contains: T[] = []
  for (const i of items) {
    const at = i.key.indexOf(q)
    if (at === 0) starts.push(i)
    else if (at > 0) contains.push(i)
  }
  return [...starts, ...contains]
}
