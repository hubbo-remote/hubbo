# Hubbo in Home Assistant over MQTT

## Context
- **The goal:** make Hubbo appear in Home Assistant as a real device, so people can use it in automations.
  - The current activity is visible and can be changed.
  - It can be turned off.
  - "Is it on?" can be checked.
  - Basic stats are shown.
- **Why MQTT:** Hubbo's existing HA integration only lets the hub call into HA. It doesn't make Hubbo show up in HA. MQTT discovery is how devices do that.
- **Dropped by the user (2026-11-13):** a remote entity, sending commands, and physical remote presses. MQTT has no `remote` platform.
- **Harmonizer** (local spike) was read for reference. It had no activity entity, and its availability and last-will never worked, so it serves mostly as a list of what not to copy.
- **The model** is HA's own Harmony integration: an activity `select` whose options are "PowerOff" followed by the activity names.
- **Docs to update:** IMPLEMENTATION_PLAN.md:10 bars an MQTT dependency without approval. The user has now approved it, so the lead updates that doc and ARCHITECTURE.md:93.

## What HA gets (one device, device-based discovery)
- **Activity** (`select`): options are `PowerOff` followed by activity names. Choosing one starts it; `PowerOff` turns everything off.
- **Power** (`switch`): automations can read it and act on it.
  - Its state is ON while any activity runs. It goes OFF when off is chosen anywhere: the select's `PowerOff`, the remote, or the web admin.
  - Switching it OFF turns everything off.
  - Switching it ON starts the last activity that ran, as the Harmony remote's `turn_on` does. If nothing has run since boot, it stays OFF and its state is republished.
  - There's no separate off button.
- **Diagnostic sensors** (`entity_category: diagnostic`):
  - Firmware version, Board, IP address, MAC address, Hostname.
  - Booted (`device_class: timestamp`). HA computes it from uptime, because the hub has no wall clock.
- **Device card:** the name follows the hub name. Also `sw` (version), `connections` (MAC) and `configuration_url` (`http://<hostname>.local/`).
- **Availability:** a retained last-will message, so HA shows the device unavailable when the hub drops off.
- **HA restarts:** when HA's `homeassistant/status` reports `online`, the hub resends discovery and states.
- **Restart** (`button`, `device_class: restart`, `entity_category: config`): restarts the hub through the same path as `POST /admin/restart`. That path is posted to httpd, and like the route it's refused while a firmware update runs.
- **Not included:** an Ethernet link sensor, which could only ever show ON.

## Topics
- **Node id:** `hubbo_<12 hex of the Ethernet MAC>`. It doesn't change on rename and is also the client id.
- **Discovery:** `homeassistant/device/<node>/config`, retained. It's one abbreviated message, about 1.6 KB, and at most about 2.8 KB with 16 activities. A test pins it at 3584 B or less.

| Topic | Payload | Retained |
|---|---|---|
| `hubbo/<node>/availability` | `online`, or `offline` (the LWT and a clean disconnect) | yes |
| `hubbo/<node>/activity` | the activity's label, or `PowerOff` | yes |
| `hubbo/<node>/power` | `ON` / `OFF` | yes |
| `hubbo/<node>/diag` | `{"version","board","ip","mac","hostname","uptime_s"}` | no |
| `hubbo/<node>/cmd/activity` (a label or `PowerOff`), `cmd/power` (`ON`/`OFF`) and `cmd/restart` (`PRESS`) | sent by HA | (retained messages are ignored) |

- **Labels:** activity names, deduplicated (` (2)`). An activity named "PowerOff" becomes "PowerOff (2)".
- **Mapping back:** the bridge maps labels back to ids through its current table. An unknown label is ignored, and the bridge republishes the state.
- **Order on connect or HA birth:** discovery, then `online`, then the states and diag about 1 s later. Diag is resent when IP or hostname changes.

## Design
- **Core (host-tested)** follows the existing WebSocket pattern (`core/include/hubbo/integrations/websocket_transport.h`):
  - `core/include/hubbo/integrations/mqtt_transport.h`: the port. It covers connect with a will, publish, subscribe, disconnect and `poll()`, and callbacks are delivered only inside `poll()`. The transport owns reconnecting.
  - `core/include/hubbo/integrations/mqtt_bridge.h` and `core/src/integrations/mqtt_bridge.cpp`:
    - `activityLabels()`, `mqttDiscoveryPayload()` and `mqttNodeId()`;
    - `MqttBridge`, with hooks for the activity-change counter, the active activity id, live info, and a transition that's posted and never blocks;
    - `configure()`, `setActivities()` and `setHubName()` (callable from any task, guarded by a mutex);
    - `state()` and `detail()` for the status line, reusing the `IntegrationState` names;
    - `poll()` on its own task.
- **The last activity** for Power ON is remembered by the bridge: the most recent non-empty active id it has seen. It's held in RAM only and lost on reboot.
- **Activity changes:** the bridge polls the existing `activityChanges` atomic (firmware/src/main.cpp:103) every pass, at 50 ms or less, then reads `Engine::activeActivityIdShared()`. There's no new observer slot.
- **Restart:** the bridge's `restart` hook posts to httpd, which calls the same restart function the `/admin/restart` route uses. That way the "refused during a firmware update" rule is shared. In hub-sim it's the sim's restart.
- **HA command to engine:** a new `api::runTransition(ConfigService&, Runtime&, activityId)` in `core/include/hubbo/api/transitions.h`, pulled out of `transition()` in core/src/api/activity_routes.cpp so the route and MQTT share it. Firmware posts it to httpd, because engine submit and ConfigService are httpd-only. The sim calls it directly.
- **Activity list and name changes:**
  - A small config-listener fan-out in main.cpp and in SimHub calls `bleReconciler` and then `mqttBridge.setActivities(...)`.
  - The `HubSettings` rename callback calls `setHubName`.
