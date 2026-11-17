# Harmony Hub Replacement — Architecture Specification
Version 0.1 • 2026-11-04 • Implementation handoff

## Product and hard requirements
Build a standalone Harmony Companion RF hub replacement on the Waveshare ESP32-S3-ETH. The ESP32 owns activities, button mappings, device registry and execution. Home Assistant is an outbound device-control integration, not the brain of the remote. The system must operate locally without Home Assistant for Bluetooth and, eventually, IR.

Non-negotiable: low RF-to-command latency; minimal external state; registered-device-only actions; one action abstraction for all triggers/sequences; API-first web/LLM access; one admin account plus restricted API tokens; responsive React/TypeScript web UI; OTA; backup/restore; release binaries and USB web flashing.

**MVP activity semantics:** Switching from A to B runs only B's startup sequence, never A's shutdown. Off runs only the active activity's shutdown sequence. The hub remembers the active activity but does not maintain guessed power/input states or a session-wide list of previously used devices. Xbox can remain powered after switching to Apple TV and self-sleep. Favor explicit power on/off and direct input commands over toggles.

**MVP scope:** one Harmony Companion remote; one Waveshare ESP32-S3-ETH board; Ethernet; RF; BLE HID Apple TV; Home Assistant outbound integration; registered devices; activities; web UI; security; OTA; backups; release automation. **Later:** IR database/import/transmitter, other boards, other remotes, gesture recognition beyond down/up, inbound Home Assistant integration.

## Hardware and build
Primary board: Waveshare ESP32-S3-ETH with W5500 SPI Ethernet and optional PoE. Planning specifications: ESP32-S3 dual core, 512 KB SRAM, 8 MB PSRAM, 16 MB flash; verify exact installed variant and flash partition scheme before implementation. ESP32-S3 supports BLE but not Bluetooth Classic. Verify experimental Apple TV HID implementation uses BLE. nRF24 and W5500 may share SPI resources: inspect actual wiring, chip selects, IRQs, bus locking, signal integrity, and radio power.

Base repository: https://github.com/soaresbrun0/harmonizer . Its `platformio.ini` specifies Arduino framework under PlatformIO; `src/portal.cpp` uses Arduino WebServer; `scripts/embed_portal.py` embeds HTML. Preserve existing RF protocol knowledge, pairing, button IDs and release semantics. The Ethernet/BLE spike is now available locally in `Harmonizer/` (see amendment 2026-11-08). Harmonizer is AGPL-3.0: retain notices and review redistribution/network obligations.

Use PlatformIO + Arduino framework initially for compatibility, with ESP-IDF components where version-compatible. Prefer Espressif's `esp_http_server` rather than Arduino WebServer, but verify against pinned Arduino-ESP32/ESP-IDF versions and test memory/task interactions before committing. Board-specific code belongs behind HAL and a board profile. Do not build other boards during MVP.

## Components and boundaries
1. **HAL/board profile:** W5500, SPI, nRF24, GPIO, BLE adapter, storage, timers, crypto/random, future IR.
2. **RF receiver:** emits `RemoteEvent(button_id, event_kind, timestamp)`; down/up required; future long/double/triple presses are trigger concerns, not integration concerns.
3. **Activity engine:** active activity ID, startup/shutdown sequences, precompiled trigger lookup tables and transition coordinator.
4. **Action executor:** normalized typed commands, ordered sequence scheduler, bounded per-integration queues.
5. **Integration adapters:** BLE HID, Home Assistant, built-in system actions; future IR.
6. **Device registry:** curated list of registered targets with stable hub IDs, provider references and display names. It is not a duplicate Home Assistant database and does not track device power state.
7. **Configuration service:** JSON schemas, versioning, validation, atomic persistence, compilation and hot swap.
8. **HTTP/API/auth:** serves compiled React assets and versioned API; lower priority than RF.
9. **Diagnostics:** connection status, errors, queue overflow, latency histograms, memory high-water, watchdog/recovery.

## Data model
**Integration instance:** stable `integration_id`, type, enabled state, protected connection settings, discovery and capability interface. Each integration manages its own pairing/discovery/storage requirements.

