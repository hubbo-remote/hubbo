// Standalone mock: `pnpm mock` serves /api/v1 on HUBBO_MOCK_PORT (default 8790).
// The Vite dev server mounts the same middleware itself, so you only need
// this to poke the mock with curl or point another tool at it.
import { createServer } from 'node:http'
import { mockMiddleware, optionsFromEnv } from './node.ts'

const port = Number(process.env.HUBBO_MOCK_PORT ?? 8790)
const host = process.env.HUBBO_MOCK_HOST ?? '127.0.0.1'
createServer(mockMiddleware(optionsFromEnv())).listen(port, host, () => {
  console.log(`hubbo mock API on http://${host}:${port}/api/v1`)
})
