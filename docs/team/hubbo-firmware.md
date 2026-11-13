# hubbo-firmware: working notes

Current as of the hub-name work on branch `hub-name`. Not a log: state,
numbers and traps. Design: `docs/design/phase5-appliance.md`.

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
- Gate wipes: `HUBBO_WIPE_ON_BOOT=1` in the build environment makes `scripts/wipe_on_boot.py`
  define `HUBBO_WIPE_ON_BOOT`, which makes `factory_reset::wipeIfRequested()` wipe on every boot,
  early in `setup()` (the same wipe the Phase 5 factory reset uses).
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

## Phase 5 (branch `phase-5`, in progress; design `docs/design/phase5-appliance.md`)

User decisions: **no rollback** (no `verifyRollbackLater()`, no health timer, no verifying state;
Arduino marks the image valid before `setup()`). Logging in again after an update and about 30 s
of remote lag during the write are accepted.

- Image marker: `HUBBO_FW_VERSION` in the build environment (passed by docker-compose) replaces git
  describe, with the same sanitizing; gate builds use it to make two images that differ in version.
  `scripts/image_marker.py` generates `$BUILD_DIR/image_gen/image_marker.cpp` (rewritten
  only when `git describe --tags --always --dirty` changes) with `HUBBO-IMAGE/1
  board=waveshare_esp32_s3_eth layout=1 version=...`. Board is `board::kName`, with underscores like
  the API fixtures; the design doc's dashes are wrong. Layout is `HUBBO_PARTITION_LAYOUT` in
  `board.h`; bump it when the partition table changes. A post-link check fails the build unless the
  .bin holds exactly one marker. `image::version()` feeds `HubInfo.version` and the banner.
- `firmware_update.*`: `FirmwareUpdate` implements `api::FirmwareUpdatePort` (core's header, by
  hubbo-api) and is set as `ApiContext.firmware`. Validation is core's `validateFirmwareImage()`
  with `EspHash` (`esp_hash.h`: hardware SHA-256 via mbedTLS, now also the global auth hash; HMAC and
  PBKDF2 stay portable). The order is claim → Busy, slot → Unavailable, validate, PSRAM copy,
  `esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES)` (no erase), then the `fw_write` task (core 0, prio 3,
  4 KB internal stack: flash writes need the cache off). The task writes 4 KB via an internal buffer,
  `vTaskDelay(1)` per sector, then `esp_ota_end`, set boot, `requestReboot("firmware update")`.
  Failed errors: write_failed, verify_failed, boot_select_failed, no_memory.
- Owner flag (`ota::claim/release`): `claim()` refuses everyone once a reboot is pending
  (`ota::setRebootPending`, set in `setup()`). ArduinoOTA claims around each `handle()` (its
  whole transfer runs in one call). A web update keeps its claim through the reboot. While Web
  holds it, the loop skips `handle()` and `ota_watch` drains the packets. The drain stays in the
  watcher, not the loop: no lwIP calls on the RF loop.
- Auth trim applied: no `clientKey`, no `setAllowedHosts`/static 421, no `loginLimiter`, no
  `CredentialState::Corrupt`. The body limit is `Router::bodyLimit()` alone (the update route's is
  the slot size). `statusLine` gained 411, lost 421 and 429. `NvsSecretStore` had no `reload()` or
  `mediumHealth()` overrides.
- Factory reset (`factory_reset.*`, the user's decision at the board test; built, not yet on the
  board): `FactoryReset` implements `api::FactoryResetPort` and is set as `ApiContext.factoryReset`.
  - `requestFactoryReset()` on httpd: a repeat request returns true. Then it claims
    `ota::Owner::Reset`, so it's false while an update holds the flag or a reboot is pending. It
    saves one NVS flag (`hubbo_reset/requested`), calls `requestReboot("factory reset")`, and
    returns true. The normal reboot path releases keys and the 202 goes out first.
  - The wipe itself runs at the next boot: `factory_reset::wipeIfRequested()` early in `setup()`,
    before NVS, the fs or the radio are used. It's the gate wipe: `nvs_flash_erase`, init,
    `flash_fs::requestFormatAtBoot`. The erase clears the flag.
  - Why not wipe in the post-response flush job: `pollReboot()`'s 5 s deadline restarts without it,
    and that would lose an answered reset. The flag also survives a power cut.
  - `ota_watch` drains OTA packets whenever someone other than ArduinoOTA holds the flag, or a reboot
    is pending, so the loop never spins on an unanswerable packet.
  - Board check at the gate: after the reset, the log shows `WIPE: factory reset; ...` and then
    `flash_fs` formatting on request, and GET /status shows `auth_state` fresh.
