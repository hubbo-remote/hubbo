# hubbo-core: working notes

Current as of the remote-editor phase, on branch `remote-editor` (device profiles and everything
before are committed). Design: `docs/design/device-profiles.md`;
the profile spec is `~/src/hubbo-devices/docs/spec.md`. This file covers state, decisions and
traps only.

## State

- hub-sim showcase (remote-editor): `hub-sim serve --showcase` (sim/src/showcase.*) is built
  from a REAL HA capture, never invented, and the capture is LOCAL ONLY: sim/fixtures/local/
  (gitignored by sim/.gitignore) holds real-ha-sample.json (states, platforms, /api/services) and
  real-ha-translations.json. The user doesn't want their HA data in the repo, so no entity id or
  name from it goes in any tracked file, tests included. Without the files, `--showcase` exits
  with a one-line message and SimShowcase skips. `FakeHaServer::enableShowcase()` (kept across
  reset) adds the captured entities, serving their state and attributes as is
  (`FakeHaEntity::attributesJson`), plus the captured services and translations.
  `showcaseConfig()` keeps the fixture's BLE Apple TV and the fixture HA devices whose entity
  isn't captured. It adds a device per captured entity, with the snapshot fetched by the real HA
  client and normalized (as registration does), the Fire TV Stick and SHIELD profiles, and
  `act_watch_tv` on the first captured player with sources (none: no activity). Fans are kind
  "climate" (there's no fan kind). The test checks generic properties only.
- HA service translations (remote-editor, uncommitted): entity details also send WS
  `frontend/get_translations` (en, services, `integration:[platform]`), and only when the entity
  has a platform. `HaEntityDetails::translations` holds the raw flat `resources` object, or "".
  It's optional: once registry, state and services are in (`detailsGathered_`), finishDetails
  turns any failure (timeout, error, send failure, dropped connection) into Ok with what was
  gathered. Only a cancel stays a cancel. hubbo-api's `ServiceDirectory::parse(json, platforms,
  translations)` consumes it. The fake HA serves it, filtered by integration prefix: in showcase
  mode from the local capture, and by default from a small inline table of
  the real strings for `baseServicesJson`'s services (the default fake reads no capture file).
  `answerTranslations = false` (reset on reset()) tests the timeout. Contract boundary field:
  sonos.set_sleep_timer sleep_time, with default 600, step 60, unit seconds and a translated
  description.
- Open: the original sim fixture (sim/src/fixtures.cpp, since the first hub-sim commit, mirrored
  in api/fixtures, core tests and web) has an LG TV whose entity id and name match the user's
  house. Renaming it spans every area; that's the lead's call.
- `ParamSpec::description` (HA field tooltip, set by hubbo-api's capabilities.cpp) and
  `ParamSpec::hasDefault` / `.shownDefault()`: `defaultValue` is shown as the API's `default`
  only when set (wait_connected's timeout_ms sets it).
- Device profiles (device-profiles, committed: 60c0e59, 19ca417, 69ba133):
  - `config/profile.h`: `parseProfile` (spec schema_version 1, strict: unknown fields are errors,
    `meta` dropped, `ir` is UnsupportedSchema at /platform), `ProfileLibrary` (index.json order,
    kinds.json, buttons.json kept raw; any issue leaves it empty + ValidationFailed),
    `builtinProfiles()` / `builtinProfilesStatus()`, `deviceFromProfile(profile, name, kind)`.
  - Built-ins are embedded from the repo's `profiles/` (synced by the lead's `bin/hubbo profiles
    sync`) by `core/tools/embed_profiles.py <dir> <out.cpp>`; CMake runs it into the build dir.
    The firmware's PlatformIO build must call the same script. sha256 comes from index.json; the
    hub never hashes. The library points into the embedded texts (`BuiltinProfile::json`,
    `buttons()`), never copies them; a test's ProfileFile texts must outlive its library.
  - A ble_hid device stores `profile:{id,revision,sha256}` plus manufacturer, model, commands
    (required) and buttons, power, timing, inputs, channels (domain `DeviceProfile`). HA devices:
    each is unknown_field. Internals shared with json.cpp: `config/json_fields.h`,
    `config/profile_fields.h`. `encodeDevice(..., DeviceDetail::Stored|Full|Summary)`: Summary
    drops commands etc., only Stored writes ha_capabilities.
  - Device rules are checked up front in createDevice/updateDevice (`checkDevice` in
    service.cpp), before anything is built, so a refusal is a 422 and a read-back failure stays
    Internal: kind in kinds.json (UnknownKind at /kind; on update only when the kind is set), a
    ble_hid device has a profile (Required at /profile) and no other does (UnknownField), and an
    update never changes provider.type (IntegrationMismatch at /provider/type). json.cpp checks
    only the kind's shape (malformed is UnknownKind too). Never on load or restore, so a kind dropped by an update blocks nothing.
  - Bluetooth catalog is per device (BleHidCommand order): press{command, hold_ms?},
    press_down/up (group remote), key_tap/down/up (keys), usage_tap/down/up {usage} (custom),
    wait_connected{timeout_ms 1000-60000, default 15000} (effect Wait). `CommandSpec` gained
    `group`, `data` (adapter's DeviceCatalog) and `timeoutMs` (executor deadline; 0 = the 5 s
    default). A Wait command is SequenceOnly in bindings and direct execute (the compiler's
    `misplacedEffect` holds the Hold/Wait placement rule).
  - Timed press (hold_ms, the step's else the command's) and wait_connected are Pending timers in
    the adapter (8 slots), finished by poll(). `IntegrationAdapter::cancel(ticket)`: the executor
    calls it for Pending work it gives up on (sequence cancelled, deadline passed), so a wait frees
    its slot and a timed press releases at once. releaseAll() releases only binding-held keys,
    never a timer's. `ExecRequest::answerAtStart` (set for SequenceOrigin::Api): a direct timed
    press answers Accepted at key-down and releases by itself, silently. A wait that times out completes Accepted with
    AdapterUnavailable, so the sequence goes on. `ExecutionObserver::onActionCompleted
    (CompletionRecord)` reports every Pending result; hub-sim logs those with an error
    (`ble dev_x wait_connected done -> accepted adapter_unavailable`).
  - Catalogs: one per distinct command list (shared across devices and compiles), never freed;
    grows per profile ever added, not per edit. The `command` param is labelled "Remote command".
  - Key table final: 98 keyboard (0x04-0x65 complete) + 24 consumer keys. Never reorder.
  - hub-sim seeds from api/fixtures/admin/backup.json, whose dev_apple_tv carries the full
    hubbo:apple-tv copy; `sim::bleDevice(..., profileId)` attaches a built-in profile.

- Several Bluetooth hosts (multi-bluetooth):
  - `integrations/ble_hosts.*` (replaces `ble_state.*`): `BleHostPort` (radioReady, status, start
    and cancel pairing, forget, retainOnly; all by device id), `BleHostStatus` {bonded, connected,
    pairing, windowRemainingMs}, `BleHostResult` {Accepted, NotReady, Busy (start only: another
    device's window), Full (request queue full: try again)}. One status mapping for the API,
    `bleDeviceState()` -> {not_paired|pairing|connected|not_connected, windowS rounded up}, plus
    `bleDeviceAvailability()`, `bleRadioState()`, `bleRadioDetail()`. No failed state: a cancel,
    timeout or full store keeps what was there (old host, or not paired). Re-bonding the same host
    is a success. A start for the device whose window is open is Accepted, no change.
  - Orphan hosts: `ConfigService::setListener(ConfigListener*)` (set before load) is told of every
    saved config the hub runs (intact record at load, every commit; never Blocked or unsaved).
    `BleHostReconciler` is that listener and calls `retainOnly(bleDeviceIds(config))`, so a host
    whose device is gone is forgotten at boot and after every commit. SimHub wires it; the
    firmware must too. retainOnly is never refused (firmware: a latest-wins slot, not a queue entry).
  - hub-sim keeps bonds across restarts like NVS: `SimHub(..., bonds)` / `ScenarioRunner(..., bonds)`
    take a separate Storage (main passes the raw --data disk, not the switchable medium) and
    write `ble_bonds` on every bond change; the runner defaults to its own memory, so a scenario
    `reboot` keeps bonds too. Only a start with no saved bonds, or `--reset`, gets the fixture's
    Apple TV bond. Without it, every restart re-bonded the fixture, so a host pruned by
    the config came back after a restart (the contract's restart checks caught it).
  - `HidTransport` is per device (`connected/sendKeyboard/sendConsumer(deviceId, ...)`).
    `BleHidAdapter` keeps held keys per device in `limits::kMaxBleDevices` fixed slots (no
    allocation on the engine task), frees a slot when nothing's held, drops a host's keys when its
    link is down (poll or execute), and releases every host on releaseAll (activity change).
  - At most 4 Bluetooth devices: the compiler adds `TooManyDevices` at
    `/devices/<i>/integration_id` for each ble_hid device past 4 (the 64-device cap is at `/devices`).
  - `ProviderSpec::peerName` is gone; a ble_hid provider is only its type, and `providerRef()` is
    the device id.
  - SimHub owns an `api::SimulatedFirmwareUpdate` (`firmware()`), advanced with hub time and
    replaced on factoryReset. It's passed as `ApiContext::firmware`. `POST /sim/firmware?fail=0|1&ready=0|1`
    sets failWrites/ready (400 without a valid one). When a simulated reboot finishes an update
    (Writing/Rebooting to Idle), `SimHub::onFirmwareRebooted` fires and SimApi clears every session,
    as the board's restart does. The reported version doesn't change (the lead's call: no API hook
    just for the sim; the board test checks the real version).
  - Factory reset (design end of section 3): SimHub holds an `api::SimulatedFactoryReset`
    (`factoryResetPort()`), advanced on hub time, passed as `ApiContext::factoryReset`. Its erase
    calls `SimHub::onFactoryReset` (like `onFirmwareRebooted`), which SimApi sets to
    `eraseEverything()`: `auth::eraseAllSecrets`, sessions cleared, `SimHub::wipe(&credentials)`,
    then `applyHomeAssistantSettings` again (no token: not_configured). `wipe()` is the board's
    wiped restart: `stopEverything()` (shared with /sim/reset: keys released, nothing active or
    saved active, remote unpaired), BLE unbonded (not `resetBle()`), the config record's slots
    removed, then `boot(ConfigSpec{}, credentials)` through bootHub() (the firmware's defaults:
    built-ins only, generation 1; `lastBoot()` reports it). `/sim/reset` gives a fresh port.
    `POST /sim/factory-reset?ready=0|1`. Without a `secrets` store (tests that don't pass one), the
    login isn't erased. Not to confuse: `SimHub::factoryReset()` is /sim/reset (seed config, bonded BLE).
  - Auth trim applied in sim: no login limiter, no clientKey, no Host allowlist (any Host is
    served; a foreign Origin on an unsafe method is 403 csrf_failed, test ForeignOriginIsRefused),
    SimulatorNoAuth only sets the simulator principal. ApiContext is built positionally in
    sim_api's `context()`, with `firmware` last.

- Phases 2 and 3 are committed. Phase 4 core work is green on `bin/hubbo test`, `tsan` and `asan`:
  - `integrations/websocket_transport.h`: the port (connect, send, close, `poll(timeout)`, and
    callbacks only from inside poll). Firmware implements it with esp_transport_ws, ws:// or wss://.
  - `integrations/json_scan.*`: a structure-only JSON scanner (member lookup; `valueStart()`, which
    finds where a value begins without scanning it; a one-pass Cursor over array elements or object
    members) for walking big results element by element. A big message is scanned once, by the
    walk; get_services is trimmed in place, never copied.
  - `integrations/ha_client.*`: the HomeAssistantClient.
    - auth, command ids and ping/pong;
    - backoff from 1 s doubling to 60 s; call_service;
    - discovery (design 3b): `config/entity_registry/list_for_display`, then every state through
      `subscribe_entities` (first event, then `unsubscribe_events`), both walked in steps. No
      services. Candidates are every allowlisted state, joined with the registry (YAML-only
      entities have platform "");
    - entity details for one entity (registration, refresh): `config/entity_registry/get`
      (not_found means YAML-only), `subscribe_entities` for that id, then `get_services` (moved out
      unparsed). Runs alongside a discovery. A completed fetch adds the entity to the stale set's
      "known" list until the next discovery;
    - any-task mailboxes for both (`Mailbox<T>`: request, take, cancel, publish by generation).
      Results from an older generation (cancelled, or from before new settings) are never
      published. A cancel frees the slot at once;
    - `entityStale()`; an optional Clock, read fresh in poll() and every transport callback (a slow
      TLS connect doesn't eat deadlines); `verifyTls` reaches `transport.connect(url, ConnectOptions)`.
  - `integrations/ha_adapter.*`: the HomeAssistantAdapter for `ha_main`. Catalogs come from
    `DeviceSpec::haCapabilities`; `execute` pushes into an SPSC ring (Pending, or AdapterUnavailable
    at once); `service()` runs on the integration task; results come back through a second ring.
  - `ErrorCode::EntityNotFound` ("entity_not_found"): HA doesn't know the target entity.
  - `DeviceSpec::haCapabilities` is stored as `ha_capabilities` (json.cpp); the ConfigService drops
    it for non-HA devices. The backup `remote` block is checked with `api::isValidRemoteEndpoint`.
- hub-sim:
  - `sim/src/fake_ha.*`: FakeHaServer and FakeHaTransport, in-process. They never touch a real HA.
    The token is "fake-ha-token" and is redacted in `received()`.
  - SimHub always owns the real client over the fake server. `realHomeAssistant` (SimHub,
    ScenarioRunner) also makes the real adapter the engine's `ha_main`. `hub-sim serve` turns it on,
    so the contract and e2e go through snapshot, buildServiceCall, client and the fake server.
    Scenarios and the engine tests use the scripted FakeHomeAssistant (immediate results, `ha latency`,
    `ha fail`).
  - In real mode, ScenarioRunner seeds snapshots with `addFakeHaSnapshots()` (makeSnapshot over the
    fake server's entities and services).
  - HA is `not_configured` until a token is set. `--ha-token` stores the fake token at boot if no
    `ha_token` is stored.
  - The serve clock adds real elapsed time on each pass, so a `/sim/advance` jump never freezes it.
  - Simulated presses are real button packets into RemoteInput. SimHub is its EventSink (like the
    firmware's EventTap): it keeps the last event, then hands it to the engine, so /remote/status
    has `last_event` and `last_packet_age_ms` as on the board. A press without a release holds (the
    thumb stays down, keepalives sent) until a release or /sim/reset, which releases everything.
  - `/sim/reset` also resets the SimulatedRemote (in SimHub; `advance()` drives its scan clock), the
    fake HA (`FakeHaServer::reset()`: fixture entities, no faults) and BLE (bonded, connected). The
    client gets `reconnect()`: it reconnects at once and forgets stale entities.
  - The fake transport drops an open link once `server.reachable` is false. Callers only flip the flag.
  - SimRuntime in real mode: `integrationState("ha_main")` is the client's state;
    `deviceAvailability()` is stale, available or unavailable.
  - /sim routes:
    - `/sim/radio/found?channel=&address=`
    - `/sim/ha?reachable=&commands=&fail=&remove=&auth=reject|ok&connect=stall|ok`. With auth or
      connect, the client reconnects at once: auth_failed, or held in connecting.
    - Bluetooth (names fixed by the API contract): `/sim/ble/bond?device=&host=` (a host bonds
      in that device's open window, connected; 409 otherwise), `/sim/ble/link?device=&up=0|1`,
      `/sim/ble/radio?ready=0|1`; `/sim/advance` runs a window out.
    - `sim::SimulatedBleHosts` (fakes.h, `SimHub::bleHosts()`) is both the API's `BleHostPort`
      (`ApiContext::bleHosts`) and the adapter's `HidTransport`. It records `reports()` and
      `reportDevices()`. The hub starts with `kBondedDevice` (dev_apple_tv) bonded to `kBondedHost`
      and connected; /sim/reset restores that; a factory reset leaves no bonds.
    - Scenario verbs: `ble connected|disconnected [device_id]` (default every host) and
      `expect_hid [@device_id] <report>`.
  - The fake HA answers the lean protocol (list_for_display, entity_registry/get,
    subscribe_entities in HA's compressed form, unsubscribe_events, get_services) and has one
    YAML-only entity, `switch.patio_fountain` (`FakeHaServer::kYamlOnlyEntity`).
  - Real names and platforms (denonavr, sonos, webostv, xbox, hue) live with the entities in
    `fixtureHaEntities()` (fixtures.cpp). get_services has one integration service per selector
    kind: `denonavr.set_dynamic_eq`, `sonos.set_option` (bool), `sonos.set_sleep_timer` (int),
    `webostv.select_sound_output` (enum); `denonavr.get_command` is text, so it's skipped.
  - SimApi passes a SimulatedRemote, the hub's client and the secret store into ApiContext. It calls
    `api::applyHomeAssistantSettings` at construction.

## Decisions (and why)

- **No backwards compatibility before the first release** (user, 2026-11-10). No retired-field
  tolerance and no migration steps: a record or backup with an unknown field (`ca_cert_pem`,
  `peer_name`, runtime state such as `availability` or `bluetooth`, ...) is unknown_field, and an
  older `schema_version` is MigrationFailed (Blocked, never overwritten). The migration hook and
  the newer-schema -> Blocked path stay for later. `ensureBuiltinIntegrations` on load is an
  invariant, not a shim, and stays.

- **`ha_main` is a built-in integration**, like `system` and `ble` (`ensureBuiltinIntegrations`,
  `kHomeAssistantIntegrationId`). It's added with no URL and enabled to any config that lacks it: when
  created, loaded or restored. A Phase 2 record gets it at boot through the existing built-in upgrade,
  which saves once. Tests: `BuiltinHomeAssistant.*` in sim_test.cpp.

- **The engine never blocks.** The adapter's execute only touches the client's atomic state and a
  ring. The mutex in the adapter guards the catalog cache, which is shared between the config task
  (commandsFor) and the integration task (service). The engine task never takes it.
- **No queueing.** While disconnected, or when the ring is full, execute returns AdapterUnavailable at
  once. A connection drop fails everything in flight with AdapterUnavailable, and nothing is replayed.
- **Catalog storage is never freed** while the adapter lives (a `std::deque`). Compiled configs hold
  pointers into it. A changed snapshot gets new storage, keyed by entity_id plus the encoded snapshot.
  This is a small, bounded leak per edit, accepted at hobby scale.
- **Discovery parses in steps** of kParseStepEntities (25) per poll, with a yield hook between steps.
  `hasWork()` tells the loop not to sleep. Big texts are freed between phases. Registry entries are
  allowlisted by domain (list_for_display has no disabled ones); states are prefiltered by id. Candidates
  are capped at 2048 (a real house overflowed 256). Over the cap, entities are counted in
  `HaDiscovery::truncated`, never dropped silently. Stale checks use every allowlisted entity seen, not
  only the capped list. The old 3 MB discovery took about 28 s on the board (about 9 ms per KB); the
  lean one is about 0.8 MB (list_for_display 276 KB, all states 515 KB), so about 7 s expected. HA
  itself answers each in under 0.1 s. Details' services JSON (about 230 KB) goes to the API.
- **Subscriptions are always ended.** A subscription's result comes first and its first event
  carries the data; the client then unsubscribes. A job that ends early (cancel, timeout, new
  settings) unsubscribes what it still has open. The fake counts open ones (`subscriptions()`).
- **Stale is runtime only, never saved, and discovery is the only source.** Real HA answers
  call_service on a nonexistent entity with success (`[]`), not an error, so a call result can't
  detect it. Stale means missing from the latest completed discovery (`entityStale()`). It's cleared
  by a later discovery that finds the entity, or by new settings. The adapter refuses a stale entity
  in `service()` with EntityNotFound, before anything is sent. `haDeviceAvailability()`
  (ha_adapter.h) is the shared stale/available/unavailable rule for the firmware and the sim, and
  `haIntegrationState()` is the shared integration state ("disabled" first).
- **The adapter's capability storage never shrinks, by design.** CommandSpec pointers outlive compiled
  configs: queued and in-flight executor work and held keys survive a publish, and EngineRequest and
  the adapter's Call carry them too. So the sets can't be freed when a config retires. Growth is one set
  per capabilities change, not per compile. Freeing them for real would need an engine quiescence
  signal; that's not built.
- **`ha_capabilities` exists only in the stored shapes.** decodeDevice accepts it only in the
  Stored/Backup shapes; Register and decodeDeviceUpdate answer unknown_field.
  `encodeDevice` writes it only with `DeviceDetail::Stored`.
- **Who closed it decides the backoff.** applySettings() and auth_invalid set the state before closing
  (Unreachable with an immediate retry, NotConfigured, or AuthFailed). onClosed() backs off only from
  Connecting, Authenticating or Connected, meaning the link dropped by itself. New settings also clear
  the stale entities, since they may point at another HA.
- **Disabling an integration never fails validation.** Its actions compile to Unavailable, and
  unpairedHolds() pairs a hold with its release on Adapter or Unavailable alike (adapter, device,
  release command, params): pairing is a property of the bindings.
- **Referrers carry the activity's name** (`Referrer::activityName`, "" for the global bindings), so
  the API's "Used by ..." messages need no lookup.
- **Timeouts:** the client's command timeout is 5 s, the same as the executor's action bound. The
  executor's fires first, so the API shows `status: timeout`, `adapter_timeout`. The client's late
  answer for that ticket is ignored.

## Gotchas

- `buildServiceCall` needs every param typed (`value.type == spec.type`), even optional ones left
  absent. In tests, fill params with `spec.params[i].defaultValue`, as resolveParams() does.
- FakeHaTransport delivers replies on the next `poll()`. A test's integration "tick" is:
  transport.poll, client.poll(now), adapter.service, transport.poll. SimHub::serviceIntegrations()
  loops this while `hasWork()`.
- The fake server records a call_service in `calls()` even when `answerCommands=false`. As real HA
  does, a call to an entity missing from `entities()` still answers success. To make an entity stale
  in a test: `removeEntity()`, then run a discovery.
- Fixture `ha_main` has url `http://homeassistant.local:8123`. `api::homeAssistantSocketUrl` turns it
  into ws://.../api/websocket, and FakeHaTransport only accepts ws:// or wss://.
