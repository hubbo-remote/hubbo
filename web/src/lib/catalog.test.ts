import { readFileSync } from 'node:fs'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { brands, CATALOG_URL, CatalogError, modelKeys, models, normalize, profilePath, profileText, search, searchModels } from './catalog'

/** fetch, served from the fixture catalog (web/mock/catalog/); `override` answers some paths itself. */
function serveFixtures(override: Record<string, Response | 'offline'> = {}) {
  vi.stubGlobal('fetch', async (url: string) => {
    const path = url.slice(CATALOG_URL.length)
    const o = override[path]
    if (o === 'offline') throw new TypeError('Failed to fetch')
    if (o) return o
    try {
      return new Response(readFileSync(new URL(`../../mock/catalog/${path}`, import.meta.url), 'utf8'))
    } catch {
      return new Response('', { status: 404 })
    }
  })
}
afterEach(() => vi.unstubAllGlobals())

describe('the catalog', () => {
  it('finds a profile’s path from its id', () => {
    expect(profilePath('hubbo:apple-tv')).toBe('profiles/hubbo/apple-tv.json')
    expect(profilePath('harmony:498661')).toBe('profiles/harmony/498/498661.json')
    expect(profilePath('harmony:42')).toBe('profiles/harmony/000/42.json')
    expect(profilePath('harmony:abc')).toBeNull()
    expect(profilePath('../x:y')).toBeNull()
  })

  it('matches normalized names, prefix first, then substring', () => {
    expect(normalize('Bang & Olufsen')).toBe('bangolufsen')
    expect(normalize('Löewe')).toBe('loewe')
    const items = ['Sony', 'Samsung', 'Harman Kardon', 'Sonos'].map((n) => ({ n, key: normalize(n) }))
    expect(search(items, 'so').map((i) => i.n)).toEqual(['Sony', 'Sonos'])
    expect(search(items, 'S A').map((i) => i.n)).toEqual(['Samsung'])
    expect(search(items, 'ar').map((i) => i.n)).toEqual(['Harman Kardon'])
    expect(search(items, ' ')).toBe(items)
  })

  it('matches models only from the start of the model or of one of its words', () => {
    expect(modelKeys('KD-85X85J')).toEqual(['kd85x85j'])
    expect(modelKeys('KDF60 XBR600')).toEqual(['kdf60xbr600', 'xbr600'])
    const items = ['KD-85X85J', 'XBR-55X850', 'KDF60 XBR600', 'Bravia KD-85'].map((m) => ({ m, keys: modelKeys(m) }))
    const find = (q: string) => searchModels(items, q).map((i) => i.m)
    expect(find('kd85')).toEqual(['KD-85X85J', 'Bravia KD-85'])
    expect(find('KD-85')).toEqual(['KD-85X85J', 'Bravia KD-85'])
    expect(find('85')).toEqual([]) // words split on spaces only: never after a dash, never mid-token
    expect(find('850')).toEqual([])
    expect(find('xbr')).toEqual(['XBR-55X850', 'KDF60 XBR600'])
    expect(find('bravia kd')).toEqual(['Bravia KD-85'])
    expect(find('kd58')).toEqual([]) // no typo tolerance
    expect(searchModels(items, ' - ')).toBe(items)
  })

  it('lists manufacturers alphabetically, and a manufacturer’s models in order', async () => {
    serveFixtures()
    expect((await brands()).map((b) => b.name)).toEqual(['Denon', 'Google', 'Pure Digital', 'Samsung', 'Sonos', 'Sony'])
    expect((await models('samsung')).map((m) => m.model)).toEqual(['HW-Q90R', 'Q-Series', 'QN55', 'QN100'])
  })

  it('joins a profile with its codeset on text, keeping the command order', async () => {
    serveFixtures()
    const text = await profileText('harmony:275862')
    expect(text).not.toContain('"codeset": "9aa0916c4acf136c"')
    expect(text).toContain('"codeset": "59ca342d80980065"') // meta's own stays
    // The codeset's order, "5" last: a parsed object would have put the digits first.
    const commands = text.slice(text.indexOf('\n  "commands": '), text.indexOf('\n  }', text.indexOf('\n  "commands": ')))
    const names = [...commands.matchAll(/^ {4}"([^"]+)": \{/gm)].map((m) => m[1])
    expect(names).toEqual(['1', '2', '3', '4', 'DirectionLeft', 'DirectionRight', 'PowerToggle', 'Select', 'VolumeDown', 'VolumeUp', '5'])
    expect(Object.keys(JSON.parse(text).commands)).toHaveLength(11)
    expect(await profileText('hubbo:android-tv')).toBe(readFileSync(new URL('../../mock/catalog/profiles/hubbo/android-tv.json', import.meta.url), 'utf8'))
  })

  it('says why a profile can’t be had', async () => {
    const reason = (id: string) => profileText(id).then(() => 'ok', (e: CatalogError) => e.reason)
    serveFixtures({
      'profiles/hubbo/fire-tv.json': new Response('{"schema_version": 2, "model": "x"}'),
      'profiles/hubbo/apple-tv.json': 'offline',
      'profiles/hubbo/portal.json': new Response('<html>Sign in to the Wi-Fi</html>'),
      'profiles/hubbo/null.json': new Response('null'),
    })
    expect(await reason('hubbo:fire-tv')).toBe('newer')
    expect(await reason('hubbo:apple-tv')).toBe('unreachable')
    expect(await reason('hubbo:portal')).toBe('unreachable')
    expect(await reason('hubbo:null')).toBe('unreachable')
    expect(await reason('hubbo:nothing-here')).toBe('missing')
  })
})
