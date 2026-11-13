// Adapts the mock handler to Node's http (standalone server and Vite middleware).
import type { IncomingMessage, ServerResponse } from 'node:http'
import { llmsTxt } from './docs.ts'
import { createMock, unknownPath, type MockOptions, type MockResponse } from './handler.ts'

import { API_BASE as API_PREFIX } from '../src/api/constants.ts'

export function optionsFromEnv(env: NodeJS.ProcessEnv = process.env): MockOptions {
  return {
    latencyMs: Number(env.HUBBO_MOCK_LATENCY ?? 120),
    firstRun: env.HUBBO_MOCK_FIRST_RUN === '1',
    requireAuth: env.HUBBO_MOCK_AUTH === '1',
    pairDelayMs: env.HUBBO_MOCK_PAIR_MS === undefined ? undefined : Number(env.HUBBO_MOCK_PAIR_MS),
    discoverDelayMs: env.HUBBO_MOCK_DISCOVER_MS === undefined ? undefined : Number(env.HUBBO_MOCK_DISCOVER_MS),
    timeScale: env.HUBBO_MOCK_TIME_SCALE === undefined ? undefined : Number(env.HUBBO_MOCK_TIME_SCALE),
  }
}

/** e2e runs specs in parallel: each Playwright worker sends this header and gets a hub of its own. */
export const MOCK_HUB_HEADER = 'x-hubbo-mock-hub'

export function mockMiddleware(opts: MockOptions) {
  const hubs = new Map<string, ReturnType<typeof createMock>>()
  const hubFor = (key: string) => hubs.get(key) ?? hubs.set(key, createMock(opts)).get(key)!
  return async (req: IncomingMessage, res: ServerResponse, next?: () => void) => {
    const url = new URL(req.url ?? '/', 'http://mock')
    // The llms.txt convention's place, outside the API (like core's host route).
    if (url.pathname === '/llms.txt' && req.method === 'GET') return send(res, llmsTxt())
    if (!/^\/api(\/|$)/.test(url.pathname)) {
      if (next) return next()
      res.statusCode = 404
      return res.end()
    }
    // Under /api but not this version's API (/api, /api/v2): like core, a 404 pointing at the guide.
    if (url.pathname !== API_PREFIX && !url.pathname.startsWith(API_PREFIX + '/')) return send(res, unknownPath(`${req.method} ${url.pathname}`))
    const chunks: Buffer[] = []
    for await (const c of req) chunks.push(c as Buffer)
    const raw = Buffer.concat(chunks)
    const hub = req.headers[MOCK_HUB_HEADER]
    const out = await hubFor(typeof hub === 'string' ? hub : '').handle({
      method: req.method ?? 'GET',
      path: url.pathname.slice(API_PREFIX.length),
      query: url.searchParams,
      headers: req.headers as Record<string, string | undefined>,
      body: undefined, // parsed by the handler, after its size and content-type checks
      rawBody: new Uint8Array(raw),
    })
    if (out.status === 0) return void req.socket.destroy() // the hub is rebooting
    send(res, out)
  }
}

/** Bytes (a blocked-raw backup, the agent docs) go out exactly as they are, with the handler's headers. */
function send(res: ServerResponse, out: MockResponse) {
  const headers: Record<string, string> = { 'cache-control': 'no-store', ...out.headers }
  if (out.body instanceof Uint8Array) {
    res.writeHead(out.status, headers)
    return res.end(out.body)
  }
  if (out.body !== undefined) headers['content-type'] = 'application/json'
  res.writeHead(out.status, headers)
  res.end(out.body === undefined ? undefined : JSON.stringify(out.body))
}