- ApiContext is an aggregate under `-Werror=missing-field-initializers`. Every new field breaks every
  brace-init, so sim_api.cpp builds it through the `context()` helper.
- `RebootRoundTrip.RandomSequences` runs 300 seeds, but only 30 under tsan/asan (detected by
  `__SANITIZE_*__` / `__has_feature`); `HUBBO_ROUNDTRIP_ITERATIONS=N` overrides. It's the slowest test.
- While working, run `bin/hubbo test -R <filter>`; tsan and asan run once at phase end.
- Build only through `bin/hubbo test|tsan|asan|sim` (they lock the build dirs). `-R <regex>` passes
  through to ctest. Run servers in the foreground; `api/contract.sh` manages its own hub-sim.
- Avoid Arduino macro names (bit, min, max, abs) in core. Use `sim::bind`, not `std::bind`, with ADL
  around sim types.

## Open items

- Measure the lean discovery and an entity-details fetch on the board (expected about 7 s and about
  2 s). The firmware logs `elapsedMs`.
- Firmware: pass the client a millis Clock and use `haIntegrationState()` in EngineRuntime (check).
- In real mode, `ha latency` and `ha fail` scenario verbs are refused; only `ha available|unavailable`
  works. Use /sim/ha for faults.
- Firmware: implement `BleHostPort` and the per-device `HidTransport`, and use the
  `ble_hosts.h` status functions (hubbo-firmware's job).
- Firmware: call embed_profiles.py from PlatformIO; override `onActionCompleted` in its
  EngineLog to log results with an error (the wait_connected timeout). Relayed via the lead.
- At the phase commit: /commits with only this area's files, staged by path.
