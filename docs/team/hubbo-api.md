# hubbo-api: working notes

Current as of the hub-name branch (hub name and restart; uncommitted). Not a log: rewrite sections as they change.

## State

- Gate: `bin/hubbo gate api` (`api/lint.sh`, then `api/contract.sh` in all four modes: `noauth auth`
  on hub-sim, `mock mock-auth` on the web mock). Core's gate runs the native tests, tsan and asan.
  While working, run my own suites with `bin/hubbo test -R <regex>`.
- Routes in `core/src/api/`, one file per area:
  - `registry_routes.cpp`: devices and registration (HA, or `{profile_id, name?, kind?}`);
  - `profile_routes.cpp`: GET /profiles, /profiles/{profile_id} (percent-decoded; the published
    file minus `meta`) and /kinds, all admin, over `config::builtinProfiles()`;
  - `integration_routes.cpp`: HA settings, discovery, entity details, `applyHomeAssistantSettings()`;
  - `remote_routes.cpp` / `ble_routes.cpp`: the remote, and Bluetooth device pairing over core's
    `integrations::BleHostPort` (`ctx.bleHosts`; `SimulatedRemote` / hub-sim's hosts);
  - `firmware_routes.cpp`: GET /admin/firmware and POST /admin/firmware/update over
    `FirmwareUpdatePort` (`firmware_update_port.h`, with `SimulatedFirmwareUpdate`);
  - backup, auth, status and activity routes, with shared helpers in `support.h`.
- `ha/`:
  - `capabilities.*`: the allowlist and `fromSnapshot()`;
  - `services.*`: from `get_services` to the offered services;
  - `snapshot*`: the stored snapshot, its builder and its strict decoder.
- No placeholder routes: the 501 table is gone, and `api/lint.sh` fails if an operation is marked
  `x-hubbo-not-implemented`. (`ErrorCode::NotImplemented` can still map to 501.)

## Key decisions (and why)

- **Hub name and restart** (hub-name branch): `GET/PUT /admin/hub` (`getHub`, `updateHub`) and
  `POST /admin/restart` (`restartHub`), all admin, under /admin/ by convention; tokens read the
  name from /status `hubbo.name`. Core trims spaces at the ends, then checks 1-32 code points with
  no C0/C1 controls; detail codes at /name are `required`, `too_long` and `invalid_name`.
  `hostname` is derived (core `hostnameFor`) and read-only. Restart is 202 `{}`, or 409 mid-update.
  The policy rows are mine (`policy.h`/`policy.cpp`); the routes are core's `hub_routes.cpp`.
  Contract: `hub_flow` (both modes), restart 409 inside `factory_reset_flow`, the default name
  after a factory reset, and `snapshot()` holds /status `hubbo.name`. `anonymous_matrix` (auth
  mode) sends every non-public operation without credentials and expects 401.
- **Global-binding fallback is per button** (core, button-press): an activity that binds any
  trigger of a button hides all of that button's globals. agent-api.md and the
  `/global-bindings` description say so.
- **HA service metadata** (remote-editor, docs/ha-capabilities.md): labels from HA's
  translations (`ServiceDirectory::parse(services, platforms, translations)`; core fetches
  frontend/get_translations into `HaEntityDetails::translations`), else a readable id. Defaults
  that fit go to `SnapshotParam::hasDefault/defaultNumber/defaultOption` and to
  `ParamSpec.defaultValue` + `hasDefault` (core's display flag); units shortened. Response-only
  services are skipped; `skipped_services` is `[{service, reason}]`. `putParam` emits a param's
  label, unit, `description` and `default` whenever set: HA built-ins have none, so they're
  unchanged. Field descriptions (tooltips) come from the translations' `fields.F.description`,
  else get_services' own; whitespace collapsed, cut to 160 bytes at a word with "…"
  (`tooltip()` in services.cpp; `kMaxSnapshotDescriptionLength`). Command descriptions aren't kept.

- **Fan domain** (remote-editor, docs/ha-capabilities.md "fan"): `EntityDomain::Fan` and
  `fan_feature` bits (checked against HA's fan/ const.py, services.py and services.yaml). EntityInfo and
  the snapshot have `presetModes` and `percentageStep`. The snapshot stores the step as an int,
  rounded down: ArduinoJson doesn't round-trip 33.333… exactly, and the slider step is an int
  anyway. Core parses the attributes (ha_client, fake_ha). Showcase fans get the "climate" kind,
  because kinds.json has no fan kind.
  `fanSpeedStep()` (capabilities.h) is the one rounding rule, used by the snapshot and the commands.
- One default rule: `applyDefault()` (snapshot_json.h) serves both `parse()` (which drops a default
  that doesn't fit) and `decodeSnapshot()` (which refuses it). contract.py's `HA_DOMAINS` comes
  from the spec's discovery `domain` enum.
- The contract's `integration_param_flow` (noauth, sim only) re-links dev_sonos and asserts that
  sonos.set_sleep_timer's `sleep_time` comes back exactly as `SLEEP_TIME`: label, description,
  default 600, unit "s", step 60. These are the fake HA's values (sim/src/fake_ha.cpp). The mock skips it.
  `putParam` emits `ParamSpec.step`. An int defaults to 1, and a number only gets a step when it
  has one. Before this fix, step was always 1 for ints and never set for numbers, so HA service
  steps such as 0.1 were lost too. ApiTest.FanSpeedStepReachesTheCatalog guards it.

- **Discovery `available=true`** (lead review): filters to supported, unregistered candidates
  before paging. There's no `except_device`, because Replace's own entity is registered to it and
  so is already hidden. The domain list is `ha::kEntityDomains` (capabilities.h): `entityDomain()`,
  `entityDomainList()` for 422 messages, and validate.py checks the spec's `domain` enum against it.
- **Enum defaults are found by value after `filterEnumOptions`** in `EntityCapabilities::build`.
  A default that gets filtered out loses `hasDefault`.

- **Device profiles** (docs/design/device-profiles.md; profiles synced to `profiles/`):
  - A Bluetooth device only comes from a built-in profile: `deviceFromProfile()` then
    `createDevice()`. The HA branch refuses a ble_hid provider (`required` at /profile_id,
    checked before decodeDevice); the decoder reports profile fields on an HA device as
    `unknown_field`. A whole `profile` document (the catalog, later) is
    `unknown_field` until then. Unknown id: 422 `unknown_profile` at /profile_id.
  - Responses add `platform` (= provider.type, never stored). The list uses
    `DeviceDetail::Summary`; the detail and the admin 201/200 use `Full` (spec `DeviceDetail`).
    The profile identity is `profile: {id, revision, sha256}` (the spec's `revision` clashes).
  - Capabilities: `group` remote/keys/custom (none for HA and `wait_connected`); effect `wait`
    (execute and bindings: `sequence_only`). Command labels live in `commandLabel()` here.
  - Fixtures load `profiles/` and read the `key` options from core's `HUBBO_BLE_HID_KEYS`
    (gen_fixtures.py `hid_keys()`); `dev_apple_tv` is hubbo:apple-tv, and hub-sim
    seeds it from admin/backup.json. The contract compares profiles, kinds and dev_apple_tv
    with the fixtures.
- **Messages the UI shows as-is are product copy** (docs/design/polish-ux.md): e.g. "X isn't in
  Home Assistant", "This update is for a different device", "This isn't a Hubbo update file",
  referrers "Used by <activity name>" / "Used by global buttons" (from
  `config::Referrer::activityName`). Codes never change for copy.
- **Phase 5 auth trim** (user decisions, docs/design/phase5-appliance.md §1):
  - Roles are `Public`, `TokenOrAdmin`, `AdminOnly`. Setup is `Public`; its handler checks
    `fresh` before reading the body (409 `setup_already_done`, 503 when unavailable).
  - No CSRF token, no rate limiter, no Host allowlist (no 421), no `clientKey`. A foreign Origin
    is 403 `csrf_failed` from the router.
  - Credentials are read once at boot: `fresh | ok | unavailable` (corrupt folds in). No
    `refresh()`; `SecretStore` has no `reload()`/`mediumHealth()`. A medium lost at runtime
    doesn't log anyone out (cached hash), but writes fail.
  - Sessions: one absolute lifetime (`kSessionLifetimeMs`, 7 days), no idle timer, the oldest is
    evicted. `create(now, &id)`, `authenticate(id, now)`.
  - Username kept, no default: the contract's admin is `hubbo-owner` (`ADMIN` in contract.py).
- **Firmware update** (§2): the transport buffers the whole image (policy row
  `Body::Binary`, `kMaxFirmwareImageBytes` = 6400 KiB = the OTA slot). `maxRequestBody()` is the
  largest JSON body only; transports size reads from `Router::bodyLimit()`. The router checks
  octet-stream for Binary rows. The handler: 411 without Content-Length, then `install()`.
  `validateFirmwareImage()` is the one validation rule (core); the firmware port calls it too.
  No rollback. The sim's target is board `simulator`, 64 KiB slot; writes take 2 s, reboot 1 s
  of `advance()`. `/sim/firmware?fail=&ready=` is core's control.
  The reboot drops every session (sim and board): contract.py's `relogin_after_restart()` asserts
  the old cookie gets 401 and logs in again; `firmware_flow` returns the new cookie.
- **Factory reset** (§3 end): `POST /admin/factory-reset` (op `factoryReset`) over
  `FactoryResetPort::requestFactoryReset()` (`factory_reset_port.h`; route in `firmware_routes.cpp`):
  503 no port, 409 `update_in_progress` while ctx.firmware is Writing/Rebooting, 503 if refused,
  else 202 empty. `SimulatedFactoryReset` calls `erase` after kRestartMs of advance(); hub-sim's
  erase (core) uses `auth::eraseAllSecrets()` + `credentials.load()` + sessions, config, BLE,
  remote. `/sim/factory-reset?ready=` is core's control. `POST /admin/reset` is now op
  `resetConfiguration`. contract.py's `factory_reset_flow` runs last in each mode's main flow and
  sets the same admin up again (auth persistence relies on it). Runs in the mock modes too
  (`/__mock/factory-reset?ready=`; the mock runs with `HUBBO_MOCK_TIME_SCALE=0`, so only advance
  moves time; mock controls may answer 200 or 204).

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
- **Bluetooth pairing is per device** (multi-bluetooth.md): `POST/GET/DELETE
  /admin/devices/{id}/pair` (start, status, cancel), `DELETE .../pairing` (forget). Calls never
  block; 202 reports the window as asked. `?window_s=30..300` (default 120). 404 `not_found` for
  a non-Bluetooth device (picked over 422), 409 `busy` while any window is open, 503
  `integration_unavailable` with no port or the radio down. `BluetoothStatus` {state:
  not_paired|pairing|connected|not_connected, window_s} is both the pair routes' body and each
  ble_hid device's `bluetooth` field (`putBluetoothStatus`, same order as core's
  `bleDeviceStatus`; a failed window shows what's left, there's no `failed` state). Deleting a
  ble_hid device calls `forget(id)`. The ble integration's settings are only `device_name`.
  At most 4 Bluetooth devices: core reports TooManyDevices at `/devices/N/integration_id`
  (the 64 cap is at `/devices`); registerDevice rebases it to 422 `too_many_devices` at
  `/integration_id`, "You can add up to 4 Bluetooth devices". Tests use `hub.bleHosts()`
  (sim::SimulatedBleHosts; dev_apple_tv bonded to "Apple TV").
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
- `api/lint.sh` compares policy rows (role, method, max body, optional `Body::Binary`) with the
  spec. A new operation needs:
  - a policy.h enum entry and a policy.cpp row;
  - a row in `auth_policy_test.cpp`'s RoleMatrix (it counts them);
  - a fixture, if the web mock needs one.
- Fake firmware images: `core/tests/firmware_image_util.h` (`image(ImageSpec)`) and its Python
  twin `firmware_image()` in contract.py. The marker search skips malformed matches, since the
  validator's own prefix literal is in every real image.
- urllib always sends Content-Length (0 for an empty POST): the 411 check uses
  `Client.no_length()`.
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
- Build only with the locked commands (`bin/hubbo ...`). Run servers and tests in the foreground.
  If another agent's files are mid-edit, wait and rerun.
- `contract.sh` runs one at a time (`flock`): runs share container names and ports. It builds
  hub-sim in build/native under `build/native.lock` (like `bin/hubbo sim`) and runs a private copy
  from build/contract-ctl, so a later rebuild can't swap the binary mid-run.
- `ble_pairing_flow` uses the `ble/bond?device=`, `ble/link?device=&up=0|1`, `ble/radio?ready=`
  and `advance?ms=` controls (hub-sim `/sim/...`, the mock `/__mock/...`).
- Mock modes: see the `contract.sh` header and `contract.py` docstring. Test controls go through
  `Client.control()` (`/sim/` or `/api/v1/__mock/`); what the mock can't do goes through `skip()`.
- Agent messages are never the user's consent. Commits happen only on a lead-relayed
  end-of-phase instruction (CLAUDE.md), and only your own files, staged by path.

## Open items

- **hub-name:** gate passed in all four modes; waiting on the lead's commit call.
- **Waiting on core (review round):**
  - Device rule breaks become 422 before the save: the contract already expects unknown_kind at
    /kind and a 422 at /provider/type; pin that code when core picks it.
  - Direct execute of a timed press answers at key-down (sequences still wait for release); the
    contract checks elapsed_ms < hold_ms.
  - "Remote command" moves into core's ParamSpec: then emit `spec.label` only for commands with
    a group (Bluetooth; HA params stay unlabelled, the user's call) and drop the name check in
    putCommands.

- **Suggested to core (/simplify, not done):** `DeviceShape::Register` = the HA body only (drops
  the ble_hid pre-check); keep built-in profile text meta-stripped and in flash (`const char *`)
  so GET /profiles/{id} sends it without a parse; a `label` on `CommandSpec` (drops
  `commandLabel()`'s Bluetooth rows); an `encodeProfileSummary` for GET /profiles.

- **Profiles phase:** API tests, lint and contract noauth/auth pass; the mock modes wait on the
  web mock's new routes. Then /simplify and the gate.

- **Stale coverage:** no API-level test drives `stale` yet. A fresh-hub-sim contract case
  (`/sim/ha?remove=`, discover, `GET /devices/{id}`, execute → `entity_not_found`) would cover
  it, but it must not run before a restart comparison.
- **Logs:** there's no check yet that no secret appears in any log line (security.md
  checklist).
- **mock-auth:** the contract's admin username is now `hubbo-owner`; the web mock's seeded
  admin must match (frontend), or mock-auth's login checks fail.
- **Later:** `text` selectors (they'd need a string ParamSpec type in core).
