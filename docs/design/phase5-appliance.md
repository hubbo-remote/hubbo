# Phase 5: Appliance management, scaled to one home

Scope: a hobby device for one home on a trusted LAN, with one admin. Phase 5 trims the auth, adds
firmware updates from the web admin, and checks backup and restore on the board. HTTPS, secure
boot and flash encryption are out of scope (user decision, 2026-11-09). Based on the API agent's
auth review and the firmware agent's feasibility note (`docs/team/hubbo-firmware.md`).

## 1. Auth trim (API, with frontend and firmware follow-ups)

Phase 2's auth is over-built for this device. Aim: one admin login plus simple API tokens, with
less to go wrong. The tests and the policy table stay.

| Piece | Decision |
|---|---|
| Password hashing (PBKDF2-SHA256) | Keep. Its cost also slows guessing, which replaces the rate limiter. |
| Username | Keep. Setup asks for a username and a password, with **no "admin" default**: the user picks it (user decision, 2026-11-09). |
| Sessions in RAM | Simplify: one absolute lifetime (about 7 days), no idle timer. A reboot still logs out. |
| CSRF token (`X-Hubbo-CSRF`) | Remove. SameSite=Strict, the Origin check and JSON-only bodies already stop forged browser requests. |
| Origin check, JSON content type | Keep (small, does the real work). A bad Origin stays 403 `csrf_failed`. |
| Rate limiting | Remove, with its 429 and Retry-After. |
| Credential states | Simplify to `fresh`, `ok` and `unavailable` (corrupt folds in), read once at boot. Can't-read is never treated as doesn't-exist. Recovery is a reboot or the wipe. |
| Account-takeover guard | Keep: setup only in `fresh`. |
| Host allowlist | Remove, with its 421 and the firmware wiring (`setAllowedHosts`, the static-file check). |
| Roles | Simplify to Public, TokenOrAdmin and AdminOnly. Tokens can control the hub and edit activities, but can't reset, restore, update firmware, manage tokens or change settings. |
| API tokens, SecretStore, the no-auth sim mode | Keep. |

- **Spec and contract:**
  - `auth_state` becomes `fresh | ok | unavailable`, and `credentials_corrupt` goes away.
  - The CSRF header, 429 and 421 are removed.
  - The contract keeps a cross-Origin case and a token-forbidden-on-admin case.
- **Frontend:**
  - The client stops sending the CSRF token.
  - The setup's admin step has an empty username field.
  - `CredentialsProblem` keeps one variant.
  - Any 429 handling is removed.
- **Firmware:** remove the Host allowlist wiring.
- **Docs:** `docs/security.md` shrinks to match.

## 2. Firmware update from the web admin (API, firmware, frontend, core sim)

Anyone without the CLI must be able to update the hub (user decision, 2026-11-09). There's **no
rollback**, either automatic or manual (user decision). Arduino's default stays: an image is
marked valid before `setup()`. The checks below stop the realistic mistake, uploading the wrong
file, before anything is erased.

- **Routes:** `POST /admin/firmware/update` (admin only, the raw .bin as
  `application/octet-stream`, body limit = the OTA slot size) and `GET /admin/firmware` (version,
  board, update state and progress). No rollback route. Shapes and error codes:
  `api/openapi.yaml`.
- **Body handling:** the image is buffered in PSRAM by the existing body path (about 2 MB; 7.5 MB
  free), so the whole image is checked before any byte is erased, and the router has no streaming
  path.
- **Port:** `FirmwareUpdatePort` (`core/include/hubbo/api/firmware_update_port.h`, like
  `BlePairingPort`). `install(image)` validates on the httpd task in milliseconds, then hands off.
  hub-sim's `SimulatedFirmwareUpdate` takes a fake image in the real layout, so the contract covers
  every refusal and a success. Its simulated reboot drops sessions like the board, but it keeps
  reporting its own version (no API hook only for the sim); the board test checks the real one.
- **Validation order:**
  1. Size fits the inactive slot.
  2. Magic is 0xE9.
  3. Chip is ESP32-S3, and the minimum revision is at or below this chip's.
  4. The app description magic is in place, which rejects a merged factory .bin.
  5. The Hubbo marker is present, compiled into the image as `HUBBO-IMAGE/1
     board=waveshare_esp32_s3_eth layout=1 version=<git describe>`. `layout` is the partition
     table version, which OTA can't change.
  6. The appended SHA-256 matches. `esp_ota_end` checks it again from flash.
