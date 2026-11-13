// What an agent finds on the hub with no prior knowledge, like core's docs routes: the API index,
// the spec as JSON, the agent guide, and /llms.txt. Read from the repo (once), so they can't drift
// from api/openapi.yaml, docs/agent-api.md and docs/llms.txt.
import { createHash } from 'node:crypto'
import { gzipSync } from 'node:zlib'
import { CORE_SCHEMA, load } from 'js-yaml'
import { API_BASE } from '../src/api/constants.ts'
import type { MockRequest, MockResponse } from './handler.ts'
import { fixtureBytes } from './fixtures.ts'

/** Where every docs hint points (error.docs on an unauthenticated 401, and on a 404 for a path the API doesn't have). */
export const GUIDE_PATH = `${API_BASE}/agent.md`
const MARKDOWN = 'text/markdown; charset=utf-8'


interface Doc {
  raw: Uint8Array
  gzip: Uint8Array
  etag: string
  contentType: string
}

const doc = (raw: Uint8Array, contentType: string): Doc => ({
  raw,
  gzip: new Uint8Array(gzipSync(raw, { level: 9 })),
  etag: `"${createHash('sha256').update(raw).digest('hex').slice(0, 16)}"`, // like core's embed_docs.py
  contentType,
})

let docs: { openapi: Doc; guide: Doc; llms: Uint8Array } | undefined
/**
 * The served documents, from the repo copies (api/ and docs/ sit beside the fixtures). Built on
 * first use and shared: every mock (the unit tests make dozens) serves the same files.
 */
export function loadDocs() {
  return (docs ??= {
    openapi: doc(new TextEncoder().encode(JSON.stringify(load(new TextDecoder().decode(fixtureBytes('../openapi.yaml')), { schema: CORE_SCHEMA }))), 'application/json'),
    guide: doc(fixtureBytes('../../docs/agent-api.md'), MARKDOWN),
    llms: fixtureBytes('../../docs/llms.txt'),
  })
}

/** A document like core's sendDoc: gzipped only when asked, always `Vary`, 304 on a current ETag. */
export function sendDoc(req: MockRequest, d: Doc): MockResponse {
  const headers: Record<string, string> = { 'content-type': d.contentType, vary: 'Accept-Encoding', 'cache-control': 'no-cache', etag: d.etag }
  if (req.headers['if-none-match'] === d.etag) return { status: 304, headers }
  const gzip = (req.headers['accept-encoding'] ?? '').includes('gzip')
  if (gzip) headers['content-encoding'] = 'gzip'
  return { status: 200, headers, body: gzip ? d.gzip : d.raw }
}

/** GET /llms.txt (outside /api/v1, so the Node adapter serves it): docs/llms.txt, exact bytes, like the guide. */
export const llmsTxt = (): MockResponse => ({ status: 200, headers: { 'content-type': MARKDOWN }, body: loadDocs().llms })
