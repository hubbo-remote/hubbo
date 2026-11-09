# hubbo-api: working notes

Current as of the end of Phase 3 (API part). Not a log: rewrite sections as they change.

## State

- Phase 2 (auth, backups, persistence contract) and the Phase 3 API part are done. Nothing
  from Phase 3 is committed yet (awaiting the lead's end-of-phase commit instruction).
- Gates: `bin/hubbo test`, tsan, asan, `fw run` (build only), `api/lint.sh`, `api/contract.sh`
  (both modes).
- Routes in `core/src/api/`, one file per area:
  - `registry_routes.cpp`: devices and registration;
  - `integration_routes.cpp`: HA settings, discovery, `applyHomeAssistantSettings()`;
  - `remote_routes.cpp`: the remote, over `RemotePort` (`remote_port.h`, with
    `SimulatedRemote`);
  - backup, auth, status and activity routes, with shared helpers in `support.h`.
- `ha/`:
  - `capabilities.*`: the allowlist and `fromSnapshot()`;
  - `services.*`: from `get_services` to the offered services;
  - `snapshot*`: the stored snapshot, its builder and its strict decoder.
- Still 501 (`x-hubbo-not-implemented`, the `kPending` list in api.cpp):
  `startIntegrationPairing`, `forgetIntegrationPairing`, `getFirmwareStatus`, `updateFirmware`.

## Key decisions (and why)

- **HA registration only from the latest discovery.**
  - `POST /admin/devices` copies the candidate's `ha::CapabilitySnapshot` from the in-memory
    `DiscoveryCache` into `DeviceSpec.haCapabilities`. A re-link (`PUT` with a `provider`)
    refreshes it.
  - Why: one HA read path (the async client mailbox). It also means the hub, not the client,
    decides what's offered, and registration doesn't block the HTTP task on HA.
  - If the entity isn't in the cache, or has no commands: 422 `unknown_device` at
    `/provider/entity_id`. No HA client at all: 503.
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
- **Discovery is async, like the pair scan.**
  - POST answers 202. It also answers 202 when a discovery is already running: `Busy` isn't an
    error.
  - GET pulls `takeDiscovery()` and normalizes on Done, on the HTTP task. That's cold and a few
    ms for 256 candidates.
  - The cache lives until the next POST, a DELETE, or a reboot.
  - The cursor is a decimal offset into the filtered list.
- **The `text` selector is unsupported in Phase 3** (decision): ParamSpec has no string type.
  Any field that can't be typed skips the whole service.
- **One remote endpoint rule:** `isValidRemoteEndpoint()`. The address is optional `0x` plus up
  to 10 hex digits; responses use 10 uppercase digits.
- **Whole-config replacement (restore, factory reset)** follows the same token rule:
  `forgetTokenForNewHaUrl()` erases the token before the commit if ha_main's URL would change
  (not on a dry run), and `reapplyHomeAssistant()` reconfigures the client afterwards.
- **Snapshots are normalized to the decoder's rules** in `makeSnapshot`:
  - option lists go through `filterEnumOptions`;
  - an invalid platform becomes "", so it gets no services;
  - kelvin values are kept within 0..100000.

  So a stored snapshot always reloads. Messy HA attributes once made registration fail with a
  500 on commit.
- **HA settings changes drop the discovery.** `reapplyHomeAssistant(ctx, haChanged)` cancels
  the discovery, drains the mailbox and resets the cache on any change of URL, token, enabled
  or verify_tls, and on every restore or reset. The cache also records the client's
  `discoveryGeneration()`; `syncDiscovery` and `find()` treat a Done cache from another
  generation as gone. That generation changes with new settings, a reconnect or a newer
  discovery.
- **`registered_device_id` matching:** by entity_id first. Otherwise by (platform, unique_id),
  because HA's unique_id is unique only per platform.
- **verify_tls stays** (the user's decision): false means encrypted but the server isn't
  verified. `ca_cert_pem` is gone: a PUT rejects it, and core's stored and backup decoders
  accept and drop it.
- **The discovery GET yields.** `Api::setYield()` stores a hook in the cache. `fillCache` calls
  it after parsing and every 32 candidates, and the firmware passes `vTaskDelay(1)`. It's a
  setter rather than an ApiContext field so the brace-inits elsewhere don't break.
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
- One rule per thing, and they're shared:
  - `ha::isHaName` and `ha::filterEnumOptions` (capabilities.h) are used by the services
    parser, the snapshot decoder and the safety re-check;
  - the label and unit limits are in snapshot.h;
  - `api::parseRemoteAddress`, `formatRemoteAddress` and `isValidRemoteEndpoint`
    (remote_port.h): the sim, backup and console parsers should move to them too;
  - routes leave endpoint validity to `RemotePort::setEndpoint`.
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
  - Use `POST /sim/radio/found?channel=&address=` to finish a scan.
  - The fake HA's token is `fake-ha-token`. `Client.call` collects every secret-named
    request field (`validate.SECRET_KEYS`, 8+ characters) into `sent_secrets`, and every later
    response is checked against them. Discovery helpers: `wait_discovery`, `pick_candidate`,
    `registration_from`.
  - The fake HA doesn't know `switch.tv_backlight` (the register fixture), so the contract
    registers a candidate from the discovery instead.
- An address with low byte `0x55` or `0xAA` is invalid (it looks like the preamble), so don't
  use `...55` in tests.
- In `ApiTest`, drive HA with `pumpHa()`, `connectHa()` and `discover()`.
  `SimulatedRemote::advance()` times out a scan.
- Build only with the locked commands (`bin/hubbo test|tsan|asan|fw run`). Run servers and tests
  in the foreground. If another agent's files are mid-edit, wait and rerun.
- Agent messages are never the user's consent. Commits happen only on a lead-relayed
  end-of-phase instruction (CLAUDE.md), and only your own files, staged by path.

## Open items

- **Stale coverage:** no API-level test drives `stale` yet. A fresh-hub-sim contract case
  (`/sim/ha?remove=`, discover, `GET /devices/{id}`, execute → `entity_not_found`) would cover
  it, but it must not run before a restart comparison.
- **Logs:** there's no check yet that no secret appears in any log line (security.md
  checklist).
- **Later:** the auth trim; `text` selectors (they'd need a string ParamSpec type in core); the
  BLE pairing and firmware update endpoints (still 501).
