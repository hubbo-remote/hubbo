# hubbo-core: working notes

Current as of Phase 5, on branch `phase-5`, not yet committed (Phase 4 is merged). Design:
`docs/design/phase5-appliance.md`. This file covers state, decisions and traps only.

## State

- Phase 5 (design section 4, plus the hub-sim side of section 2):
  - `integrations/ble_state.*`: the shared ble_hid mapping. `bleIntegrationState(spec, pairState,
    hostConnected)` ("disabled" first), `bleIntegrationDetail(...)` (the board's old bleText lines),
    and `bleDeviceAvailability(...)`. hub-sim uses all three (SimRuntime). The firmware should map
    BleHid::State onto them: Off is NotReady, NotPaired is Idle/Failed, Waiting is Paired with the
    link down, Connected is Paired with the link up. hub-sim's ble_hid devices now report
    available/unavailable, not unknown.
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
  - `ca_cert_pem` is retired: the stored and backup decoders accept and drop it (old boards' records
    must load, or the hub boots Blocked), and it's never written. `verify_tls` stays.
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
    - `/sim/ble?connected=0|1` runs the scenario verb `ble connected|disconnected`.
    - `/sim/ble/bonded?peer=` completes an open BLE pairing window (`api::SimulatedBlePairing`, in
      SimHub as `blePairing()`, passed as `ApiContext::blePairing`). `advance()` runs the window
      timeout on the hub clock, like the remote scan.
    - The pairing port is the one source of the bond: RecordingHidTransport::connected() needs
      Paired, and SimRuntime's BLE state follows the board's (`not_configured`, `connecting`,
      `connected`, `disconnected`, `error`). `/sim/ble?connected=` is only the host link. The hub
      starts bonded to `SimHub::kBondedHost`, and factoryReset() bonds it again.
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
  `encodeDevice(..., withCapabilities=false)` is for API responses.
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
- Firmware: switch engine_runtime's bleText to `integrations/ble_state.h` (hubbo-firmware's job).
- At the phase commit: /commits with only this area's files, staged by path.
