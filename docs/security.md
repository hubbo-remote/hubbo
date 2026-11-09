# Security

Hubbo is a LAN appliance that controls real devices and holds a Home Assistant credential. This
document covers the threat model, how access control works, and the transport and storage
choices. The code lives in `core/include/hubbo/auth/` and `core/include/hubbo/ha/`; the API
contract is `api/openapi.yaml`.

## Threat model

**Assets**, most valuable first:

1. The Home Assistant access token. It can do whatever its HA user can do in the whole house,
   not only what Hubbo has registered.
2. The admin password and session. They give full control of Hubbo, including the HA settings.
3. API tokens. They can drive every registered device and rewrite activities.
4. BLE bond keys (the Apple TV pairing).
5. Configuration (devices, activities). Not secret, but tampering means the remote does the
   wrong thing.

**Attackers we design for:**

- **Anything else on the LAN**: a compromised IoT device, a guest phone. It can reach the
  hub's HTTP port and sniff plaintext traffic on a shared segment. *Defence:* authentication on
  every route except login/session, rate-limited login, TLS (below), no default credentials.
- **A malicious web page in the admin's browser** (CSRF, DNS rebinding). *Defence:*
  SameSite=Strict HttpOnly cookie, per-session CSRF token header, JSON-only bodies, Origin check,
  no CORS headers, and a Host check (see below).
- **A leaked or over-eager API token**, e.g. an LLM agent. *Defence:* tokens can't register
  devices, change integrations, see secrets, manage tokens, pair, restore or flash. They can only
  touch registered devices through allowlisted commands with typed params. Tokens can be revoked
  instantly.
- **Crafted payloads** aimed at turning Hubbo into an open HA proxy (nested `target`,
  `entity_id: all`, raw service calls). *Defence:* strict schemas (`additionalProperties: false`,
  scalar params only), target-like names refused, HA calls built only from the static allowlist
  (docs/ha-capabilities.md).

**Out of scope / accepted:**

- Someone with physical access. They can read flash unless flash encryption is on (below), hold
  the boot button, or just press the remote. The remote's RF link is unauthenticated by design
  (Harmony protocol), so anyone nearby with an nRF24 can inject button presses. That is accepted:
  it gives the same power as holding the remote.
- Internet exposure. Hubbo must not be port-forwarded. Remote access belongs to a VPN or HA.
- A compromised Home Assistant. Hubbo trusts HA's responses about entities and states.

## Roles

One admin account and any number of API tokens (max 16). No other roles.

| | Admin (session) | API token |
|---|---|---|
| Status, remote status, buttons | yes | yes |
| List devices, capabilities | yes | yes |
| Execute a registered command | yes | yes |
| Create/edit/delete/validate/activate activities, Off, global bindings | yes | yes |
| Integration summaries | yes | yes |
| Integration settings, discovery, BLE pairing | yes | **no** |
| Register/edit/delete devices | yes | **no** |
| Remote pairing | yes | **no** |
| Tokens, admin password | yes | **no** |
| Backup, restore, firmware | yes | **no** |

The table is data in `core/src/auth/policy.cpp` (role, method and CSRF exemption per operation).
Every operation in the spec has `x-hubbo-role` (and login/setup `x-hubbo-csrf-exempt`), and
`api/lint.sh` fails if the two disagree. Each route handler calls
`authorize(operation, context)` before reading the body. `core/tests/auth_policy_test.cpp`
contains the full matrix written out by hand, so a table change that widens access fails a test.

Registered-device restriction is enforced in validation, not by roles: an action may only name a
registered device of the same integration and a command from that device's catalog, with
parameters that pass its schema. This applies to every action, including nested startup,
shutdown and binding actions, and including the admin. Only the RF hot path skips HTTP auth, and
it runs prevalidated compiled actions.

## Credentials

### Admin password

- Created by the first-run wizard (`POST /auth/setup`), which only works while no admin exists.
  Whoever reaches a fresh hub first creates the admin. No physical confirmation step (user
  decision, 2026-11-08): the hub lives on a trusted home LAN and is set up right after flashing.
