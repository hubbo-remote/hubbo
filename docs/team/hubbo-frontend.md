# hubbo-frontend: working notes

Current state, not a log. Update when a piece of work lands.

## State of the web admin (Phase 5 in progress on `phase-5`, not committed)

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
- Phase 5 (docs/design/phase5-appliance.md §1–2):
  - **Auth trim**: no CSRF token (client sends none; the hub checks Origin → 403 `csrf_failed`),
    no 429, no `corrupt`. `CredentialsProblem` has one screen (`unavailable`: restart or wipe).
    Credentials are read once at boot, so a session outlives storage going down: `storage.spec`
    now runs under `--auth` too, and `storage-auth.spec` and the `last` project are gone.
  - Setup and Login start with an **empty username** (autofocus there; `autoComplete` username /
    new-password / current-password). The mock's set-up admin and e2e `ADMIN` are the login
    fixture's (`hubbo-owner`, like api/tools/contract.py).
  - **Firmware** (`components/admin/FirmwareUpdate.tsx`, Settings → Firmware): version and board;
    a .bin picker that reads the file's marker (`lib/firmware.ts readMarker`) to say "Hubbo vX for
    <board>" (the hub decides; nothing blocks client-side); `api.uploadFirmware` is XHR for
    `upload.onprogress` (the browser sets Content-Length). After the 202 it polls GET
    /admin/firmware every 1 s: writing % → rebooting → network errors/502/504 = "Restarting…" →
    idle, or a 401 = the hub restarted and sessions are gone (the app shows Login). Module memory
    `pending` = {from, expected: the file's marker version}; after the sign-in the section remounts
    and polls again for a fresh answer. "Update installed" only if the running version is the
    expected one; otherwise "The update didn't take" (a reset mid-write or a crashing image boots
    the old one, and the RAM-only `failed` state is gone). Every 4xx/503 is shown as "Nothing was
    written". A `failed` state shows "still running X". A reload mid-update picks the poll up
    again. `run` (a ref counter) lets only the latest upload/watch update the section.
  - **Mock firmware**: `mock/firmware.ts` builds real-layout fake images (`buildFirmwareImage`,
    used by unit tests and e2e) and checks 0xE9, the marker, the board and the SHA. Like
    SimulatedFirmwareUpdate: 2 s write, 1 s reboot, in hub time = `/__mock/advance` plus real
    time × `timeScale` (default 1/3; e2e env `HUBBO_MOCK_TIME_SCALE=0`; the factory reset uses it too). The second
    half of the reboot drops connections (`status: 0` → socket destroyed); at its end the
    version changes and every session is cleared. `/__mock/firmware?fail=&ready=` mirrors
    `/sim/firmware`; mock-only `?revert=1` reboots into the old version.
- **Backup & reset** (Settings, after the board test):
  - "Reset configuration" (POST /admin/reset, `api.resetConfiguration`): devices, activities,
    global buttons and integration settings go; login, tokens and pairings stay. Typed `reset`.
  - "Factory reset" (`components/admin/FactoryReset.tsx`, POST /admin/factory-reset): erases
    everything; typed `erase`; says to forget "Hubbo" on the Apple TV. After the 202 it shows
    "Erasing… the hub is restarting", polls the public /auth/session (not /status: sessions are
    gone) until `fresh`, then reloads into setup. Refusals show "Nothing was erased".
  - After a restore, if ha_main has a URL but no token, a notice links to Integrations (backups
    never hold the token; the hub keeps a stored token only when the restored URL is the same).
  - Mock: restore applies ha_main settings with that token rule; factory reset is down for 1000 ms
    of hub time, then a first-run hub (no admin, sessions, tokens, remote, config; BLE unbonded);
    `/__mock/factory-reset?ready=0` mirrors `/sim/factory-reset` (503).
  - Both typed confirms use `TypedWordField` (common.tsx) and `typedMatches` (lib/utils.ts);
    `sleep` is shared from lib/utils.ts too.
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
  `configDamaged`, `startupRejected`, `authState` (unavailable), `discoverFails`, `scanNeverFinds`.
- Mirroring hub-sim's `/sim/…`: `factory-reset?ready=`, `firmware?fail=&ready=`, `storage?health=`, `ha?state=|remove=|auth=&connect=`,
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
  `control(page, path)` (`/sim/<path>` or `/__mock/<path>`).
- Projects: first-run → seeded → app and phone (both depend on seeded; phone on the mock only).
  `phone` (390×844, touch) runs dashboard, remote, ble and a11y.
  `workers: 1` (one shared hub). `board` is read-only against a real board (`HUBBO_E2E_BASE_URL`),
  keeps no trace/screenshot/video, and **the normal projects must never point at a board**.
- A file filter is a substring (`ble` matches `unavailable.spec.ts`; use `ble.spec`). Project
  regexes are anchored (`board` once matched `dashboard.spec.ts`).
- **One whole pass per command** (foreground, `timeout` 600000; see "Testing without waiting
  around" in CLAUDE.md). While working, run only the specs a change touches. The phase gate is
  `bin/hubbo gate web`: check, build, then
  - mock, desktop and phone (~4.2 min): `bin/hubbo web e2e`
  - hub-sim `--auth`, desktop (~2.6 min): `bin/hubbo web e2e --sim --auth`
  - hub-sim no-auth, only the `noauth` project (~15 s): `bin/hubbo web e2e --sim --project=noauth`
  If a long run moves to the background, wait for its completion notice; don't split it.
  `--sim` builds hub-sim, starts it on fresh data on :8780, sets `HUBBO_API_TARGET` and removes
  it afterwards; never start a sim by hand. A single spec: `bin/hubbo web e2e --sim [--auth] <file>`.
- Projects against hub-sim: no `phone` (layout and touch are the mock's job); `noauth` (no
  dependencies) matches the specs that behave differently without auth: firmware (no sign-in
  after the reboot) and tokens (the no-auth sim has no admin until `ensureAdmin`). Under `--auth`
  its tests skip themselves (`hub` fixture). Add a spec there when its no-auth behaviour differs.
- Why a pass is slow: `workers: 1` (one shared hub, reset per test) and real-time polls (1–3 s)
  and HA/BLE waits inside tests. Backlog: one hub per worker (a mock per worker, or a hub-sim port
  per worker) for `workers > 1` or sharding.
- Under `--auth` use a fresh `--data` dir so setup.spec sees no admin. In a full pass only the
  first setup test runs; the others need their own fresh hub (`setup.spec.ts:83` and `:132`, each
  alone), when setup changes. hub-sim starts paired, so
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
  without auth.
  - hub-sim's firmware version doesn't change after an update (so there the UI says "didn't
    take") and its sessions survive the simulated reboot; the reboot can fall between two 1 s
    polls. firmware.spec allows for all three.
  - hub-sim answers no 501s now: `unavailable.spec` (stubbed 501s) runs on the mock only.

## Key decisions

- **No UI copy or UX changes until the polish pass** unless the lead relays a functional need. The
  user finds the UI has far too many notes; UX and copy get reworked in the polish pass (fast,
  without the board). A Bluetooth line in the restore note was added and reverted for this reason.

- Secrets: nothing secret in browser storage (no CSRF token any more); the HA token is write-only. e2e checks with `expectNotStoredInBrowser`.
- Backups are an opaque blob; restore sends the file exactly as chosen.
- Never round-trip `ha_capabilities` (422 unknown_field): PUTs send name/kind/provider/revision.
- HA TLS: "Verify certificate" (`verify_tls`, https only) is real; no CA field.
- A stale device's commands are a 200 `failed` / `entity_not_found`.
- Discovery is async everywhere: wait for `done` before registering or re-linking. `truncated` > 0
  shows a count, no action.
- Remote address rules live once, in `lib/remote.ts addressProblem` (UI and mock).
- Don't restyle existing pages (the redesign is deferred). Hobby scale.

## Open items

- Phase 5 web work isn't committed; `/simplify` done. `bin/hubbo gate web` green on this tree
  2026-11-09 (mock 93 passed; `--sim --auth` 59; `noauth` 4), plus contract mock + mock-auth.
- Skipped in /simplify: drive the firmware poll through `useQuery` pollMs (drops the run ref and
  eslint-disables); a shared ProgressBar (PairRemote has the same markup) and key/value `Facts`
  component; one mock clock for every timer; hub-sim reporting the new version and dropping
  sessions after a simulated reboot (core/API ask) would remove the `SIM_URL` branches.
- Flaky once each, passed on rerun: `activity.spec:163` (mock), `watch-apple-tv` (hub-sim).
- Board check of the firmware flow is pending (the real reboot: network errors, then a 401).
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
