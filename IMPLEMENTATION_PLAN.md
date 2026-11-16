# Harmony Hub Replacement — Phased Implementation Plan
Version 0.1 • 2026-11-04

**Companion source of truth:** [ARCHITECTURE.md](ARCHITECTURE.md). Read that document **before** implementing any phase. Do not reinterpret the product requirements. Each milestone is a separate coding-agent task/PR with test evidence. The goal is incremental working firmware, not a one-shot rewrite.

## Execution rules for coding agents
- Start every task by identifying the architecture sections and API/schema contracts affected.
- Preserve existing working RF protocol code where possible. Experimental Ethernet/BLE fork is not yet supplied; request or inspect it when available, otherwise use a clearly isolated implementation and record the difference.
- Implement one milestone at a time, build/test it, report firmware size, heap/PSRAM use and relevant latency; do not silently advance through failed acceptance gates.
- Never add hardware targets, power-state tracking, unrestricted HA commands, MQTT beyond Home Assistant device discovery (approved 2026-11-13, docs/design/mqtt-discovery.md), or extra authentication roles without explicit approval.
- Keep firmware hot path independent of HTTP, config parsing and network operations.
- No web UI code may call private firmware internals: all changes go through versioned API.
- Keep code modular and document behavior/failure modes; include test fixtures and CI checks with each milestone.
- Use feature flags for unfinished adapters; no fake success for unimplemented functionality.

## Phase 0 — Native-first foundation and hardware baseline
**New hard gate:** the portable C++ core and a runnable desktop simulator must exist before substantive activity-engine implementation. The simulator and ESP32 build must share core sources rather than duplicate logic.

**0.0 Native simulator bootstrap**
- Start simulator with no authentication by default (`SIMULATOR_NO_AUTH`), granting simulated admin access for agent-driven development, including fixture/device management.
- Bind unauthenticated simulator to loopback only and use fake integrations/credentials; display `AUTH DISABLED — SIMULATOR`.
- Compile-time exclude auth bypass from all ESP32 production builds. Add a separate auth-enabled simulator test mode and CI checks for production policy enforcement.

- Define hardware-free core interfaces for clock, storage, RF event source, integration adapters, executor scheduling, and HTTP route handling.
- Add CMake or PlatformIO native build for `hub-sim`; mock RF/BLE/HA adapters and deterministic simulated time.
- Expose host HTTP adapter using same API route handlers as firmware; TypeScript mock remains for UI fixtures.
- Provide one command to run simulator + web dev server, fixture reset, and scripted scenarios.
- CI runs native unit/integration/API tests and cross-compiles ESP32 firmware.
**Acceptance:** coding agent can develop, execute simulated button presses, exercise activity transitions, and call local HTTP endpoints without ESP32 hardware. No Arduino includes in portable core.

## Phase 0 — Repository and hardware baseline
**Outcome:** Reproducible build, known hardware configuration and measurable existing behavior.

**0.1 Repository bootstrap**
- Fork/base from `soaresbrun0/harmonizer`; preserve license, notices and attribution.
- Establish `firmware/`, `web/`, `mock-api/`, `api/`, `docs/`, `scripts/`, `.github/workflows/` (or equivalent clear boundaries).
- Pin PlatformIO, Arduino core, ESP-IDF component versions and library dependencies.
- Add lint/format/unit-test commands, GitHub Actions build and artifact upload.
- Record commit and dependency versions.
**Acceptance:** clean checkout builds deterministically in CI; source/license files present.

**0.2 Board bring-up**
- Confirm Waveshare ESP32-S3-ETH variant, W5500 pins, SPI wiring, RF24 CE/CSN/IRQ, flash/PSRAM, PoE, BLE coexistence.
- Add one `waveshare_esp32_s3_eth` board profile and HAL interfaces, not generic support for every board.
- Ethernet link, nRF24 reception, BLE initialization and watchdog each get smoke tests.
**Acceptance:** stable 30-minute Ethernet+RF receive test, no SPI contention, logged memory baseline.

**0.3 Latency baseline**
- Instrument RF packet reception, button event decode, and current experimental direct command handoff where available.
- Establish a repeatable press/release and hold test harness; document measuring method.
**Acceptance:** baseline measurements stored as CI artifact/manual test report; release events do not stick.

## Phase 1 — Core activity engine (first user-visible functional increment)
**Outcome:** Remote switches between hardcoded activities, actions run without Home Assistant or web UI.