- **Settings:**
  - A new integration type `mqtt`, with a built-in `mqtt_main` ("MQTT", disabled by default).
  - `IntegrationSpec` gains `host`, `port` (1883) and `username` (core/include/hubbo/domain/config.h, core/src/domain/config.cpp, the core/src/config/json.cpp allowlist).
  - The password goes in SecretStore under `mqtt_password`, following the `ha_token` pattern. It's write-only (`password` in, `has_password` out), omitted keeps it and `null` clears it, like the HA token. Changing the host clears it unless a new one comes with it.
  - `applyMqttSettings()` next to `applyHomeAssistantSettings()` (core/include/hubbo/api/api.h).
- **Firmware:**
  - `firmware/src/mqtt_transport.{h,cpp}` is an `EspMqttTransport` on the bundled esp-mqtt.
    - The event handler copies each event into a FreeRTOS queue, and `poll()` delivers it on our task.
    - It publishes with `esp_mqtt_client_enqueue`, with `out_size` 5120, an 8 KB outbox (both in PSRAM), inbound data capped at 256 B, keepalive 15 and auto-reconnect.
  - A new `mqtt` task on `IntegrationTask` (formerly `HaTask`, now named per task): core 0, priority 4, PSRAM stack. It never writes flash and does no TLS.
  - The MAC comes from `EthernetLink::mac()`, which is on main from hub-name.
- **The broker** is plain MQTT on the LAN with a username and password. No TLS. The host can be an IP or a hostname; `.local` names resolve (LWIP mDNS queries are on). A `host:port` is refused; the port has its own field.

## Work by agent (branch `mqtt` off main)
0. **Lead:**
   - Write docs/design/mqtt-discovery.md.
   - Update IMPLEMENTATION_PLAN.md and ARCHITECTURE.md.
   - Relay the interfaces.
   - Firmware first spikes esp-mqtt: it includes and links `mqtt_client.h`, then reports the MQTT task's core and priority, the buffer defaults, and the mDNS-query flag.
1. **hubbo-core:**
   - The config fields and built-in.
   - `mqtt_bridge`.
   - `sim/src/fake_mqtt.{h,cpp}`: a fake broker with retained messages, `+`/`#` matching, LWT, and down and refuse-auth modes.
   - SimHub wiring.
   - `GET /sim/mqtt` and `POST /sim/mqtt/publish` for tests.
   - Unit tests:
     - golden discovery JSON and the size bound;
     - label dedupe;
     - command mapping (`PowerOff`, an unknown label, a retained command ignored);
     - the Power switch (OFF turns off; ON starts the last activity, or with none does nothing and republishes OFF);
     - a resend on the birth message and on reconnect;
     - LWT;
     - rename and activity edits republishing;
     - `auth_failed`, `unreachable` and disabled.
2. **hubbo-api:**
   - Extract `runTransition`; the existing tests stay unchanged.
   - GET/PUT `/admin/integrations/mqtt_main`, `applyMqttSettings`, and the status entry.
   - OpenAPI (the `mqtt` type and `MqttSettings`), fixtures and contract cases: the password is never echoed, `has_password`, and the status state.
   - docs/agent-api.md, docs/security.md (anyone on the broker can switch activities) and docs/ha-capabilities.md if relevant.
3. **hubbo-firmware:**
   - `EspMqttTransport`, the mqtt task, and main.cpp wiring: the config fan-out, the rename hook, transitions posted to httpd, and EngineRuntime state.
   - A boot log line.
4. **hubbo-frontend:**
   - `web/src/components/admin/MqttForm.tsx`, built from HomeAssistantForm.tsx: host, port, username, a write-only password, enabled, and the status line.
   - A card in web/src/pages/Integrations.tsx.
   - The mock.
   - `web/e2e/mqtt.spec.ts` (save, password write-only, status reaches connected on hub-sim).

The phase ends the usual way: /simplify, then the lead's independent review, then the gates, then the board test, then the commit when the user asks.

## Verification
- **Gates:** `bin/hubbo gate core`, `gate api` (all four contract modes), `gate web` and `gate fw`.
- **hub-sim:** the contract and e2e check the retained discovery and states through `/sim/mqtt`, then publish an activity choice and see the hub switch.
- **Board** (remotely, against the user's HA Mosquitto add-on):
  1. Set the broker in the MQTT card; the status line reaches connected.
  2. The Hubbo device appears in HA with all its entities.
  3. Choosing an activity in HA switches the hub, and an activity started from the web admin shows in HA within about 50 ms.
  4. Choosing `PowerOff` or switching Power off turns everything off, and both entities follow. Switching Power on starts the last activity.
  5. A hub rename and an activity edit update HA.
  6. After an HA restart, the states come back.
  7. A bad password shows auth failed.
  8. Restart from HA reboots the hub, and HA shows it unavailable, then back.
  9. The loop gap stays about 2-3 ms, the heap stays steady, and the watchdog doesn't fire.
- **Watching the traffic by hand:** `docker run --rm --network host eclipse-mosquitto mosquitto_sub -h <broker> -u .. -P .. -t 'hubbo/#' -t 'homeassistant/device/#' -v`.

## Risks
- **esp-mqtt runs its own task.** If it isn't on core 0, priority 4 still yields to the RF loop, and firmware judges whether that's OK. The fallback is a small MQTT 3.1.1 client in core.
- **Disabling MQTT** leaves the HA device showing unavailable. The user deletes it in HA.
