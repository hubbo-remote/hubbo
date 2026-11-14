# Security

Hubbo is a LAN appliance that controls real devices and holds a Home Assistant credential. It's
sized for one home on a trusted LAN with one admin (Phase 5 trimmed the auth to match). This
document covers the threat model, how access control works, and the transport and storage
choices. The code lives in `core/include/hubbo/auth/` and `core/include/hubbo/ha/`; the API
contract is `api/openapi.yaml`.

## Threat model

**Assets**, most valuable first:

1. The Home Assistant access token. It can do whatever its HA user can do in the whole house,
   not only what Hubbo has registered.
2. The admin password and session. They give full control of Hubbo, including the HA settings.
3. API tokens. They can drive every registered device and rewrite activities.
4. BLE bond keys (up to 4 paired hosts, one per Bluetooth device).
5. The MQTT broker password. On its own broker it can do whatever that login can, and through
   Hubbo's topics it can switch activities and restart the hub.
6. Configuration (devices, activities). Not secret, but tampering means the remote does the
   wrong thing.

**Attackers we design for:**

- **Anything else on the LAN**: a compromised IoT device, a guest phone. It can reach the
  hub's HTTP port and sniff plaintext traffic on a shared segment. *Defence:* authentication on
  every route except setup/login/session, a slow password hash, no default credentials (not even
  a default username).
- **A malicious web page in the admin's browser** (CSRF). *Defence:* SameSite=Strict HttpOnly
  cookie, JSON-only bodies, an Origin check and no CORS headers (see below).
- **A leaked or over-eager API token**, e.g. an LLM agent. *Defence:* tokens can't register
  devices, change integrations, see secrets, manage tokens, pair, restore or flash. They can only
  touch registered devices, through their commands; on a Home Assistant device that includes
  the Home Assistant action (below). Tokens can be revoked instantly.
- **Crafted payloads** aimed at turning Hubbo into an open HA proxy (nested `target`,
  `entity_id: all`, raw service calls). *Defence:* strict schemas (`additionalProperties: false`,
  scalar params only), target-like names refused, typed HA calls built only from the static
  allowlist and HA's own service metadata (docs/ha-capabilities.md). The target is always a
  registered entity.
- **The Home Assistant action** (`call_service`, the user's decision 2026-11-13) is a deliberate
  escape hatch: anyone who can edit bindings or execute commands (the admin or an API token, no
  new role) can send any service of a registered HA device's own integration (its snapshot's
  platform: `webostv.*` on a webOS TV) with free JSON data to that device's entity. Only
  media player, light, switch and fan devices have such services. That reaches whatever HA
  allows for the integration's services, as far as the HA token's user may: for an MQTT light,
  `mqtt.publish` to any topic. Lock and alarm entities never get the action. What still holds: no other
  domain (`homeassistant.stop`, `hassio.*`, `script.*`: `forbidden_param`), the target is only
  the registered entity (no target keys in `data`), the data is at most 1024 bytes and 4 levels, templates are
  refused (HA's call_service wouldn't render them anyway), no response is ever requested, and at
  most 16 such actions per config. Accepted at hobby scale on a trusted LAN; the UI offers only
  the device's own services.

**Out of scope / accepted:**

- Someone with physical access. They can read flash unless flash encryption is on (below), hold
  the boot button, or just press the remote. The remote's RF link is unauthenticated by design
  (Harmony protocol), so anyone nearby with an nRF24 can inject button presses. That is accepted:
  it gives the same power as holding the remote.
- Internet exposure. Hubbo must not be port-forwarded. Remote access belongs to a VPN or HA.
- Sniffing on the LAN. HTTPS, secure boot and flash encryption are out of scope (user decision,
  2026-11-09): the LAN is trusted.
- DNS rebinding. The Host allowlist was removed in Phase 5. A rebinding page can't send the
  SameSite=Strict cookie, so it gets only what an anonymous caller gets.
- Online password guessing. There's no rate limit; each guess costs a PBKDF2 hash (about 0.5 s
  on the board), and the admin picks the password.
- A compromised Home Assistant. Hubbo trusts HA's responses about entities and states.
- Anyone who can publish to the MQTT broker. With `mqtt_main` enabled, they can switch
  activities, turn everything off and restart the hub through `hubbo/<node>/cmd/*` (no more
  than the remote or the HA device page can). Restart is refused during a firmware update, as
  `POST /admin/restart` is. Secure the broker with its own logins and ACLs.
