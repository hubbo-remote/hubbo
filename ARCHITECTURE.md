# Hubbo architecture

Hubbo is open-source software for a hub you build: an ESP32-S3 board with an nRF24 radio that
replaces the Logitech Harmony Hub for a Harmony Companion remote. The hub owns the devices,
activities and button maps, and runs them itself: infrared and Bluetooth work with nothing else on
the network, and Home Assistant devices sit next to them as peers. It's managed from a browser over
one HTTP API, which scripts, Home Assistant and LLM agents use too.

This is the map. The design docs in [docs/design/](docs/design/) hold the detail and the reasons;
[docs/development.md](docs/development.md) has the commands and the repo layout.

## Principles

- **The hub is the brain.** Activities, bindings and devices live on the hub and run without
  Home Assistant or any cloud. HA is an integration, not a dependency.
- **The remote comes first.** The radio-to-command path is precompiled, allocation-free and never
  waits on the network, flash or another task. Everything else (HTTP, HA, MQTT, updates) runs
  below it.
- **Registered devices only.** Every action names a registered device and a command from that
  device's catalog, with typed, checked params. No raw HA service calls, entity ids or payloads.
- **Read state, don't track it.** Hubbo keeps the active activity and nothing else about the
  room. When it needs a device's state it asks HA at that moment.
- **Simulator first.** The real core runs natively in `hub-sim`, so most work and every test
  happens without hardware.
- **Hobby scale.** One hub, one home, a trusted LAN, one admin. Simple over hardened.

## The parts

| Part | Where | What |
|---|---|---|
| Core | `core/` | Portable C++17: engine, domain, config, storage, remote input, integrations, auth and the API routes. No Arduino, ESP-IDF or FreeRTOS headers; platforms inject clocks, storage, transports and adapters. |
| Firmware | `firmware/` | PlatformIO (Arduino on ESP-IDF) for the Waveshare ESP32-S3-ETH: the radio, Bluetooth HID (NimBLE), the IR transmitter, Ethernet, LittleFS and NVS, the HTTP server, MQTT, updates. Wiring only; behaviour lives in core. |
| hub-sim | `sim/` | The real core with fake adapters (Bluetooth hosts, HA, IR, MQTT broker) and deterministic time. Serves the real `/api/v1` on loopback, runs scenario scripts, and has `/sim/*` controls for tests. |
| Web app | `web/` | React + TypeScript + Vite + Tailwind. Built into the firmware and served by the hub; the browser does the rendering. A TypeScript mock API, seeded from the same fixtures, serves fast UI work. |
| API | `api/` | `openapi.yaml` is the source of truth for `/api/v1`. Fixtures and a contract suite run against hub-sim (with and without auth) and the mock. |
| Profiles | `profiles/` | The built-in device profiles and the spec's `kinds.json` and `buttons.json`, synced from [hubbo-profiles](https://github.com/hubbo-remote/hubbo-profiles). |

### Core modules

| Module | What |
|---|---|
| `remote/` | Decodes the remote's packets. The remote sends "everything held" per button group with keepalives about every 40 ms; `RemoteInput` diffs those sets into `down`, `up`, `tap` and `hold` per button and releases everything after 500 ms of silence. |
| `domain/` | Config types (integrations, devices, activities, bindings, smart buttons), triggers, ids, limits and error codes. |
| `config/` | JSON encode/decode, validation, profiles, and `ConfigService`: the commit pipeline and its states. |
| `engine/` | The compiler, the engine (active activity, trigger lookup, transitions), the executor and its lanes, the hold ledger, run history and the active-activity persister. |
| `integrations/` | The adapters: Bluetooth HID and hosts, IR, the HA client (WebSocket) and adapter, state checks, the MQTT bridge, the firmware release feed. |
| `ha/` | HA capabilities: domain allowlists, feature bits, service discovery and the device snapshot. |
| `storage/`, `auth/`, `api/`, `http/` | The record store, credentials and sessions, the route handlers, and the router. |

## Data model

**Integration.** `system`, `ble`, `ir`, `ha_main` and `mqtt_main` are built in and always present;
each has an enabled flag and its own settings. A disabled integration's devices compile to
unavailable.

**Device.** A registered target with a stable id (`dev_…`), a name, a kind and its integration.
- Bluetooth and IR devices are profiles that have been added: manufacturer, model, commands, power
  steps, timing and inputs, copied from the profile ([device-profiles.md](docs/design/device-profiles.md)).
- Home Assistant devices are one HA entity each, with a capability snapshot taken at registration:
  the domain's allowlisted commands gated by HA's feature bits, enum options from the entity's
  attributes, and the integration's own services ([ha-domains.md](docs/design/ha-domains.md)).
- Only the admin adds, edits and removes devices.

**Action.** `{integration_id, device_id, command_id, params}`, plus `call` for the Home Assistant
action ([ha-custom-action.md](docs/design/ha-custom-action.md)). The built-in `system.wait` takes
a bounded `duration_ms`; a Bluetooth device's `wait_connected` waits for its host. Commands declare
an effect: `momentary`, or `hold` (needs its release).