- Loop stats across a restart: every deliberate restart goes through `restartNow()` in main.cpp,
  which prints the `loop: max gap ...` line and keeps it in RTC no-init memory
  (`diag::keepLoopStatsAcrossRestart`). At boot, `diag::printPreviousRun()` logs
  `previous run (restart: <why>): loop: max gap ...` right after the reset reason. A power cycle or
  a crash has no such line. To measure a web update's write stall: `lat reset`, update, then read
  `/debug/log` after the reboot (the RAM ring itself doesn't survive a restart).
- /simplify done (no behaviour change). Both envs build: default 1.87 MB flash, 83.9 KB static
  RAM; `_ota` 1.90 MB.
- Skipped from /simplify: handing the transport's body buffer to the writer instead of copying
  (port change in core; the lead accepted the copy).
- Validator check against real files: `firmware/.pio/vcheck/` (not in git) builds core's
  `validateFirmwareImage()` natively and runs it on files. Results: both .bins Accepted with the
  marker's version; one flipped byte Corrupt; another board WrongBoard; a merged factory image and
  the bootloader NotAnImage. Rebuild with `g++ -std=c++17 -Icore/include main.cpp
  core/src/api/firmware_update_port.cpp core/src/auth/crypto.cpp`.
- Pending: the board test (an update from Settings, wrong files refused, the per-sector loop stall
  during a write, the remote afterwards). Don't flash until the lead says so.
- Facts from the feasibility check:
  - Rollback is on in the prebuilt bootloader, unused.
  - Partitions: app0 0x10000 and app1 0x650000, 0x640000 each.
  - The lib builder's `esp_app_desc_t` doesn't identify Hubbo, hence the marker.
  - A merged factory .bin has no app-desc magic at offset 32.
  - Flash ops stall **both** cores, the loop included (no auto-suspend, no PSRAM XIP).
    `YIELD_DURING_ERASE` is on. A web update's per-sector erase stall isn't measured yet (the board
    test will measure it).
- The transport buffer plus the copy means an image over about 3 MB gets Unavailable. Ours is 1.9 MB.

## Multi-Bluetooth (branch `multi-bluetooth`, built, not on the board yet; design `docs/design/multi-bluetooth.md`)

- Limits: 5 connections and 5 bonds (4 hosts plus one). A window needs a free connection when all 4
  hosts are connected, and a re-pair's new bond lands before the old one is deleted. Core's device
  limit stays 4. CCCDs 32.
  - All three in `include/sdkconfig.h`, which shadows the framework's (`#include_next`, then
    redefine; a `-D` loses to sdkconfig.h). PlatformIO puts `include/` only on our sources' path, so
    `scripts/sdkconfig_shadow.py` prepends it for libraries too. Without that, NimBLE built with 3
    and NimBLEServer's `std::array` disagreed with ours (ODR). `static_assert`s in ble_hid.cpp.
  - Verified in the ELF: `ble_hs_conn_elem_mem` 0x258 (5 x 120), `ble_store_config_our_secs` 0x1b8
    (5 x 88), `ble_store_config_cccds` 0x200. `bt_cfg.ble_max_act` follows at runtime.
- Store full: `NimBLEDeviceCallbacks::onStoreStatus` returns `BLE_HS_ESTORE_CAP` on OVERFLOW (the
  default deletes the oldest bond). `adoptBond()` checks `isBonded()` and fails the window.
- `BleHid`: the host table (`hubbo-ble`/`hosts`), no handle table. Sends call
  `ble_gap_conn_find_by_addr(identity)` (matches a resolved RPA) plus an encrypted check, then
  `notify(..., handle)`. `connected(id)` is the `linked` flag `refreshLinks()` sets on link events.
  - Callbacks only set `linksChanged`. `refreshLinks()` adopts a new bond (window open) or drops
    and deletes it (no window), then refreshes `linked`.
  - Advertising: whenever a paired host is unlinked or a window is open, and fewer than 5 links.
    Whitelist-only connects outside a window. The whitelist is rebuilt (advertising stopped)
    after the table changes. Checked on link events and once a second.
  - Window: bonding is allowed only while open. A host already paired to another device stays
    tied to it. The first new bond goes to the window's device, and only then is its old bond
    deleted and old link dropped. The device's own host re-bonding (an auth-complete on its
    handle: `authenticated` bitmask from the callback) also closes it, deleting nothing. A
    timeout, cancel or full store keeps what was there. No failed state.
  - NVS (bond deletes, table writes) goes to httpd via the offload, in order (`pendingJobs_`
    when the queue is full). Boot runs it inline.