- Bluetooth hosts nearby. Outside a pairing window, only paired hosts may connect (the
  advertising whitelist). While an admin's window is open, any host in range can bond to that
  device (as with any keyboard); the admin sees which device it landed on, and Forget undoes it.

## Roles

One admin account and any number of API tokens (max 16). No other roles.

| | Admin (session) | API token |
|---|---|---|
| Status, remote status, buttons | yes | yes |
| List devices, capabilities | yes | yes |
| Execute a registered command | yes | yes |
| Create/edit/delete/validate/activate activities, Off, global bindings | yes | yes |
| Integration summaries | yes | yes |
| Integration settings, discovery, Bluetooth device pairing and forget | yes | **no** |
| Register/edit/delete devices | yes | **no** |
| Remote pairing | yes | **no** |
| Tokens, admin password | yes | **no** |
| Backup, restore, firmware | yes | **no** |
| Rename the hub, restart | yes | **no** (the name is in status) |

The table is data in `core/src/auth/policy.cpp`: per operation the requirement (`Public`,
`TokenOrAdmin` or `AdminOnly`), the method, the body limit and the body type. Every operation in
the spec has `x-hubbo-role` (`public`, `token` or `admin`) and `api/lint.sh` fails if the two
disagree. Each route carries a requirement (`route.access`; an API route copies its operation's
row) and the router calls `authorize(route.access, context)` before the handler runs. Setup is
`Public`; its handler refuses it unless the credentials are `fresh`. The API's own description is
`Public` too: the index (`GET /api/v1/`), the spec (`/api/v1/openapi.json`), the agent guide
(`/api/v1/agent.md`) and `/llms.txt`. They're built into the firmware and say nothing about the
home; the index leaves out even the hub's name. The router's `401` (no valid credentials) and
its `404` for an unknown path point at the guide (`error.docs`). `core/tests/auth_policy_test.cpp`
contains the full matrix written out by hand, so a table change that widens access fails a test.

Registered-device restriction is enforced in validation, not by roles: an action may only name a
registered device of the same integration and a command from that device's catalog, with
parameters that pass its schema. This applies to every action, including nested startup,
shutdown and binding actions, and including the admin. Only the RF hot path skips HTTP auth, and
it runs prevalidated compiled actions.

## Credentials

### Admin password

- Created by the first-run wizard (`POST /auth/setup`), which only works while the credentials
  are `fresh`. The user picks the username (`^[A-Za-z0-9_.-]{1,32}$`); there's no default. Whoever
  reaches a fresh hub first creates the admin. No physical confirmation step (user decision,
  2026-11-08): the hub lives on a trusted home LAN and is set up right after flashing.
- 10–128 bytes, no composition rules.
- Stored as `pbkdf2-sha256$<iterations>$<salt>$<key>` (16-byte random salt, 32-byte key) in NVS.
  Default 10,000 iterations (the minimum accepted). Measured on the dev ESP32-S3 with the
  firmware built at -O2: about 550 ms per setup or login. The earlier default of 60,000 would
  take over 3 s there (and about 4.6 s in the -O0 build that was measured first, at the 5 s
  task watchdog: setup rebooted the board). The hash's cost is also what slows online guessing,
  since there's no rate limit. The iteration
  count is in the string, so existing hashes (including 60,000 ones) still verify and the
  default can change without a migration.
- Verification compares in constant time. An unknown username is verified against
  `dummyPasswordHash()` so it takes as long as a wrong password.
- PBKDF2 is the choice because mbedTLS has it (`mbedtls_pkcs5_pbkdf2_hmac_ext`) and it needs no
  big RAM buffers. A memory-hard KDF (Argon2, scrypt) would be stronger against offline cracking,
  but it would need a few MB of PSRAM per login. Revisit if the password hash ever leaves the
  device. Today it never does: it's not in backups, and NVS can be encrypted.

### Unreadable or damaged credentials

"Can't read" is never "doesn't exist" (`docs/design/persistence-and-contracts.md`, parts 1 and
6). `auth::Credentials` reads the store once, at boot, into one of three states, and only `fresh`
allows `POST /auth/setup`. Anything else would let whoever asks first take over a hub whose real
admin just can't be read:

- `fresh`: the store is readable and positively has no admin.
- `ok`: the admin (and tokens) loaded. If the medium goes away later, the cached hash and tokens
  keep working; writes (new tokens, a password change) fail.
