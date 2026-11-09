# hubbo-frontend: working notes

Current state, not a log. Update when a piece of work lands.

## State of the web admin (end of Phase 3 UI, /simplify done, not committed yet)

- React 19 + TS + Vite + Tailwind, shadcn-style components in `web/src/components/ui`.
  Routing via wouter, hash URLs. Our own tiny query cache in `src/api/hooks.ts`
  (`useQuery`, `invalidate`, `setQueryData`, optional `pollMs`).
- Every request goes through `src/api/client.ts`. Types are aliases over the generated
  `src/api/schema.gen.ts` (`src/api/types.ts`). After any `api/openapi.yaml` change run
  `bin/hubbo web gen:api` and fix the type fallout.
- Done:
  - Phase 2: backup/restore (opaque blob, `X-Hubbo-Backup` config | blocked-raw, 409/503 messages,
    unsupported_schema), blocked/storage banners, conflict dialogs.
  - Phase 3:
    - HA settings with a live connection line (all IntegrationState values); token write-only;
      a URL change warns that it clears the token.
    - Async discovery (`HaCandidates.tsx`: start, poll, cancel, search), with a preview of the
      candidate's command ids (built-in vs integration services, skipped services, dropped
      options).
    - Register; stale banner, Re-link and "Refresh from Home Assistant" on the device page.
    - Backup while storage is down: storage failing after a good load still exports the running
      config (200, the last acknowledged save) and the page says so; only a load that couldn't read
      storage gets 503 (core decision, 2026-11-09).
    - Remote tab: endpoint view and manual entry (`lib/remote.ts` validation = console
      `radio set`), "Scan from Hub" with a 120 s progress bar, cancel, and resuming after a reload.
- Bundle: ~150 KB gzipped (`bin/hubbo web build` prints sizes).

## Modes

- **Mock** (default): `web/mock` runs inside Vite (`mock/node.ts` middleware) and is seeded from
  `api/fixtures`. It's the spec in TypeScript, so keep it faithful; its contract tests live in
  `mock/handler.test.ts` (every request fixture must be accepted).
  - Env: `HUBBO_MOCK_LATENCY`, `HUBBO_MOCK_FIRST_RUN=1`, `HUBBO_MOCK_AUTH=1`, `HUBBO_MOCK_PAIR_MS`,
    `HUBBO_MOCK_DISCOVER_MS` (e2e sets both to 0).
- **hub-sim**: `HUBBO_API_TARGET=http://127.0.0.1:8780` proxies to `bin/hubbo sim serve`, which is
  the real core with fakes. It has a fake HA in-process at `http://homeassistant.local:8123`,
  token `fake-ha-token`, and a SimulatedRemote.
- **Board**: `HUBBO_API_TARGET=http://<board>`. A dev server already runs on :5180 proxied to the
  board. Never start another on 5180. e2e uses its own Vite on :5181.

## Mock controls (never in the real API)

- `POST /__mock/reset` body = `ResetFlags` (`mock/handler.ts`):
  - `firstRun`, `requireAuth`, `configState` (blocked | storage_unavailable), `configDamaged`;
  - `startupRejected`, `authState`;
  - `discoverFails`, `scanNeverFinds`.
- `POST /__mock/storage?health=`, `POST /__mock/ha?state=` (HA client state; cleared by the next
  HA settings PUT), `POST /__mock/ha?remove=<entity_id>` (HA forgets it; its device goes stale at
  the next discovery, like hub-sim).
- `POST /__mock/radio/found?channel&address` and `POST /__mock/advance?ms`, mirroring hub-sim's
  `/sim/radio/found` and `/sim/advance`.
- The mock gives the fixture's `demo`-platform entities realistic platforms (sonos, denonavr,
  webostv, hue) plus integration services and skipped ones, so the preview has something to show.

## e2e (Playwright in Docker, `web/e2e`)

- `e2e/hub.ts` is the harness:
  - the `hub` fixture resets the target per test; `test.use({ reset })` asks for a state, and the
    test skips where the target can't produce it;
  - target-neutral helpers: `connectHa`, `setHaState`, `hubAnswersScan`, `advance`, `setStorage`,
    `api(page)`, `removeHaEntity`, `discoverHa` (POST discover, poll to `done`). The control
    calls share one private `control(page, path)` (`/sim/<path>` or `/__mock/<path>`).
  - Match buttons by a plain string name (Playwright does a case-insensitive substring match), not a
    hand-escaped RegExp.
- Projects run in order: first-run → seeded (blocked / storage-boot / credentials-corrupt) → app →
  last (storage-auth). `workers: 1`, because the specs share one hub.