- Key release on disconnect: core's `BleHidAdapter` drops a device's held keys when
  `connected(id)` turns false (confirmed by the lead, covered by core's tests); nothing in firmware.
- `BleHostRuntime` (`ApiContext.bleHosts`): a snapshot of every non-default device status and a
  queue of 8 requests under a spinlock. `status(id)` and Busy see pending requests applied:
  `windowLocked()` replays the queue once for the window's owner; forgets clear bonds. Start: Accepted with no change if that device's window is open,
  Busy if another's is, Full if the queue is. Cancel and forget are never Busy.
  - `retainOnly(set)`: one latest-wins slot (never Full), applied after the queue once the radio
    is up; `status()` already shows it. `retainSeq_` tells `store()` whether a newer set came in. `BleHostReconciler` is the ConfigService listener (set in
    `bootState()` before `bootHub`), so the boot load and every commit forget removed devices.
- Ids are `IdString` in memory (refuses over-long ids); the NVS record keeps a fixed `char[33]`.
- `EngineRuntime` uses `bleRadioState/bleRadioDetail` and `bleDeviceAvailability(status(id))`.
- No migration: the old single bond has no device, so the first boot deletes it. Re-pair.
  `main.cpp` still prints `migratedFrom` (core keeps the migration framework).
- Board test passed (Fire TV + Apple TV; keys per activity, Pair/Cancel, Apple TV reboot, power
  cycle). Loop: 2.2 ms after boot. A ~10 ms stall now and then is a flash erase on core 0 (LittleFS
  `active` save after a switch, NimBLE NVS): IDF pauses core 1 for up to
  `CONFIG_SPI_FLASH_ERASE_YIELD_DURATION_MS` (10). Accepted by the user; not a fix item.
- After a boot the Fire TV's link stayed unencrypted for about 40 s (presses failed
  adapter_unavailable). The link log (device-profiles phase) explains it: see the open item there.
- To measure on the board (section 7): loop max gap and over-5 ms with 4 hosts, sends to each,
  the whitelist with RPA hosts (Apple TV), store-full refusal, re-pair keeping the old host on
  cancel/timeout.

## Device profiles (branch `device-profiles`, in progress; design `docs/design/device-profiles.md` §7)

- Link log: a GAP event listener (`ble_gap_event_listener_register`, beside NimBLE-Arduino's
  handler) records connect (status, handle), encryption (status, encrypted, bonded) and
  disconnect (reason) into core's `SpscRing` (16 events) on the host task; `loop()` prints
  `ble: <ms> <device|(no device)> <address> connected|encryption ok/failed|disconnected (...)`.
  A full ring drops and counts, never waits. On the board.
- Sending: the transport already takes a raw keyboard report and a raw consumer usage, so any
  usage in the report map goes through. The report map's maxima are core's
  `limits::kHidKeyboardUsageMax` (0x65) and `kHidConsumerUsageMax` (0x3FF), used in the map's
  bytes, and the sends refuse anything above them. Core's action shape ("a page plus a usage") maps page 0x07 to a key slot and
  page 0x0C to `sendConsumer`; nothing else in firmware needed.
- Built-in profiles: `scripts/core_embeds.py` (pre; was builtin_profiles.py, now also the agent docs) runs core's `core/tools/embed_profiles.py`
  on `../profiles` into `$BUILD_DIR/profiles_gen/builtin_profiles.cpp` (replaced only on a
  change) and compiles it; core's `builtinProfiles()` reads `kBuiltinProfileFiles`. The script
  adds `core/include` to CPPPATH (the generated source is built before the library finder adds it).
  The generator itself leaves an unchanged file alone, so an unchanged set doesn't recompile.
  The table and text are in flash rodata (0x3c...).
- `EngineLog::onActionCompleted` logs a Pending action that finished with an error, e.g.
  `action <ms> ble dev_apple_tv wait_connected done -> accepted adapter_unavailable`.
- Board test passed: profile keys, down/up and timed holds, wait_connected, the link log, two
  hosts routed by device, boot with the built-in profiles.
- Gate (final): serial RAM 87,640 B, flash 1,976,887 B; OTA RAM 95,856 B, flash 2,005,327 B
  (about +1.4 KB RAM and +99 KB flash on multi-Bluetooth, mostly core and the profiles).