- `unavailable`: the store couldn't be read, or holds damaged credentials (an intact record that
  doesn't decode, tokens without an admin, slots that exist but none is intact). Nothing can be
  authenticated: protected requests, login and setup get `503 storage_unavailable`. Recovery is
  a reboot, or the wipe (`bin/hubbo ota <ip> --wipe`) when the damage is real.

### Sessions

- `SessionStore`, RAM only, at most 8. A ninth login evicts the oldest.
- The session ID is 32 random bytes (base64url). `authenticate(id)` compares it against every
  slot in constant time, with no allocation.
- One absolute lifetime of 7 days from login, no idle timer. Logout deletes it. Changing the
  password ends every other session. A reboot ends all of them (a firmware update too), because
  nothing is written to flash.
- Cookie: `hubbo_session=<id>; Path=/; HttpOnly; SameSite=Strict`, plus `Secure` when served
  over HTTPS.

### Cross-site requests

There's no CSRF token (removed in Phase 5). These stop forged browser requests:

1. `SameSite=Strict`: browsers don't send the cookie on cross-site requests at all.
2. `Content-Type: application/json` is required for request bodies (the firmware image:
   `application/octet-stream`). Cross-site forms can't send either without a CORS preflight,
   and Hubbo sends no CORS headers, so the preflight fails.
3. `Origin`, when present, must match the `Host` header (`originAllowed()`), on every unsafe
   request, API and host-tool routes alike. `http::Router` checks it before authorization and
   answers `403 csrf_failed`.

### API tokens

- Format `hbt_<8-char id>_<43-char secret>` (256 random bits). Shown once at creation.
- Stored as `{id, name, SHA-256(token)}` in NVS. A fast hash is enough for a 256-bit random
  secret; a slow KDF would make every API call slow without adding security.
- Lookup hashes the presented token once and compares against every stored hash in constant
  time, without stopping early at the first match.
- `last_used_at` is kept in RAM only, to save flash wear; it reads `null` again after a reboot.
- Authentication (`api::Authenticator`): `Authorization: Bearer` wins over a cookie, and a bad
  bearer token never falls back to the cookie.

### Entropy

All secrets come from an injected `RandomSource`. On the ESP32 that's `esp_fill_random()`. Its
output is only truly random while the RF subsystem (BLE) is on, or with the bootloader entropy
source enabled. BLE is always on in Hubbo, but the firmware should only create the first
session/token after BLE init. If the source fails, the auth code fails closed.

## Simulator auth bypass

`SIMULATOR_NO_AUTH` exists only in the native simulator:

- It is the compile-time define `HUBBO_SIMULATOR_NO_AUTH`. `Principal::simulator()` only exists
  when it is defined, and `policy.h` has an `#error` if it is defined together with
  `ESP_PLATFORM` or `ARDUINO`. No runtime flag or HTTP parameter can enable it.
- `authorize()` also treats a `SimulatorBypass` credential as anonymous in any build without
  the define, which `auth_policy_test` checks for every operation.
- The simulator must bind to 127.0.0.1, show `AUTH DISABLED — SIMULATOR`, report
  `auth_mode: simulator_no_auth` in `/status` and `/auth/session`, and only drive fake adapters.

## Transport (TLS)

The admin web UI and API are plain HTTP on the LAN. HTTPS is out of scope (user decision,
2026-11-09): the LAN is trusted, and anyone who can sniff it can see the admin password, the
session cookie and API tokens. The `Secure` cookie attribute is set only if a TLS transport ever
sets `Request::secure`.

Outbound to Home Assistant: an `https://` URL connects over `wss://`, verified against the
public CA bundle in the firmware (`verify_tls: true`, the default). `verify_tls: false` keeps
the connection encrypted but doesn't verify the server, so anyone on the network path could pose
as HA and get the token: only for a self-signed HA on a trusted LAN. There's no custom CA
setting. `http://` HA URLs work but send the HA token in plaintext. The UI should warn.
Changing the HA URL clears the stored token unless a new one is sent, so a token can't be
redirected to another host by editing the URL.

Outbound to the MQTT broker: plain MQTT on the LAN, no TLS (user decision, 2026-11-13). The
broker username and password cross the LAN in plaintext, like the admin password does. Changing
the broker `host` clears the stored password unless a new one is sent, for the same reason as
the HA URL.

## Secrets at rest

