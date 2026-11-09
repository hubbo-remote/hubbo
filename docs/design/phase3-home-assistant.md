# Phase 3: Home Assistant integration and remote endpoints

Scope: a hobby device for one home on a trusted LAN (see ARCHITECTURE.md, "Scope and threat
model"). Keep it simple. Persistence and API rules from `persistence-and-contracts.md` still apply.

## 1. HA client (core, portable)

- **Protocol:** HA WebSocket API at `ws://<host>:8123/api/websocket`.
  - The server sends `auth_required`; we send `auth` with the long-lived token; it answers
    `auth_ok` or `auth_invalid`.
  - Commands carry an increasing `id` and get a `result` back:
    - `get_states`
    - `get_services`
    - `config/entity_registry/list`
    - `call_service` (`domain`, `service`, `target: {entity_id}`, `service_data`)
  - `wss://` is required: the home's HA is only reachable over https, behind a reverse proxy with a
    Let's Encrypt certificate. The firmware verifies it against the ESP-IDF certificate bundle.
    The stored URL is the base http(s) URL; `api::homeAssistantSocketUrl` turns it into
    ws(s)://…/api/websocket.
- **Transport port** (`WebSocketTransport`, provided by each platform): connect(url), send(text),
  close, plus received-text, connected and closed callbacks, all delivered on the integration's
  task. The firmware implements it on the ESP-IDF `esp_websocket_client`; hub-sim implements it
  against a fake HA server.
- **States:**
  - `not_configured` (no URL or token);
  - `connecting`;
  - `authenticating`;
  - `connected`;
  - `auth_failed` (bad token: don't retry until the config changes);
  - `unreachable` (reconnect with backoff, 1 s doubling to 60 s).

  These show in `/status` and `/integrations`.
- **Discovery** runs on demand (admin, from the web UI). It fetches states, services and the entity
  registry and builds a candidate list: entity_id, unique_id, name, domain, platform
  (integration), supported_features and attributes. It never runs on the RF path. Parsing streams
  or filters by domain to keep memory bounded; PSRAM is fine.

## 2. HA adapter (core)

- `HomeAssistantAdapter : IntegrationAdapter` for integration id `ha_main`.
- `execute` sends `call_service` for a compiled HA action. The target is always the registered
  entity, and the data keys come from the command's parameter schema (`ha/` builds the call). It
  returns **Pending**; the HA `result` completes it as Accepted or Failed, with a timeout of about
  5 s.
- While HA isn't connected: `adapter_unavailable` at once. No queueing or replay of old actions.
- Every press is sent at once, with no per-device queueing and no combining of steps (decided
  2026-11-09, after the board test). Quick successive relative steps, such as `brightness_step`
  on a group light, can collapse into about one step, because HA hasn't updated the brightness
  yet. Pressing at a slower rhythm works. Queueing would add bug surface and make the steps feel
  laggy and uneven.
- Never blocks the RF task. HA work runs on the integration task, and results return through the
  executor's completion path.

## 3. Capabilities (ha/)

- Built-in domain allowlist (media_player, light, switch), as today.
- **Integration-specific services**, resolved from `get_services` (decided 2026-11-08):
  - offer a service when its `target` filter matches the entity's integration, domain and
    features;
  - map field selectors to typed params: `select` → enum, `number` → int or number with range,
    `boolean` → bool, `text` → bounded string;
  - skip a service if any field uses an unsupported selector;
  - entity-specific target only.
- The capabilities are stored with the device at registration or refresh, so they work offline and
  don't need HA at compile time.

## 4. Registration (API)

- **Config:** `PUT /admin/integrations/ha_main` takes `{url, access_token}`. The token goes in the
  SecretStore and is never returned; responses show `has_access_token`. Changing the URL clears
  the token.
- **Discover:** `POST /admin/integrations/ha_main/discover` returns candidates (with a search
  filter). Admin only.
- **Register:** `POST /admin/devices` with provider `{type: home_assistant, entity_id, unique_id}`.
  It stores the capabilities snapshot.
- **Stale references:** stale is runtime status and is never saved.
  - HA answers `call_service` on a nonexistent entity with success (`[]`), not an error, so a
    call result can't detect a removed entity. Discovery is the only source.
  - A device is `stale` when its entity is missing from the latest completed discovery. It clears
    when a later discovery finds the entity, or when the HA settings change.
  - The adapter refuses a command to a stale entity before sending: it fails with
    `entity_not_found`, and nothing reaches HA.
  - Until a discovery runs, a removed entity's commands are reported `accepted`, because HA gives
    no way to know.
  - Discovery runs only on request: Discover, Refresh or Re-link in the web admin. It never runs
    on boot, on reconnect or on a timer (decided 2026-11-09).

## 5. Remote endpoint (API + firmware)

- `GET /admin/remote` shows the current channel/address and the radio state.
- `PUT /admin/remote/endpoint {channel, address}` sets a known remote's endpoint, with the same
  validation as the console's `radio set`.
- `POST /admin/remote/pair` starts a Hub scan (timeout 120 s); `GET` shows progress;
  `DELETE` cancels.
- These go through a runtime port that the firmware implements over `RemoteRadio`. hub-sim fakes
  it.

## 6. Gate

One activity on the dev board that drives the guest room Apple TV over BLE and the HA "Guest Room
Lights" (`light.guest_room_lights`) at the same time. The user accepted lights instead of the
plan's Denon/Sonos volume and source test (2026-11-09). Media-player volume and source are
covered in hub-sim only. Phase 3 is done when:
- the remote stays local and fast;
- HA being down doesn't affect BLE;
- an unregistered entity can't be targeted;
- the token never shows in any response, log or backup.

## Ownership

| Agent | Owns |
|---|---|
| Core | HA client and adapter, the transport port, hub-sim's fake HA server and its transport |
| API | `ha/` (selector normalization), integration config/discover/register endpoints, remote endpoint routes, spec, contract |
| Firmware | `esp_websocket_client` transport, HA task, SecretStore wiring for the token, the remote runtime port over `RemoteRadio` |
| Frontend | HA settings (token entry, never shown), discovery and add-device flow, remote endpoint/scan UI |
