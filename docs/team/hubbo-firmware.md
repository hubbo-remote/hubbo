# hubbo-firmware: working notes

Current as of the Phase 4 gate (passed), on branch `phase-4`; Phase 3 merged. Not a log: state,
numbers and traps. Design: `docs/design/phase3-home-assistant.md`.

## State

- Phase 2 (7797413): the restore OOM fixed with extmem(0), `-O2`, loop priority 16, Ethernet on
  core 0, `/debug/bench`.
- Phase 3 (merged, PR #2):
  - `ws_transport.*`: `EspWsTransport`, the core `WebSocketTransport` port on `esp_transport_ws`.
    - ws:// runs over TCP; wss:// runs over `esp_transport_ssl`.
    - `ConnectOptions.verifyTls` on: the ESP-IDF certificate bundle, plus esp-tls's default
      hostname check. Off: `attachNoVerify` sets `MBEDTLS_SSL_VERIFY_NONE`, so it's encrypted but
      unchecked. The prebuilt esp-tls refuses a connection with no verification option, since
      `CONFIG_ESP_TLS_INSECURE` is off. SNI is sent either way.
    - `lastError()` names the failed stage: certificate rejected (verify flags), upgrade HTTP
      status, or connect/TLS codes.
    - Fragments are reassembled up to 4 MB.
    - mbedTLS allocates from PSRAM (`useExternalTlsMemory()`).
  - `ha_task.*`: core 0, priority 4, 8 KB PSRAM stack; a job queue plus a service callback that
    returns `client.hasWork()` (an idle pass sleeps 10 ms on the queue).
  - `main.cpp` wires `haTransport` → `haClient` → `haAdapter("ha_main")`; the service is
    `transport.poll(hasWork ? 0 : 50)`, `client.poll(now)`, `adapter.service()`;
    `client.setYield(vTaskDelay(1))`; `applyHomeAssistantSettings` at boot (the API on PUT).
  - `remote_runtime.*`: `api::RemotePort` over RemoteRadio. `status()` reads a copy the loop
    publishes; requests go through a single-slot mailbox the loop takes (spinlock on copies only).
  - `engine_runtime.*` reports the HA integration state (core's `haIntegrationState`, spec via
    `setConfig()`) and device availability (`integrations::haDeviceAvailability()`, as the sim).
  - `RadioEndpoint::isValid()` uses `api::isValidRemoteEndpoint`.
- Phase 4 (not committed; gate passed). 4.5: the web admin is embedded in the image.
  - `scripts/web_assets.py` (pre-build): packs `web/dist` into `$BUILD_DIR/web_gen/web_assets.cpp`
    (gzip -9, mtime 0; path table, ETag over the dist, CSP with the inline theme script's
    SHA-256). It fails with "run `bin/hubbo web build`" when `index.html` is missing, skips
    `.map` files, and refuses unknown extensions. `src/web_assets.h` is the table's interface.
  - `HttpServer::serveWebAdmin()`: GET `/*`, registered after `/api` and `/debug`.
    - Headers through `EspHeaders` (the API's reader). Host allowlist: `router.hostAllowed`,
      otherwise a JSON 421 like the API's.
    - `/a/*` is immutable; other files get `no-cache` with the ETag, and 304 on a match.
    - Bodies are always gzipped, with nosniff, no-referrer and the CSP.
    - A non-file path gets `index.html` when the request accepts HTML (never under `/a/`),
      else a JSON 404.
    - `httpd_resp_send` sends straight from flash.
  - `max_open_sockets` is 7.
- §3a: `ble_pairing_runtime.*`, an
  `api::BlePairingPort` over BleHid (`ApiContext.blePairing`), built like `RemoteRuntime`.
  - httpd posts Start or Forget into a single-slot mailbox. The loop runs `ble.startPairing()` or
    `ble.forget()`, the console's `pair` and `ble forget` paths, so bond deletion still goes
    through BleHid's httpd offload.
  - `status()` reads a snapshot built in `publishRuntime()` from what it already reads (state,
    cached peer, `ble.pairingFailed()`); no second copy of BLE state. After a request the loop
    publishes at once, and a request not yet taken shows in `status()` already.
  - Failed lives in BleHid: set when a window times out with no bond, cleared by startPairing
    or forget (console too). `pair stop` reads Idle.
  - `forget()` clears `secured`, so a forgotten host isn't Connected while its disconnect lands.
- Gate wipes: `HUBBO_WIPE_ON_BOOT=1` in the build environment makes `scripts/wipe_on_boot.py`
  define `HUBBO_WIPE_ON_BOOT`, which compiles in `wipeForGate()` early in `setup()`.
  - It erases all NVS, then calls `flash_fs::requestFormatAtBoot()` (the `fs format` path), so
    `mountFilesystem()` formats at once. It logs `WIPE: factory reset build; ...`.
  - OTA survives, because its password hash is compiled in.
  - The image wipes on **every** boot, so `bin/hubbo ota <ip> --wipe` (the lead's) flashes the
    normal build straight after it.
  - The variable must be passed into the container: list it in `docker-compose.yml` beside
    `HUBBO_OTA_PASSWORD`.
- Dev-only endpoints (`_ota` env): `/debug/log`, `/debug/run?cmd=...`, `/debug/bench`,
  `/debug/coredump`, `/debug/ws-echo` and `/debug/dns`.
  - `/debug/ws-echo?url=ws[s]://...` with `&mode=open`, `&hold=N` or `&async=1` (logs instead).
    **Use async when you measure API latency**: a synchronous probe blocks httpd, faking a stall.
  - `/debug/dns?host=...`: one lookup, on the HA task.
  - Query values decode through core's `http::queryValue`, so both `+` and `%20` work.
    `Request.query` stays raw (the core contract); decode per parameter, never the whole query.
  - The ws echo test server is `firmware/.pio/wstest/echo.py`. It isn't in git. Run it with
    `bin/hubbo run python3 -u /work/firmware/.pio/wstest/echo.py 18765`; the host is 192.168.1.151.
    For wss://, add `cert.pem key.pem` (same folder, self-signed). **Stop it when done.**
- No background wait loops (CLAUDE.md): the lead messages when other agents' interfaces land.
- The HA client has its own clock (`haNowMs`), and the service reads it after
  `transport.poll()`. A connect blocks up to 15 s, so a time taken before it made the auth
  deadline stale.

## Task layout (priorities)

| task | core | prio | notes |
|---|---|---|---|
| loopTask (RF + engine) | 1 | 16 | busy loop; above every unpinned task |
| tiT (lwIP) | 0 | 18 | |
| w5500_tsk | any | 15 | ends up on core 0 because the loop outranks it |
| httpd | 0 | 5 | API plus dev endpoints; single-threaded |
| ha | 0 | 4 | **below httpd**: a TLS handshake is seconds of CPU |
| IDLE0 | 0 | 0 | watched by the 5 s task watchdog. Never weaken it. |

## Measured (dev board)

- Phase 3 gate (HA over wss): BLE read→HID p50 0.45 ms, p99 0.46 ms; loop max gap 1.9 ms, 0 over
  5 ms; one HA light about 118 ms through the board (70–90 ms direct); HA down: presses fail at
  once, BLE keeps working, reconnect in about 6 s.
- Loop max gap: idle 0.5–0.8 ms; during a wss handshake under API load 0.9–2.5 ms; held
  0.75–2.5 ms. 0 iterations over 5 ms in all cases.
- API latency (120 × GET /api/v1/status, which answers 401): p50 22.5–23.5 ms, p99 28–41 ms,
  idle, during a handshake or held alike.
- TLS handshake to the proxy (ECDSA, X25519, a 5-cert chain): 3.0–3.4 s idle, 6–9 s under
  heavy API load; the connect timeout is 15 s.
- TLS memory with the PSRAM allocator: −0.7 KB internal and about 44 KB PSRAM per session. On
  internal RAM (the prebuilt default) it cost 44 KB internal, min 36 KB, at the same speed.
- Internal RAM free: about 89 KB idle, about 79 KB with HA and BLE connected. Flash (OTA env)
  1.88 MB: TLS and the bundle added about 80 KB, the web admin 156 KB.
- verify_tls: on refuses a wrong name and a self-signed cert; off accepts both. The proxy by IP
  fails at TLS either way (it routes by SNI).
- Web admin (22 files, 154 KB gzipped): cold load 1.25–1.41 s over 6 keep-alive connections;
  warm load (304) 27–31 ms; the 98 KB bundle about 230 ms (about 450 KB/s, limited by lwIP's fixed
  5.7 KB send buffer). During repeated cold loads the loop gap stays under 2.7 ms (0 over 5 ms),
  and internal RAM stays above 75.6 KB. A real browser passes with no CSP violations.
- The board's LAN round trip is 5.5 ms by ping (W5500 on its INT pin, 20 MHz SPI; the lead said
  to leave it). That sets the floor: about 6 ms to connect, 15 ms to the first byte.
- DNS through DHCP: 7–16 ms. An unresolvable `.local` name takes about 7 s (HA task only).

## Gotchas

- Toolchain: Docker only.
  - `bin/hubbo fw ...` holds `build/firmware.lock`; don't work around it.
  - Flash only with `bin/hubbo ota 192.168.1.196`.
  - espota picks a random host port. "Address already in use" is a one-off collision; just retry.
- Prebuilt libs (arduino-esp32 3.3.12, IDF 5.5.5):
  - No `esp_websocket_client`; `tcp_transport` (ws, ssl), esp-tls and the full cert bundle exist.
  - mbedTLS: TLS 1.2 only, no ECC hardware, `ECP_RESTARTABLE` off.
  - `MBEDTLS_INTERNAL_MEM_ALLOC=y` forces internal RAM. Override it at runtime with
    `mbedtls_platform_set_calloc_free`, which is global.
- esp_transport_ws masks the payload **in place**. Sending a literal from flash rodata caused a
  CacheError, so `send()` copies into a writable buffer.
- The WS transport doesn't own its parent (tcp or ssl): destroy both.
- The TLS handshake is CPU-bound and never blocks. At HA priority 6 it starved httpd, causing
  0.5–3.5 s API stalls and core-1 loop gaps up to 13 ms. Priority 4 fixed both.
  - The mbedTLS allocator also yields a tick every 20 ms during a handshake, about 2,400 allocations
    per handshake, so IDLE0 (and the watchdog) keep running.
- esp-tls reports the mbedTLS code negated, as a positive number. The bundle's rejection is
  -0x2700 with verify flags 0, so `lastError()` maps that code to "certificate rejected".
- Test servers via `bin/hubbo run timeout N ...`: `docker stop` afterwards. A quick restart can
  fail to bind; check the server log before trusting a run.
- No lwIP socket calls on the RF loop: each takes lwIP's global SYS_ARCH_PROTECT mutex, held by
  tiT and the HA task on core 0. ArduinoOTA's per-loop `ioctl(FIONREAD)` stalled the loop up to
  6.3 ms during HA discoveries. `ota.cpp` has a core-0 `ota_watch` task `select()` on its socket
  (found by port 3232) and runs `handle()` only after a packet, for 10 s.
- The dev console's 30 s `printStatus()` takes 1.5–2 ms; it's the usual idle peak.
- PSRAM stacks: fine for the HA task and `ota_watch`, which never write flash.
- `heap_caps_malloc_extmem_enable(0)` runs after the drivers start, so all later mallocs try PSRAM
  first.
- W5500 RX at priority 1 used to take core 1 for 10–17 ms. Keep the loop at 16, above every
  unpinned task.
- `max_uri_handlers = 12`: the API uses 4 (one per method), the dev console 6 and the web admin 1.
  Raise it before adding more. The web admin's `/*` must be registered last.

## Open items

- Commits: only on the lead's instruction, /simplify first, then /commits on `firmware/`,
  `docs/hardware/` and this file, staged by path.
- Possible later cleanups from the Phase 3 /simplify, skipped for now:
  - alias `RadioEndpoint` and the RemoteRadio enums to the `api::` types (about 30 references);
  - in `EspWsTransport`, reserve and append the reassembly buffer instead of regrowing it from zero
    for each message;
  - an atomic fast path for `RemoteRuntime::poll()`'s spinlock on the loop.
