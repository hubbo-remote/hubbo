# hubbo-firmware: working notes

Current as of Phase 3 (Home Assistant; the board gate has passed), committed on branch `phase-3`. This file is not a
log: it covers state, numbers and traps. See `docs/design/phase3-home-assistant.md` for the design.

## State

- Phase 2 is committed (7797413). It covered:
  - the restore OOM, fixed with extmem(0);
  - the `-O2` build;
  - loop priority 16, and Ethernet on core 0;
  - `/debug/bench`.
- Phase 3 firmware is committed on `phase-3` (510599f, 1c24800, e204fd1, 6061aee) and running on
  the dev board, except the `lastError()` message fix, which goes out with the next OTA.
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
  - `ha_task.*`: the HA task.
    - Core 0, priority 4, 8 KB stack in PSRAM.
    - It runs a job queue plus a service callback. The service returns `client.hasWork()`; a pass
      that neither waited nor has work sleeps 10 ms on the job queue.
  - `main.cpp` wires:
    - `haTransport`, then `haClient`, then `haAdapter("ha_main")`, registered in `adapters`;
    - the HA task's service, the core contract:
      `transport.poll(hasWork ? 0 : 50)`, then `client.poll(now)`, then `adapter.service()`;
    - `client.setYield(vTaskDelay(1))`;
    - `api::applyHomeAssistantSettings` at boot, while the API reconfigures on PUT;
    - `ApiContext.remote`, `.homeAssistant` and `.secrets`.
  - `remote_runtime.*`: `api::RemotePort` over RemoteRadio.
    - The status is a copy the loop publishes. `status()` shows a requested scan as Scanning
      until the loop takes it.
    - Requests go through a single-slot mailbox the loop takes; a spinlock guards only the copies.
  - `engine_runtime.*` reports:
    - the HA integration state, from core's `haIntegrationState(client, spec)`. The spec is looked
      up through `setConfig()`, set at boot and read on httpd. Both /status and the integration
      list need it.
    - HA device availability: core's `integrations::haDeviceAvailability()`, shared with the sim.
  - `RadioEndpoint::isValid()` uses `api::isValidRemoteEndpoint`.
- Dev-only endpoints (`_ota` env): `/debug/log`, `/debug/run?cmd=...`, `/debug/bench`,
  `/debug/coredump`, `/debug/ws-echo` and `/debug/dns`.
  - `/debug/ws-echo?url=ws[s]://...`:
    - `&mode=open` opens, then closes without sending;
    - `&hold=N` holds the connection N seconds;
    - `&async=1` reports to the log, which keeps httpd free. **Use async when you measure API
      latency**: a synchronous probe blocks httpd and fakes a stall.
  - `/debug/dns?host=...`: one lookup, on the HA task.
  - Query values decode through core's `http::queryValue`, so both `+` and `%20` work.
    `Request.query` stays raw (the core contract); decode per parameter, never the whole query.
  - The ws echo test server is `firmware/.pio/wstest/echo.py`. It isn't in git. Run it with
    `bin/hubbo run python3 -u /work/firmware/.pio/wstest/echo.py 18765`; the host is 192.168.1.151.
    For wss:// with a self-signed cert, add `cert.pem key.pem` (both in the same folder, valid 30
    days from Phase 3).
    **Stop it when the test is done**: never leave a test server running in the background.
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

## Measured (dev board, Phase 3 build)

- Phase 3 board gate (passed; the user on the remote, HA connected over wss):
  - BLE read→HID p50 0.45 ms, p99 0.46 ms;
  - loop max gap 1.9 ms, 0 iterations over 5 ms;
  - HA single light about 118 ms through the board (70–90 ms to HA directly); group lights 1–3 s,
    which is HA's own time;
  - HA unreachable (dead IP): HA presses fail at once with adapter_unavailable, and BLE keeps
    working;
  - reconnect after the real URL was restored: about 6 s;
  - the token appears in no response, backup or debug log.

