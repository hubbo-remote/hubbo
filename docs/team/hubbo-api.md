# hubbo-api: working notes

Current as of the ir branch. Not a log: rewrite sections as they change.

## State

- Gate: `bin/hubbo gate api` (`api/lint.sh`, then `api/contract.sh` in all four modes: `noauth auth`
  on hub-sim, `mock mock-auth` on the web mock). Core's gate runs the native tests, tsan and asan.
  While working, run my own suites with `bin/hubbo test -R <regex>` and one contract mode. Never
  start a gate myself: the lead starts the gates once per phase, when the code is frozen. A change
  after the gates gets a targeted test, not another gate, unless the lead calls it broad.
- Routes in `core/src/api/`, one file per area:
  - `registry_routes.cpp`: devices and registration (HA, `{profile_id, name?, kind?}` or
    `{profile, name?, kind?}`);
  - `profile_routes.cpp`: GET /profiles, /profiles/{profile_id} (percent-decoded; the published
    file minus `meta`) and /kinds, all admin, over `config::builtinProfiles()`;
  - `integration_routes.cpp`: HA and MQTT settings, discovery, entity details,
    `applyHomeAssistantSettings()` / `applyMqttSettings()`;
  - `transitions.cpp` (`api/transitions.h`): `runTransition()` (activate, "" = off) and
    `requestHubRestart()`, shared by the routes and the MQTT bridge's hooks;
  - `remote_routes.cpp` / `ble_routes.cpp`: the remote, and Bluetooth device pairing over core's
    `integrations::BleHostPort` (`ctx.bleHosts`; `SimulatedRemote` / hub-sim's hosts);
  - `firmware_routes.cpp`: GET /admin/firmware, POST /admin/firmware/update over
    `FirmwareUpdatePort` (`firmware_update_port.h`, with `SimulatedFirmwareUpdate`), and
    POST /admin/firmware/check and /install over `FirmwareReleasePort` (`ctx.releases`);
  - backup, auth, status and activity routes, with shared helpers in `support.h`.
- `ha/`:
  - `capabilities.*`: the allowlist and `fromSnapshot()`;
  - `services.*`: from `get_services` to the offered services;
  - `snapshot*`: the stored snapshot, its builder and its strict decoder.
- No placeholder routes: the 501 table is gone, and `api/lint.sh` fails if an operation is marked
  `x-hubbo-not-implemented`. (`ErrorCode::NotImplemented` can still map to 501.)

## Key decisions (and why)

- **Discovery** (agent-docs): `GET /api/v1/` (`getApiIndex`), `/api/v1/openapi.json`
  (`getOpenApi`), `/api/v1/agent.md` (`getAgentGuide`) are Public policy rows; `/llms.txt` is
  core's Public host route (not in the spec). gzip only when `Accept-Encoding` allows it
  (`Vary`), so plain curl works. The index has no hub name. `Error.error.docs` (const
  `/api/v1/agent.md`) is on every 401 and on the router's 404 for unknown paths, never on a
  handler's missing-id 404. Core embeds the spec (as JSON) and the guide at build time, so
  **docs/agent-api.md and docs/llms.txt are served verbatim** (the contract compares bytes): write it for outside agents (no repo paths, no
  phases), about 20 KB, with `#/components/schemas/...` pointers instead of "see the spec". The
  button rules in it mirror web/src/lib/commands.ts (`writeMain`/`writeLongPress`, `isTimed`):
  re-check them when the web's button model changes. The fixtures (gen_fixtures.py
  `passthrough()`) and core's sim seeds bind the same way: HA on `:down`, Bluetooth as
  down/up pairs, tap only with a Long press or for activate/off.
- Contract: `discovery_flow` (both modes, no credentials; parsed-spec and byte-exact guide
  equality, gzip vs identity, index links, /llms.txt, the 404 hint and its absence on a missing
  id) and `anonymous_matrix` also requires `docs` on each 401. Ops marked `x-hubbo-describes-api`
  (getOpenApi) skip the body scans (revisions >= 1, secret keys); `x-hubbo-returns-secret`
  (createToken) skips the secret scan. GUIDE and SPEC_LINK are read from the spec's `ApiIndex`
  consts in main(). All requests go through `Client._send` (call, host_call, fetch). The guide must not contain the contract's
  `PASSWORD` or `HA_TOKEN` values, or the sent-secrets check trips.
- **Hub name and restart** (hub-name branch): `GET/PUT /admin/hub` (`getHub`, `updateHub`) and
  `POST /admin/restart` (`restartHub`), all admin, under /admin/ by convention; tokens read the
  name from /status `hubbo.name`. Core trims spaces at the ends, then checks 1-32 code points with
  no C0/C1 controls; detail codes at /name are `required`, `too_long` and `invalid_name`.
  `hostname` is derived (core `hostnameFor`) and read-only. Restart is 202 `{}`, or 409 mid-update.
  The policy rows are mine (`policy.h`/`policy.cpp`); the routes are core's `hub_routes.cpp`.
  Contract: `hub_flow` (both modes), restart 409 inside `factory_reset_flow`, the default name
  after a factory reset, and `snapshot()` holds /status `hubbo.name`. `anonymous_matrix` (auth
  mode) sends every non-public operation without credentials and expects 401.
