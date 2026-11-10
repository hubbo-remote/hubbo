# hubbo-frontend: working notes

Current state, not a log. Update when a piece of work lands.

## State of the web admin (Phase 4 done on `phase-4`, not committed)

- React 19 + TS + Vite + Tailwind, shadcn-style components in `web/src/components/ui`. wouter with
  hash URLs. Our own small query cache in `src/api/hooks.ts`: `useQuery(key, fn, { pollMs,
  keepPrevious })` (one timer per key, paused while the page is hidden), `invalidate` (prefix),
  `setQueryData`, `getQueryData`.
- Every request goes through `src/api/client.ts`. Types alias the generated `schema.gen.ts`
  (`src/api/types.ts`). After an `api/openapi.yaml` change run `bin/hubbo web gen:api`.
- Phases 2–3: backup/restore (opaque blob), blocked/storage banners, conflict dialogs; HA settings
  with a live connection line (token write-only); async discovery in `HaCandidates`; register,
  stale banner, Re-link and Refresh on the device page; remote endpoint and "Scan from Hub".
- Phase 4 (docs/design/phase4-web-admin.md, sections 1–4):
  - **Wizard**: admin, network, remote, Home Assistant, done (no devices or activities). It resumes
    from hub state only (`useResumeStep` in `Setup.tsx`, reading /status: no admin → admin;
    unpaired → remote; HA not_configured → HA; else done). `#/setup/<step>` opens a step. Creating
    the admin goes to `#/setup/network` (hub state can't tell Network was seen). The resumed step is
    fixed once known. Once finished, the wizard only opens via `#/setup` or Settings.
  - **Errors after a save attempt**: the `ShowErrors` context in `ui/input.tsx` (Field, Input,
    Select obey it; default shows). Forms provide `false` until the first save/submit. Save stays
    enabled; an invalid Save just shows the errors.
  - **Selects show what's stored**: unset is "Choose…" (required) or "Not set" (optional; picking
    it clears). A stored value that isn't an option is its own "<value> (not available)" option; a
    ref shows its raw id while activities load. `defaultParams` leaves an activity_ref unset.
    Unit tests: `ParamForm.test.tsx`.
  - **HA Save** (wizard and Integrations, one component): Save stores the config, calls onSaved,
    invalidates status; it never reads /status itself, so a status failure can't be a save error.
    `HaConnection` remembers the pre-save state and shows "Connecting…" until the state moves or
    `HA_APPLY_GRACE_MS` (2 s) passes (the HA task applies settings on its next pass, so an early
    read is the OLD connection). Polls 1 s until the outcome is in, then 3 s; 20 s → "still trying".
  - **Remote endpoint** (wizard and Settings, one `RemoteEndpoint`): the fields always show the
    stored endpoint (null edit state = stored), so a save never blanks them. Save says "Saved.
    Listening on channel N, address …" and mounts `PressCheck` (also on a scan's "Remote paired"):
    it polls /remote/status every 500 ms through the cache and shows "Got it: <Button> pressed" per
    new `last_event` (`isNewEvent` in `lib/remote.ts`: younger, or another button/kind; the first
    read is the baseline), "Waiting for a button press…" before, and a hint after 30 s.
  - **BLE pairing** (§3a, Integrations only): `BlePanel` polls the pair status every 1 s while
    pairing, confirms before replacing a bond or forgetting, shows `failed` until the next action.
    Bond and peer name come from the pair status.
  - **HA entity details** (§3b): picking an entity loads its details (`loadEntityDetails` in
    `lib/ha.ts`, Retry on failure), which register, re-link and refresh need (409
    `details_required` otherwise). `EntityPreview` shows them. Details with `registered_device_id`
    (the same entity, or same unique_id+platform after a rename) show "Already added as <name>"
    with a link instead of Add; a register 422 `duplicate_id` reloads the details to show the same.
  - iPhone: `keepPrevious` keeps the HA search box mounted while a search loads (the keyboard
    closed per letter). `Select` draws a real ChevronDown. PageHeader truncates only string titles
    (it clipped the activity-name focus ring).
  - Duplicate asks for the name. Steps reorder by drag (mouse) and Alt+↑/↓; phones use the buttons.
  - Re-link refused when it breaks an activity: 422 paths by activity index; `haDeviceError` turns
    them into "Watch Apple TV, startup step 4: …".
  - Accessibility: axe (WCAG 2 A/AA, light and dark) clean of serious/critical on the main pages;
    the test waits for CSS transitions (axe measured mid-fade). Phone keys have ≥ 24 px hit areas.