- Loop max gap: idle 0.5–0.8 ms; during a wss handshake under API load 0.9–2.5 ms; held
  0.75–2.5 ms. 0 iterations over 5 ms in all cases.
- API latency (120 × GET /api/v1/status, which answers 401): p50 22.5–23.5 ms, p99 28–41 ms,
  idle, during a handshake or held alike.
- TLS handshake to the proxy (ECDSA, X25519; chain leaf, YE2, Root YE, X2, X1):
  - 3.0–3.4 s idle;
  - 6.2 s under continuous API load;
  - 9.4 s under heavier load.
  - The connect timeout is 15 s.
- TLS memory with the PSRAM allocator: −0.7 KB internal and about 44 KB PSRAM per session. On
  internal RAM (the prebuilt default) it cost 44 KB internal, min 36 KB, at the same speed.
- Internal RAM free at idle is about 89 KB. Flash is about 1.72 MB for the OTA env; TLS and the
  bundle added about 80 KB.
- verify_tls on the board (all through `/debug/ws-echo`, no token):
  - real proxy, verify on: connects in 3.5 s;
  - `wrong.host.badssl.com`, verify on: refused (mbedTLS -0x2700, X509 verify failed). Verify off:
    TLS passes, then the upgrade gets HTTP 200, which shows the certificate was accepted;
  - self-signed local echo, verify on: refused (the server logs the board's bad-certificate
    alert). Verify off: full script passes over TLS, the 200 KB message in about 750 ms;
  - proxy by IP: fails at TLS in both modes (`unrecognized_name` alert, -0x7780), because it
    routes by SNI;
  - loop during these tests: 0.4–1.5 ms, 0 iterations over 5 ms, in about 20 runs. One unexplained
    9.2 ms gap (1 iteration over 5 ms) appeared in the first combined pass and never recurred.
    The dev `status` command alone costs about 2.4 ms on the loop.
  - Internal RAM free with HA connected and BLE connected: about 79.6 KB, min 77.6 KB (about 89 KB
    without HA).
- DNS: ha.svc.mm.maddox.casa resolved to 192.168.1.151 in 7–16 ms through DHCP DNS.
- An unresolvable `.local` name takes about 7 s; only the HA task waits.

## Gotchas

- Toolchain: Docker only.
  - `bin/hubbo fw ...` holds `build/firmware.lock`; don't work around it.
  - Flash only with `bin/hubbo ota 192.168.1.196`.
  - espota picks a random host port. "Address already in use" is a one-off collision; just retry.
- Prebuilt libs (arduino-esp32 3.3.12, IDF 5.5.5):
  - Missing: `esp_websocket_client`.
  - Present: `tcp_transport` (ws and ssl), esp-tls, and the full cert bundle
    (`esp_crt_bundle_attach`).
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
- Test servers via `bin/hubbo run timeout N ...`: stop the container afterwards
  (`docker stop`). A restart right after can fail to bind ("Address already in use"); check the
  server log before trusting a run.
- PSRAM stacks: fine for the HA task, which never writes flash.
- `heap_caps_malloc_extmem_enable(0)` runs after the drivers start, so all later mallocs try PSRAM
  first.
- W5500 RX at priority 1 used to take core 1 for 10–17 ms. Keep the loop at 16, above every
  unpinned task.
- `max_uri_handlers = 12`: the API uses 4, one per method, plus 6 dev handlers. Raise it before
  adding more.

## Open items

- Commits: only on the lead's instruction, /simplify first, then /commits on `firmware/`,
  `docs/hardware/` and this file, staged by path.
- Possible later cleanups from the Phase 3 /simplify, skipped for now:
  - alias `RadioEndpoint` and the RemoteRadio enums to the `api::` types (about 30 references);
  - in `EspWsTransport`, reserve and append the reassembly buffer instead of regrowing it from zero
    for each message;
  - an atomic fast path for `RemoteRuntime::poll()`'s spinlock on the loop.