**Device:** `id`, `name`, `integration_id`, internal provider target, metadata, schema version. Example: `dev_72`, `Family Room Denon`, `ha_main`, internally linked to `media_player.denon`. HA references should record stable registry identifiers where available; entity IDs may change. Bluetooth stores pairing reference. IR will store downloaded code-set reference. Only admin may register/edit/remove devices.

**Action instance:** `{integration_id, device_id, command_id, params}`. For built-in `system.wait`, device_id is null and params include bounded `duration_ms`. The integration must match the device's owner. Commands and parameter schemas are discovered from registered device capabilities; arbitrary HA service payloads, target overrides, URLs and scripts are prohibited. Human names are not execution identifiers.

Examples:
```json
{"integration_id":"ha_main","device_id":"dev_72","command_id":"media_player.volume_up","params":{}}
{"integration_id":"ble","device_id":"dev_apple","command_id":"key_down","params":{"key":"ARROW_UP"}}
{"integration_id":"ble","device_id":"dev_apple","command_id":"key_up","params":{"key":"ARROW_UP"}}
{"integration_id":"system","device_id":null,"command_id":"wait","params":{"duration_ms":1000}}
```

**Activity:** `{id,name,startup:Action[],shutdown:Action[],bindings:Record<Trigger,Action[]>,macros:Trigger[],schema_version,revision}`. A trigger is button ID plus event kind. Every binding is an array. A binding is a single command unless its trigger is listed in `macros` (down, tap or hold only; absent means none); a macro is a list of 1 to 32 actions, run on the executor's Macro lane (one running, at most four waiting), cancelled by an activity switch and recorded in the run history. The same ordered-action sequence executor runs startup, shutdown and macros. Global bindings carry `macros` the same way. Unbound triggers are no-ops. See docs/design/macros.md.

**Activity switching:** Select B → validate/ensure compiled B → atomically select B's mapping → enqueue B startup sequence. Never run A shutdown. Press Off → enqueue current activity shutdown and clear active mapping per documented policy. Sequences must not block RF. Serialise transition requests; cancel or generation-guard stale delayed actions so startup A cannot resume after switching to B. On action error, report it and go on to the next step (docs/design/activity-runs.md); no retries. In startup and shutdown, an HA step whose state HA already reports is skipped. An HA step waits at most 1 s for HA's reply, then moves on (docs/design/state-checks.md). Hubbo reads state at the moment it needs it; it never tracks it.

## Performance architecture
**Compile configuration, don't interpret it on button press.** At config save, validate device registration, command ownership, parameter bounds, and schema; resolve adapter indices and bounded parameters into compact runtime structs. Precompute `button_id × event_type → Action[]` lookup. Stage and atomically swap compiled configuration, keeping old storage alive until readers finish.

Hot RF path: RF decode → index active table → dispatch/enqueue typed action. No JSON, flash I/O, heap allocation, HTTP, TLS, provider discovery, name lookup or network wait. Prefer internal SRAM for hot lookup tables; PSRAM for cold configuration, HTTP buffers and catalogs. Preallocate bounded queues and define overflow behavior; missed release must not leave a held HID key stuck.

BLE HID: key-down/key-up semantics, reconnect and release-all safeguards, timeout for missing key-up. Home Assistant: asynchronous worker/queue, persistent transport if useful, explicit timeout and error reporting; HA response latency is not the same as RF dispatch latency. System wait uses sequence scheduler, not sleep in RF callback.

Instrument RF receive-to-dispatch and adapter handoff independently from remote-to-device response. Internal dispatch targets: **p95 <2 ms and p99 <5 ms** under idle and web/API load. They're set from the measured baseline on boards in daily use (2026-11-15: p50 ~0.5 ms, p95 ~0.65 ms, p99 ~0.8 ms), with headroom; the first placeholders were 10 ms and 25 ms. Test RF under Ethernet SPI traffic, BLE reconnect, web load and OTA.

## Integrations
**BLE HID:** Apple TV paired as keyboard; integration is generic HID, with `key_down`, `key_up`, `key_tap` and typed key parameter. UI may offer friendly presets. Held remote button sends down and release sends up. Prevent stuck keys after disconnect, activity switch, timeout.