**1.1 Domain types and event contracts**
- Define stable button IDs and `RemoteEvent` down/up, `Action`, `Sequence`, `Activity`, `IntegrationAdapter`.
- Create typed built-in actions and error codes. No arbitrary string execution.
- Build unit tests for malformed IDs, invalid params, empty arrays, and unbound buttons.
**Acceptance:** compile-time API contracts and host-side tests pass.

**1.2 Action executor and scheduler**
- Implement bounded queue, ordered action execution, nonblocking wait, adapter dispatch and failure reporting.
- Implement mock adapter that records commands, then BLE adapter stub if experimental code unavailable.
- Explicitly define overflow, cancellation and timeouts.
**Acceptance:** sequences execute in order; wait does not block RF; failures reported.

**1.3 Activity compiler and hot path**
- Compile hardcoded activity bindings to fixed-index lookup tables.
- Use atomic pointer/table switch and avoid allocation/JSON/flash in RF dispatch.
- Test repeated button down/up, bursts and no-op mappings.
**Acceptance:** measured dispatch within initial architecture targets on real board; no lost releases/stuck keys in stress test.

**1.4 Activity lifecycle**
- Implement activate B, startup B, no shutdown A; Off runs only current shutdown.
- Guard delayed actions with transition generation so old waits cannot resume after new activity.
- Two sample activities: Watch Apple TV and Play Xbox; use mock or available adapters.
**Acceptance:** Xbox→Apple TV does not send Xbox off; Off on Apple TV executes only Apple TV shutdown; repeated switches deterministic.

**1.5 BLE HID working integration**
- Integrate existing Apple TV BLE pairing/HID code from experimental fork when provided; otherwise implement isolated adapter with documented verification steps.
- Key down/up/tap, disconnect cleanup, hold/repeat, timeout release-all.
**Acceptance:** actual Apple TV navigation, pause, sustained up/down and release work; RF-to-HID timing recorded.

**Additional Phase 1 gate:** all core activity behavior passes deterministic native simulator tests before hardware validation.

**Phase 1 gate:** Remote usable with at least two activity maps, startup/shutdown behavior proven, no web or HA dependency, latency and hold behavior measured.

## Phase 2 — Persistence, registry and API contract
**Outcome:** Activities editable without firmware rebuild; device IDs and authorization boundaries defined.

**2.1 Versioned JSON schemas**
- Define integration, device, action and activity JSON Schema/OpenAPI models.
- Implement LittleFS persistence with temp-write, validation, recovery and migration version.
- Add corruption/power-loss simulation tests.
**Acceptance:** config survives reboot; failed save preserves last good version; no flash writes per button press.

**2.2 Device registry and capability interface**
- Stable hub IDs, integration ownership, registration lifecycle, normalized command/parameter schemas.
- System and BLE registered-device examples. Reject unregistered IDs and mismatched integration IDs.
- Never use raw HA entity IDs in action-execution request shapes.
**Acceptance:** unauthorized target cannot be represented in valid action config; registry operations validated.

**2.3 OpenAPI-first REST skeleton**
- Implement `GET /status`, devices, capabilities, activities CRUD, validate, activate/off and registered-command execution.
- Full activity replacement is atomic; optimistic revisions avoid lost updates.
- Request size limits, structured error codes, strict parameter validation.
- Include OpenAPI spec and generated TS types in repo.
**Acceptance:** HTTP integration tests and schema conformance pass; no half-written activity after invalid request.

**2.4 Runtime compile and config swap**
- Compile new activity revision off hot path, publish atomically, invalidate stale references.
- Reject actions referencing deleted devices; remove or block device deletion while referenced with clear error.
**Acceptance:** edits while RF traffic runs do not crash, stall or partially update mapping.

**Phase 2 gate:** Create/edit/activate activity through curl or generated client; reboot retains config; remote remains responsive.

## Phase 3 — Integrations and secure discovery
**Outcome:** Selected Home Assistant devices usable without making HA the remote's brain.

**3.1 Home Assistant transport**
- Admin-configured endpoint/token, TLS validation, reconnect/backoff, health, timeouts.
- Use documented HA APIs for entities, services and attributes; normalize metadata.
- Do not expose secrets in API/logs.
**Acceptance:** discover known media player/light; cleanly report HA unavailable; local BLE still works.

**3.2 HA capability normalization**
- Map supported services to safe typed commands and parameter schemas; feature checks, selectors, enums and ranges.
- Start with media player volume up/down/set, mute, play/pause, power where supported, select source; extend by tests, not assumptions.
- Enforce entity-specific target; reject area/all-device service targets and nested override fields.
**Acceptance:** Denon volume up and set-volume tested; invalid values and broad targets rejected.

