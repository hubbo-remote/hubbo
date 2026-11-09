// Adapts the mock handler to Node's http (standalone server and Vite middleware).
import type { IncomingMessage, ServerResponse } from 'node:http'
import { createMock, type MockOptions } from './handler.ts'

import { API_BASE as API_PREFIX } from '../src/api/constants.ts'

export function optionsFromEnv(env: NodeJS.ProcessEnv = process.env): MockOptions {
  return {
    latencyMs: Number(env.HUBBO_MOCK_LATENCY ?? 120),
    firstRun: env.HUBBO_MOCK_FIRST_RUN === '1',
    requireAuth: env.HUBBO_MOCK_AUTH === '1',
    pairDelayMs: env.HUBBO_MOCK_PAIR_MS === undefined ? undefined : Number(env.HUBBO_MOCK_PAIR_MS),
  }
}

export function mockMiddleware(opts: MockOptions) {
  const mock = createMock(opts)
  return async (req: IncomingMessage, res: ServerResponse, next?: () => void) => {
    const url = new URL(req.url ?? '/', 'http://mock')
    if (!url.pathname.startsWith(API_PREFIX + '/')) {
      if (next) return next()
      res.statusCode = 404
      return res.end()
    }
    const chunks: Buffer[] = []
    for await (const c of req) chunks.push(c as Buffer)
    const raw = Buffer.concat(chunks)
    let body: unknown = undefined
    if (raw.length && (req.headers['content-type'] ?? '').includes('json')) {
      try {
        body = JSON.parse(raw.toString('utf8'))
      } catch {
        res.writeHead(400, { 'content-type': 'application/json' })
        return res.end(JSON.stringify({ error: { code: 'invalid_json', message: 'Body is not valid JSON' } }))
      }
    }
    const out = await mock.handle({
      method: req.method ?? 'GET',
      path: url.pathname.slice(API_PREFIX.length),
      query: url.searchParams,
      headers: req.headers as Record<string, string | undefined>,
      body,
      rawBody: new Uint8Array(raw),
    })
    const headers: Record<string, string> = { 'cache-control': 'no-store', ...out.headers }
    // Bytes (a blocked-raw backup) go out exactly as they are, with the handler's content-type.
    if (out.body instanceof Uint8Array) {
      res.writeHead(out.status, headers)
      return res.end(out.body)
    }
    if (out.body !== undefined) headers['content-type'] = 'application/json'
    res.writeHead(out.status, headers)
    res.end(out.body === undefined ? undefined : JSON.stringify(out.body))
  }
}