**Home Assistant outbound:** Admin configures URL and token. Discovery can browse all entities, but only explicitly registered entities become hub devices. Normalize capabilities from HA entity metadata, supported features, service definitions and parameter selectors; not every service applies to every entity. Example `volume_set` takes a normalized 0–1 value; `select_source` may offer available source options. Permit a curated set of entity-specific actions; disallow area/all-entity targets, arbitrary service payloads, nested target overrides and indirect broad operations. Device registry restriction is application security, **not** a HA token sandbox. A stolen HA credential may still grant broad access.

**IR (post-MVP):** admin selects a model from externally hosted legacy code database; download only that model's IR codes to local flash; then run offline. Device commands become ordinary actions. Later support learning. Do not ship entire ~300 MB corpus to ESP32.

**System:** `wait(duration_ms)` first; future conditional/repeat actions require explicit design and bounded execution.

## API and contract
Use `/api/v1`, strict JSON schemas, bounded payload sizes, meaningful validation errors, revision/optimistic concurrency for edits, atomic activity replacement, and OpenAPI as source of truth. Generate TypeScript client/types and mock fixtures. A command execution endpoint accepts **hub device ID + allowed command ID + typed params**, never raw HA entity IDs/service calls.

Proposed groups:
- `GET /status` and `/remote/status`, `/remote/buttons`.
- `POST /auth/login`, `/auth/logout`, `GET /auth/session`.
- `GET /devices`, `/devices/{id}`, `/devices/{id}/capabilities`.
- `POST /devices/{id}/commands/{command}/execute` for registered commands.
- `GET/POST /activities`, `GET/PUT/DELETE /activities/{id}`, `POST /activities/validate`, `POST /activities/{id}/activate`, `POST /activities/off`.
- `GET /integrations` (safe summaries).
- Admin-only: `/admin/integrations/{id}` configure/discover, `/admin/devices` register/update/delete, `/admin/remote/pair`, `/admin/tokens`, `/admin/backup`, `/admin/restore`, `/admin/firmware/update`.
All paths above are under `/api/v1`; final paths and response models must be specified in OpenAPI before implementation.

**Agent instructions:** publish `AGENTS.md` or `docs/agent-api.md` documenting auth, discovery of registered devices, capabilities, dry-run validation, one-request activity creation, activation, troubleshooting, and explicit prohibition on arbitrary HA calls. A future MCP server or inbound HA integration may use the same API, but is not MVP.

## Security
One admin, with a username and password chosen at setup, and browser sessions kept only in RAM (a reboot logs everyone out). Secrets (the password hash, the HA token, API token hashes) stay in NVS and never appear in responses, logs or backups. HTTPS, secure boot and flash encryption are out of scope for this one-home LAN device; no public internet port forwarding. Details, including what was trimmed in Phase 5: `docs/security.md`.

API tokens: created/revoked by admin, displayed once, stored hashed, supplied as Bearer tokens. They may list registered devices/capabilities, create/edit/delete activities and mappings, test registered commands and activate activities. They **cannot** register/edit/remove devices, configure integrations, pair remotes, manage tokens/credentials, restore backups or update firmware. Tokens may still control sensitive **registered** devices, so protect them. Every config/action API must enforce this, including nested action validation; only trusted RF path bypasses HTTP auth, using prevalidated compiled actions.

For test execution, return accepted/failed/timeout and avoid claiming physical device response. No secrets in diagnostics, browser storage or backup by default.

## Web admin
React + TypeScript + Vite + Tailwind CSS + shadcn/ui. Static assets prebuilt and served from ESP32 flash; browser performs UI rendering, ESP32 performs API/network work. Light/dark mode, responsive desktop/phone, visual Harmony Companion button map.

Pages: **Dashboard** (active activity, connectivity, diagnostics), **Devices** (curated list, add by integration, per-device capability/test screen), **Activities** (startup ordered sequence, clickable remote for mappings, shutdown ordered sequence), **Integrations** (connection health/settings), **Settings** (admin credentials, tokens, network, OTA, backup/restore, logs). First-run wizard: admin creation, networking, RF pairing, optional Home Assistant connection and initial devices. No MQTT setup in the wizard. The outbound HA client calls HA services; separately, an optional MQTT integration (Integrations page) publishes the hub to HA as a device: activity select, power switch, restart and diagnostics (docs/design/mqtt-discovery.md).