- Bundle: ~155 KB gzipped (`bin/hubbo web build` prints sizes).

## Modes

- **Mock** (default): `web/mock` runs inside Vite, seeded from `api/fixtures`. It's the spec in
  TypeScript, so keep it faithful; `mock/handler.test.ts` holds its tests. Env:
  `HUBBO_MOCK_LATENCY`, `_FIRST_RUN=1`, `_AUTH=1`, `_PAIR_MS`, `_DISCOVER_MS` (e2e sets both to 0).
- **hub-sim**: `HUBBO_API_TARGET=http://127.0.0.1:8780` proxies to `bin/hubbo sim serve` (real
  core with fakes: fake HA at `http://homeassistant.local:8123`, token `fake-ha-token`).
- **Board**: a dev server already runs on :5180 proxied to the board. Never start another on 5180;
  e2e uses its own Vite on :5181.

## Mock controls (never in the real API)

- `POST /__mock/reset` body = `ResetFlags`: `firstRun`, `requireAuth`, `configState`,
  `configDamaged`, `startupRejected`, `authState`, `discoverFails`, `scanNeverFinds`.
- Mirroring hub-sim's `/sim/…`: `storage?health=`, `ha?state=|remove=|auth=&connect=`,
  `ble?connected=`, `ble/bonded?peer=`, `radio/found?channel&address`, `advance?ms` (also runs
  down the BLE window and entity details), `remote/<button>/<press|release|tap>` (only records
  `last_event`; no bindings run).
- Mock only: `ha?rename=<id>&to=<new>` (the old id gone; `renamedFrom` makes the new one's
  details name the old device, and registering it is 422 `duplicate_id`, like the hub).
- Entities: the fixture's `demo` entities get realistic platforms (sonos, denonavr, webostv, hue);
  services, skipped ones and unique ids come only with the details. Shared with hub-sim's fake HA:
  YAML-only `switch.patio_fountain` (no platform/unique_id) and a free `media_player.denon`.
- `api/contract.sh mock mock-auth` runs the API contract against the standalone mock. The Node
  path parses bodies itself (413 before parsing, 415, 400 `malformed_json`, depth 8; 12 for
  restore). Admin/token routes before setup are 401, like the hub.
- BLE state like hub-sim: pairing → connecting, no bond → not_configured, else connected/disconnected.

## e2e (Playwright in Docker, `web/e2e`)

- `e2e/hub.ts` is the harness: the `hub` fixture resets the target per test; `test.use({ reset })`
  asks for a state and the test skips where the target can't produce it. Target-neutral helpers:
  `connectHa`, `setHaState`, `fakeHaNextConnect`, `letHaConnect`, `setBle`, `hubAnswersScan`,
  `advance`, `setStorage`, `registerHa`, `removeHaEntity`, `discoverHa`, `api(page)`, all over
  `control(page, path)` (`/sim/<path>` or `/__mock/<path>`, with CSRF).
- Projects: first-run → seeded → app and phone (both depend on seeded) → last (depends on both).
  `workers: 1` (one shared hub). `phone` (390×844, touch) runs dashboard, activate, activity,
  devices, remote, ble and a11y. `board` is read-only against a real board (`HUBBO_E2E_BASE_URL`),
  keeps no trace/screenshot/video, and **the normal projects must never point at a board**.
- A file filter is a substring (`ble` matches `unavailable.spec.ts`; use `ble.spec`). Project
  regexes are anchored (`board` once matched `dashboard.spec.ts`).