- **Default hub name and real hostname** (hub-name, docs/design/hub-name.md): a new hub is
  "Hubbo XXXX" (core `defaultHubName(mac)`; hub-sim "Hubbo 534D"). Hub is `{name, hostname,
  hostname_conflict}`: `hostname` is `HubIdentity::shownHostname()` (what mDNS advertises after
  a clash), the bool is `hostnameConflict()`; /status network.hostname is the same. A rename
  clears it. The contract learns the hub's default on its first read (pattern `^Hubbo
  [0-9A-F]{4}$`) and expects it back after a factory reset, so hub-sim and the mock needn't
  share a suffix. `validate.load_spec()` (validate and contract) loads the spec with a strict loader (duplicated keys fail: js-yaml
  refuses them, PyYAML doesn't).
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
  anyway. Core parses the attributes (ha_client, fake_ha). Fans get the `fan` kind.
  `fanSpeedStep()` (capabilities.h) is the one rounding rule, used by the snapshot and the commands.
- **Newer domains** (ha-domains, docs/design/ha-domains.md; HA dev checked 2026-11-13):
  scene, script, cover, button, input_boolean, humidifier, select (remote came later, below).
  Climate was dropped by the user. `cover_feature` and
  `humidifier_feature` bits are in capabilities.h. EntityInfo/snapshot add `availableModes`,
  `options` and `minHumidity`/`maxHumidity` (snapshot: whole percent via `humidityRange()`, the
  one rule; 0/0 = not reported, HA's 0..100). Every new id is `<domain>.<service>`. Toggle is
  offered for cover, humidifier and input_boolean. `offersIntegrationServices()` keeps platform
  services to media_player/light/switch/fan/remote (offerFor and fromSnapshot both check). Per-domain
  facts (suggested kind, integration services) are one `kDomainTraits` table in EntityDomain
  order, beside `kEntityDomains` (which stays a plain name array: validate.py parses it).
- **Remote domain** (ha-remote, docs/design/ha-remote.md, 2026-11-14). An earlier, bigger
  ha-remote attempt was discarded as over-designed; this one is "just bring in remote":
  - `EntityDomain::Remote` / `"remote"` last in kEntityDomains, kind `other`, integration
    services on. Allowlist `remote.turn_on/turn_off/toggle`, no params (activity is text).
  - The one exception to "platform's services only": a remote.* also gets HA's `remote` services
    that target it, always as custom services (never typed, not in `skipped`); an allowlisted one
    only with fields left (`ha::isAllowlistedService`; turn_on's activity with ACTIVITY 4, the
    fixture remote has 0, so none). They come before the platform's, send_command first, so the
    2048-byte hint trim keeps them. YAML remote (platform ""): only those.
  - The one rule is `ha::serviceDomains(entityId, platform)` (capabilities.h; services.h
    includes it): "remote" first for a remote, then the platform unless it's the entity's own
    domain. It's the parse list (fillDetails, showcase, fake_ha, ha_client), offerFor's loop and
    order, and `ha::isDeviceService` (domain in it): the snapshot decoder's custom services
    (typed `services` stay platform-only) and core's serviceOnDevice.
  - Fixture `remote.living_room_tv` (NEW_DOMAIN_ENTITIES, demo, features 0) with
    REMOTE_CUSTOM_SERVICES (HA's real services.yaml + strings.json, the hub's 160-byte cut);
    matches core's fake HA. Contract: new_domains_flow runs `remote_action()` on it (send_command
    reaches last_call; webostv.button is forbidden_param); ha_action_flow refuses
    remote.send_command on the LG TV (all modes, so the mock must refuse it too).
  - Tests: HaServices.RemoteGetsTheRemoteDomainsServicesAsCustomServices,
    HaSnapshot.RemoteServicesDecodeOnlyOnARemote, ApiTest.YamlRemoteGetsTheRemoteDomainsServices.
- **`suggestedKind(domain, platform, deviceClass)`** (capabilities.h) is the one kind rule:
  candidates carry `suggested_kind` (required, DeviceKind; candidates only, not details),
  hub-sim's showcase calls it, and gen_fixtures.py `suggested_kind()` mirrors it.
  Core's `HaCandidate::deviceClass` feeds it.
- Fixtures: `integrations/ha_main.entities/<entity_id>.capabilities.json` holds the catalog that
  registering each made-up newer-domain entity gives (NEW_DOMAIN_ENTITIES; the web mock serves
  these). Fake HA has the same 8 entities, and contract `new_domains_flow` (sim only) registers
  each, compares its capabilities and suggested_kind with the fixtures, then deletes it.
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
  Trial boot with rollback (`rolled_back_from`). The sim's target is board `simulator`, 64 KiB slot; writes take 2 s, reboot 1 s
  of `advance()`. `/sim/firmware?fail=&ready=` is core's control.
  The reboot drops every session (sim and board): contract.py's `relogin_after_restart()` asserts
  the old cookie gets 401 and logs in again; `firmware_flow` returns the new cookie.
- **Online updates** (`docs/design/firmware-updates.md`): `FirmwareReleasePort`
  (`firmware_release_port.h`) over core's `hubbo::release` types (`UpdateCheckState`). One update
  state: an install shows as `Downloading` on the update port, then the same validation, write and
  reboot as an upload; `firmwareUpdateRunning` includes Downloading. Install errors:
  `download_failed`, `image_mismatch`, `invalid_image` (constants in `firmware_update_port.h`;
  `SimulatedFirmwareUpdate::beginDownload/finishDownload/failDownload` drive it in the sim).
  Routes: check 202 / 409 `busy` / 503 no port; install `{version}` (X.Y.Z, detail `pattern`):
  Busy before NotAvailable (409 `update_in_progress` / `not_available`). A null `ctx.releases`
  reads as never checked and check/install answer 503. A failed check keeps `available` from the
  last good check (core's rule). Release builds report plain `X.Y.Z`; a dev build's git describe
  is never offered one. `check_updates` lives in `HubSettings` (GET/PUT /admin/hub; PUT takes
  any of name, check_updates and radio_power, `{}` is 422). `Api::setFirmwareVersion()` is hub-sim only (the
  installed version after its reboot). Contract: `firmware_release_flow` over `/sim/release`
  (`running=` first: the sim defaults to dev build `0.0.0-sim`); check 500 ms, download 1000 ms.
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
- **One device per entity:** `registeredDevice()` matches entity_id first. Failing that, to
  follow HA renames, it matches unique_id within the same domain and platform
  (`ha::entityDomainName`). HA keys its registry by (domain, platform, unique_id), and Android TV
  Remote gives a TV's media_player and its remote the same unique_id (the TV's MAC). Without the
  domain check, adding the remote said it was already added as the TV (fix-ha-unique-id; test
  SameUniqueIdInAnotherDomainIsAnotherDevice). Candidates have no unique_id, so they match by
  entity_id only. The details find a renamed entity, and register refuses it (422
  `duplicate_id`; re-link instead).
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

- **httpd's stack is small (16 KB on the board).** A handler's helpers inline into one frame
  with every local live at once, so big records (DeviceSpec, DeviceUpdate, EngineRequest) live
  on the heap, and decoders fill their caller's record in place (`decodeSnapshot`,
  `decodeService`, `decodeParam`, `serviceDraft`) rather than return a copy. Rule in
  `api/support.h`. No noinline: measured, separate frames stacked deeper than the inlined ones.
  Measure with the Xtensa compiler (host -Os inlines differently): `build/scratch/xsu-api.sh
  /work/core <out>` in `bin/hubbo run` (core's `xsu.sh` covers config/).
- A header edited mid-build (another agent's) can leave stale objects in build/native:
  segfaults across ApiTest while ASan passes. Touch the headers and rebuild.
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

- **IR (branch ir, uncommitted; docs/design/ir.md). Built and /simplify'd; `bin/hubbo gate api`
  passed in all four modes (2026-11-24); waiting on the lead's commit call.**
  - Import: `POST /admin/devices {profile, name?, kind?}` (`deviceFromImport` in
    registry_routes.cpp), any platform. Parsed once: core's `parseProfile(JsonVariantConst)` and
    `profileSha256(JsonVariantConst)` read the body's own profile object, issues go out as
    `report.rerooted("", "/profile")` (keeps `dropped()`), and `deviceFromProfile(ProfileDocument&&)`
    moves it. A fractional number (meta) is `wrong_type` at /profile.
  - Body limit: registerDevice's row is `kMaxProfileImportBodyBytes` = 384 KiB (policy.cpp): the
    spec's 256 KiB counts the profile minified without `meta`, and a file carries both, so the body
    has headroom. Core's `parseProfile` (both overloads) refuses a profile over 256 KiB minified
    without meta: `too_long` at "" -> /profile (ApiTest and contract `import a profile over 256 KiB`,
    built from many 1000-byte strings: ArduinoJson caps one string near 64 KB).
    `maxRequestBody()` (unknown routes) is still restore's 640 KiB.
  - A Bluetooth or IR provider without a profile (`hasProfile`): 422 `required` at /profile (was
    /profile_id for Bluetooth).
  - `LimitReached` (a count cap or the 512 KB config cap) is 409 `limit_reached` "There's no room on
    the hub for another <subject>" in `sendCoreError` (one rule for every create route).
  - `decodeSettings` accepts every type but system (the type comes from the stored integration).
  - `ir` integration: listed and in /status from config (core built-in); state `ready`/`not_ready`
    (added to IntegrationState). Settings are `{type: ir}` only (IrSettings, also the PUT form);
    name and enabled are editable.
  - Spec: IrProvider, IrCommand + ProntoCode (`^0000 [0-9A-F]{4}( [0-9A-F]{4})+$`; the hub checks
    the rest), ProfileCommands max 512 with oneOf Ble/IR, DeviceProfileContent picks the command
    shape by provider type, `ir_repeats` in ProfileTiming (refused on ble by the schema's else),
    DeviceRegisterImport (profile is a loose object: the hub validates), `invalid_pronto` in the
    detail enum ("Not a raw Pronto code" in issueMessage). IR availability is always `unknown`.
    Execute `message` for IR: `adapter_unavailable` (no emitter, nothing sent), `adapter_failed`
    (a send accepted then failed; core's ticket), `queue_full`.
  - Fixtures: seeded `dev_bedroom_tv` (sim/fixtures/ir/nec-tv.json; gen_fixtures `ir_device`,
    `ir_commands`, `profile_sha256` = Python's canonical hash, matches core), `ir` in the
    integrations list and backup, `admin/register_ir.{request,}.json` (sony-receiver.json),
    `devices/execute.ir_press.request.json`, `integrations/ir.config.json`,
    `errors/invalid_pronto.json`.
  - Contract `ir_flow` (all modes; /sim/ir sends and `ir?ready=0|1` sim only): fixture device,
    press, emitter not ready -> not_ready + adapter_unavailable, import equals the fixture (id and
    revision aside) then delete, bad Pronto path, partial profile, 413 one byte over 384 KiB. The
    profile-device checks accept ir; "IR device without a profile" in ble_pairing_flow's list.
  - gen_fixtures: `device_from_profile` + `PLATFORMS` build both platforms' devices,
    `remote_commands(profile)` is the shared remote group; contract `integration(c, id, label)`.
  - ApiTest.ImportsAProfileFile.
  - Core added the JsonVariantConst overloads, the moving deviceFromProfile and
    `ValidationReport::rerooted` on request; the route uses them.

- **remote log (branch radio-power, committed, gates passed):** `GET /remote/log` (op
  `getRemoteLog`, role token, next to /remote/status: validate.py keeps /admin admin-only). No
  read-only tokens and no scopes: a normal token reads it, and the token matrix still asserts 403
  on every admin op. `ApiContext::remoteLog` (last field, core's `remote::RemoteLog`): null on a
  release build, and then the route isn't registered at all (the router's own 404). No #ifdef in
  the route (core's stub compiles). Route in status_routes.cpp: `sendRemoteLog` serializes each
  entry into the body string (never one document for up to 8192 entries; `serializeJson` into a
  std::string replaces it, so use a scratch string). Shape `{entries, span_ms, dropped}`, entries
  oldest first, oneOf on `type`; button/trigger/ids nullable. An action's `result` + `error`
  match an activity run's step: core's `stepOutcome()` and the shared `putStepOutcome()`. The
  body is a std::string in PSRAM on the board (`extmem_enable(0)` after boot); a full log is about
  1 MB, so no cap and no chunking. Contract:
  `remote_log_flow` (hub-sim `?signal=` sticks, so the release resets it to strong; skipped on
  the mock) plus the token read; ApiTest.RemoteLog; AuthPolicy.RoleMatrix row.
- **radio-power (branch radio-power, committed, gates passed):** `radio_power` on GET/PUT
  /admin/hub (`RadioPower` enum low|medium|high|max, default max) over core's
  `HubSettings::radioPower()/setRadioPower()`. Parsed before anything saves: a bad value is 422
  `invalid_enum_value` at `/radio_power` and a name in the same body isn't saved. Like the name
  and check_updates it's hub state: never in a backup, restore leaves it, reset sets max.
  Contract: hub_flow (each level, refusals); persistence_flow sets `medium` before a process
  restart, and `snapshot()` now carries the whole GET /admin/hub. The web mock returns
  radio_power. ApiTest.HubNameAndRestart covers it.

- **Known, accepted (hobby scale):** PUT /admin/hub with both `name` and `check_updates` saves
  the name first; if saving check_updates then fails (StorageFailed, 503), the rename stays.
- **firmware-updates:** routes, spec, fixtures, ApiTest and the noauth contract are done. The web
  mock's release feed is frontend's (the mock modes skip the release flow). No gate yet.

- **mqtt (in progress, no gate yet):**
  - Done: transitions.h; GET/PUT mqtt_main (host and username checked by core's
    `isValidMqttHost`/`isValidMqttUsername` in domain/ids.h, the same rule as config decode:
    host up to 253 of [A-Za-z0-9._:-], never exactly one ':' (host:port), IPv6 fine; port 1..65535; `password` write-only: omit keeps, null clears, which the
    lead approved over the design's ""); `kMqttPasswordKey`; `ApiContext::mqtt` (last field);
    spec, fixtures (`integrations/mqtt_main.*`, list/status/backup entries); restore `requires`
    `mqtt_password` (the restored broker has a host); `RestartOutcome::NoPort` (the route's
    null-port 503 lives in requestHubRestart); ApiTest `TheMqttPasswordIsWriteOnly` and
    `SharedTransitionAndRestart`; contract `mqtt_flow` (noauth passes, with the fake broker); security.md.
  - A failed erase in `forgetSecretsForNewEndpoints()` re-applies the clients before its 503
    (an earlier slot's secret may be gone); ApiTest's `secrets.failEraseKey` fails one erase.
  - Secrets: one `kSecretSlots` table (integration, type, key, settings field, `has_*` flag,
    restore follow-up, endpoint) drives decode, `has_*`, "a new URL/host forgets the secret"
    (PUT, and `forgetSecretsForNewEndpoints()` for restore/reset) and `putSecretFollowUps()`.
    One `reapplyIntegrations(ctx, haChanged)` re-applies HA and MQTT after every change; it relies
    on both clients ignoring unchanged settings (HA does; MqttBridge::configure is core's, so I asked for the same).
  - Waiting: the web mock's mqtt_main (the mock modes fail on its 404). Core switched
    POST /admin/restart to `requestHubRestart()`, and detail() no longer names the host.

- **ha-domains:** DeviceKind follows profiles/kinds.json (43 kinds, adds cover, scene and script);
  lint checks the order. Every cover suggests `cover` (the user renamed `shade`; garage doors and
  gates too). A snapshot's humidity bounds are both or neither, min < max (decode refuses
  otherwise). `bin/hubbo gate api` passed in all four modes after /simplify and the lead's
  review fixes (2026-11-13).
  Not done: option labels for enum params (ParamSpec has none).
- **Stack (merged with agent-docs):** done and simplified; gate passed. Board: adding
  a Sonos peaked near 6 KB of 16 KB. Xtensa frames now: create 336, update 352, execute 496,
  decodeSnapshot 496, fromSnapshot 400. Not done (each under 900 B): `syncDetails` 800,
  `mediaPlayer` 592, ConfigureIntegration 672, `status_routes` 752. Suggested to core:
  `updateDevice(DeviceUpdate &&)` like `createDevice`.
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

- **Default Off binding (setup-fixes):** POST /admin/reset restores core's `factoryDefaultConfig()`
  (built-ins + global `off:tap` → system off); `ApiContext::factoryDefaults` is gone (hub-sim's
  positional init dropped its slot). backup.empty.json carries the binding; the resetConfiguration
  description says so. ApiTest's MQTT reset case asserts the binding. Contract noauth passes; no
  reset flow assumed empty global bindings.

- **Mute toggle (setup-fixes, built; no gate yet):** HA has no mute toggle. `media_player.mute_toggle`
  "Toggle mute" (VOLUME_MUTE; next to volume_mute). Generic hook: `EntityCapabilities::stateAttribute(i)`
  (the `CommandDef` row's `stateAttribute` column, capabilities.cpp) names the attribute to read; core's adapter reads the one
  entity (subscribe_entities) at press time and passes it as `buildServiceCall(..., out, state)`
  (trailing `const ServiceValue *`, default nullptr). Unknown or non-bool counts as false, so it mutes.
  HA drops `is_volume_muted` when the player is off. Fixture bindings use mute:down to mute_toggle
  (core's sim too). Contract `mute_toggle_flow` (noauth, sim only): muted=1 → false, again → true,
  muted=0 → true.

- **Home Assistant action (setup-fixes, built; no gate yet; docs/design/ha-custom-action.md):**
  - Action/ExecuteRequest `call` (HaServiceCall {service, data}); only with `call_service` on an
    HA device. Core decodes it (`config::decodeHaServiceCall`); the execute route requires it for
    call_service, else `unknown_field` /call. Codes: pattern, wrong_type, too_long, too_deep,
    forbidden_param, template_not_supported; 16 calls per config (`capacity_exceeded`).
  - The picker list is the snapshot's `custom_services` (services skipped as unsupported_field or
    too_many_fields; never response_only), with hints {name, required, example (JSON text,
    at most 64 B), description}. They're served as call_service's `services` in capabilities
    (`ha::encodeCustomServices`, shared with the snapshot encoder; no label → none sent, the
    web shows the id). call_service is offered only when there are any. Budget: 16 services,
    8 fields, `kMaxCustomHintBytes` 2048 per device (checked at decode) and `kMaxConfigHintBytes`
    16384 per config (core's compiler checks; `snapshotFor` trims to what's left). One budget
    rule, `trimCustomServices(&vector, budget)`: keeps in order what fits (bytes and 16), skips
    what doesn't and goes on; used by offerFor (2048) and snapshotFor. ConfigAtTheLimits fills
    it exactly.
  - `decodeSnapshot(..., path, entityId)`: custom_services on a domain that never offers
    integration services (scene, script, ...) is unknown_field (core passes the provider's
    entity_id).
  - Old-style services.yaml (no `target`, an `entity_id` field with an entity selector:
    webostv.*, sonos.snapshot) now count that field as the target. So sonos.snapshot/restore
    become typed commands after a re-link.
  - `ha::isHaServiceId` (also the snapshot decoder's service check), `buildCustomCall` (sets
    `ServiceCall::rawData`); buildServiceCall refuses call_service. call_service's group
    "custom" is the `CommandDef` row's `group` column.
  - Fixtures: dev_lg_tv is platform webostv with 3 custom services; act_play_xbox `info:down`
    → webostv.button INFO. Contract `ha_action_flow` (all modes; last_call is sim only).
  - Existing devices get call_service after one re-link (no shim).
  - Domain rule (core's compiler): a call's service domain must be the snapshot's platform, else
    `forbidden_param` at `.../call/service` (execute and config). Only media_player, light,
    switch and fan devices ever get the action. The reach is the integration's own services
    (e.g. mqtt.publish on an MQTT light); security.md says so. Contract: homeassistant.stop on
    execute, hassio.host_shutdown in a binding. Not a malicious fixture: those must fail the
    schema, and this one passes it.
- **Simplify (setup-fixes):** done. Skipped: a shared execute/decodeAction `call` helper (needs
  core's config/json.cpp). The config hint total became `ha::configHintBytes` (below).
- **Shared with core (setup-fixes):** `ha::isTargetKey(name)` (capabilities.h; the five target keys,
  a subset of `isForbiddenParamName`) and `ha::configHintBytes(devices, exceptId = "")`
  (snapshot.h; customHintBytes over the config's HA devices). lint checks kTargetKeys against
  HaServiceCall's data propertyNames and kTargetKeys + kPayloadNames against ParamName.
- **GET /status integrations carry `detail`** (`putIntegrationSummary` adds it for both /status and
  /integrations), so the Status page polls once. registry_flow asserts the state and detail match the
  list. MQTT's detail is empty while connected or connecting (core), and the fixtures match.
- **Remote keys per integration:** designed, parked for a later phase. It uses a per-integration
  `ha-keys.json` that turns into a profile-shaped `press` command; the HA action is the fallback.

- **Activity runs (activity-runs branch; docs/design/activity-runs.md).** A failed step no longer
  stops a sequence. The hub keeps the last 8 startup and shutdown runs in RAM.
  - Port: `Runtime::activityRuns(ActivityRuns *out) const` (api.h; core's
    engine/run_history.h). The firmware and sim implement it with `engine.runsShared`. False means
    no history, which the API shows as none. `ActivityRuns` is about 14 KB on the board, so
    `readRuns()` heap-allocates it.
  - Routes in `status_routes.cpp`:
    - `GET /activity-runs` (`listActivityRuns`, token) returns `{runs}`, newest first.
    - `/status` `last_run` (required, nullable) is `{activity_id, kind, outcome, failed_steps}`
      (failed and timed_out).
    - Each run and last_run has an `id` (core's `uint32_t id`): it counts from 1, is never reused
      and survives /sim/reset. The web keys on it.
    - `started_ago_ms` = `runs->nowMs` − startedMs. The runtime fills nowMs in activityRuns(), so
      the route doesn't build a snapshot.
    - A step's `duration_ms` is null unless `RunStep::hasDuration()` (it started and isn't
      pending; a wait stays pending until the next step starts).
  - Step `error`: the code for failed or timed_out steps (`adapter_timeout`, ...). A cancelled
    step carries the run's reason (`cancelled`, or `queue_full` for a rejected run). It's null
    when ok or pending. `duration_ms` is null if the step never started.
  - No other API reports a run's outcome. PhaseResult (activate/off) is only queued, rejected or
    none, because runs are async. `ActionSequence`'s description has the keep-going rule.
  - Fixtures: activity_runs/list.json (completed_with_errors, completed, cancelled). status*.json
    carries last_run; blocked has null.
  - Contract `activity_runs_flow` checks that /activity-runs and /status last_run agree in every
    mode. On hub-sim only, it also runs `/sim/ha?fail=media_player.turn_on` on Watch Apple TV:
    the run is completed_with_errors, step 0 failed, and the rest still ran; then off's shutdown
    is newest. The token matrix makes one GET.
  - ApiTest ActivityRunsShowEachStepNewestFirst checks running, then completed, then a cancelled
    step's reason.
  - /status reads `Runtime::lastRun(LastRun *)` (core's `RunHistory::readLatest`, about 40 B on
    the stack), so a dashboard poll no longer copies the whole history.
- **Macros (macros branch; docs/design/macros.md), working first pass, not gated.** Runs gain
  kind `macro`, `button` (Button) and `trigger` (`press`/`long_press`), both null for
  startup/shutdown; `activity_id` is null only for a global macro. Macros are explicit:
  `macros` (trigger keys, next to `bindings`) on activities and global bindings, optional on
  input/backup (absent = none), always in responses (Activity, GlobalBindings; the PUT body is
  GlobalBindingsInput). A listed trigger is a macro at any length; an unlisted list of more
  than one action is `macro_required` at /bindings/<trigger>; bad entries at /macros/<i>
  (`wrong_type`, `invalid_trigger`, `duplicate_trigger`, `unbound_macro`). maxItems 144. Same on /status last_run
  (`putRunHead` in status_routes.cpp, a template over ActivityRun/LastRun; core's
  `Trigger trigger`, Hold = long_press). A dropped macro (lane full: 1 running + 4 queued) is
  not recorded. Waits are allowed only in a macro (core's compiler); an unmarked wait-only
  binding stays `sequence_only` (message now "...startup, shutdown or macro").
  - Fixture: Watch Apple TV `guide:down` = press Home, wait 500, press Down; the newest run
    (id 13) is that macro, so status last_run is a macro. hub-sim seeds from backup.json, so the
    seed has this macro too: the contract drops `guide:*` before adding its own.
  - Contract `macro_runs_flow` (sim only, after activity_runs_flow): activity tap, global dvr
    (null activity), 1.5 s hold (long_press); full lane with a 2 s wait (sim taps take 100 ms):
    5 completed in id order, 5 volume_up + 5 volume_down in /sim/log; off mid-macro cancels.
    The shared `activity_runs()` checker also runs on the mock: it needs button/trigger.
  - Unit: `ApiTest.AGlobalMacroIsARunWithItsButtonAndNoActivity`,
    `ApiTest.TryingAMacroQueuesItAsWrittenAndRecordsIt`.
  - **POST /macros/run** (`runMacro`, token, 32 KB like activity writes; activity_routes.cpp): `{actions, trigger,
    activity_id}` → 202 `{run_id}`. Trigger (parseTrigger, not `up`) and activity (must exist,
    needn't be active, not switched to) are checked here; `ConfigService::compileMacro` checks
    the actions (issues rooted at the body); all issues go out in one 422. Then
    `EngineRequest::runMacro` → `MacroRunResult`; `queue_full` → 503 busy (not recorded).
    `Field::Kind::NullableString`/`Any` were added to checkObject for it. queue_full goes through
    sendCoreError (503 busy). The trigger is checked with core's `isMacroTrigger()`
    (domain/trigger.h). Open (core's call): compileMacro taking the whole request.
  - Core limits (documented lightly): macros together hold at most 32 of the 64 action nodes
    (past that a press or /macros/run is refused like a full lane: 503 busy, not recorded); a
    binding/macro of only waits is `sequence_only` at .../0/command_id; macros take at most 5
    of the 8 run-history slots, and a running run is never evicted.
- **State checks (activity-runs branch; docs/design/state-checks.md).** The wire change is two
  step results: `skipped` (error `already_set`, a duration equal to the read time) and `no_reply`
  (error null, about 1000 ms, up to about 1300 when the step was also checked). Neither counts in
  failed_steps, and a run with only ok, skipped and no_reply steps is `completed`. A skipped step
  has no value field (the lead's decision). The 1 s limit lives in core's HA adapter.
  - Fixture: list.json's newest run has a skipped step 0 (TV), a no_reply step 1 (Sonos) and a
    failed step 6.
  - Contract `activity_runs_flow` (hub-sim):
    - It runs after mute_toggle_flow and first asserts that ha_main is connected. Earlier in
      noauth HA isn't connected, so every step fails with `adapter_unavailable`.
    - A `switch()` helper sets up `/sim/ha`, POSTs, advances 6 s and resets.
    - The state check uses `state=<denon>:on&attr=<denon>:source=TV&slow=<denon>:5000`; Play
      Xbox then gives the Denon's power skipped/already_set, its input no_reply (1000-1500 ms),
      and the run is completed.
    - `slow=none` clears every slow entity (it doesn't take `<entity>:none`).
    - The fake's media players start "off".
- **Smart buttons (smart-buttons branch; docs/design/smart-buttons.md). Simplified on 2026-11-15
  after the board test:** a slot is only a device id. The hub derives the actions, and repeat is gone.
  - Routes in `smart_button_routes.cpp`: GET /smart-buttons gives `{slots, actions, revision}`.
    PUT takes `SmartButtonsInput` `{slots, revision}` (token, 4 KiB; sending `actions` back is
    `unknown_field` at /actions) and answers like GET. `actions` per slot comes from core's
    `smartSlotActions(device, catalogFor(ctx, device))` (engine/smart_actions.h, the table that
    used to be `fillDefaults`). A null slot gets `SmartSlotActions{}`. It's encoded with
    `config::encodeSmartActions`. GET /smart-buttons/actions?device_id= (`getSmartButtonActions`, token) previews one
    device's actions, nothing saved, for the web's instant preview on Global: 422 `required` or
    `invalid_id` at /device_id, 404 for an unknown device, empty presses for a non-HA device or
    another kind. The route shares `actionsFor()` with GET. Fixtures:
    smart_buttons/actions.{dev_family_room_lights,dev_lg_tv}.json, which the contract compares
    on every mode. I re-derive rather than reading the compiled slots: the compiled
    form isn't ActionSpec. GET /smart-buttons/defaults is removed (spec, policy row, route).
  - gen_fixtures.py writes only the actions it needs: `light_actions()` (the lights) and
    `no_actions()`, so it no longer mirrors core's table (/simplify). The other kinds are
    checked by ApiTest and the contract on hub-sim. Fixtures: get.json (light_1 =
    dev_family_room_lights, rev 7), replace.request.json and replace.json (switch_2 = dev_lg_tv,
    a kind with no actions), errors/smart_buttons_unknown_device.json. The backups carry
    `{slots: ids, revision}`, with no actions. hub-sim's showcase seed (core's) must match get.json.
  - Light +/- is tap only, `brightness_step` ±26 (10%). That was the user's call after the board test.
  - `/status` `selected_slot` comes from `RuntimeSnapshot::selectedSlot`.
  - Errors: an unknown device is `unknown_device` at /slots/<slot>. Deleting a slot's device is
    409 `device_in_use` with a detail at /smart_buttons/slots/<slot>, "Used by smart buttons"
    (`Referrer::owner`).
  - Backup `smart_buttons` is optional (missing = four empty slots, user-approved).
  - `cover.position_step` "Change position": `CommandDef::step` column; `step` param's data key
    is `position`; buildServiceCall adds round(state) and clamps 0..100, no numeric state →
    `ErrorCode::StateUnavailable` (`state_unavailable`, ExecuteResult.message). Offered whenever
    SET_POSITION is supported (review fix: a `reports_position` gate left covers saved earlier,
    including the daily-use hub's, without it until a re-link; it's gone from the snapshot).
  - Contract: `smart_buttons_flow` (all modes) covers the seeded slots and actions, the replace's
    actions against replace.json, 409, and `refused_at` for unknown_device and for `actions`
    sent back. It also checks the delete is refused, and selected_slot. The restore without
    smart_buttons (four empty slots) is in persistence_flow (hub modes only).
    `new_domain_actions` (sim only) sets Light 2 once, reads GET per kind, then clears it so the
    device can be deleted: the cover (plus a position_step execute 50→60), and input_boolean as
    other, then switch, then light. snapshot() includes /smart-buttons.
  - ApiTest: SmartButtonsReplaceWithRevisions, StatusShowsTheSelectedSlot,
    SmartButtonActionsByKindFromTheOfferedCommands (every kind and fallback, through PUT and GET).
  - `StateUse {Toggle, Step}` (capabilities.h); `ha::entityDomainName()`.
  - `kRequestNestingLimit` is 10 (lowered from 11 at the lead's request, in core's json.h, with
    the spec text). The deepest legal body is a binding's call.data, 9 deep. The extra level
    makes data one level too deep a 422 `too_deep` at its path, not a 400. Contract: "binding
    data too deep" in ha_action_flow. A restore parses as a file (kFileNestingLimit 12).
  - Docs: agent-api.md covers slots as ids, the derived actions, release, a hold that fires
    once, the +/- tap merge, and a global +/- binding that runs only while the selected slot has
    no +/- actions. ha-capabilities.md's Hue note now describes the engine's tap merge (no
    adapter queue).
  - Open: no fake-HA cover without SET_POSITION, so the cover fallback is ApiTest-only. The mock
    modes need the frontend's mock to serve the new routes, selected_slot and restore.

- **Stale coverage:** no API-level test drives `stale` yet. A fresh-hub-sim contract case
  (`/sim/ha?remove=`, discover, `GET /devices/{id}`, execute → `entity_not_found`) would cover
  it, but it must not run before a restart comparison.
- **Logs:** there's no check yet that no secret appears in any log line (security.md
  checklist).
- **mock-auth:** the contract's admin username is now `hubbo-owner`; the web mock's seeded
  admin must match (frontend), or mock-auth's login checks fail.
- **Later:** `text` selectors (they'd need a string ParamSpec type in core).