- Open, next Bluetooth item (no code yet): the link log after a reboot showed the Fire TV
  connected at 2.08 s but never started encryption; the link hit supervision timeout (reason
  0x208) at 45.08 s, and it reconnected and encrypted at 45.2 s. So the "40 s late encryption" is
  a stalled first link, not slow encryption. We already send a Security Request in `onConnect()`
  (`startSecurity`), which this Fire TV ignored on that link. Candidate fix: if a paired host stays
  connected but unencrypted for a few seconds, request security again, and failing that drop the
  link so the host reconnects at once (a stalled link costs ~43 s today). Bound the retries so a
  host that always fails can't loop.

## Hub name and Restart (branch `hub-name`, built, not on the board yet)

- Core's `config::HubSettings` (hub.h) owns the name in the StateStore (`hub_name` record on
  LittleFS, so the factory reset's format clears it). `loadHubName()` runs right after
  `mountFilesystem()` (no bootHub needed), feeds `HubName::begin()` and sets `onChanged` →
  `HubName::apply()` (httpd). `ApiContext.hub` and `.restart` are set; HubInfo.hostname is dropped
  (core reports the live hostname).
- `hub_name.*` holds only the boot hostname, `mdnsStarted_` and the BLE mailbox (a `FixedString`
  under a spinlock, behind an atomic flag the loop checks; the loop never takes HubSettings' mutex).
  Names come from `hubSettings.identity()`; core guarantees their lengths, so firmware cuts only
  for transport limits. Boot hostname for DHCP (`Network.setHostname`) and ArduinoOTA (its own mDNS is off;
  `bin/hubbo ota` addresses by IP, nothing uses the old `hubbo-<MAC>` name). mDNS now through the
  IDF API (no ESPmDNS): `mdns_init`, instance = name (cut at 63 bytes, UTF-8 safe), hostname = slug,
  `_http._tcp` with a NULL instance (follows the global one). A rename calls
  `mdns_instance_name_set` then `mdns_hostname_set` on httpd: each sends goodbyes and re-probes.
  `mdns_hostname_set` blocks on `xQueueSemaphoreTake` until the mDNS task (core 0, prio 1) takes it
  (checked in the .a), so never on the loop.
- BLE: `BleHid::setName()` on the loop (mailbox): `NimBLEDevice::setDeviceName` (GAP name, whole),
  stop advertising, `setScanName()` (boot calls only this; init() set the GAP name): scan response = the name (complete ≤ 29 bytes, else cut at a UTF-8 boundary
  and typed "shortened"), `updateAdvertising()` restarts it. The advert itself stays flags + HID
  appearance + 0x1812. Bonds don't involve the name: no re-pair; hosts show the new name when they
  read it again (usually the next connection). The GAP name max is raised to 128 bytes with
  `MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME_MAX_LENGTH` in `include/sdkconfig.h` (nimconfig.h hard-sets
  the CONFIG_ one to 31); ELF: `ble_svc_gap_name` is 0x81 bytes.
- `ble.begin()` uses the saved name; it stays right after `radio.begin()`.
- Restart: `Restart::requestRestart()` claims `ota::Owner::Reboot` (renamed from Reset; the factory
  reset uses it too), then `requestReboot("restart")`. `flushAndRestart` (the httpd reboot job) now
  starts with a 100 ms `vTaskDelay` so lwIP sends the response.
- Lead review fix: a factory reset whose NVS save fails releases `Owner::Reboot` only if it took it
  (`ota::holds()` before the claim), so a pending restart keeps the flag. Only httpd claims Reboot.
- Two unrenamed hubs both want `hubbo.local`; mDNS renames one, /status wouldn't know. Left as is
  (one hub). If needed: `mdns_register_hostname_changed_callback` / `mdns_hostname_get` exist.
- /simplify done. Skipped: `utf8Prefix` into core (beside checkHubName, testable; core's call),
  folding Restart and FactoryReset into one class (5 lines). Core's `rename()` already skips a
  same-name rename (no save, no onChanged).
- Gate (fw) passed: serial RAM 88,160 B, flash 2,018,847 B; OTA RAM 96,376 B, flash 2,045,851 B.
  The "specify upload_port" line in the OTA build log is espota config noise, not a failure.
- Board test to do: rename live (BLE scan name, `dns-sd -B _http._tcp`, `<slug>.local`, paired
  hosts still work), a 32-emoji name (GAP read, shortened scan name), DHCP name after a reboot,
  POST /admin/restart answers before the reboot, loop max gap during a rename (HCI stop/start on
  the loop, as `updateAdvertising` already does).

## Agent docs (branch `agent-docs`, built, not on the board yet)

- `scripts/core_embeds.py` runs core's embed_profiles.py and embed_docs.py (spec, guide, llms.txt; PyYAML)
  into `$BUILD_DIR/{profiles,docs}_gen` and builds them; unchanged output doesn't recompile.
- http_server: `registerUri()` for every handler; the API is `/api/?*` (httpd's wildcard: also bare
  `/api`, not `/apix`); GET `/llms.txt` → the router, registered before the web admin's `/*`. Status lines come from core's `http::statusLine()` (firmware's copy gone).
- Docs in rodata about 170 KB (OpenAPI 117 KB raw / 25 KB gz, guide 20 KB / 7.6 KB). Each 200
  copies the doc into a PSRAM std::string (skipped: a flash string_view body in core).
- Gate (fw) passed: serial RAM 88,192 B, .bin 2,236,240 B; OTA RAM 96,416 B, .bin 2,264,112 B (34.5% of the
  slot). Board test: plain and gzip GETs incl. the 117 KB body, heap, httpd stack, loop gap.

## httpd stack overflow (agent-docs branch; board test passed)

- Crash adding an HA media_player (Sonos): reset reason panic. The core dump (decoded with
  `.pio/coredump/py`'s esp_coredump plus `tool-xtensa-esp-elf-gdb`; keep dumps and decodes in the
  session scratchpad) showed httpd's end-of-stack watchpoint (DebugException) at 8,144 of 8,192
  bytes in use. The path: POST /admin/devices → createDevice → commit → parseConfig →
  decodeDevice → ha::decodeSnapshot. It isn't recursion, just big frames: the route lambda about
  1.5 KB, createDevice, the decode lambdas and decodeSnapshot. Core and api are moving those
  locals off the stack.
- Fix: `HttpServer::kStackBytes` is 16 KB (internal RAM, since httpd writes flash: about 8 KB more
  internal heap in use). After every request, `noteStackLow()` logs
  `http: stack low N of 16384 bytes free` to /debug/log at each new low (uxTaskGetStackHighWaterMark,
  posted jobs included). Read it after the Sonos add to confirm the margin.
- Hub-name bug fixed: `mdns_instance_name_set` returns INVALID_ARG until a hostname is set, so
  registerMdns now sets the hostname first. Until now the board advertised with no instance name
  until the first rename.
- Board: the Sonos added fine. The stack low was 10,368 B free during the add (10,288 after the
  edits that followed), so the add used about 6 KB. Heap min free 35,712 B. The mDNS boot line is clean.
- Loop: max gap 11.4 ms, 1 iteration over 5 ms, in the window of config saves 42-44 (the device add
  and activity edits): the known flash-erase stall (both cores, up to 10 ms per erase; accepted).
  Outside it, 2.6 ms.
- Gate (fw) passed: serial RAM 88,208 B, flash 2,175,635 B; OTA RAM 96,432 B, flash 2,203,099 B.

## Task layout (priorities)

| task | core | prio | notes |
|---|---|---|---|
| loopTask (RF + engine) | 1 | 16 | busy loop; above every unpinned task |
| tiT (lwIP) | 0 | 18 | |
| w5500_tsk | any | 15 | ends up on core 0 because the loop outranks it |
| httpd | 0 | 5 | API plus dev endpoints; single-threaded; 16 KB internal stack |
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

- Config save (Phase 5, measured by the lead): `lat reset`, five `PUT /global-bindings` saves
  (revisions 15–19, about 70 ms each), then `lat`: loop max gap 2077 us, 0 over 5 ms. A LittleFS
  config save costs the loop about 2 ms, about the same as the idle `printStatus()` peak.

## Gotchas

- Toolchain: Docker only.
  - `bin/hubbo fw run` rebuilds web/dist first when it's stale, so a broken web build blocks the
    firmware build. To build anyway against the existing dist, keep both locks:
    `flock build/firmware.lock flock build/web-dist.lock bin/hubbo fw --no-ansi run -e ...`
    (a global pio option first skips the script's own `run` branch).
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
- `max_uri_handlers = 13`: the API uses 4 (one per method), `/llms.txt` 1, the dev console 6 and
  the web admin 1.
  Raise it before adding more. The web admin's `/*` must be registered last.

## Open items

- Commits: only on the lead's instruction, /simplify first, then /commits on `firmware/`,
  `docs/hardware/` and this file, staged by path.
- Possible later cleanups from the Phase 3 /simplify, skipped for now:
  - alias `RadioEndpoint` and the RemoteRadio enums to the `api::` types (about 30 references);
  - in `EspWsTransport`, reserve and append the reassembly buffer instead of regrowing it from zero
    for each message;
  - an atomic fast path for `RemoteRuntime::poll()`'s spinlock on the loop.