**3.3 HA device registration**
- Admin discovers all HA candidates, selects a device, stores only provider reference and metadata.
- Hub registry exposes curated selected devices to activities and tokens.
- Handle renamed/missing HA entities as explicit stale references.
**Acceptance:** unregistered HA entity cannot be executed by token or activity; registered entity works.

**3.4 Device test execution**
- Same command executor used by UI/API; accepts registered device ID, command ID and typed params.
- Return accepted/error/timeout, not unverified physical success.
**Acceptance:** test volume set with numeric value and source selector; no raw HA payload bypass.

**Phase 3 gate:** Apple TV via BLE + Denon/Sonos via HA in one activity, RF remains local, registered-device policy enforced.

## Phase 4 — Web development foundation and UI
**Outcome:** Complete polished web admin, developed without constant reflashing. The UI can run against either the fast TypeScript fixture server or the **real C++ simulator's HTTP API**; the latter is the primary end-to-end acceptance environment.

**4.1 Web workspace**
- React + TypeScript + Vite + Tailwind + shadcn/ui; theme tokens, light/dark, responsive layout.
- Generate client/types from OpenAPI, strict TypeScript and CI.
- TypeScript mock server with realistic device/activity fixtures and error cases; Vite hot reload and proxy-to-real-board mode.
**Acceptance:** UI development works with zero hardware; contract tests catch mock/firmware drift.

**4.2 Dashboard and devices**
- Dashboard: active activity, remote/BLE/HA/Ethernet status, errors and latency.
- Devices: curated list, integration-based add flow, details, command test forms from capability metadata.
- Admin-only registration controls; no advanced direct-HA action picker.
**Acceptance:** add HA Denon, inspect commands, test volume; phone and desktop layouts.

**4.3 Activities**
- List/create/edit/duplicate/delete activities.
- Startup and shutdown ordered action editors with wait; single-action-per-trigger UI backed by `Action[]`.
- Visual clickable Harmony Companion remote with down/up bindings, selected button editor and validation messages.
- Apply/dry-run/revision conflict UI; switching activity updates status.
**Acceptance:** create Watch Apple TV from scratch, map Apple TV arrows and Denon volume, test startup/Off semantics.

**4.4 First-run wizard and settings**
- Wizard: create admin, network, RF pairing, optional HA setup, initial devices; no MQTT event-relay setup.
- Settings: connection health, account, tokens, backup/restore, firmware update, diagnostics.
- Accessible keyboard navigation, mobile layout, no secrets shown after entry.
**Acceptance:** clean device can be configured through browser and is usable without CLI.

**4.5 Production asset pipeline**
- Build compressed/static assets, cache/version safely, package into flash without runtime JS on ESP32.
- Verify actual firmware flash/PSRAM headroom and cold-load performance.
**Acceptance:** UI served locally from board, no CDN/internet dependency, remote latency unaffected.

## Phase 5 — Security and appliance management
**As built:** sized for one home on a trusted LAN (user decisions, 2026-11-09). No HTTPS, secure boot, flash encryption or rollback; the CSRF token and rate limits were removed. See `docs/design/phase5-appliance.md`.

**Important:** Security boundaries and protected API architecture are implemented from Phase 2 onward; this phase completes production hardening. Never ship Phase 2/3 API exposed on an untrusted network without auth.

**5.1 Authentication**
- One admin account; password hash in protected persistent storage.
- Opaque random cookie sessions stored only in RAM; HttpOnly, SameSite, expiry, logout, CSRF defenses and rate limits.
- Token issuance/revocation: hashed at rest, shown once, Bearer auth.
- **Admin:** device/integration/credential management, pairing, token management, backups and OTA.
- **Token:** registry read, activity CRUD, command tests/execution, activity activate/off; no device registration or integration modification.
**Acceptance:** matrix of allowed/denied requests tested, including nested malicious HA target parameters; reboot invalidates web sessions.

**5.2 Transport and secrets**
- HTTPS/certificate plan or explicitly configured trusted TLS termination; no plaintext credentials over LAN by default.
- Review secure boot, flash encryption, NVS encryption and physical access assumptions.
- Prevent leakage via logs, backups, diagnostics and API responses.
**Acceptance:** security test checklist, no secret-bearing responses or backup contents.

**5.3 Backup and restore**
- Download versioned JSON bundle of nonsecret config; upload, validate/migrate, atomic restore.
- Document need to reenter credentials/repair BLE pairing after restore.
**Acceptance:** export → factory reset → import restores devices/activities and mappings; corrupt/foreign backup safely rejected.