- **Foreground only, under ~170 s per command.** The Bash tool moves anything longer to the
  background, and a background sim stalled an earlier agent for 44 min. Start the sim detached,
  poll until ready, run e2e in chunks, then stop the sim yourself:

  ```sh
  docker compose run --rm -T --name hubbo-sim-web-e2e toolchain bash -c \
    'cp build/native/sim/hub-sim /tmp/hub-sim && exec /tmp/hub-sim "$@"' _ serve --data build/sim-e2e --reset [--auth] &
  until curl -fs -o /dev/null http://127.0.0.1:8780/api/v1/auth/session; do sleep .5; done
  HUBBO_API_TARGET=http://127.0.0.1:8780 bin/hubbo web e2e e2e/a.spec.ts e2e/b.spec.ts --no-deps
  docker rm -f hubbo-sim-web-e2e
  ```

  Build first with `bin/hubbo sim --help` (foreground). Chunks that fit:
  - (setup, remote, unavailable, activate, activity, backup, conflict);
  - (devices, login, tokens, ha, storage, storage-auth).
  - Under `--auth` use a fresh `--data` dir, so setup.spec sees no admin.
- Blocked seeds: `bin/hubbo sim write-record --data DIR --name config --file
  sim/fixtures/config.newer-schema.json`, then `serve` without `--reset`, then
  `--project seeded --no-deps`. For a damaged seed, overwrite a few bytes of `DIR/config.a`.
- **hub-sim quirks:**
  - It runs on sim time. A reconnect after the fake HA comes back only happens as time advances,
    so `connectHa`/`setHaState` call `/sim/advance` and then poll `/status`.
  - No HA token unless `serve --ha-token`; `connectHa` PUTs the fake one per test.
  - Its entities are named by id; unregistered ones are `media_player.denon` and
    `media_player.limits_test`.
  - Poll discovery to `done` before registering (as on the board): POST discover then
    POST /admin/devices straight away can 422.
  - A re-link is checked against the activities that use the device, so re-link a fresh device
    in tests.
  - `/sim/reset` doesn't bring back fake-HA entities removed with `?remove=` (the stale test
    removes `media_player.denon`), so later specs in the same sim run mustn't rely on it.
  - Test routes: `/sim/ha?reachable=&commands=&remove=&fail=`, `/sim/radio/found`,
    `/sim/advance`, `/sim/storage`, `/sim/reset` (which resets the radio too).
- Auth modes: no-auth sim = AUTH DISABLED (simulator only); `--auth` is the production
  authenticator. Login specs skip without auth; storage-down specs split between storage.spec
  (no-auth) and storage-auth.spec (auth, runs last).

## Key decisions

- Secrets: the CSRF token lives in memory only; nothing secret goes in browser storage; the HA token
  is write-only. e2e checks this with `expectNotStoredInBrowser`.
- Backups are downloaded as an opaque blob; restore sends the file text exactly as chosen.
- HA devices snapshot the hub's latest discovery: the add flow runs discovery first, and Refresh
  runs `lib/ha.ts runDiscovery` before the PUT. `haDeviceError` maps 422 unknown_device and 503.
- HA TLS: the "Verify certificate" switch (`verify_tls`, https:// URLs only) is real; the firmware honours it.
  Off still encrypts but doesn't verify, for a self-signed HA on a trusted LAN. There's no CA field
  (`ca_cert_pem` was removed; backups still accept it and ignore it).
- Never round-trip `ha_capabilities` (422 unknown_field): PUTs send only name/kind/provider/revision.
- No free-text params (the `string` ParamSpec type was removed).
- A stale device's commands are a 200 result `failed` / `entity_not_found` (decided 2026-11-09);
  the mock does the same.
- Discovery `truncated` > 0 shows "N entities weren't loaded (more than 2048 supported entities)." No
  action is suggested: search runs over the capped list. Query values go as encodeURIComponent (%20).
- Discovery is async on the board too: always wait for `done` before registering or re-linking.
  The UI only lists candidates once it's done, and Refresh waits in `runDiscovery`.
- Remote address rules live once, in `lib/remote.ts addressProblem`; the UI and the mock's
  PUT /admin/remote/endpoint both use it (the mock still refuses byte separators, as the hub does).
- Don't restyle existing pages (the redesign is deferred). Hobby scale: keep it simple.

## Open items

- Typed params can't be previewed before registering: candidates carry only command ids, so params
  show on the device page afterwards.
- The mock doesn't check a re-link against the activities that use the device (the hub does).
- Phase 3 web work isn't committed: `/simplify` is done (2026-11-09); `/commits` on web/ only at
  the commit instruction.
- Possible later trims (skipped in /simplify, all small): PairRemote could read `pairing` from the
  `remote` query instead of its own 1 s `/admin/remote/pair` poll; RemoteEndpoint polls `remote`
  every 5 s (kept so a console `radio set` shows up).
  on `web/` only.