- 10–128 bytes, no composition rules.
- Stored as `pbkdf2-sha256$<iterations>$<salt>$<key>` (16-byte random salt, 32-byte key) in NVS.
  Default 60,000 iterations: measure on the board and raise it if login stays well under half a
  second. The iteration count is in the string, so it can go up without a migration.
- Verification compares in constant time. An unknown username is verified against
  `dummyPasswordHash()` so it takes as long as a wrong password.
- PBKDF2 is the choice because mbedTLS has it (`mbedtls_pkcs5_pbkdf2_hmac_ext`) and it needs no
  big RAM buffers. A memory-hard KDF (Argon2, scrypt) would be stronger against offline cracking,
  but it would need a few MB of PSRAM per login. Revisit if the password hash ever leaves the
  device. Today it never does: it's not in backups, and NVS can be encrypted.

### Unreadable or damaged credentials

"Can't read" is never "doesn't exist" (`docs/design/persistence-and-contracts.md`, parts 1 and
6). `auth::Credentials` has four states, and only `fresh` allows `POST /auth/setup`; anything
else would let whoever asks first take over a hub whose real admin just can't be read:

- `fresh`: the store is readable and positively has no admin.
- `ok`: the admin (and tokens) loaded.
- `unavailable`: the medium can't be read. Nothing can be authenticated, so protected requests,
  login and setup get `503 storage_unavailable`.
- `corrupt`: the secrets exist but can't be used: an intact record that doesn't decode, tokens
  without an admin, or a record whose slots are present but none is intact (a torn or rotted
  write). Setup and login get `409 credentials_corrupt`. The network can't repair this.

The authenticator calls `Credentials::refresh()` on every request. Outside `ok` it reloads with
backoff (at once on a medium health transition, then 1 s doubling to 60 s), so a broken store
costs one reload a minute, not one per request. `setupAdmin()` ignores that schedule and always
re-reads the store before deciding, so a stale `fresh` or `unavailable` never leads to a new
admin.

### Sessions

- `SessionStore`, RAM only, at most 8. A ninth login evicts the least recently used.
- Session ID and CSRF token are each 32 random bytes (base64url). One allocation-free
  `authenticate(id, csrf)` call per request checks both, comparing against every slot in
  constant time.
- Idle timeout 8 h, absolute lifetime 24 h. Logout deletes it. Changing the password ends every
  other session. A reboot ends all of them, because nothing is written to flash.
- Cookie: `hubbo_session=<id>; Path=/; HttpOnly; SameSite=Strict`, plus `Secure` when served
  over HTTPS.

### CSRF and browser defences

1. `SameSite=Strict`: browsers don't send the cookie on cross-site requests at all.
2. Synchronizer token: the CSRF token comes back from login and `GET /auth/session` in the JSON
   body. The SPA keeps it in memory (not `localStorage`) and sends `X-Hubbo-CSRF` on every
   POST/PUT/DELETE. `authorize()` refuses cookie-authenticated unsafe requests without a valid one
   (`403 csrf_failed`), except login and setup, which run before a session exists.
3. `Content-Type: application/json` is required for request bodies. Cross-site forms can't
   send that without a CORS preflight, and Hubbo sends no CORS headers, so the preflight fails.
4. `Origin`, when present, must match the `Host` header (`originAllowed()`), on every unsafe
   request, API and host-tool routes alike (`http::Router` checks it before
   authorization).
5. **DNS rebinding:** `http::Router::setAllowedHosts()` rejects requests whose `Host` (port
   ignored, case-insensitive) is not one of the hub's own names, with `421 host_not_allowed`,
   before routing. A missing Host is rejected too. hub-sim allows `localhost`, `127.0.0.1` and
   `[::1]`; the firmware must set its IP address, mDNS name and hostname (and update the list
   when DHCP changes the address).