- **Writing:**
  - A one-shot writer task on core 0 does sequential writes (`OTA_WITH_SEQUENTIAL_WRITES`, one
    4 KB sector erase at a time) and yields between sectors.
  - Then it sets the boot slot and reboots through the existing `requestReboot`, which releases
    held keys and flushes the active activity.
  - Flash erases stall both cores (cache off). So the remote may lag for about 30 s during an
    update. That's accepted (user decision), and it's the same as `bin/hubbo ota` today.
- **One update at a time:** an owner flag (idle, web or arduino) shared with ArduinoOTA. ArduinoOTA
  and `ota_watch` stay for development, and `bin/hubbo ota` keeps working.
- **UI:** Settings gets a firmware section.
  - It shows the current version.
  - It has a file picker, with upload progress from XHR `upload.onprogress`.
  - It shows writing progress from `GET /admin/firmware`.
  - It warns that the remote may lag.
  - After the board reboots, it waits until the hub answers again, then shows the new version.
  - Logging in again after an update is expected (sessions are in RAM; user decision).
- **Measured:** a config save erases flash too: five saves gave a loop max gap of 2.1 ms, none
  over 5 ms.

## 3. Backup and restore on the board

The API and the Settings page already back up and restore. Phase 5 proves it on the board, which
also runs the wipe's final form (NVS erase plus filesystem format) for the first time.

Found at the board test (user decisions, 2026-11-09):
- **Reset configuration:** the Settings button called "Factory reset" only reset the
  configuration (devices, activities, global buttons, integration settings) and kept the login,
  tokens and pairings. The user expected onboarding. It's renamed Reset configuration, with a
  typed confirmation. API: `POST /admin/reset` becomes operation `resetConfiguration`.
- **Factory reset:** a real one, for handing the hub to someone else or feeling sure it's clean.
  - `POST /admin/factory-reset` (admin only, typed confirmation in the UI) answers 202, then the
    hub erases everything (NVS: login, tokens, the HA token, BLE bonds, the remote endpoint; the
    config filesystem) and restarts into first-run setup.
  - The board reuses the wipe code (`HUBBO_WIPE_ON_BOOT`'s NVS erase plus the fs format at the
    next mount), never on the RF task, and releases held keys first like any reboot.
  - hub-sim erases its state the same way and drops sessions, so e2e can check the UI lands on
    the wizard.
  - The UI says the Apple TV must also forget "Hubbo".
- **After a restore,** the UI says when the Home Assistant token needs re-entering. Backups never
  hold secrets or Bluetooth bonds.
  A bond belongs to this board's own Bluetooth address (the chip's public address), so it couldn't
  move to another board anyway (user decision, 2026-11-09).

## 4. Shared BLE pairing state

hub-sim and the firmware each map BLE pairing state by hand. Move the mapping into one core
function used by both (core, then firmware).

## Gate

On the dev board:
1. **Update:**
   - Update the firmware from the Settings page with a normal build.
   - Then try the wrong file (a merged .bin or a non-Hubbo image) and check it's refused before
     anything is written.
   - The remote recovers after the update.
2. **Backup and restore:**
   - Download a backup, factory-reset the board from Settings, and run the wizard.
   - Restore the backup from Settings. Reset configuration and restore again.
   - Devices, activities and button mappings come back.
   - Only the HA token and the Apple TV pairing need re-entering. Forget "Hubbo" on the Apple TV
     first.
3. **Auth:** set up with a username of your choice, log in, use an API token for an activity, and
   check a token can't update firmware.

Also: the contract passes in both auth modes, and the loop stays under 5 ms outside an update.

## Ownership

| Agent | Owns |
|---|---|
| API | Auth trim; the `FirmwareUpdatePort` header and `SimulatedFirmwareUpdate` (like `BlePairingPort`); firmware routes, body limit exception, spec, contract, `docs/security.md` |
| Core | hub-sim wiring for the simulated firmware port; the shared BLE state function |
| Firmware | The port over esp_ota, the marker, the writer task, the owner flag, removing the Host allowlist, using the BLE function, board measurements |
| Frontend | Firmware section in Settings, auth UI changes, mock, e2e |