All core secrets go through one port, `auth::SecretStore` (`core/include/hubbo/auth/secret_store.h`):
a small key-value interface shaped for NVS (keys up to 15 chars of `[a-z0-9_]`, values up to
512 bytes, durable on return). `auth::Credentials` keeps the admin username and password hash
in one key (`admin`: `<username>|<hash>`, so setup is a single atomic write) and one record per API token (`tok_<id>`: SHA-256, created time,
name) there. hub-sim and the tests use `MemorySecretStore`; the firmware implements the port on
NVS. Nothing secret goes into the ConfigService JSON, backups or responses.

The Home Assistant access token is the key `ha_token`. `PUT /admin/integrations/ha_main` is the
only way in (write-only: omit to keep, `null` to clear); responses show `has_access_token`. A
new HA URL erases the stored token before the URL is saved, unless the same request sends a
new token, so a token is never sent to a host it wasn't entered for. The platform reads it only
to hand it to the HA client (`api::applyHomeAssistantSettings`), which uses it in the auth
message and nowhere else.

The MQTT broker password is the key `mqtt_password`, and it works the same way.
`PUT /admin/integrations/mqtt_main` is the only way in (`password`: omit to keep, `null` to
clear). Responses show `has_password`, and a new `host` erases it. The platform reads it only to
hand it to the MQTT bridge (`api::applyMqttSettings`), which puts it in the CONNECT packet and
nowhere else. A restore asks for it again (`requires: mqtt_password`) when the restored broker
has a host.

| Secret | Where | In backups | In API responses |
|---|---|---|---|
| Admin password hash | NVS | no | never |
| API token hashes | NVS | no | never (plaintext once, at creation) |
| HA access token | NVS | no | never (`has_access_token: true/false`) |
| MQTT broker password | NVS | no | never (`has_password: true/false`) |
| BLE bond keys (and each host's identity address) | NimBLE's NVS store | no | never |
| TLS private key | NVS | no | never |
| Sessions | RAM | no | never (the cookie only, at login) |

NVS options on the ESP32-S3:

- **NVS encryption** (`CONFIG_NVS_ENCRYPTION`): encrypts NVS with keys held in a separate
  `nvs_keys` partition. Without flash encryption, the key partition is readable too, so on its
  own it only stops casual dumps.
- **Flash encryption** (release mode): AES-XTS on flash contents with an eFuse key the CPU can't
  read out. With NVS encryption on top, this protects every secret above from someone who
  desolders or dumps the flash. It's one-way: in release mode the board can't be reflashed over
  USB with plaintext images anymore, only OTA or encrypted images. Development mode allows a
  limited number of reflashes.
- **Secure Boot v2**: only images signed with the user's key boot. It pairs with signed OTA (the
  architecture requires authenticated updates), but it's also one-way, and losing the key bricks
  OTA.
- **HMAC-based NVS key protection** (ESP-IDF 5.x, eFuse HMAC key): derives the NVS key from an
  eFuse key without a key partition. Check whether arduino-esp32 3.3.12's prebuilt IDF config
  allows it.

Flash encryption and secure boot are out of scope (user decision, 2026-11-09). The notes below
stay for reference.

Recommendation: MVP ships with NVS encryption off but the code written so it can be turned on
(all secrets in NVS through one storage interface, nothing secret in LittleFS). Document an
opt-in "hardened" build: flash encryption (release) + NVS encryption + Secure Boot v2, with key
management written up before anyone enables it. These are permanent eFuse changes, so they need
the user's explicit decision and a spare board to test on. Arduino's prebuilt sdkconfig may
need a custom build (PlatformIO `custom_sdkconfig`) to enable them. Verify that before promising
it.

## Logs, diagnostics and errors

- Never log request bodies, `Authorization` or `Cookie` headers, the HA token, the MQTT
  password, passwords, or token plaintext. Log token IDs (`tok_…`) only.
- Error messages are fixed strings plus IDs and field paths. They never echo a submitted
  secret.
- `/status` diagnostics hold no URLs with credentials and no HA responses verbatim.
- Backups list what they exclude (`excluded`), and restore reports what must be re-entered
  (`requires`). While the config is blocked, the backup is the saved config record's raw bytes
  (`X-Hubbo-Backup: blocked-raw`); that record holds no secrets either.

## Firmware updates

`POST /admin/firmware/update` is admin-only (a token gets `403`). The body is the whole app
image, buffered in PSRAM by the transport, with its own body limit (the OTA slot, 6.25 MiB; the
policy table's one `Body::Binary` row). Before anything is erased, `api::validateFirmwareImage()`
checks it fits the slot, it's an ESP32-S3 app image for this chip revision (not a merged factory
image), it carries the Hubbo marker for this board and partition layout, and its appended
SHA-256 matches. Then the firmware's writer task writes it, sets it to boot and reboots. There's
no rollback (user decision).

