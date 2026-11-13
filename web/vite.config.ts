/// <reference types="vitest/config" />
import path from 'node:path'
import { defineConfig, type Plugin, type ProxyOptions } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { mockMiddleware, optionsFromEnv } from './mock/node.ts'

// HUBBO_API_TARGET picks where /api goes in dev:
//   mock (default)            in-process TypeScript mock (mock/)
//   http://127.0.0.1:8780     hub-sim (the real C++ core)
//   http://hubbo.local        a board on your LAN
const target = process.env.HUBBO_API_TARGET || 'mock'

// The hub rejects a foreign Origin on unsafe requests (403 csrf_failed). Served from the hub the UI
// is same-origin; through this dev proxy it isn't, so send the target's Host and drop Origin (the hub
// accepts requests without one).
const toHub: ProxyOptions = {
  target,
  changeOrigin: true,
  secure: false,
  configure: (proxy) => proxy.on('proxyReq', (req) => req.removeHeader('origin')),
}

function hubboMock(): Plugin {
  return {
    name: 'hubbo-mock-api',
    configureServer(server) {
      server.middlewares.use(mockMiddleware(optionsFromEnv()))
    },
  }
}

export default defineConfig({
  plugins: [react(), tailwindcss(), ...(target === 'mock' ? [hubboMock()] : [])],
  resolve: { alias: { '@': path.resolve(import.meta.dirname, 'src') } },
  // e2e on the mock and on hub-sim may run at once: each Vite keeps its own dependency cache.
  cacheDir: process.env.HUBBO_VITE_CACHE_DIR || undefined,
  server: {
    host: process.env.HUBBO_WEB_HOST || '0.0.0.0',
    port: Number(process.env.HUBBO_WEB_PORT || 5180),
    strictPort: true,
    // The API, and /llms.txt beside it (the agents' entry point), go to the hub.
    proxy: target === 'mock' ? undefined : Object.fromEntries(['/api', '/llms.txt'].map((path) => [path, toHub])),
  },
  build: {
    target: 'es2022',
    cssCodeSplit: true,
    reportCompressedSize: false,
    modulePreload: { polyfill: false },
    rollupOptions: {
      output: {
        // Short hashed names: they become LittleFS paths on the board.
        entryFileNames: 'a/[name]-[hash:8].js',
        chunkFileNames: 'a/[name]-[hash:8].js',
        assetFileNames: 'a/[name]-[hash:8][extname]',
        // Fold tiny chunks (single icons) into their importers: fewer flash files and requests.
        experimentalMinChunkSize: 4096,
      },
    },
  },
  test: {
    // Node by default (mock, lib); component tests opt into jsdom per file.
    environment: 'node',
    include: ['src/**/*.test.{ts,tsx}', 'mock/**/*.test.ts'],
    setupFiles: ['src/test-setup.ts'],
  },
})