- **Foreground only, ≤ ~165 s per command, one e2e run per command** (longer gets backgrounded).
  Build the sim first with `bin/hubbo sim --help`, start it detached, wait for
  `/api/v1/auth/session`, run chunks, stop it:
  ```sh
  docker compose run --rm -T --name hubbo-sim-web-e2e toolchain bash -c \
    'cp build/native/sim/hub-sim /tmp/hub-sim && exec /tmp/hub-sim "$@"' _ serve --data build/sim-e2e --reset [--auth] &
  ```
  Chunks (with `--project=first-run --project=seeded --project=app`):
  - A: setup blocked storage-boot credentials-corrupt activate activity backup conflict dashboard devices
  - B: ha login remote storage tokens unavailable watch-apple-tv ble.spec (+ a11y on the mock);
    on hub-sim run `--project=app a11y` separately
  - then `--project=phone`, then `--project=last --no-deps` (under `--auth`).
- Under `--auth` use a fresh `--data` dir so setup.spec sees no admin. Each first-run test needs
  its own fresh hub: the resume test runs alone (`setup.spec.ts:82`). hub-sim starts paired, so
  that test opens `#/setup/remote` by URL there.
- Blocked seeds: `bin/hubbo sim write-record --data DIR --name config --file
  sim/fixtures/config.newer-schema.json`, then `serve` without `--reset`, then `--project seeded
  --no-deps`.
- **hub-sim quirks**:
  - DELETE /activities/<id> needs `?revision=N`.
  - Registering needs the entity's details first (`registerHa`); poll discovery to `done` first.
  - BLE has one source, the pairing port: bonded to "Apple TV" at start and after /sim/reset.
  - Sim time: HA reconnects and remote event ages only move with `/sim/advance`. Specs advance
    1 s before a press so a new event is younger than the last.
  - `/sim/reset` restores entities, BLE and the radio, and releases held buttons. Specs tap.
  - A re-link is checked against the activities that use the device: re-link a fresh device.
- Auth: no-auth sim = AUTH DISABLED; `--auth` is the production authenticator. Login specs skip
  without auth; storage-down specs split between storage.spec and storage-auth.spec (runs last).

## Key decisions

- Secrets: the CSRF token lives in memory only; nothing secret in browser storage; the HA token is
  write-only. e2e checks with `expectNotStoredInBrowser`.
- Backups are an opaque blob; restore sends the file exactly as chosen.
- Never round-trip `ha_capabilities` (422 unknown_field): PUTs send name/kind/provider/revision.
- HA TLS: "Verify certificate" (`verify_tls`, https only) is real; no CA field.
- A stale device's commands are a 200 `failed` / `entity_not_found`.
- Discovery is async everywhere: wait for `done` before registering or re-linking. `truncated` > 0
  shows a count, no action.
- Remote address rules live once, in `lib/remote.ts addressProblem` (UI and mock).
- Don't restyle existing pages (the redesign is deferred). Hobby scale.

## Open items

- Phase 4 web work isn't committed. `/simplify` done and gates green (2026-11-09). Use `/commits`
  on `web/` only, when the commit instruction comes.
- Typed params can't be previewed before registering (candidates carry only command ids).
- Closing the Duplicate dialog mid-save doesn't navigate to the copy.
- Possible later trims (skipped in /simplify):
  - Keep the wizard step in the URL always (drops the resume effect and the e2e `SIM_URL` goto).
  - Load entity details through `useQuery` instead of the `loadId` counter, or one helper that
    owns details → mutate → error mapping (or the hub loading details itself: an API ask).
  - Have /status report the settings revision the HA client runs, instead of the 2 s grace (core ask).
  - A `useShowErrorsOnSubmit` hook for the five forms that repeat the `attempted` pattern.
  - One fake-HA entity set for mock and hub-sim (then drop the mock rename control and the e2e
    target tables); have the mock's scan wait for `radio/found` like hub-sim.
  - `MockRequest` raw-only; a board target in `hub.ts`; derive the remote's `HIT` from the scale.