```json
{"integration_id": "ha_main", "device_id": "dev_receiver", "command_id": "media_player.volume_up", "params": {}}
{"integration_id": "ble", "device_id": "dev_streamer", "command_id": "press_down", "params": {"command": "DirectionUp"}}
{"integration_id": "ir", "device_id": "dev_tv", "command_id": "press", "params": {"command": "PowerOn"}}
{"integration_id": "system", "device_id": null, "command_id": "wait", "params": {"duration_ms": 1000}}
```

**Activity.** `{id, name, startup[], shutdown[], bindings, macros}`. A trigger is a button plus an
event kind (`down`, `up`, `tap`, `hold`); a binding maps a trigger to actions. Global bindings
apply under every activity unless the activity binds that button. Unbound triggers do nothing.

**Activity switching** (Harmony's rules, kept):
- Switching from A to B runs only B's startup, never A's shutdown, so power-on steps must be safe
  to repeat. Off runs the active activity's shutdown.
- Re-activating the active activity re-runs its startup ("fix it").
- Deleting the active activity switches to the one that takes its place in the list and runs its
  startup; with none left, the hub is off.
- The active activity survives a reboot: it's saved a few seconds after a switch (never from the
  RF path), and on boot its button map comes back without running its startup.

**Smart buttons.** Light 1/2 and Switch 1/2 each take an HA light, fan, cover or switch; the hub
derives what the button and +/- do from the device's kind, and +/- follow the last one pressed
([smart-buttons.md](docs/design/smart-buttons.md)).

**Macros.** A Press or Long press can be marked as a macro: a list of up to 32 actions run on its
own executor lane, so a macro's waits never hold up other buttons ([macros.md](docs/design/macros.md)).

**Runs.** One executor runs startups, shutdowns, macros and presses. A failed step is reported and
the sequence keeps going; a switch cancels the old run. The last 8 runs are kept in RAM for the
Activity Log ([activity-runs.md](docs/design/activity-runs.md)). In startups and shutdowns, an HA
step whose state HA already reports is skipped, and an HA step waits at most 1 s for HA's reply
([state-checks.md](docs/design/state-checks.md)).

## The hot path

**Compile the config; don't interpret it on a press.** At commit the config is validated
(registered devices, command ownership, param bounds, hold/release pairs) and compiled into compact
runtime tables: adapter indices, bounded params, and a `button × event → actions` lookup per
activity. The compiled config is swapped in atomically.

A press is: radio read → `RemoteInput` → index the active table → hand typed actions to the
executor and adapters. No JSON, flash, heap allocation, HTTP, name lookup or network wait. Network
work (HA, MQTT, update checks) runs on its own tasks through bounded queues; a full queue refuses,
it never blocks.

- **Holds:** the engine keeps a ledger of open holds per button and releases them on the button's
  up, a radio timeout, a failed or cancelled sequence, a disconnect or a transition, so a key is
  never left stuck.
- **Transitions** are serialized and generation-guarded, so a delayed step of an old startup can't
  run after a switch.
- **Latency:** radio read to Bluetooth send is about 0.5 ms median. The board budget below holds
  p95 under 2 ms and p99 under 5 ms while loaded.

## Integrations

| Integration | How |
|---|---|
| Bluetooth HID | The hub is a BLE keyboard and consumer-control device. Up to 4 hosts, each a device, all connected at once; keys go to the named device's host, never a "current" one ([multi-bluetooth.md](docs/design/multi-bluetooth.md)). |
| IR | One emitter on a fixed GPIO, sending Pronto codes from the device's profile via the RMT peripheral: taps, holds that repeat until release, toggle codes. Send only ([ir.md](docs/design/ir.md)). |
| Home Assistant (outbound) | A WebSocket client with a long-lived token. Discovery browses entities; only registered ones become devices. Commands are allowlisted per domain, plus the device-scoped Home Assistant action for services past the allowlist. The registry limits what Hubbo sends; it doesn't sandbox the HA token. |
| Home Assistant (inbound, MQTT) | Optional. With a broker set up, Hubbo publishes itself through MQTT discovery as one HA device: an activity select, a power switch, a restart button and diagnostics ([mqtt-discovery.md](docs/design/mqtt-discovery.md)). |
| Home Assistant (API) | Automations and scripts can also call the HTTP API with a token. |

## Profiles and the catalog