Bearer tokens are not sent automatically by browsers, so bearer requests skip CSRF checks.

### API tokens

- Format `hbt_<8-char id>_<43-char secret>` (256 random bits). Shown once at creation.
- Stored as `{id, name, SHA-256(token)}` in NVS. A fast hash is enough for a 256-bit random
  secret; a slow KDF would make every API call slow without adding security.
- Lookup hashes the presented token once and compares against every stored hash in constant
  time, without stopping early at the first match.
- `last_used_at` is kept in RAM only, to save flash wear; it reads `null` again after a reboot.
- Authentication (`api::Authenticator`): `Authorization: Bearer` wins over a cookie, and a bad
  bearer token never falls back to the cookie.

### Login rate limiting

`LoginRateLimiter`, also used for password change:

- Per client (IPv4 address, or IPv6 /64): 5 free failures, then a lockout that doubles from 1 s
  up to 15 min. A record is forgotten after an hour without failures. Success clears it.
- Global: more than 30 failures in a minute locks out every client until the minute ends, so
  rotating addresses doesn't help.
- Fixed 16-entry table. A full table recycles the stalest record.
- Responses are `429 rate_limited` with `Retry-After`.

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

Plain HTTP on the LAN exposes the admin password, session cookie and API tokens to anyone who
can sniff. The architecture requires HTTPS or trusted TLS termination. Options, best first:

1. **TLS on the ESP32 with a locally issued certificate.** `esp_https_server` (mbedTLS) works
   with arduino-esp32 3.x. Costs: about 40–50 KB of RAM per concurrent TLS session (put buffers
   in PSRAM), a slower first handshake, and certificate handling. Use ECDSA P-256 for faster
   handshakes. Certificate choices:
   - *Self-signed, generated on first boot*: works offline, but the browser warns, and users
     learn to click through warnings. Show the SHA-256 fingerprint in the first-run wizard and on
     the serial console so the user can check it once.
   - *User-supplied cert/key* (e.g. from their own CA or a Let's Encrypt DNS-01 cert for
     `hubbo.home.example.com`): no warnings, more setup. Upload is admin-only, and the key goes
     in NVS.
2. **Trusted TLS termination** (a reverse proxy the user already runs, e.g. Caddy or HA's NGINX
   add-on) with Hubbo reachable only from that proxy. Needs an explicit "trusted proxy" setting,
   and then Hubbo must refuse direct HTTP from other hosts. This is easy to misconfigure.
3. **Plain HTTP**: only for first boot and for the simulator. The UI should show a persistent
   warning, and the `Secure` cookie attribute can't be used.

Recommendation: ship (1) with a self-signed cert generated on first boot, keep HTTP on port 80
only to redirect to HTTPS (after setup), and allow a user-supplied cert. Measure RAM and
latency on the board before committing (it's in the architecture's verification list). RF
dispatch must not wait on TLS. The HTTP server runs at lower priority than the RF and engine
tasks.

Outbound to Home Assistant: verify HA's certificate (`verify_tls: true` by default). For a
self-signed HA, the admin pastes HA's CA certificate (`ca_cert_pem`) rather than turning
verification off. `http://` HA URLs work but send the HA token in plaintext. The UI should warn.
Changing the HA URL clears the stored token unless a new one is sent, so a token can't be
redirected to another host by editing the URL.

## Secrets at rest

All core secrets go through one port, `auth::SecretStore` (`core/include/hubbo/auth/secret_store.h`):
a small key-value interface shaped for NVS (keys up to 15 chars of `[a-z0-9_]`, values up to
512 bytes, durable on return). `auth::Credentials` keeps the admin username and password hash
in one key (`admin`: `<username>|<hash>`, so setup is a single atomic write) and one record per API token (`tok_<id>`: SHA-256, created time,
name) there. hub-sim and the tests use `MemorySecretStore`; the firmware implements the port on
NVS. Nothing secret goes into the ConfigService JSON, backups or responses.

