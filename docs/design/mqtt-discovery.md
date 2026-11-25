# Hubbo in Home Assistant over MQTT

Hubbo's HA integration lets the hub call into HA, but that doesn't make Hubbo show up in HA. MQTT
discovery does: with a broker set up, Hubbo appears in HA as a device, so automations and dashboards
can see and change the current activity, turn it off, check whether it's on, and show basic stats.

The model is HA's own Harmony integration: an activity `select` whose options are `PowerOff`
followed by the activity names. There's no remote entity and no way to send commands or remote
presses over MQTT; MQTT has no `remote` platform, and HA reaches devices directly anyway.

## What HA gets (one device, device-based discovery)

- **Activity** (`select`): options are `PowerOff` followed by activity names. Choosing one starts it;
  `PowerOff` turns everything off.
- **Power** (`switch`):
  - ON while any activity runs. It goes OFF when off is chosen anywhere: the select's `PowerOff`,
    the remote, or the web admin.
  - Switching it OFF turns everything off.
  - Switching it ON starts the last activity that ran, as the Harmony integration's `turn_on` does.
    If nothing has run since boot, it stays OFF and its state is republished.
- **Restart** (`button`, `device_class: restart`, `entity_category: config`): restarts the hub
  through the same path as `POST /admin/restart`, so it's refused while a firmware update runs.
- **Diagnostic sensors** (`entity_category: diagnostic`): firmware version, board, IP address, MAC
  address, hostname, and Booted (`device_class: timestamp`, computed by HA from uptime, because the
  hub has no wall clock).
- **Device card:** the name follows the hub name. Also `sw` (version), `connections` (MAC) and
  `configuration_url` (`http://<hostname>.local/`).
- **Availability:** a retained last-will message, so HA shows the device unavailable when the hub
  drops off.
- **HA restarts:** when HA's `homeassistant/status` reports `online`, the hub resends discovery and
  states.

## Topics

- **Node id:** `hubbo_<12 hex of the Ethernet MAC>`. It doesn't change on rename and is also the
  client id.
- **Discovery:** `homeassistant/device/<node>/config`, retained. One abbreviated message, about
  1.6 KB, and at most about 2.8 KB with 16 activities. A test pins it at 3584 B or less.

| Topic | Payload | Retained |
|---|---|---|
| `hubbo/<node>/availability` | `online`, or `offline` (the LWT and a clean disconnect) | yes |
| `hubbo/<node>/activity` | the activity's label, or `PowerOff` | yes |
| `hubbo/<node>/power` | `ON` / `OFF` | yes |
| `hubbo/<node>/diag` | `{"version","board","ip","mac","hostname","uptime_s"}` | no |
| `hubbo/<node>/cmd/activity` (a label or `PowerOff`), `cmd/power` (`ON`/`OFF`) and `cmd/restart` (`PRESS`) | sent by HA | (retained messages are ignored) |

- **Labels:** activity names, deduplicated (` (2)`). An activity named "PowerOff" becomes
  "PowerOff (2)".
- **Mapping back:** the bridge maps labels back to ids through its current table. An unknown label
  is ignored, and the bridge republishes the state.
- **Order on connect or HA birth:** discovery, then `online`, then the states and diag about 1 s
  later. Diag is resent when the IP or hostname changes.

## Design

- **Core (host-tested)** follows the WebSocket transport's pattern:
  - `core/include/hubbo/integrations/mqtt_transport.h` is the port: connect with a will, publish,
    subscribe, disconnect and `poll()`. Callbacks are delivered only inside `poll()`. The transport
    owns reconnecting.
  - `core/include/hubbo/integrations/mqtt_bridge.h`: the discovery payload, labels and node id;
    `MqttBridge`, which watches the active activity, maps HA commands to transitions (posted, never
    blocking), and takes activity-list and hub-name changes from any task under a mutex. It runs
    `poll()` on its own task.
- **The last activity** for Power ON is the most recent non-empty active id the bridge has seen,
  in RAM only.
- **HA command to engine:** `api::runTransition()` (`core/include/hubbo/api/transitions.h`) is shared
  by the activity routes and MQTT. The firmware posts it to httpd, which owns the engine's submit
  and the config service; hub-sim calls it directly.
- **Settings:** the built-in integration `mqtt_main` ("MQTT", disabled by default) with `host`,
  `port` (1883) and `username`. The password is a secret (`mqtt_password`), write-only like the HA
  token: `password` in, `has_password` out, omitted keeps it, `null` clears it. Changing the host
  clears it unless a new one comes with it. A `host:port` is refused; the port has its own field.
- **Firmware:** an `EspMqttTransport` on the bundled esp-mqtt. Its event handler copies each event
  into a queue, and `poll()` delivers it on Hubbo's `mqtt` task (core 0, priority 4, PSRAM stack),
  which never writes flash. Outbox and buffers are in PSRAM; inbound data is capped at 256 B.
- **The broker** is plain MQTT on the LAN with a username and password, no TLS. The host can be an
  IP or a hostname; `.local` names resolve. Anyone who can publish to the broker can switch
  activities ([security.md](../security.md)).
- hub-sim has a fake broker (retained messages, wildcards, LWT, down and refuse-auth modes) with
  `GET /sim/mqtt` and `POST /sim/mqtt/publish`, so the contract and e2e tests run the whole flow.

## Known behaviour

Disabling MQTT leaves the device in HA showing unavailable; delete it in HA.