A device profile describes a device in an open format: its commands with normalized names, how
each is sent (Bluetooth HID usage or Pronto IR code), power and input steps, and timing. The spec,
the schema and all profile data live in [hubbo-profiles](https://github.com/hubbo-remote/hubbo-profiles)
(CC0), including the whole Harmony device archive converted into the format. The catalog is
published at [hubbo.tv/devices](https://hubbo.tv/devices/).

The web admin's Add device searches the catalog from the browser and installs the profile through
the ordinary import, so the hub makes no outbound request for it. A profile file can also be
imported directly ([catalog-hosting.md](docs/design/catalog-hosting.md)).

## The API

`/api/v1`, specified in `api/openapi.yaml`: strict JSON, bounded bodies, field-level errors, and
revisions with 409 on a stale edit. The web app, scripts, Home Assistant and LLM agents all use it.
[docs/api.md](docs/api.md) is the guide for token users and agents, also served by the hub at
`/api/v1/agent.md` with `/llms.txt`.

- **Execute** takes a device id, a command id and typed params, and reports `accepted`, `failed`
  or `timeout`, never a claim about the device's physical state.

## Persistence

- **Config** (integrations, devices, activities, bindings, smart buttons) is one JSON record; the
  active activity is another. Each record is two CRC'd slots on LittleFS; a save writes the older
  slot and verifies it before it's acknowledged, and a load takes the newest intact slot.
- **Secrets** (the password hash, API token hashes, the HA token, the MQTT password) are in NVS
  and never appear in responses, logs or backups. Bluetooth bonds are in NimBLE's NVS store.
- **The hub runs what's saved:** every commit serializes, parses back, compiles and saves before
  it goes live, and every object's revision is the config's generation.
- **Unreadable is never absent:** a config that can't be read or used blocks edits rather than
  being replaced with defaults.
- **Backup and restore:** one JSON file of the config, without secrets or bonds; a restore
  validates and commits atomically, and integrations and Bluetooth hosts may need setting up again.
  A reset clears the config and keeps credentials and pairings; a factory reset erases
  everything and returns to first-run setup.

Details: [persistence-and-contracts.md](docs/design/persistence-and-contracts.md).

## Firmware updates

A tag `vX.Y.Z` on `main` is a release: CI builds the image and publishes it on GitHub with a
manifest signed with ECDSA P-256, whose public key is built into the firmware. Each hub checks the
latest release daily or on demand and installs it only when the admin chooses. Every new image
boots on trial and rolls back unless it's healthy within 2 minutes. A `.bin` can also be uploaded
by hand, through the same install path ([firmware-updates.md](docs/design/firmware-updates.md)).

## The board

- **Waveshare ESP32-S3-ETH:** dual core, 16 MB flash, 8 MB PSRAM, W5500 Ethernet with optional
  PoE. ESP32-S3 Bluetooth is BLE only.
- **Radio:** an nRF24L01+ (or a PA+LNA module) on its own SPI bus. Hubbo listens on the address
  of an existing Harmony Hub, learned once at setup while the Hub's pair button is pressed. Pairing
  a remote with no Hub at all isn't supported.
- **IR emitter** on GPIO 1. Wiring for all parts: `firmware/README.md` and the
  [build guide](https://hubbo.tv/build/).
- **Tasks:** core 1 runs only the radio and engine loop. HTTP, network, Bluetooth, HA, MQTT, IR
  and updates run on core 0, below the loop. Flash writes happen only on the HTTP task.
- **Partitions:** two app slots for updates and rollback, LittleFS for the config, NVS for
  secrets and bonds.
- **Board budget:** `firmware/budget.json` sets the limits a build must meet on real hardware
  (loop gap, latency, heap, stacks, image size), checked with `bin/hubbo board-check`.
- **Network:** DHCP and mDNS (`hubbo-xxxx.local`, following the hub name; see
  [hub-name.md](docs/design/hub-name.md)). Plain HTTP.

## Security

Hubbo is for one home, on a trusted LAN, with one admin. It isn't exposed to the internet and
isn't multi-user. Effort goes to what matters there: never losing or corrupting config, a remote
that keeps working, and devices doing the right thing.

- One admin with a username and password created at first run (the first person to reach a fresh
  hub creates it); sessions are in RAM, so a reboot signs everyone out. API tokens are shown once
  and stored hashed.
- **Roles:** the admin (a browser session) can do everything. API tokens can read status, devices
  and capabilities, run registered commands, edit activities, bindings and smart buttons, and
  activate them. They can't register devices, configure integrations, pair, manage credentials,
  back up, restore or update firmware. The table is data in core and is checked against the spec.
- No HTTPS, secure boot or flash encryption. Someone with the flash can read hashes, not passwords.
- hub-sim runs without auth by default, for development. That bypass is compile-time and
  host-only; a firmware build that enables it fails.

Threat model, roles and details: [docs/security.md](docs/security.md).

## Diagnostics

`/status` reports connectivity, integration states, the config state and the last run. Dev builds
add the remote log (the last 5 minutes of packets, presses and their results;
[remote-log.md](docs/design/remote-log.md)) and a debug console with loop timing, latency
histograms, heap and task stats.

## Lineage

Hubbo builds on the RF protocol work in [soaresbrun0/Harmonizer](https://github.com/soaresbrun0/harmonizer)
and continues its AGPL-3.0 license. Ported code is marked at the top of each file.