**5.4 OTA**
- Dual app slots and rollback; verify board ID, size, integrity/authenticity; failed boot rolls back.
- Expose update UI/status and ensure safe reboot/recovery.
**Acceptance:** successful Ethernet OTA, interrupted download recovery, incompatible image rejected.

## Phase 6 — Release and stabilization
**6.1 GitHub Actions release:** build board-specific PlatformIO binary, bootloader/partitions or merged image, manifest, hashes, license/source notices, release notes. Pin toolchain.
**6.1a Production OTA env:** today the only OTA build env (`waveshare_esp32_s3_eth_ota`) is the dev build with the `/debug/*` console. Release builds need an OTA-capable env without it, and `bin/hubbo ota` should be able to choose between them.
**6.2 Browser USB installation:** ESP Web Tools compatible installer/manifest for Waveshare board; first flash without Arduino IDE.
**6.3 Full-system soak:** 24–72 hour test with Ethernet, BLE, RF, HA reconnections, repeated activities, UI traffic; collect memory leak, watchdog, queue overflow and latency metrics.
**6.4 Docs:** installation, wiring, first-run, pairing, API/OpenAPI, LLM agent instructions, troubleshooting, backup/OTA recovery, security threat model and explicit MVP limitations.
**Gate:** new user can flash from browser, configure devices, create an activity, use remote, OTA update, backup/restore, and reproduce tests.

## Several Bluetooth devices (before polish round 2)
Up to four Bluetooth hosts, each a device with its own pairing, all connected at once (user decision, 2026-11-10). Design: `docs/design/multi-bluetooth.md`.

## Device profiles (after several Bluetooth devices)
**A profile is a device** (user, 2026-11-10). A device on the hub is a profile that's been added: manufacturer, model, platform, commands, power and timing, plus local fields (id, name, Bluetooth pairing). The hub's device record mirrors the profile, so adding a device is close to a copy. Profiles come from the Harmony database (converted) at scale, and are hand-written for Bluetooth. Spec and profiles: the spec agent, in `~/src/hubbo-profiles`. Research: `docs/design/research-device-catalog.md`.
- **Platforms are shared:** a profile's platform is the hub's (`ble_hid`, later `ir`). What happens after adding depends on the platform alone: Bluetooth opens the hub's one pairing flow, and IR is just added.
- **Every profile is complete.** No inheritance, no brand aliases. Generic profiles (Apple TV, Android TV / Google TV, Fire TV, Other) are ordinary profiles; people who can't find their exact device pick one.
- **Contributions** by pull request or feedback on hubbo-profiles.
- **The hub bundles copies** of the generic profiles, so adding works offline.
- **The add flow:** Add device → find or browse a profile → added (→ pairing for Bluetooth).
- Commands come from the device's profile, so an activity's buttons can be pre-filled.
- **Device kinds mirror Harmony's device types** (user, 2026-11-10), about 30, so the whole database fits. A kind is only a label, an icon and a browse category. Home Assistant doesn't change.

## Hubbo site (its own phase)
Two repos (user, 2026-11-13): `~/src/hubbo-profiles` holds the open profile spec, its schema and the whole catalog as data, CC0, open to pull requests; `~/src/hubbo-site` is the homepage and catalog site built from it. The Harmony archive is imported into hubbo-profiles once and nothing is removed. Design: `docs/design/catalog-site.md`. Comes before Phase 6, which is last.

## Polish pass (before Phase 6)
Round 1 (copy and navigation) is `docs/design/polish-ux.md`. Phase 6 comes after the polish pass (user decision, 2026-11-10). Visual polish and a redesign were deferred in Phase 4 (user decision, 2026-11-09). Collected from
the Phase 4 board gate, where the user set up a wiped board on an iPhone:
- **Clarity over correctness:** the setup flow worked but felt "a little wonky". Review every screen
  for clear state and feedback: what's saved, what's happening now, what to do next.
- **Device kind:** it's stored but nothing uses it yet. Give it icons and grouping, or hide the field.
- **Remote setup:** make the pair-by-scan vs. type-the-code choice and the live press check obvious.
- **Mobile:** a full pass on a phone (layout, tap targets, keyboards, focus rings).
- **Fewer notes:** the UI has far too many explanatory notes and descriptions (user, 2026-11-09). Cut them down; say what matters in the controls and states themselves. This includes what a restore leaves out (HA token, Bluetooth pairing).