Web and agents use identical API service functions. Dev modes: Vite hot reload + TypeScript mock server seeded with realistic devices/activities; Vite proxy to real ESP32 API; production static build. Generate types from OpenAPI; contract-test mock and firmware against same fixtures. Avoid needing firmware flash for visual edits.

## Persistence, OTA and distribution
LittleFS JSON for nonsecret device/activity/config files; versioned schemas and migrations. Atomic update via temp/write/validate/rename plus recovery strategy; validate full configuration before swapping runtime table. No flash writes on RF events or activity switches. Secrets in protected NVS/secure storage, not ordinary exported JSON.

Backup downloads one versioned JSON archive of user-facing config; restore uploads, validates, migrates and commits atomically. Exclude admin password, API tokens, Home Assistant secrets and BLE bonding secrets by default; document that restored systems require reauthorization/repairing where necessary. Do not leak secrets through backups.

OTA over Ethernet with signed/authenticated update validation, compatible board ID and partition checks, rollback on failed boot, no interruption of critical RF processing during normal operation (temporary degradation during flash must be measured). 16 MB flash requires deliberate partition plan for two app slots, static web assets and config. Browser-based USB installer via ESP Web Tools or equivalent; GitHub Actions build reproducible PlatformIO firmware for the single MVP board and publish bootloader/partition/app or merged binary plus manifest, checksums and release notes. Additional boards become build-matrix entries later.

As built (2026-11-16, docs/design/firmware-updates.md): a tag `vX.Y.Z` on main is built by GitHub Actions into a Release with the app image and a manifest signed with ECDSA P-256; the public key is in the firmware. The hub checks it daily or on demand, and installs only when the admin chooses. Every new image boots on trial and rolls back unless it's healthy within 2 minutes.

## Decisions requiring verification, not new product design
- Exact Waveshare module flash/PSRAM, PoE and SPI pin map; actual RF wiring.
- Arduino/ESP-IDF version compatibility for `esp_http_server`, BLE and W5500.
- Home Assistant service metadata and safe command allowlist by supported entity type.
- OTA partition sizing with web assets and rollback.
- TLS certificate provisioning and ESP32 memory budget.
- Existing experimental BLE code and RF performance baseline.
These are implementation investigations with tests, not reasons to reopen the agreed architecture.

