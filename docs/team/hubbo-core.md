# hubbo-core: working notes

Current as of the smart-buttons branch (ha-remote, setup-fixes and everything before are committed).
Design: `docs/design/smart-buttons.md`; the profile spec is `~/src/hubbo-devices/docs/spec.md`.
This file covers state, decisions and traps only.

## State

- Smart buttons (smart-buttons, uncommitted; docs/design/smart-buttons.md). Simplified 2026-11-15
  after the board test: a slot is only a device id; nothing else is configurable. Repeat is gone.
  - Model: `domain/smart_buttons.h` (`SmartSlot`, `smartSlotName`, `smartSlotForButton`); `SmartButtonsSpec` {slots: vector<optional<string>> always sized 4,
    revision} in domain/config.h; `ConfigSpec::smartButtons`. The old slot object (presses, repeat)
    is just invalid now (no compat).
  - Derivation: `engine/smart_actions.h` `smartSlotActions(DeviceSpec, CommandCatalog)` ->
    `SmartSlotActions` {button, plus, minus} of `SmartPressActions` {tap, hold, release}. The table
    moved verbatim from api's old fillDefaults (light on/off, ±26 tap; fan on/off, speed; switch
    on/off; cover open/close, position_step ±10 + hold open/close + release stop, else open/close
    taps). Only commands the catalog offers; non-HA or other kinds: none. api uses it for GET's
    read-only actions with `config::encodeSmartActions`.
  - JSON: `decodeSmartButtons` / `encodeSmartButtons` ({revision, slots: {light_1: "dev_x"|null,
    ...}}, all four required). In config.json and backups `smart_buttons` is OPTIONAL (the user's
    one tolerance); absent = four nulls with the config's revision. Present, revision required in
    config.json, optional in backups. Nesting: request 10 (api's change; a binding's call.data is 9,
    +1 for too_deep), file 12 (11 + 1); both checked by the owner.
  - Compile: a slot's device must exist (unknown_device / invalid_id at /smart_buttons/slots/<slot>,
    the PUT re-roots to /slots/<slot>). Actions come from smartSlotActions against the device's
    adapter catalog and compile as bindings (no hold-effect commands). A device that fails its own
    checks gives no actions (its issue is under /devices). `CompiledSmartPress` {tap, hold, release}.
  - Service: `replaceSmartButtons(spec, base)` (own revision). Deleting a slot's device is refused
    like any reference: device_in_use, referrer path `/slots/<slot>` (owner SmartButtons; api
    prefixes /smart_buttons). Slots have no actions in forEachAction any more.
  - Engine: resolution per button (activity binds any trigger > the slot press if `set()` >
    globals), decided at Down and kept in `pressed_[button]` until the next Down; selection on Down
    of a slot button the activity doesn't bind; `selected_` atomic, Light 1 at boot. `holdFired_`
    gives `release` on the Up after Hold. `SequenceOrigin::SmartButton`; its Up never sets
    upSequence_.
  - Tap merge: the compiler marks + and - taps (`Compiler::smartPress`): a single tap action with an
    int `step` param sets `CompiledAction::stepParam` and `CompiledSmartPress::merges`. The first
    such Tap copies that action with its device into `step_.command` (a self-contained
    CompiledCommand); later taps only count. `kSmartStepWindowMs` 300 after the last Tap, poll()
    sends it via `Executor::enqueueCommand` with step × taps, clamped to the param's min/max. No
    timer while that button is down (`held_`). Flushed at once by a tap the other way, a slot
    change, a Hold of + or -, a config change (syncConfig, before the new config is used: always
    as tapped, never what the slot holds after the save) and `endActivity()` (the one teardown
    shared by transition() and syncConfig's "active activity gone"; after cancelAll, under the new
    generation, so it isn't dropped). The executor knows nothing about merging.
  - Known and accepted (lead review 2026-11-15): a save that empties a cover's slot mid-hold
    skips its Stop (the release is read from the running config by slot and press, as documented).
    Capturing it at Hold would need another copied command per button; left.
  - Not taken from /simplify (2026-11-15): `holdFired_` duplicating RemoteInput's (would need an
    `afterHold` on RemoteEvent, outside this phase); slots derived by `kind` not entity domain (as
    the design and api's old fillDefaults); `smartPress()` out-params.
  - Showcase: light_1 = the first captured dimmable light's id. fixtureConfig() has four nulls,
    revision 1.
  - Tests: core/tests/smart_buttons_test.cpp (SmartButtonsConfig.* incl.
    ActionsComeFromTheDeviceKindAndItsCommands on a hand-built catalog: every kind, no brightness,
    no position, another kind, non-HA; SmartButtonsEngine.* with Light 1 = dev_lights and Switch 1
    = a cover, incl. the tap merge: count, clamp, window, paused while down, every flush, a fan's
    instant taps (a test-built fan snapshot), a config save sending pending taps as tapped),
    AdapterTest.PositionStep*.
  - cover.position_step: api's catalog; no or non-numeric state -> `ErrorCode::StateUnavailable`;
    the adapter passes it through. Fakes: `kFakeCoverPosition` 50, FakeHaEntity::position.
- HA remotes (ha-remote, committed; design docs/design/ha-remote.md, kept deliberately small). An
  earlier, bigger attempt (a text param type, kMaxParams 6, text plumbing) was thrown away as
  over-designed: don't bring any of it back. Core's part:
  - `serviceOnDevice` (compiler.cpp) is api's `ha::isDeviceService(entityId, platform, service)`,
    the one rule (snapshot decode uses it too): the platform's services, plus `remote.*` on a
    `remote.*` device even with no platform (YAML-only). Other devices unchanged.
  - Client: get_translations' integration list is api's `ha::serviceDomains(entityId, platform)`
    ({platform} plus "remote" on a remote), and the request is skipped only when that's empty.
    showcase.cpp and addFakeHaSnapshots pass the same list to `ServiceDirectory::parse`.
  - Fake HA: `remote.living_room_tv` ("Living Room TV", platform demo, features 0, state on; api's
    NEW_DOMAIN_ENTITIES); the `remote` domain's get_services entries and component.remote.*
    translations copied verbatim from the real HA capture (generic HA strings, no personal data).
    The showcase picks the remote up as a fake-only domain.
  - Tests: AdapterTest.ARemoteTakesTheRemoteDomainsServices, ARemoteCommandIsSentAsWritten, the
    new-domains round trip (turn_on, toggle, and the three custom services with HA's labels),
    ClientTest.EntityDetailsAreFetchedForOneEntity (remote and YAML-only remote translations),
    SimShowcase.EveryCapturedEntityIsADevice.
- Home Assistant action (setup-fixes, docs/design/ha-custom-action.md): `ActionSpec::call`
  (`HaServiceCallSpec` {service, data as compact JSON text}). `config::decodeHaServiceCall()` is
  the one checker (config decode, backup/restore and api's execute route): pattern via api's
  `ha::isHaServiceId`, data object, `kMaxHaCallDataBytes` 1024 (too_long), depth 4 (too_deep),
  top-level target keys (forbidden_param, api's `ha::isTargetKey`), `{{`/`{%` anywhere (template_not_supported, first
  one). decodeAction: call required with `call_service`, unknown_field on any other command.
  New ErrorCodes: pattern, forbidden_param, too_deep, template_not_supported.
  `kRequestNestingLimit` is 11 now (smart buttons; an activity binding alone needed 9).
  - Compile: `CompiledAction::call` is a `HaServiceCallRef` (shared_ptr<const HaServiceCallSpec>,
    domain/config.h), made by the compile and owned by the compiled config or direct
    CompiledCommand, plus every copy still carrying it (executor nodes, `ExecRequest::call`, the
    adapter's Call until the call is built). No global store: a dry run's or replaced config's
    calls go with it. Executor work survives a config publish, so a raw pointer into the config
    would dangle. freeNode() resets a node's ref; the SPSC ring's pop moves out, so slots hold
    nothing. Gotcha: the engine task can drop the last ref (a queued, never-sent call_service
    whose config was replaced and reclaimed), a rare free on the engine task, never an allocation.
  - Domain rule: the call's service must be `<device's snapshot platform>.*` (webostv.* on a
    webostv TV), else forbidden_param at .../call/service (config) or /call/service (direct
    execute, ValidationFailed). Not the exact custom_services list (re-link trimming would orphan
    bindings). Budget: `kMaxHaServiceCalls` 16 per
    config (capacity_exceeded at .../call). Hints: devices() sums customHintBytes as it goes and
    flags the device that first crosses `ha::kMaxConfigHintBytes` (capacity_exceeded at
    /devices/N/ha_capabilities/custom_services); api's `ha::configHintBytes` gives only the total,
    so the compiler keeps its running sum. `compileDeviceCommand(..., call)` and
    `ConfigService::compileCommand(..., call)` take the execute body's call.
  - `ExecRequest::call`; the HA adapter keeps the stale check, then api's `buildCustomCall`;
    the client sends `ServiceCall::rawData` verbatim as service_data.
  - Fake HA: webostv.button (text `button`) and select_sound_output (text) on webostv players,
    as real HA's services.yaml; any service is recorded in `last_call`.
- Fresh-hub defaults (setup-fixes): `factoryDefaultConfig()` (domain/config.h) is the built-ins plus
  one global binding, `kOffTrigger` "off:tap" -> system.off. It's the defaults for the board's
  first boot and factory reset (firmware passes it to bootHub; hub-sim's wipe() uses it). An
  ordinary binding: a stored config is never touched, no migration. `/admin/reset`'s fallback
  (api's backup_routes.cpp, `ctx.factoryDefaults` unset) and `backup.empty.json` are api's.
  `emptyConfig()` (run-nothing, service.cpp) stays empty on purpose.

- MQTT discovery (mqtt, committed). Hubbo is an HA device over the user's broker.
  - Port `integrations/mqtt_transport.h` (WebSocket pattern: callbacks only in poll(), the transport
    reconnects, clean session so resubscribe on each onConnected). Firmware: esp-mqtt.
  - `integrations/mqtt_bridge.{h,cpp}`: device-based discovery on
    `homeassistant/device/<node>/config` (Activity select, Power switch, Restart button,
    diag sensors), retained LWT on `hubbo/<node>/availability`, cmds on `hubbo/<node>/cmd/+`,
    resend on HA's `homeassistant/status` "online". Node id `hubbo_<mac hex>` (sim:
    hubbo_02000000534d). Labels dedupe with " (2)"; an activity named PowerOff is "PowerOff (2)".
    Power ON starts the last activity seen (RAM only). `detail()` never contains the host (API
    caps detail at 120; hosts go to 253).
  - Config: IntegrationSpec host/port/username (mqtt only; the scrub clears them elsewhere),
    built-in `mqtt_main` (disabled). `Fields::integer()` and `Fields::text(valid)` added.
    `ConfigFanOut` in service.h. The field rules live in domain/ids.h: `isValidMqttHost` (no
    host:port, i.e. exactly one ':'; IPv6 ok) and `isValidMqttUsername`, used by decode and api's PUT.
  - Integrations cap: `limits::kMaxIntegrations` (adapters + system + mqtt_main), checked by decode
    and compile via `integrationCountWithBuiltins()`, so ensureBuiltinIntegrations can't make a
    list the next decode rejects.
  - A connect() that refuses to start is Unreachable and retried after `kConnectRetryMs` (5 s).
    `mqttMacText(mac)` is the colon form (diag, `cns`); firmware switches its macText to it.
  - hub-sim: `FakeMqttBroker`/`FakeMqttTransport` (sim/src/fake_mqtt.*), SimHub wires the bridge
    (config fan-out after the BLE reconciler, rename hook, restart via api::requestHubRestart),
    SimApi sets `hub.runTransition` (api::runTransition), calls api::applyMqttSettings at boot and
    after eraseEverything, and serves GET/POST /sim/mqtt and POST /sim/mqtt/publish. SimRuntime
    forwards mqtt_main state/detail to the bridge.
  - `MqttBridge::detail()` is only for problems (unreachable, auth_failed); "" while connected or
    connecting, as HA's client (the state alone says it).
  - POST /admin/restart (hub_routes.cpp) goes through api::requestHubRestart; the null-port 503
    keeps its own message.
  - Tests: core/tests/mqtt_bridge_test.cpp (golden discovery, size bound, commands, power, birth,
    LWT/reconnect, states, detail cap), ConfigJson.MqttSettings,
    ServiceTest.MqttIntegrationSettings, SimApi.MqttDrivesTheHub / MqttRestartButton.
- More HA domains (ha-domains, committed; design `docs/design/ha-domains.md`): scene, script,
  cover, button, input_boolean, humidifier, select. Remote and climate were dropped by the user
  (2026-11-13), as were locks. So was send_command: free text has no param type.
  - The client already filters by `ha::entityDomain()`, so new EntityDomain values (api's) need no
    client change. `entityInfo()` also reads min/max_humidity, available_modes and options.
    `HaCandidate::deviceClass` (the state's device_class) feeds api's `ha::suggestedKind`.
  - Fake HA: `FakeHaEntity::deviceClass`; info-built states emit the new attributes (humidity only
    when min < max, api's "not reported" rule) and use `FakeHaEntity::state` when set.
    call_service answers any service already. `fixtureHaEntities()` (with `state` and
    `deviceClass`) has one made-up entity per new domain, mirroring api/tools/gen_fixtures.py
    NEW_DOMAIN_ENTITIES (a drift guard: the contract registers each and checks capabilities and
    suggested_kind). Change both together.
  - Showcase: kinds come from `ha::suggestedKind(domain, platform, device_class)` (kindFor is
    gone); a captured entity outside the allowlist is skipped; a domain the capture lacks (the
    humidifier) gets the fixture's made-up entity. The capture (58) nearly fills the
    64-device cap, so the fixture's HA devices are no longer kept (only its BLE Apple TV), the
    light buttons bind the first captured light, and over the cap the showcase fails with a message.
  - Tests: ClientTest.TheNewDomainsAttributesAreRead and
    AdapterTest.TheNewDomainsRoundTripAndCallTheRightServices, both over the fixture's made-up
    entities (addFakeHaSnapshots, then a config round trip, then execute, then the fake's call line).
- Agent docs (agent-docs, committed). The hub describes itself to LLM agents:
  - `core/tools/embed_docs.py <api/openapi.yaml> <docs/agent-api.md> <docs/llms.txt> <out.cpp>`
    (PyYAML, in the toolchain venv) generates `agent_docs.cpp`, defining
    `hubbo::api::docs::kOpenApiJson` and `kAgentGuide` (`api/agent_docs.h`: raw and gzip bytes, a
    quoted ETag from the raw bytes) and `kLlmsTxt` (the text, NUL-terminated).
    CMake runs it into the build dir. The firmware runs the same script from a PlatformIO
    pre-script. Both copies are stored (about 180 KB of flash).
  - `api/docs_routes.cpp`: the public GET routes `/api/v1/` (an index, with no hub name),
    `/api/v1/openapi.json` and `/api/v1/agent.md`. The two docs send gzip only when
    Accept-Encoding has it, always with `Vary`, an ETag and a 304. `/llms.txt` is a Public
    *host* route: `Router::addHostRoute` takes an `auth::Requirement` now (admin by default).
    The texts of llms.txt (docs/llms.txt) and the index are hubbo-api's.
  - Hints (http/route.cpp, done here with the lead's OK): `jsonError(..., docs)` adds
    `error.docs`. The router stays API-agnostic: `addDocsRoutes` calls
    `Router::pointErrorsTo("api", docs::kAgentGuidePath)`, and the router then sets it on a 401
    unauthenticated and on a 404 for an unknown path under /api (/api included), and nowhere
    else. The doc paths live in agent_docs.h.
  - `http::statusLine(int)` ("304 Not Modified") is the one status table, used by hub-sim and
    the firmware. An unknown status reads as 400 or 500 by its class.
  - embed_docs.py emits string literals (the faster compile) through embed_profiles.py's
    `literal()` and `write_if_changed()`, and uses libyaml's CSafeLoader when it's there.
  - hub-sim has no web admin, so its `GET /` (a Public host route in sim_api) answers with the
    llms.txt text (`docs::sendLlmsTxt`).
  - The sim seeds (`fixtureConfig`, the showcase) bind the way the web admin writes a Press. A
    device command passes straight through: BLE as a key_down/key_up pair, an HA command once
    on `:down`. Hubbo's own actions (Off, activate) go on `:tap`, and a Long press on `:hold`.
  - On the board, /llms.txt must be registered with httpd before the web admin's `/*` (firmware's).
- Hub name and restart (committed on hub-name, PR #11). `config/hub.h`:
  - Default name (hub-name branch, uncommitted; docs/design/hub-name.md):
    `HubSettings(store, defaultName)`, where the platform passes `defaultHubName(mac)` ("Hubbo 3F2A",
    the last two MAC bytes). The board uses the Ethernet MAC; hub-sim uses `sim::kSimMac`, so its
    name is "Hubbo 534D". load() with nothing saved or with an unreadable value, and forget(), use
    it. A saved plain "Hubbo" is kept. kDefaultHubName/kDefaultHostname are only HubIdentity's bare
    default (no HubSettings) and hostnameFor's fallback.
  - Advertised hostname: `HubIdentity::advertisedHostname` ("" = unknown; never set on hub-sim),
    `shownHostname()` (advertised, else hostname) and `hostnameConflict()`.
    `setAdvertisedHostname()` (any task) takes only the identity mutex, never renameMutex_: the
    board's onChanged waits on the mDNS task, whose callback calls it. rename() clears it before
    onChanged, so a report made during onChanged stays. load() and forget() clear it.
    GET/PUT /admin/hub (hub_routes.cpp) send `{name, hostname: shownHostname(), hostname_conflict}`.
  - Storage: `HubSettings` keeps the name in the StateStore under `kHubNameKey` ("hub_name"),
    never in the activity config or backups. On the board that's the RecordStateStore on
    LittleFS (firmware main.cpp), and in hub-sim the record store. `HubSettings::forget()`
    removes it (`StateStore::remove`, which uses `RecordStore::remove`) and goes back to the
    default. Every hub-sim reset path calls it: the pinned reset (`serve --reset`, `/sim/reset`),
    `wipe()` (factory reset) and the unpinned scenario reset. A sim reset also drops a pending
    restart (`SimulatedRestart::reset()`). `/status hubbo.name` is always present, as the
    default without HubSettings.
  - Limits: `kHubNameMaxChars` 32 code points, `kHubNameMaxBytes` 128. `kMaxStateValueLength`
    was raised to 128 for it.
  - `trimHubName` / `checkHubName` (Required, TooLong, or InvalidName for non-UTF-8, C0/C1/DEL or
    untrimmed) and `hostnameFor` (`kHostnameMaxLength` 63, default "hubbo").
  - `load()` reads the saved name. On Ok it's used. On NotFound, or an unreadable or invalid
    value, the hub uses the default, and nothing is written over what's saved.
  - `rename()`: trim, check, save, then `onChanged` on the caller's task (httpd). Renaming to the
    same name is Ok with no save and no `onChanged`, but only when the in-memory name is known to
    match the store (`matchesStore_`). After an unreadable load it still saves.
  - Restart: `api/restart_port.h` (`RestartPort`, `SimulatedRestart`). In hub-sim a restart
    clears the sessions.
  - Routes (assigned to core by the lead, though core/src/api is hubbo-api's): `hub_routes.cpp`.
    GET/PUT `/admin/hub` returns `{name, hostname}`; an invalid name is 422 at /name.
    POST `/admin/restart` returns 202 `{}`, 409 during a firmware update, 503 when the port
    refuses or is missing.
  - `/status` reports `hubbo.name`, and `network.hostname` is live from HubSettings, its only
    source (`HubInfo.hostname` was removed).
  - Tests: HubName.*, HubSettings.*, ApiTest.HubNameAndRestart, SimApi factory reset.
- Binding fallback is per BUTTON (button-press). If the active activity binds any
  trigger of a button to at least one action, none of that button's global bindings apply (an
  unbound kind is simply unbound). Globals apply only to buttons the activity leaves alone, so a
  pass-through `:down`/`:up` never runs with a global `:tap`/`:hold`. It's precomputed as
  `TriggerTable::bound` (a ButtonSet) in the compiler, so `CompiledConfig::lookup` costs one mask
  test. An empty action list doesn't count as bound. The hold ledger still matters when the active
  activity changes during a global hold. Tests: CompilerTest.AnActivityBindingMasksTheWholeButtonsGlobals,
  EngineTest.AnActivityPassThroughMasksTheGlobalTap and
  EngineTest.AGlobalHoldIsReleasedWhenAnActivityTakesOverTheButton.
- hub-sim showcase (remote-editor): `hub-sim serve --showcase` (sim/src/showcase.*) is built
  from a REAL HA capture, never invented, and the capture is LOCAL ONLY: sim/fixtures/local/
  (gitignored by sim/.gitignore) holds real-ha-sample.json (states, platforms, /api/services) and
  real-ha-translations.json. The user doesn't want their HA data in the repo, so no entity id or
  name from it goes in any tracked file, tests included. Without the files, `--showcase` exits
  with a one-line message and SimShowcase skips. `FakeHaServer::enableShowcase()` (kept across
  reset) adds the captured entities, serving their state and attributes as is
  (`FakeHaEntity::attributesJson`), plus the captured services and translations.
  `showcaseConfig()` keeps the fixture's BLE Apple TV and adds a device per captured entity, with the snapshot fetched by the real HA
  client and normalized (as registration does), the Fire TV Stick and SHIELD profiles, and
  `act_watch_tv` on the first captured player with sources (none: no activity). The test checks
  generic properties only.
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
    - `readAttribute(entityId, attribute, done(ErrorCode, optional<ServiceValue>))`: one attribute
      read fresh (Kind::ReadState: subscribe_entities for that id, first event, unsubscribe),
      `kStateReadTimeoutMs` 2 s; the adapter then sends the call with what's left of
      kCommandTimeoutMs (`callService(..., timeoutMs)`, `client.nowMs()`), so both fit the 5 s. Ok+nullopt when the
      attribute is left out/null/object/array; EntityNotFound when the event has no state for the
      id. Runs alongside calls, discovery and details (`isDetails()` keeps forgetPending off it);
    - any-task mailboxes for both (`Mailbox<T>`: request, take, cancel, publish by generation).
      Results from an older generation (cancelled, or from before new settings) are never
      published. A cancel frees the slot at once;
    - `entityStale()`; an optional Clock, read fresh in poll() and every transport callback (a slow
      TLS connect doesn't eat deadlines); `verifyTls` reaches `transport.connect(url, ConnectOptions)`.
  - `integrations/ha_adapter.*`: the HomeAssistantAdapter for `ha_main`. Catalogs come from
    `DeviceSpec::haCapabilities`; `execute` pushes into an SPSC ring (Pending, or AdapterUnavailable
    at once); `service()` runs on the integration task; results come back through a second ring.
    A command with `EntityCapabilities::stateAttribute()` (mute_toggle -> is_volume_muted) reads
    it first, then `buildServiceCall(..., &state)`: a failed/timed-out read still sends (unknown
    = mute), EntityNotFound and AdapterUnavailable finish unsent. One result per ticket (the
    call's). No cache, no standing subscription (fast double presses may both read the old state).
  - `integrations/ha_catalogs.*` (HaCatalogs): the catalog store both the real adapter and the
    scripted FakeHomeAssistant use: snapshot catalogs cached per device, `keep()` for the fake's
    addEntity ones, `owner(command, entityId)` (one locked scan per call; the result is used
    unlocked, since kept capabilities never change or move) and static `buildCall(caps, ...)`
    (custom or service call). Never frees.
  - Client internals: every pending slot has one `Answer(ErrorCode, const std::string *text)`
    (null text on failure/timeout); callService and readAttribute wrap their callbacks in it;
    discovery and details have none and dispatch by Kind.
  - `ErrorCode::EntityNotFound` ("entity_not_found"): HA doesn't know the target entity.
  - `DeviceSpec::haCapabilities` is stored as `ha_capabilities` (json.cpp); the ConfigService drops
    it for non-HA devices. The backup `remote` block is checked with `api::isValidRemoteEndpoint`.
- hub-sim:
  - `sim/src/fake_ha.*`: FakeHaServer and FakeHaTransport, in-process. They never touch a real HA.
    The token is "fake-ha-token" and is redacted in `received()`. `FakeHaEntity::muted`: an
    answered volume_mute applies it, so subscribe_entities serves it (`setMuted`/`muted`).
    `/sim/ha?muted=<media player id>:0|1` sets it (400 otherwise); `/sim/ha` answers `last_call`
    (`lastCallJson()`: {service, entity_id, data} or null; reset() clears it).
  - The scripted FakeHomeAssistant keeps each call's data as the entity's attributes, so
    mute_toggle toggles in scenarios and engine tests too. Fixtures and showcase bind mute to
    `media_player.mute_toggle`.
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
- **Catalog storage is never freed** while the adapter lives (HaCatalogs' `std::deque`). Compiled
  configs hold pointers into it. A changed snapshot gets new storage, keyed by device id, entity_id
  and the snapshot.
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

- A header edited by another agent while a build compiles can leave an object that looks newer than
  the edit (ninja then skips it): odd segfaults and garbage in the native build while asan passes.
  `touch` the shared headers and rebuild.

- MQTT: liveInfo (it allocates on the board) is read only while connected, and once in
  onConnected so the first discovery carries the version and hostname. All five hooks are required
  and unchecked. `configure()` ignores settings equal to the current ones (api re-applies them
  after every integration PUT, restore and reset; a reconnect would blip HA's availability). In hub-sim the transition hook only
  queues: runTransition advances the hub, so calling it inside the bridge's poll would re-enter;
  SimHub::advance drains the queue at its end. A live publish reaches subscribers with retain
  clear (MQTT 3.1.1); only the copy sent on subscribe is retained.
- Discovery size: plain 64-byte names x16 with the longest hub name fit 3584 B; names made of
  characters JSON escapes (" and \) reach about 4.4 KB, over esp-mqtt's 4096 out_size.
- **The HTTP task's stack (8 KB, firmware raising it to 16 KB).** POST /admin/devices runs route →
  createDevice → add → commit → parseConfig (the full config) → decodeSections → decodeDevice →
  ha::decodeSnapshot. Keep big values off those frames: decodeList decodes into
  `out->emplace_back()` (never a `T` local; a DeviceSpec is about 600 B on Xtensa), decodeDevice
  decodes `ha_capabilities` in place, and createDevice, replaceIntegration, add() and replace()
  take `&&`, as does commit(). updateDevice edits inside the `next` copy and shares
  `commitItem()` with replace(). Paths come from `detail::child()` (std::to_chars), since
  std::to_string inlines a 240 B frame. Measure with Xtensa `-O2 -fexceptions
  -fstack-usage` (the firmware's flags; a host build is no guide). On 2026-11-12, createDevice went
  688 → 112 B, decodeList<DeviceSpec> 672 → inlined, decodeSections 336 → 160 and decodeDevice
  368 → 240. updateDevice (PUT, a re-link) was 1,248 B plus replace<DeviceSpec>'s 608 B; after
  the /simplify pass, createDevice is 272, updateDevice 224 and replace 224. The board test
  (a Sonos add) peaked at about 6 KB of the HTTP task's 16 KB. commit keeps its re-parse of
  the whole config (the hub runs what's saved).
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

- The fakes special-case state that calls write (volume_mute -> is_volume_muted,
  set_cover_position -> current_position) in two places; a shared table would scale better if
  more state-reading commands come.

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