## Home Assistant remotes (user, 2026-11-14)
HA's `remote` domain, added like any other domain: any `remote.*` entity is its own device. Turn on, Turn off and Toggle are typed; everything else (`remote.send_command`, ...) goes through the Home Assistant action, which on a remote also accepts `remote.*` services. Nothing else changes. Design: `docs/design/ha-remote.md`.

## Smart buttons (user, 2026-11-14; simplified 2026-11-15)
Light 1/2 and Switch 1/2 each take a device; the hub derives what the button and +/- do from its type.
Set on the Global remote; activities override like any button. Quick +/- step taps merge into one
call. Also fixes boot using up internal RAM (the saved config now loads into PSRAM). Design:
`docs/design/smart-buttons.md`.

## Activity runs (user, 2026-11-15)
A failed step no longer stops a sequence: it's reported and the next step runs. The hub keeps the last
8 activity runs step by step, and the web admin shows them (Status → Activity Log, and a Log tab on each activity). Design:
`docs/design/activity-runs.md`.

## State checks (user, 2026-11-15)
On the same branch as activity runs. In startup and shutdown, an HA step is skipped when HA's
current state already matches it. The rule is generic: power, plus fields that match attributes;
no per-device lists. An HA step also waits at most 1 s for HA's reply, then moves on and records
`no_reply`, which isn't a failure. Steps don't depend on each other, nothing is tracked, and waits
stay. Design: `docs/design/state-checks.md`. Dropped: a "wait until on" step.

## Firmware robustness (queued, user 2026-11-14)
Found on a second board whose nRF24 lacked its capacitor: after a power-related reset the hub kept
running (remote, Bluetooth) but never came back on the network.
- **Retry Ethernet start:** `EthernetLink::begin` tries `ETH.begin` once at boot. If the W5500 doesn't
  answer, the hub stays off the network until a power cycle. Retry every few seconds until it starts.
- **Show why the hub last reset:** today the reset reason is only on the serial console. Keep it (and
  the time since boot) somewhere the web admin can show, so a brownout or crash can be diagnosed
  without a cable.

## Post-MVP backlog
- **Parallel e2e:** one hub per Playwright worker (a mock per worker, or a hub-sim port per worker), so e2e can run with several workers or shards. Today it's `workers: 1` against one shared hub.
- **Discover an unknown remote's address without a Hub:** nRF24 promiscuous sniffing (research).
- **HA `text` selector params:** a bounded string parameter type, so HA services with free-text fields can be bound. These services are skipped in Phase 3.
- IR transmitter, model-specific code-set download from external legacy database, offline IR commands, optional IR learning.
- More board profiles/build-matrix variants and Wi-Fi.
- Multiple remotes and remote profiles.
- Long press, double/triple click trigger recognition; multi-action button editor.
- Inbound Home Assistant integration to activate activities/control registered devices via hub API token.
- More sophisticated transition and device power policies **only after deliberate review** of stateless principle. (The first was reviewed and approved on 2026-11-15: HA state checks, `docs/design/state-checks.md`.)

## Cross-cutting acceptance and handoff checklist
Every milestone PR includes: changed architecture/API sections; tests and reproducible commands; hardware exercised; latency p50/p95/p99 where applicable; free internal RAM/PSRAM/flash footprint; failure/recovery behavior; security boundary tests; known limitations; demo instructions; next milestone prerequisites.

Before shipping, verify:
1. Button press path never parses JSON, touches filesystem or waits for HA.
2. Held BLE keys release on normal up, timeout, disconnect and activity switch.
3. Switching activities never invokes old activity shutdown; Off only invokes current shutdown.
4. HA entities cannot be targeted without explicit admin registration; token cannot register devices.
5. Web and LLM clients use identical validated API and schemas.
6. Browser UI is usable on phone/desktop, including light/dark mode and visual remote.
7. Reboot, power-loss during save, HA outage, OTA failure and restore failure recover safely.
8. A release is installable over USB from browser and updatable over Ethernet.

## Decisions deliberately deferred to implementation verification
Exact board pins/flash partitions, HTTP server compatibility with pinned Arduino core, TLS certificate provisioning, exact HA capability metadata, and BLE HID integration details. Agents should research/test these and document results, but **not change agreed product semantics** without approval.

## Simulator-driven iteration and release gate (amendment)
Every implementation milestone should first pass host-native tests, simulator API tests and UI E2E tests (when relevant), followed by an ESP32 cross-build. Hardware tests are required at defined integration gates, not after every UI edit. The coding agent must report exactly which behavior was simulated versus physically verified. Do not claim Bluetooth pairing, RF latency, SPI arbitration or OTA safety are validated by mocks.