## Amendment 2026-11-08 — kickoff decisions
- **Name:** the project is **Hubbo**. License: AGPL-3.0, continuing Harmonizer's.
- **Hold triggers are MVP.** Trigger event kinds are `down`, `up`, `tap` (released before the hold threshold) and `hold` (held past the threshold, fires once). The deployed family-room setup already depends on click-vs-hold (Info held). Double/triple press stays post-MVP.
- **RF semantics (verified in the spike):** button payloads carry the full held set per group (`0xC1` keypad, `0xC3` media/home), not edge events. Keepalives arrive about every 40 ms while held. The RF receiver derives events by diffing per-group sets and releases everything after 500 ms of silence.
- **Remote pairing:** Hubbo listens on the address of an existing Harmony Hub, learned by scanning while the Hub's pair button is pressed. A Hub is needed once per remote at setup. Pairing a remote with no Hub at all is unsolved and is not MVP.
- **Hardware verified by the spike:** W5500 SCLK 13, MISO 12, MOSI 11, CS 14, INT 10, RST 9. nRF24 on its own SPI pins: SCK 15, MISO 16, MOSI 17, CSN 18, CE 38. The nRF24 needs a 10 µF capacitor across VCC/GND on PoE. BLE HID to Apple TV works on NimBLE-Arduino 2.5.1. Never connect USB and PoE at once.
- **Toolchain:** all builds run in Docker (`bin/hubbo`). Firmware is pinned to PlatformIO `espressif32@8.0.0` (arduino-esp32 3.3.12). See `docs/development.md`.
- **Dev hardware:** the former guest room Harmonizer board (reflashed with Hubbo; its remote is on channel 5, address `0xFC5C97550E`). The family room Harmonizer board stays untouched.
- **Re-activating the active activity** re-runs its startup (Harmony's "fix it" behaviour).
- **Deleting the active activity** switches to the next activity (the one that takes its list position, wrapping to the first) and runs its startup; no shutdown runs. Inactive only when none remain.
- **Active activity survives reboot:** the active id is saved to NVS a few seconds after the last switch (only if changed, Off = none), from the config side, never the RF path. On boot the hub restores that activity's button map without running its startup. If the activity no longer exists, it boots with nothing active.
- **First-run setup:** the first person to reach a fresh hub creates the admin. No physical confirmation step.
- **HA integration-specific services** (e.g. `webostv.button`) are resolved from HA's `get_services` target filters and field selectors, never entered as free text. See `docs/ha-capabilities.md`.
- **No held-button repeat** for HA actions in the MVP.
- **Flash encryption / Secure Boot:** not in the MVP. Revisit before any public release.
- **ThreadSanitizer:** `bin/hubbo tsan` runs the native tests under TSan in a one-off container with seccomp unconfined.

## Scope and threat model (2026-11-09)
Hubbo is a hobbyist's device: one board in one home, on a trusted home LAN, with one user. It isn't sold, isn't multi-user and isn't exposed to the internet. Engineering effort goes to what affects that user: never losing or corrupting config, a remote that keeps working, and correct device behaviour. The auth (admin password, sessions, API tokens) is kept small, as sensible LAN hygiene. Hardening against hostile LAN clients, DoS and scale is out of scope unless explicitly requested.

## Native desktop simulator — required architecture (amendment)
The **actual C++ domain and application code** must build and run on a developer computer without ESP32 hardware. Separate `core` (activity engine, action models, compilation, registry, configuration validation, authorization policy, action scheduling) from `platform` adapters (RF, BLE, W5500, LittleFS, NVS, ESP-IDF HTTP). Core must not include Arduino headers or depend on FreeRTOS primitives directly; inject clocks, storage, task scheduling, transport, and integration interfaces.

Build a native executable `hub-sim` with deterministic simulated RF events, BLE HID, Home Assistant service calls, IR placeholders, and time advancement. Prefer the same HTTP **route handlers and business logic** as firmware, behind a host HTTP adapter; the host listener may differ from ESP-IDF's HTTP server. This is a real-core simulator, not just a TypeScript mock. The TypeScript mock API remains useful for fast frontend fixture work, but **must not be the primary source of behavioral truth**.

Provide a command to launch the simulator and Vite frontend together, seed fixtures, reset state, run automated scenario scripts, and capture logs. Run C++ unit tests, API contract tests, integration/transition tests, and web end-to-end tests against the simulator in CI. Test token/admin boundaries, corrupted JSON, action cancellation, held-key releases, and config swaps. CI must also cross-compile the ESP32 firmware to catch platform divergence.

Hardware testing is still mandatory before release: SPI contention, BLE pairing/hold behavior, actual RF decode, Ethernet and Home Assistant timing, RAM/flash pressure, TLS, OTA and real-device latency cannot be validated by simulation. Aim to complete the **majority of feature development** on the simulator; do not promise 90% of physical behavior is simulated.

## Local simulator authentication bypass (explicit development-only requirement)
The **native desktop simulator** must start in a clearly labeled `SIMULATOR_NO_AUTH` mode by default, so a coding agent can call every local API endpoint without logging in, managing cookies, or generating tokens. This includes admin endpoints for adding devices/integrations, resetting fixtures, creating activities, and exercising actions against **simulated adapters only**. Requests are treated as a simulated administrator for convenience.

This bypass must be **impossible in ESP32 production firmware**: implement it behind a host-native compile-time flag and platform-specific dependency injection, not a runtime flag or HTTP parameter that a deployed hub can enable. Production authentication and role checks remain mandatory. The simulator HTTP listener must bind to `127.0.0.1` by default, reject non-loopback exposure unless explicitly configured for a trusted development environment, display an obvious `AUTH DISABLED — SIMULATOR` indicator, and never load real Home Assistant credentials or issue live hardware/network control commands while bypass is active.

Maintain separate automated test modes: **no-auth simulator** for rapid development, and **auth-enabled simulator** using the same production authorization policy for security tests. CI must prove that ESP32 builds cannot enable the bypass, that token restrictions work in auth-enabled mode, and that no-auth simulator commands only reach fake adapters.
