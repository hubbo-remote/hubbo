# Phase 4: Web admin, functional completion, served from the board

Scope: a hobby device for one home on a trusted LAN. Phase 4 finishes the web admin's features and
serves it from the board. Visual polish and a redesign are deferred until near the end of the
project (user decision, 2026-11-09). Don't restyle existing pages.

The user's priorities are the activity editor, the dashboard and the first-run wizard. Most of
4.1–4.4 already exists (see `docs/team/hubbo-frontend.md`). This phase closes the gaps.

## 1. Activity editor (4.3)

- **Duplicate:** ask for the new name, defaulting to "X copy", before creating the copy.
- **Reordering steps:** startup and shutdown steps can be reordered by drag and by keyboard (for
  example Alt+↑/↓ on a focused step). The up/down buttons stay.
- **Re-link check:** the mock refuses a re-link that breaks an activity's commands, as the hub
  does.
- **Acceptance (e2e on hub-sim):** build "Watch Apple TV" from scratch:
  - map the Apple TV arrows (down/up pairs), select and back;
  - map an HA media player's volume up and down, and a light's on and off;
  - set the startup and shutdown order with a wait;
  - activate it and check the startup ran, then Off and check the shutdown ran.
  - The fake HA needs a realistic media player and light with services (core).

## 2. Dashboard (4.2)

- **Already done:** active activity, the remote, BLE, HA and Ethernet tiles, latency and recent
  errors.
- **Gap: hub-sim can't produce every dashboard state.** e2e needs controls for BLE connected and
  disconnected, and for HA states (core: `/sim/ble`, extending `/sim/ha`). Then the dashboard
  states get covered on hub-sim, not only the mock.
- **Board check:** the dashboard against the real board, with true BLE, HA and remote state.

## 3. First-run wizard (4.4)

- **Resume:** the wizard returns to the right step after a reload. Derive the step from hub state
  (admin exists, remote paired, HA configured); don't store progress in the browser.
- **Steps:** admin, network, remote, Home Assistant, done. The wizard doesn't ask
  for devices or activities: people aren't ready for that during setup (user decision,
  2026-11-09). "Done" goes to the dashboard.
- **Remote:** pair by scan, or enter a known channel and address (`PUT /admin/remote/endpoint`).
- **Home Assistant:** Save is not Continue. Save stores the URL and token, then waits for the
  connection result and shows it: connected, or the error (auth failed, unreachable) with the form
  still there to fix and save again. Continue (or Skip) moves on.
- **Acceptance:** a clean hub (hub-sim with fresh data, then the dev board after a full wipe) can
  be set up in the browser alone, with no CLI.

## 3a. BLE pairing from the web admin (found at the gate)

`POST/DELETE /admin/integrations/{id}/pair` were still 501, so a wiped board couldn't pair the
Apple TV without the serial console. Implement them like the remote endpoint:
- **API:** a runtime port (`api::BlePairingPort`, like `RemotePort`) with a simulated version for
  hub-sim; `POST` opens a pairing window (`window_s`, a new bond replaces the old), `GET` (add it)
  shows `PairingStatus`, `DELETE` forgets the bond. Spec, contract.
- **Firmware:** implements the port over `BleHid` (the console's `pair` and `ble forget`); never
  blocks the RF loop.
- **Frontend:** the integration page's pair and forget, polling status while the window is open.
  Not in the wizard.

## 3b. Faster HA discovery (found at the gate)

Discovery took about 28 s on the board for the home's HA, because it pulled about 3 MB (full entity
registry 1.9 MB, all states 0.87 MB, all services 0.23 MB). The board handles about 9 ms per KB;
HA itself answers in about 0.1 s. Decided with the user (2026-11-09):
- **Discovery** uses `config/entity_registry/list_for_display` (compact, enabled entities only)
  plus all current states via `subscribe_entities` (then unsubscribe), about 0.8 MB, so roughly
  7 s. All states, not only registry ids: YAML-only entities (no registry entry) must stay
  pickable.
- **Actions** (`get_services`) aren't part of discovery. They're fetched when a device is
  registered or refreshed, for that entity, about 2 s then.
- Stale rules unchanged in meaning: stale = missing from the latest completed discovery.

## 4. Accessibility and mobile (4.4)

- **Phone-sized Playwright project:** run the main specs at 390×844, with touch.
- **axe checks:** add axe to e2e on the main pages, and fix serious and critical violations.
- **Fixes:**
  - the skip link doesn't navigate away under hash routing;
  - the Activities row menu supports arrow keys;
  - the remote picture's tap targets are big enough on a phone.

## 5. Mock and hub drift (4.1)

- `api/contract.sh` also runs against the Vite mock. Fix mismatches on whichever side is wrong
  (the API owns the contract, the frontend owns the mock).

## 6. Serving the UI from the board (4.5)

Firmware design, from the firmware agent's feasibility note:

- **Assets are embedded in the firmware image**, not stored in LittleFS. One OTA updates both, and
  a rollback keeps them in sync. A filesystem upload would erase the config, which lives in the
  same partition.
- **Build:**
  - `bin/hubbo fw` and `ota` run `bin/hubbo web build` first.
  - A PlatformIO pre-build script packs `web/dist` into a generated `.cpp` under `firmware/.pio`:
    deterministic gzip, a path table and an ETag.
  - A missing `web/dist` fails the build with a clear message.
- **Serving:**
  - One static GET handler on `/*`, registered after `/api` and `/debug`.
  - Always `Content-Encoding: gzip`.
  - Hashed assets are cached as immutable. `index.html` gets `no-cache` with an ETag and answers
    304.
  - SPA fallback to `index.html` for HTML requests; unknown asset paths get 404.
  - The same host allowlist as the API.
  - A CSP header.
  - Send straight from flash, with no heap copy.
- **httpd:** raise `max_open_sockets` from 4 to 7 or 8 for browser cold loads.
- **Measure on the board:**
  - cold and warm load times;
  - free internal RAM during a cold load with HA connected, which must stay above about 70 KB;
  - the remote loop's max gap during repeated reloads, still about 3 ms with 0 iterations over
    5 ms.
- The Vite dev server proxied to the board keeps working for development.

## Gate

On the dev board, with the UI served by the board itself (no host dev server):
1. Back up the config, factory-reset the board, and set it up through the wizard alone.
2. Build an activity with the Apple TV and Guest Room Lights in the editor.
3. Use it from the remote.
4. Restore the backup afterwards if wanted.

Also:
- Check the dashboard shows true state.
- Check the main pages on a phone.
- The remote stays fast while the UI loads.

## Ownership

| Agent | Owns |
|---|---|
| Frontend | Sections 1–4 UI work, the mock, e2e (including phone and axe) |
| Core | Realistic fake-HA media player and light; `/sim/ble` and the extended `/sim/ha` controls |
| API | Contract against the mock (5); any spec gaps the frontend finds |
| Firmware | Section 6 (embedding, static handler, sockets, board measurements) |