These checks stop the realistic mistake, uploading the wrong file. They aren't a signature:
there's no secure boot, so an attacker with the admin session can flash anything that passes
them. That's the admin's power by definition. ArduinoOTA (for `bin/hubbo ota`) stays, behind
its own compiled-in password hash, and shares the one-update-at-a-time owner flag.

## Factory reset

`POST /admin/factory-reset` is admin-only (a token gets `403`) and is refused (`409`) during a
firmware update. It answers `202`, then the hub erases every secret (the login, tokens, the HA
token, the MQTT password), the BLE bonds, the remote endpoint and the config filesystem, and restarts into
first-run setup (`fresh`). Like the first boot, whoever reaches the hub first can then set it
up, which is fine on a trusted LAN. The Apple TV keeps its side of the pairing until it forgets
"Hubbo". `POST /admin/reset` (reset configuration) keeps all secrets and pairings, except an HA token
or MQTT password whose URL or host the reset changes. With
`unavailable` credentials nobody can authenticate, so recovery stays the wipe.

## Restart and the hub's name

A restart ends every session; tokens and everything saved stay. The hub's name isn't a
secret (it's in `/status` and on the network as the hostname); only the admin changes it.

## Security test checklist

Covered by unit tests today (`core/tests/auth_*_test.cpp`, `route_test.cpp`,
`firmware_update_test.cpp`, `ha_capabilities_test.cpp`):

- [x] Role matrix: every operation × anonymous/token/admin.
- [x] Tokens get 403 on every admin operation, firmware and factory reset included.
- [x] A foreign Origin is `403 csrf_failed` on API and host-tool routes.
- [x] Setup works only while `fresh`, and takes the user's own username.
- [x] Credentials read once at boot: unreadable or damaged is `unavailable`, never `fresh`.
- [x] Forged simulator principal gets nothing in non-simulator builds.
- [x] Sessions: absolute expiry, eviction, logout, reboot loses all.
- [x] Factory reset: refused mid-update, 202 first, then every secret erased and setup open
      again (contract, both modes).
- [x] Tokens: wrong secret, wrong id, malformed and revoked tokens rejected.
- [x] Password hashing: RFC test vectors, salts differ, malformed stored hashes rejected.
- [x] Firmware images: every validation step refuses its mistake, in order; the body type and
      limit come from the policy table.
- [x] HA: `entity_id`/`target`/`area_id`/… params refused; ranges, types, enums; no toggle,
      play_media or join; broadcast and malformed entity IDs refused; only allowlisted data keys.
- [x] OpenAPI: nested objects/arrays in params, raw service fields and HA entity IDs as
      `device_id` are refused by the schema (`api/fixtures/malicious/`, checked by
      `api/lint.sh`).

Covered over HTTP against hub-sim by `api/contract.sh` (both modes):

- [x] Auth-enabled simulator: setup (bad username refused, no default `admin`), login, cookies,
      tokens, every admin operation 403 for a token (a real firmware image too), revocation,
      logout, cross-origin requests refused, wrong passwords stay 401.
- [x] Discovery: the index, spec, guide and `/llms.txt` need no credentials; the served spec
      and guide equal the repo's (plain and gzipped); every anonymous `401` and an unknown
      path's `404` carry `docs`, a missing id's `404` doesn't.
- [x] Restart and rename: anonymous 401, token 403 (the role matrix); restart refused
      mid-update, 202, then every session gone and the name kept.
- [x] Firmware update: every refusal (411, 413, 415, 422 `wrong_board`/`invalid_image`, 409,
      503) changes nothing; a good image writes, reboots and ends idle; a failed write.
- [x] Credentials unreadable at boot or damaged: `unavailable`, setup and login 503.
- [x] The malicious fixtures are refused (400/422).
- [x] Body limits (`413`), JSON depth limit (`400`), `415` for non-JSON.
- [x] No secret-named field in any response (except the one-time token creation).
- [x] The HA token, once sent, appears in no response of any operation or status, the backup
      included; a new URL clears it; the token is write-only (`has_access_token` only).

Still to do:

- [ ] No secret in any log line (grep the simulator's output for the test secrets). Backups are
      covered for the HA token (above).
- [ ] CI proves the ESP32 build cannot define `HUBBO_SIMULATOR_NO_AUTH`.
- [ ] On hardware: `esp_fill_random` after BLE init.