| Secret | Where | In backups | In API responses |
|---|---|---|---|
| Admin password hash | NVS | no | never |
| API token hashes | NVS | no | never (plaintext once, at creation) |
| HA access token | NVS | no | never (`has_access_token: true/false`) |
| BLE bond keys | NimBLE's NVS store | no | never |
| TLS private key | NVS | no | never |
| Sessions | RAM | no | CSRF token only, to its own session |

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

Recommendation: MVP ships with NVS encryption off but the code written so it can be turned on
(all secrets in NVS through one storage interface, nothing secret in LittleFS). Document an
opt-in "hardened" build: flash encryption (release) + NVS encryption + Secure Boot v2, with key
management written up before anyone enables it. These are permanent eFuse changes, so they need
the user's explicit decision and a spare board to test on. Arduino's prebuilt sdkconfig may
need a custom build (PlatformIO `custom_sdkconfig`) to enable them. Verify that before promising
it.

## Logs, diagnostics and errors

- Never log request bodies, `Authorization` or `Cookie` headers, the HA token, password, or
  token plaintext. Log token IDs (`tok_…`) only.
- Error messages are fixed strings plus IDs and field paths. They never echo a submitted
  secret.
- `/status` diagnostics hold no URLs with credentials and no HA responses verbatim.
- Backups list what they exclude (`excluded`), and restore reports what must be re-entered
  (`requires`). While the config is blocked, the backup is the saved config record's raw bytes
  (`X-Hubbo-Backup: blocked-raw`); that record holds no secrets either.

## Firmware updates

`POST /admin/firmware/update` is admin-only and CSRF-protected. The image is verified (signature
if Secure Boot / signed OTA is set up, board ID, size, SHA-256) before it is marked bootable, and
a failed first boot rolls back. Until signing is in place, an attacker with the admin session
can flash anything. That's the admin's power by definition, but it raises the value of the admin
session, which is another reason for TLS.

## Security test checklist

Covered by unit tests today (`core/tests/auth_*_test.cpp`, `ha_capabilities_test.cpp`):

- [x] Role matrix: every operation × anonymous/token/admin.
- [x] Tokens get 403 on every admin operation.
- [x] CSRF required for cookie POST/PUT/DELETE; not for bearer; login/setup exempt.
- [x] Setup closes once an admin exists.
- [x] Forged simulator principal gets nothing in non-simulator builds.
- [x] Sessions: expiry (idle and absolute), eviction, logout, reboot loses all, CSRF token bound
      to its session.
- [x] Tokens: wrong secret, wrong id, malformed and revoked tokens rejected.
- [x] Password hashing: RFC test vectors, salts differ, malformed stored hashes rejected.
- [x] Rate limiter: free attempts, doubling, cap, global limit, forgetting.
- [x] HA: `entity_id`/`target`/`area_id`/… params refused; ranges, types, enums; no toggle,
      play_media or join; broadcast and malformed entity IDs refused; only allowlisted data keys.
- [x] OpenAPI: nested objects/arrays in params, raw service fields and HA entity IDs as
      `device_id` are refused by the schema (`api/fixtures/malicious/`, checked by
      `api/lint.sh`).

Covered over HTTP against hub-sim by `api/contract.sh` (both modes):

- [x] Auth-enabled simulator: setup, login, cookie + CSRF, tokens, every admin operation 403 for
      a token, revocation, logout, login rate limiting, cross-origin login refused.
- [x] The malicious fixtures are refused (400/422).
- [x] Body limits (`413`), JSON depth limit (`400`), `415` for non-JSON.
- [x] Host-header allowlist (`421`).
- [x] No secret-named field in any response (except the one-time token creation).

Still to do:

- [ ] No secret in any log line or backup (grep the simulator's output for the test secrets).
- [ ] CI proves the ESP32 build cannot define `HUBBO_SIMULATOR_NO_AUTH`.
- [ ] On hardware: PBKDF2 login time, TLS RAM and handshake time, `esp_fill_random` after BLE
      init.
