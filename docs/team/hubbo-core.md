# hubbo-core: working notes

Current as of Phase 3 (Home Assistant), on branch `phase-2`, not yet committed. See
`docs/design/phase3-home-assistant.md` for the design. This file covers state, decisions and traps only.

## State

- Phase 2 (persistence, ConfigService pipeline, bootHub, hold ledger) is committed.
- Phase 3 core work is done and green on `bin/hubbo test`, `tsan` and `asan`:
  - `integrations/websocket_transport.h`: the port (connect, send, close, `poll(timeout)`, and
    callbacks only from inside poll). Firmware implements it with esp_transport_ws, ws:// or wss://.
  - `integrations/json_scan.*`: a structure-only JSON scanner (member lookup, ArrayCursor) for walking
    multi-MB discovery results element by element.
  - `integrations/ha_client.*`: the HomeAssistantClient.
    - auth, command ids and ping/pong;
    - backoff from 1 s doubling to 60 s;
    - call_service;
    - chunked discovery: registry, then states, then services;
    - an any-task discovery mailbox (request, take, cancel) with a generation: results from an older
      generation (cancelled, or from before new settings) are never published;
    - `entityStale()`;
    - an optional Clock, read fresh in poll() and every transport callback, so deadlines set after a
      slow transport.poll() (a TLS connect) count from the real time;
    - `verifyTls` goes through configure() to `transport.connect(url, ConnectOptions)`.
  - `integrations/ha_adapter.*`: the HomeAssistantAdapter for `ha_main`.
    - Catalogs come from `DeviceSpec::haCapabilities`.
    - `execute` pushes into an SPSC ring and returns Pending, or AdapterUnavailable at once.
    - `service()` runs on the integration task.
    - Results come back through a second ring.
  - `ErrorCode::EntityNotFound` ("entity_not_found"): HA doesn't know the target entity.
  - `ca_cert_pem` is retired: the stored and backup decoders accept and drop it (old boards' records
    must load, or the hub boots Blocked), and it's never written. `verify_tls` stays.
  - `DeviceSpec::haCapabilities` is stored as `ha_capabilities` (json.cpp).
    - The ConfigService drops it for non-HA devices.
    - The backup `remote` block is checked with `api::isValidRemoteEndpoint` (channels 0–124).
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
  - `/sim/reset` also resets the SimulatedRemote (the scan stops and the endpoint is cleared) and the
    fake HA (`FakeHaServer::reset()`: fixture entities, no faults). The client gets `reconnect()`, which
    goes through applySettings(): it reconnects at once and forgets stale entities. The
    SimulatedRemote lives in SimHub: `advance()` drives its scan clock and factoryReset() resets it.
  - The fake transport drops an open link once `server.reachable` is false. Callers only flip the flag.
  - SimRuntime in real mode:
    - `integrationState("ha_main")` is the client's state;
    - `deviceAvailability()` is stale, available or unavailable.
  - /sim routes:
    - `/sim/radio/found?channel=&address=`
    - `/sim/ha?reachable=&commands=&fail=&remove=`
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
  allowlisted by domain and disabled ones skipped; states are prefiltered by the registry. Candidates
  are capped at 2048 (a real house overflowed 256). Over the cap, entities are counted in
  `HaDiscovery::truncated`, never dropped silently. Stale checks use every allowlisted entity seen, not
  only the capped list. `elapsedMs` is there for the board log (the real house took about 28 s). The raw services JSON is handed to the API, which frees it after normalizing.
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
- Build only through `bin/hubbo test|tsan|asan|sim` (they lock the build dirs). `-R <regex>` passes
  through to ctest. Run servers in the foreground; `api/contract.sh` manages its own hub-sim.
- Avoid Arduino macro names (bit, min, max, abs) in core. Use `sim::bind`, not `std::bind`, with ADL
  around sim types.

## Open items

- e2e `storage.spec.ts` "backup is refused with a clear 503" fails against hub-sim. By the spec, storage
  failing after a good load exports the running config (200); only a load that couldn't read storage
  gives 503. The mock answers 503 for any storage_unavailable. That's the frontend's or a spec decision,
  not core.
- Firmware: pass the client a millis Clock and use `haIntegrationState()` in EngineRuntime.
- API: pass `integration->verifyTls` to `client.configure()`. Until then, the sim test
  RealHa.VerifyTlsReachesTheTransport fails. Check `HaDiscovery::generation` against
  `discoveryGeneration()` in the DiscoveryCache.
- The API can drop `rejectSnapshot` and the strip in `putDevice`, and pass `withCapabilities=false`
  to `encodeDevice`.
- Remote addresses are parsed with `api::parseRemoteAddress`, in both json.cpp (backup: exactly 10
  digits) and sim_api.cpp.
- In real mode, `ha latency` and `ha fail` scenario verbs are refused; only `ha available|unavailable`
  works. Use /sim/ha for faults.
- At the phase commit: run /simplify, then /commits with only this area's files, staged by path.
