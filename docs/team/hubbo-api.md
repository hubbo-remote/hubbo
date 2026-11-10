# hubbo-api: working notes

Current as of Phase 4 (contract against the web mock). Not a log: rewrite sections as they change.

## State

- Gates: `bin/hubbo test`, tsan, asan, `fw run` (build only), `api/lint.sh`, `api/contract.sh`
  (all four modes: `noauth auth` on hub-sim, `mock mock-auth` on the web mock).
- Routes in `core/src/api/`, one file per area:
  - `registry_routes.cpp`: devices and registration;
  - `integration_routes.cpp`: HA settings, discovery, entity details, `applyHomeAssistantSettings()`;
  - `remote_routes.cpp` / `ble_routes.cpp`: the remote and BLE pairing, over `RemotePort` /
    `BlePairingPort` (with `SimulatedRemote` / `SimulatedBlePairing`);
  - backup, auth, status and activity routes, with shared helpers in `support.h`.
- `ha/`:
  - `capabilities.*`: the allowlist and `fromSnapshot()`;
  - `services.*`: from `get_services` to the offered services;
  - `snapshot*`: the stored snapshot, its builder and its strict decoder.
- Still 501 (`x-hubbo-not-implemented`, the `kPending` list in api.cpp): `getFirmwareStatus`,
  `updateFirmware`.

## Key decisions (and why)

- **HA registration only from the entity's loaded details** (Phase 4, §3b). Discovery is lean
  (no `get_services`, no full registry): candidates carry a domain-only command preview, no
  `unique_id`, no `skipped_services`, no platform if YAML-only. Picking loads details: `POST/GET
  .../ha_main/entities/{entity_id}/details` (client `requestEntityDetails`/`takeEntityDetails`,
  one at a time; a new POST cancels the old). `fillDetails` builds the snapshot from
  `get_services` and frees it. Register and re-link take `completedDetails()` (Done, same
  entity_id) or answer 409 `details_required`; no commands: 422 `unknown_device`. Details live in
  `DiscoveryCache::details` (`FetchState`, fields shared with candidates via `HaEntity`), dropped
  by `dropDetails()` with new HA settings, restore or reset (not by a reconnect).
  Why: one HA read path (the mailbox), the hub decides what's offered, and the HTTP task never
  waits on HA.
- **The snapshot is server-only.**
  - It's stored and backed up as `ha_capabilities`. Core's `config/json.cpp` decodes it only
    in the Stored and Backup shapes, so a client-sent one is 422 `unknown_field` from the
    decoder itself.
  - `putDevice` calls `encodeDevice(device, out, false)`, so it never reaches a response.
  - Why: the snapshot decides which HA services can be called, so it must come only from HA.
    The Device schema is `unevaluatedProperties: false`.
- **The HA token is write-only.**
  - It lives in the SecretStore as `kHaTokenKey` = `ha_token`. A PUT omits it to keep it, sends
    `null` to clear it, or sends a string to set it.
  - Order on PUT:
    1. erase the token first if the URL changed or it's being cleared (a failure is a 503 and
       nothing changes);
    2. commit the config;
    3. write the new token (a failure is a 503 "settings saved, token not");
    4. `applyHomeAssistantSettings()`.
  - Why: a token must never be sent to a host it wasn't entered for, even after a partial
    failure.
- **`ha_main` is built in** (core's `ensureBuiltinIntegrations`, like `system` and `ble`), with
  `url: ""` until it's set up, and reports `not_configured` until then. `""` is a valid URL in
  the spec and in a PUT (it unsets the URL and clears the token). The contract's
  `fresh_ha_setup()` covers a first-boot hub.
- **Stored URLs are base `http(s)://host[:port]`** (the spec pattern). The client gets
  `ws(s)://host[:port]/api/websocket` via `homeAssistantSocketUrl()`. Firmware boot and hub-sim
  both call `applyHomeAssistantSettings()`, so there's one rule for `enabled`, the URL and the
  token. The user's real HA is https, so it maps to wss.
- **Discovery is async, like the pair scan.** POST answers 202 (also while one runs). GET pulls
  `takeDiscovery()` and normalizes on Done, on the HTTP task. The cache lives until the next
  POST, a DELETE or a reboot; the cursor is a decimal offset into the filtered list.
- **The `text` selector is unsupported in Phase 3** (decision): ParamSpec has no string type.
  Any field that can't be typed skips the whole service.
- **BLE pairing follows RemotePort:** httpd-task calls that never block; 202 reports the window
  as asked (the loop opens it shortly). `?window_s=30..300` (default 120), a query value since
  the policy row allows no body. `failed` = the latest window closed with no bond. `ble_hid`
  only (404), 409 `busy` while open, 503 with no port or BLE not running. `blePairing` is the
  last ApiContext field so positional initializers still compile. The config's
  `settings.bonded` still comes from `Runtime::integrationState`, not the port.
- **One remote endpoint rule:** `isValidRemoteEndpoint()`. The address is optional `0x` plus up
  to 10 hex digits; responses use 10 uppercase digits.
- **Whole-config replacement (restore, factory reset)** follows the same token rule:
  `forgetTokenForNewHaUrl()` erases the token before the commit if ha_main's URL would change
  (not on a dry run), and `reapplyHomeAssistant()` reconfigures the client afterwards.
- **Snapshots are normalized to the decoder's rules** in `makeSnapshot` (`filterEnumOptions`,
  an invalid platform becomes "", kelvin within 0..100000), so a stored snapshot always reloads.
- **HA settings changes drop discovery and details.** `reapplyHomeAssistant(ctx, haChanged)` on a
  URL, token, enabled or verify_tls change, and every restore or reset. `syncDiscovery` also
  treats a Done cache from another `discoveryGeneration()` (new settings, reconnect) as gone.
- **One device per entity:** `registeredDevice()` matches entity_id, else unique_id within the
  platform (HA renames). Candidates have no unique_id, so they match by entity_id only; the
  details find a renamed entity, and register refuses it (422 `duplicate_id`; re-link instead).
- **verify_tls stays** (the user's decision): false means encrypted but the server isn't
  verified. `ca_cert_pem` is gone: a PUT rejects it, and core's stored and backup decoders
  accept and drop it.
- **The discovery GET yields:** `Api::setYield()` (a setter, so ApiContext brace-inits don't
  break); `fillCache` calls it every 32 candidates; the firmware passes `vTaskDelay(1)`.
- **Stale is runtime state, from discovery only.**
  - `availability` comes from `Runtime::deviceAvailability()` (core: `client.entityStale()`):
    the entity is missing from the latest discovery. It's never saved.
  - The adapter refuses a stale entity before sending: `status: failed`,
    `message: entity_not_found`. Real HA answers a call to a missing entity with success, so
    until a discovery runs (after every boot too) such commands report `accepted`. The spec
    says so.
  - An unanswered call is `status: timeout` with `adapter_timeout`.
- **Hobby scale:** don't extend auth (a trim is planned). One review round per phase, then
  commit.

## Gotchas

- `-Werror=missing-field-initializers`: `ApiContext` brace-init in tests must list every
  trailing field, and an aggregate with a default member initializer needs a constructor (see
  `ParamDraft` and `SimulatedRemote`).
- The 501 list is checked twice: `api/lint.sh` compares `kPending` with
  `x-hubbo-not-implemented`, and policy rows (role, CSRF, max body) with the spec. A new
  operation needs:
  - a policy.h enum entry and a policy.cpp row;
  - a row in `auth_policy_test.cpp`'s RoleMatrix (it counts them);
  - a fixture, if the web mock needs one.
- Query strings: read values only with `http::queryValue()`, which percent-decodes one
  parameter (`+` and `%XX`). The raw slicer is private to route.cpp. Never decode the whole
  query, or `%26` splits parameters. The firmware console uses `http::percentDecode()`.
- One rule per thing, shared: `ha::isHaName`/`filterEnumOptions` (capabilities.h), label and
  unit limits (snapshot.h), `api::parseRemoteAddress`/`formatRemoteAddress`/
  `isValidRemoteEndpoint` (remote_port.h; routes leave validity to `RemotePort::setEndpoint`).
- Fixtures are generated: edit `api/tools/gen_fixtures.py`, then run `api/lint.sh gen`. The
  frontend's mock reads them, so tell the lead when they change.
- Contract and hub-sim:
  - `hub-sim serve` runs ha_main as the real adapter over the fake HA. `/status` integrations
    report the client's state, and `contract.py`'s `ha_state()` reads it from there. At boot,
    serve stores `fake-ha-token` if no token is stored.
  - `POST /sim/ha` options: `reachable`, `commands`, `fail=<domain.service>|none`, and
    `remove=<entity_id>` (HA forgets the entity, so it turns stale after the next discovery).
    A removed entity comes back when hub-sim restarts, so don't leave one stale before a
    `survives_restart` check.
  - Backup fixtures need `ha_capabilities` on their HA devices, or the real adapter offers
    them no commands and restores fail with `unknown_command`. `gen_fixtures.py`'s
    `stored_device()` adds them from `HA_ENTITIES` (platform `demo`).
  - The fake HA's token is `fake-ha-token`. `Client.call` checks every response against all
    secret-named request fields sent so far (`sent_secrets`).
  - Register a discovery candidate: `registration_from()` loads its details (`load_details`).
- An address with low byte `0x55` or `0xAA` is invalid (it looks like the preamble), so don't
  use `...55` in tests.
- `ApiTest`: `pumpHa()`, `connectHa()`, `discover()`; `SimulatedRemote::advance()` times out a scan.
- Build only with the locked commands (`bin/hubbo test|tsan|asan|fw run`). Run servers and tests
  in the foreground. If another agent's files are mid-edit, wait and rerun.
- `contract.sh` runs one at a time (`flock`): runs share container names and ports.
- `ble_pairing_flow` uses the `ble/bonded?peer=` and `advance?ms=` controls.
- Mock modes: see the `contract.sh` header and `contract.py` docstring. Test controls go through
  `Client.control()` (`/sim/` or `/api/v1/__mock/`); what the mock can't do goes through `skip()`.
- Agent messages are never the user's consent. Commits happen only on a lead-relayed
  end-of-phase instruction (CLAUDE.md), and only your own files, staged by path.

## Open items

- **Stale coverage:** no API-level test drives `stale` yet. A fresh-hub-sim contract case
  (`/sim/ha?remove=`, discover, `GET /devices/{id}`, execute → `entity_not_found`) would cover
  it, but it must not run before a restart comparison.
- **Logs:** there's no check yet that no secret appears in any log line (security.md
  checklist).
- **Later:** the auth trim; `text` selectors (they'd need a string ParamSpec type in core); the
  firmware status and update endpoints (still 501).
