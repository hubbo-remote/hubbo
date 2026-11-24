# hubbo-firmware: working notes

Current as of the MQTT work on branch `mqtt`. Not a log: state,
numbers and traps. Design: `docs/design/phase5-appliance.md`.

## State

- Phase 2 (7797413): the restore OOM fixed with extmem(0), `-O2`, loop priority 16, Ethernet on
  core 0.
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
    - mbedTLS allocates from PSRAM (`tls::useExternalMemory()`, tls_memory.h; handshakes in a `tls::YieldScope`).
  - `integration_task.*` (was ha_task): core 0, priority 4, 8 KB PSRAM stack; a job queue plus a service callback that
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
- Dev-only endpoints (`_ota` env, dev_console.cpp): `/debug/log`, `/debug/run?cmd=...`,
  `/debug/coredump`, `/debug/release`, `/debug/stats`. (`/debug/bench`, `/debug/ws-echo` and
  `/debug/dns` were bring-up tools; removed 2026-11-24.)
  - Query values decode through core's `http::queryValue`, so both `+` and `%20` work.
    `Request.query` stays raw (the core contract); decode per parameter, never the whole query.
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
- Default name and the advertised hostname: see "Unique default name" below.
- /simplify done. Skipped: `utf8Prefix` into core (beside checkHubName, testable; core's call),
  folding Restart and FactoryReset into one class (5 lines). Core's `rename()` already skips a
  same-name rename (no save, no onChanged).
- Gate (fw) passed: serial RAM 88,160 B, flash 2,018,847 B; OTA RAM 96,376 B, flash 2,045,851 B.
  The "specify upload_port" line in the OTA build log is espota config noise, not a failure.
- Board test to do: rename live (BLE scan name, `dns-sd -B _http._tcp`, `<slug>.local`, paired
  hosts still work), a 32-emoji name (GAP read, shortened scan name), DHCP name after a reboot,
  POST /admin/restart answers before the reboot, loop max gap during a rename (HCI stop/start on
  the loop, as `updateAdvertising` already does).

## MQTT discovery (branch `mqtt`, gate passed, on the board, connected to the user's broker)

- Design: `docs/design/mqtt-discovery.md`. Core owns `MqttBridge` and the `MqttTransport` port.
  Firmware supplies `EspMqttTransport` (`mqtt_transport.{h,cpp}`), the `mqtt` task and the wiring.
- esp-mqtt (prebuilt, IDF v5.5.5):
  - The task isn't pinned. `esp_mqtt_client_start` creates it with tskNO_AFFINITY, and the
    sdkconfig has no core selection.
  - Its defaults are prio 5, a 6144 B stack and 1024 B buffers. We set prio 4 (can't preempt
    the loop at 16), a 4 KB internal stack, a 512 B in buffer, a 5120 B out buffer (escaped
    names can push a discovery to about 4.4 KB) and an 8 KB outbox. Keepalive is 15 s, and it reconnects every 5 s.
  - `esp_mqtt_client_stop` sends DISCONNECT itself, then waits for its task to end.
  - The client is made on the mqtt task, after `heap_caps_malloc_extmem_enable(0)`, so its
    buffers are in PSRAM. Only the 4 KB stack is internal.
  - `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=1`, so a `.local` broker host should resolve. Check
    it on the board.
  - Flash cost about 16 KB for the library, about 54 KB with the bridge and wiring.
- Transport:
  - Link state never goes through the queue. The handler keeps atomics: connects, drops
    (counters), up and authRefused (a CONNACK 4/5 since the last connect). They're atomics and
    not a spinlock, because esp-mqtt's task can run on core 1. It sends a wake-up only when the
    queue is empty. `poll()` compares it with what it last delivered, so a connect and a
    drop between two polls are both seen. A drop is delivered (AuthFailed or Unreachable),
    then a connect if the link is up.
  - Only DATA goes through the 16-deep queue (sent with no wait, and dropped when full). It's
    delivered after the link, and only while connected. `connect()` resets the link before
    `esp_mqtt_client_start`.
  - DATA is whole messages only: a payload of at most 256 B and a topic of at most 128 B.
  - `publish` uses `esp_mqtt_client_enqueue(store=true)`.
  - `disconnect()` waits up to 1 s for the outbox to drain, then stops and destroys the client,
    and drops anything still queued.
- `startMqtt()` runs after `heap_caps_malloc_extmem_enable(0)`, so esp-mqtt's client and
  buffers are always in PSRAM. Each bridge state change is logged once, e.g.
  `mqtt: connected: <detail>` or `mqtt: auth_failed: <detail>`.
- Task: `IntegrationTask` (was `HaTask`; `start(name, service)`) runs "ha" and "mqtt": core 0, prio 4, an 8 KB PSRAM stack. The
  service is `mqttTransport.poll(50)` plus `mqttBridge.poll(now)`. Stack lows: `/debug/stats`
  (the board budget), not log lines.
- The wire MAC, checked in ETH.cpp: `begin(..., spi_host, ...)` calls `beginSPI(mac=nullptr)`,
  which uses `esp_read_mac(ESP_MAC_ETH)`, adds `_eth_index` (0 for `ETH`), then applies
  `esp_derive_local_mac`. So `EthernetLink::mac()` is exact: BE:1F:3F:BC:7F:8D (locally
  administered), and "Hubbo 7F8D" stands.
- Wiring (main.cpp):
  - `ethernetMac` is read once in a static constructor; it feeds the default name and the node id.
  - `EthernetLink::mac()` now applies `esp_derive_local_mac`, so it returns the wire MAC itself.
    The last 2 bytes, and so the default name, are unchanged.
  - Core's `config::ConfigFanOut{&bleReconciler, &mqttActivities}` (an `MqttActivitySync`),
    wired the same way as hub-sim.
  - `setHubName` is called at boot and in `onChanged`.
  - Boot runs `configureMqtt()` (`applyMqttSettings`; the API applies it later).
  - `context.mqtt` is set, and `runtime.setMqtt` makes `integrationState/Detail("mqtt_main")`
    the bridge's.
  - The hooks: `activityChanges`; `engine.activeActivityIdShared()`; `liveInfo` (`runtime.ip()`, core's `mqttMacText` made once,
    a new any-task read, and `shownHostname()`).
  - `transition` and `restart` are posted to httpd (`runTransition`, `requestHubRestart`), and a
    refusal is logged there. The RestartPort is never called from the mqtt task.
- Gate (fw) passed (after the second /simplify): serial RAM 89,032 B, flash 2,245,503 B; OTA RAM
  97,248 B, flash 2,268,247 B. On the board (connected, at 30 s): internal free 31,980 B, min
  29,748 B (+0.5 KB with startMqtt after extmem), largest 22,516 B. esp-mqtt stack low 1,792 B free.
  Loop max gap 2,975 us, 0 over 5 ms. The boot log shows connecting, then unreachable (before DHCP),
  then connected 5 s later.
- Board, connected (the user's broker, a `restart: restart` boot): internal free 31,968 B, min
  29,216 B, largest 22,516 B at 30 s. That's about 6 KB under disabled (the 4 KB esp-mqtt stack
  plus socket state). esp-mqtt stack low 1,792 of 4,096 bytes free. Loop max gap 2,455 us, 0 over 5 ms.
- Board boot check (f33b293-dirty, MQTT disabled): the boot line reads `mqtt: disabled, node hubbo_be1f3fbc7f8d,
  password not_found`. The mqtt task is core 0 prio 4, with no esp-mqtt task. Loop max gap 2,727 us, 0 over 5 ms.
  Internal heap: free 37.9 KB, min 28,280 B (reached by the boot mem line), largest 17,396 B. That's
  below the 35.7 KB min in the agent-docs notes.
- Main's build is no baseline: it BLOCKS a config saved by the MQTT build (`wrong_type` at the
  mqtt integration; no compat, by design). Blocked, with HA off, it showed 84.8 KB free and
  83.2 KB min. The MQTT build's min comes at boot, from bootState's config parse before
  extmem(0). Measure the MQTT cost within one build: disabled, then connected.
- Board test to do:
  - Min free internal heap with MQTT connected (it was about 35.7 KB before).
  - esp-mqtt stack low.
  - Loop gap.
  - A `.local` broker.
  - The doc's 9 checks.

## Unique default name (branch `hub-name`, /simplify done, gate passed, board checked)

- Design: `docs/design/hub-name.md`. `HubSettings(stateStore, defaultHubName())` in main.cpp. The
  default is core's `config::defaultHubName(mac)` on `EthernetLink::mac()`, which wraps
  `esp_read_mac(ESP_MAC_ETH)` (efuse, so it's readable in a static constructor, before ETH starts). Arduino's SPI ETH
  puts `esp_derive_local_mac(ESP_MAC_ETH)` on the wire, which changes only byte 0, so the suffix
  matches the "eth: W5500 started, MAC" boot line.
- mDNS is espressif/mdns 1.13.1 (`versions.txt` in the esp32s3-libs; source at tag mdns-v1.13.1).
  An A/AAAA collision while probing, or on the re-probe after a later conflict, mangles the
  global hostname itself. It adds or bumps a numeric suffix after the last `-`: `hubbo-3f2a` becomes `hubbo-3f2a-2`, but
  `hubbo-1234` becomes `hubbo-1235` and `room-2` becomes `room-3`. It never reverts. An SRV
  collision mangles the instance name the same way (out of scope, not reported).
- `mdns_register_hostname_changed_callback` fires on the mDNS task every time the hostname
  changes: our `mdns_hostname_set` (inside the action, before probing) and every mangle. It
  doesn't fire when the name is unchanged. `HubName` registers it after `mdns_init`, and it calls
  `HubSettings::setAdvertisedHostname` (`startMdns(hubSettings, port)` passes it as the arg; the
  log line uses plain `con.print`s, since the mDNS task has a 4 KB stack). Core takes only `mutex_`, never `renameMutex_`, since
  onChanged runs under renameMutex_ and waits on the mDNS task). A rename clears the advertised
  name before onChanged, so a same-slug rename (no callback) correctly shows `identity.hostname`.
  There's no `mdns_hostname_get` after registering: it would race a mangle and could undo it.
  There's no new task or timer.
- /status `network.hostname` and GET /admin/hub `hostname`: the firmware never filled them.
  Core/api serve `shownHostname()` and `hostname_conflict`.
- Board test to do: the dev board has a saved name ("Guest Room Hubbo"), so the default only shows
  after a user reset (it should be "Hubbo XXXX" and match the ETH MAC). With the saved name: the
  boot log shows `mdns: advertising guest-room-hubbo.local`, and `/status` hostname matches it. A clash can be tested by
  running `dns-sd -P` (or avahi-publish) with the same hostname from a Mac before the boot or the
  rename: expect a numeric suffix added or bumped in /status, and `hostname_conflict` true. Measure the loop gap.
- Gate (fw) passed: serial RAM 88,336 B, flash 2,187,363 B; OTA RAM 96,560 B, flash 2,214,727 B.
- Board (1436f35-dirty): `mdns: advertising guest-room-hubbo.local` fired once at boot, and
  /status and /admin/hub match it (no conflict). MAC BE:1F:3F:BC:7F:8D, so the default would be
  "Hubbo 7F8D" (unseen: the name is saved). Loop max gap 2,086 us, 0 over 5 ms. Still open: the
  default after a user name reset, and an optional clash test.

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

## Setup fixes (branch `setup-fixes`, built, the lead does the board)

- `bootState()` boots with core's `factoryDefaultConfig()` instead of an empty `ConfigSpec`.
  That's the built-in integrations plus Off bound to the system's off. hub-sim and backup restore
  use the same seed.
- The fw gate passed:

  | Env | Static RAM | Flash | .bin |
  |---|---|---|---|
  | Default | 89,680 B | 2,293,587 B | 2,336,208 B |
  | OTA | 97,896 B | 2,314,983 B | 2,358,384 B (36.0% of the 0x640000 slot) |

## Smart buttons (branch `smart-buttons`, built, the lead does the board; design `docs/design/smart-buttons.md`)

- Firmware's only change: `EngineRuntime::snapshot()` fills `selectedSlot` from
  `engine_.selectedSlotShared()` (as the sim). The console status `engine:` line shows `slot <name>`.
- Repeats need nothing here: core drives them in `engine.poll(nowMs)`, which the loop calls every
  iteration (about 1 ms), so 300 ms repeats are late by a loop gap at most. Each repeat goes to the
  executor like any press (a full lane skips it), so the RF loop never waits on HA.
- Gate passed. `_ota` image: 2,353,343 bytes flash (35.9%), 98,064 bytes static RAM; default env
  2,334,375 bytes (35.6%), 89,848 bytes.
- To check on the board: loop max gap and over-5 ms while holding + on a light slot (a repeat every
  300 ms for up to 12 s).

## Boot heap (smart-buttons, 2026-11-15; on .196, in this phase's commit)

- Symptom: .196 sent no frame over about 190 B (ping 160 B+ 100% loss, GET / and login stalled);
  RX and small replies worked. Cause: internal RAM starved, 9,880 B free (min 6,604, largest
  3,572) with Ethernet unplugged, flat over time. `bootState()` ran before
  `heap_caps_malloc_extmem_enable(0)`, so the whole saved config and its compiled form sat in
  internal RAM, growing with the user's config. lwIP and the W5500 path then fail big sends.
- Fix in `setup()`: `extmem_enable(0)` before `bootState()`, back to
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (4096) after it, so the drivers started next behave as
  before. `printMemory(label)` logs `boot: before config` / `boot: after config` mem lines, showing what the config costs (keep
  them: they show config growth). `printMemory()` (boot and every status) adds
  `mem: WARNING internal RAM low ...` under 20 KB internal free. A log line only; no restart.
- On .196, everything up (PoE, HA, MQTT): internal free 78 KB, min 76.6 KB, largest 36.8 KB.
  86,820 B at 30 s with no Ethernet. Loop max gap 2.6 ms, 0 over 5 ms. The old build had 9,880 B.
- Final build on .196: internal RAM about 151 KB through the config load, loop max gap 2.4 ms, 0 over 5 ms.
- Phase gate (fw) passed: default RAM 90,320 B, flash 2,335,367 B (35.6%); `_ota` RAM 98,536 B,
  flash 2,356,327 B (36.0%).
- Traps from this round:
  - A hand-built `_ota` image needs `HUBBO_OTA_PASSWORD` in the environment, or ArduinoOTA is
    compiled out (`ota disabled` in status) and `bin/hubbo ota` can't reach it. The lead supplies it.
  - Never flash a merged .bin at 0x0 to a board whose NVS must survive: merge-bin pads 0x9000-0xdfff
    with 0xFF. Flash `boot_app0.bin` at 0xe000 and `firmware.bin` at 0x10000 instead.
  - The size-cliff probe: `ping -s N` sweep. To read a long response from a sick board, a small
    advertised MSS doesn't help when the board can't allocate.

## Activity runs (branch `activity-runs`; on .196, idle checked)

- `EngineRuntime::activityRuns()` → `engine_.runsShared()` (whole history, about 14 KB; sets
  `out->nowMs = millis()`, the clock for each run's "ago") and
  `lastRun()` → `engine_.lastRunShared()` (the newest run, about 40 B, for /status polls).
- `_ota` env builds: RAM 98,664 B, flash 2,370,531 B (36.2%).
- State checks (`docs/design/state-checks.md`, core only; the 300 ms one-entity read and 1 s reply cap
  are on the integration task): no firmware change. `_ota` RAM 98,736 B, flash 2,377,519 B (36.3%).
- On .196 (lead's OTA, idle only, up 30–140 s): loop max gap 0.44–2.2 ms, 0 over 5 ms (75 s
  window with the 30 s status print: 1,457 us). Internal free 75.1 KB, min 71.9 KB, largest 34.8 KB.
  Loop stack left 3,144 B. Pending: the same counters right after the user's activity switching.
  The HA task's stack low isn't readable (`tasks` doesn't print stack watermarks).
- Phase gate (fw) passed: default RAM 90,792 B, flash 2,357,595 B (36.0%); `_ota` RAM 99,008 B,
  flash 2,378,495 B (36.3%).
- Gotcha: the console's `tasks` command costs the loop about 5 ms itself (5,108 us, 1 over 5 ms).
  Run `lat reset` after it before measuring.

## Latency targets (branch `latency-targets`)

- README dispatch targets now p95 < 2 ms, p99 < 5 ms (ec7782c).
- BLE always-on was tried and reverted (the user wants it toggleable): `bleRadioState` keeps the
  integration; firmware is unchanged from HEAD. `_ota` RAM 99,008 B, flash 2,382,671 B (36.4%).

## Macros (branch `macros`; core only, no firmware change)

- Phase gate (fw) passed: default RAM 91,232 B, flash 2,388,411 B (36.4%); `_ota` RAM 99,448 B,
  flash 2,408,623 B (36.8%).
- Gate sizes vs `bin/hubbo ota` sizes: the gate builds without `HUBBO_OTA_PASSWORD`, so ArduinoOTA
  is compiled out and it reads lower than the lead's OTA build. Compare gate to gate.

## Firmware updates (branch `firmware-updates`; built, on .196, not committed)

- Files: `firmware_releases.*` (api::FirmwareReleasePort, the update task, the trial, the rollback
  record), `esp_release_feed.*` (release::ReleaseFeed: esp_http_client + bundle, redirects, tx buffer
  2048, 10 s per op, 30 s manifest, 180 s image; ECDSA verify with mbedTLS), `tls_memory.*` (PSRAM
  allocator + per-task `YieldScope`, thread_local; `yieldIfDue()` for non-blocking loops; ws_transport
  uses it), `release_key.h` +
  `scripts/release_key.py` (builds in `release-key.pub.pem`). `verifyRollbackLater()` returns true
  (firmware_releases.cpp), in every build.
- FirmwareUpdate: `beginDownload`/`setDownloadProgress`/`failDownload`/`installDownloaded` (takes the
  PSRAM buffer, no copy; validates on httpd with its hash), `setTrial` (all updates Busy),
  `setOnWritten` (writer task -> httpd: StateStore `fw_pending` = version, clears `fw_rolled_back`).
- Update task: IntegrationTask "update" (core 0, prio 4, PSRAM stack; idle passes sleep 1 s, the new
  `start(..., idleWaitMs)`). Never writes flash or calls `esp_ota_get_state_partition` (a flash read):
  store writes and `esp_ota_mark_app_valid_cancel_rollback` are posted to httpd. The image download
  uses core's `release::downloadImage` (fetch + checkImage). Feed and releases share main's `EspHash`.
- Boot: `fw_pending` != running version -> rolled back (`fw_rolled_back`, shown until the next
  update). PENDING_VERIFY -> trial: each 1 s, Ethernet up + config Ok/Blocked/Fresh + GET
  http://127.0.0.1/api/v1/status answers (any status) -> mark valid on httpd. Not by 120 s ->
  `requestReboot("update trial failed")`. Pending record but no PENDING_VERIFY: one log line, no trial.
- Dev only: `/debug/release?op=test&base=&running=`, `op=check`, `op=install&version=`,
  `op=crash`; every answer ends with the status. The 13th (last free) URI handler.
- Test key (review fix: no runtime key override): `HUBBO_TEST_RELEASE_KEY_FILE` (passed through by
  the toolchain service; path relative to the repo) builds a second key into dev
  (`HUBBO_DEV_CONSOLE`) builds only, as `kTestPem`; the build prints `*** release key: this dev
  build also accepts the test key ...`. Unset -> `kTestPem = nullptr`. Put the throwaway public key
  at `firmware/.pio/test-release-key.pub.pem` (gitignored); delete it and reflash after testing.
- A verify failure refetches the manifest + signature once (two fetches, then bad_signature).
  `fail()` stores the error before releasing the OTA claim. `claimForUpdate()` refuses during a trial.
- Measured on .196 against a local feed (python http.server on the host, throwaway key):
  wrong key -> bad_signature (484 ms); right key -> available. Install: 2,541,520 B downloaded in
  13.0 s, written in ~17 s, rebooted, trial healthy after 4-7 s. Failing image (no answer to its
  self request) -> restart at 120 s -> previous image, `rolled_back_from 9.9.9`. Same-size tampered
  image -> image_mismatch; bigger -> download_failed. Download only: loop max gap 3.2 ms, 0 over
  5 ms, internal min 70.8 KB. The write: 20-22 ms max gap, 1,242 over 5 ms (the known flash stall).
  Idle after: loop 0.4-2.4 ms, 0 over 5 ms; internal 72.5 KB free. Real GitHub check: 404 (repo
  private) -> unreachable in 2.7 s.
- Sizes: `_ota` RAM 102,256 B, flash 2,491,543 B (38.0%); default RAM 91,584 B, flash 2,439,255 B.
- Pinned toolchain (2026-11-17): platformio.ini `platform_packages` pins framework 4.30312.261024,
  esp32s3-libs 3.3.12, xtensa 14.2.0+20260121, esptool, scons. The platform asks only for
  `~4.30312.0`, so a fresh PlatformIO dir (CI, a worktree) floated to 4.30312.261116, whose build
  script adds `-Os` after our `-O2` (everything at -Os, 358 KB smaller; v0.1.0 shipped that way and
  was deleted). Same IDF, libs and bootloader otherwise. The user chose to stay on -O2. A fresh-dir
  build now gives RAM 91,464 B, flash 2,438,067 B, the same as local.
- Gotchas: a connection to the hub's own Ethernet IP fails (ESP_ERR_HTTP_CONNECT); use 127.0.0.1.
  A fast (LAN) download starved IDLE0 -> task watchdog at ~35%: the read loop calls
  `tls::yieldIfDue()` (a tick per 20 ms); re-measured after /simplify: full LAN download, no reset,
  loop max gap 2.5 ms, 0 over 5 ms.
  Port 8099 on the host is taken; the test feed used 18099.
- Re-verified after the review fixes (2026-11-17, test-key build): trial healthy after 5 s; wrong
  key -> 2 manifest fetches, bad_signature in 1,652 ms; test key -> available, "signed with the dev
  build's test key"; install 2,541,520 B in 13.2 s, download max gap 0.4 ms, write max gap 16.5 ms;
  9.9.9 trial healthy after 4 s. Then reflashed without the variable (`kTestPem = nullptr`, no
  build line): trial healthy after 7 s, idle loop max gap 1.4 ms, 0 over 5 ms, internal 72.7 KB free
  (min 71.1 KB). `_ota` RAM 99,680 B, flash 2,460,271 B.
- Open: the trial-busy refusal wasn't hit on the board (the trial ends as Ethernet comes up).

### The spike (2026-11-16)

Design: `docs/design/firmware-updates.md`. Spike code kept out of the tree.
- Rollback: the framework (IDF v5.5.5) has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. .196's
  bootloader is byte-identical to the one we build (sha256 84bb3656… over 19,968 B; desc: idf v5.5.5,
  built Sep 16 2026). With `extern "C" bool verifyRollbackLater() { return true; }` a new OTA boots
  PENDING_VERIFY. Unmarked + `esp_restart()`: back to the old image. Unmarked + task watchdog: back
  to the old image (reset reason task watchdog). `esp_ota_mark_app_valid_cancel_rollback()` + restart
  stays.
- While PENDING_VERIFY, `esp_ota_begin` returns ESP_ERR_OTA_ROLLBACK_INVALID_STATE (FirmwareUpdate's
  path); ArduinoOTA writes partitions directly, so `bin/hubbo ota` still works during a trial.
- `esp_app_desc_t.version` is the lib-builder's ("6671d0b"), not ours: `rolled_back_from` needs our
  version from somewhere else (e.g. saved before the reboot).
- HTTPS (esp_http_client + `esp_crt_bundle_attach`, our PSRAM TLS allocator) on the HA task, idle:
  internal peak −1.1 to −2.1 KB; PSRAM −55 KB connected, −64 KB peak. HA task stack low 4,060 of
  8,192 B. The handshake needs the allocator's 20 ms yield slices (ws_transport's are private: share
  them). Manifest-style URL: 3 hops (github.com 302 ×2, then release-assets.githubusercontent.com),
  hop 0 2.5–2.8 s (handshake), hop 1 30–260 ms, hop 2 630–670 ms (new host). `buffer_size_tx` must
  be ≥ ~1 KB (2048 used): the 929-char Location fails `open` with ESP_FAIL at the default 512.
  rx default 512 is fine with 5.5 KB header blocks. 2,267,912 B into PSRAM in 9.0 s (252 KB/s), sha256
  matched the host. Loop max gap during fetches 0.4–2.4 ms, 0 over 5 ms.
- Cert bundle: already linked (ws_transport), 68,983 B. esp_http_client + spike code: +41 KB flash.
- ECDSA P-256: `mbedtls_pk_parse_public_key` + `mbedtls_pk_verify(MBEDTLS_MD_SHA256)` over an
  `openssl dgst -sha256 -sign` DER signature: OK; tampered: −0x4e00. 362–395 ms per verify (software
  ECC, no yield), internal peak 1.5 KB.
- Gotcha: `esp_flash_read` into a PSRAM buffer took the internal minimum down by about 17 KB
  (inferred: a bounce buffer).

## Moving a board from other firmware to Hubbo over USB

- Use USB when the board runs other firmware. An OTA can't change the bootloader or the partition
  table, and the old firmware's NVS and data partition would be left behind.
- **Image:** one merged file, written at 0x0. It's the `_ota` env, built with the OTA hash
  compiled in, so `bin/hubbo ota <ip>` can update it later. The hash grants OTA access, so keep
  the file private (`build/` is gitignored).

  | Offset | Content |
  |---|---|
  | 0x0 | `bootloader.bin` |
  | 0x8000 | `partitions.bin` (`default_16MB.csv`) |
  | 0xe000 | `boot_app0.bin` (framework `tools/partitions/`) |
  | 0x10000 | `firmware.bin` |

  The flash settings are DIO, 80m and 16MB.
- **Merge** in the toolchain (`bin/hubbo run bash -c '...'`), with `B` set to
  `firmware/.pio/build/waveshare_esp32_s3_eth_ota`:
  `/platformio/packages/tool-esptool/esptool --chip esp32s3 merge-bin -o build/hubbo-esp32s3eth-full.bin
  --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 $B/bootloader.bin 0x8000 $B/partitions.bin
  0xe000 /platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
  0x10000 $B/firmware.bin`. Afterwards, record the image's sha256.
- **Erase first. This is required.** Hubbo formats only a blank `spiffs` partition, so leftover
  data leaves it with no storage. The erase also clears the old firmware's NVS: its settings and
  its NimBLE bonds.
- **PoE:** never connect USB and PoE at the same time. Unplug the PoE cable before plugging in
  USB.
- **Web flasher** (Chrome, no installs: https://espressif.github.io/esptool-js/):
  1. Connect over the board's USB-C (native USB, "USB JTAG/serial debug unit"). If it won't
     connect, hold BOOT, tap RESET, release BOOT, and try again.
  2. Click Erase Flash.
  3. Write the merged bin at address 0x0 with Program.
  4. Press RESET, then reconnect the network cable.
- `bin/hubbo` has no USB flash command, so there's no device passthrough to Docker.
- **First boot:** it gets its address over DHCP. Its name is "Hubbo XXXX", from the Ethernet MAC.
  It then needs first-run setup.
- **Remote:** a known endpoint (the channel and the 40-bit address, which the old firmware may
  show) can be entered by hand with `PUT /admin/remote/endpoint`. The original Harmony hub isn't
  needed for that; a scan needs the hub transmitting.
- **Bluetooth hosts:** forget the old name on each host, then pair again with Hubbo.

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

## Radio power (branch `radio-power`; on .196, board test in progress)

- Core's `HubSettings::radioPower()` (one relaxed atomic load) is polled every loop pass:
  `radio.setPower(hubSettings.radioPower())` before `radio.loop()`. `begin(power)` applies the
  saved level before listening. Map Low/Medium/High/Max → RF24_PA_MIN/LOW/HIGH/MAX, LNA on.
- `setPower` is a no-op when unchanged. A change is RF24 `setPALevel` (RF_SETUP read + write, about
  4 CSN toggles at 5 us `csDelay`, a few tens of us). W_REGISTER is standby-only per the datasheet,
  so when listening it drops CE around the write (RF24 itself writes RX_ADDR_P0 with CE high, so
  this is caution). The PA bits matter only on TX: the auto-acks and the scan's writes.
- Logs `radio: power <level>` on a change; the boot `ready` line and `status` show the level.
- README: the PA+LNA wiring (an AMS1117 socket adapter, VCC to VSYS pin 39).
- .196 (plain module): Max at boot, idle loop 2,762 us max gap, 0 over 5 ms (60 s). PUT low
  (the lead's): `radio: power low` logged live, `status` shows `power low`; loop 2,486 us, 0 over
  5 ms (60 s). Presses: the user checked Low on the media room hub instead; .196 presses skipped.
- Board PUTs: the lead's `scratchpad/hub196.py` (`put <path> '<json>'`, `get <path>`) reads
  admin.json without printing it.
- /simplify: comments trimmed. Skipped: a radio-only enum (firmware already uses core types) and
  dropping `begin(power)` for the first poll (the brief says apply at start).
- Build (`_ota`): RAM 102,136 B, flash 2,492,667 B (38.0%).

## Remote log (branch `radio-power`; design `docs/design/remote-log.md`; on .196)

- Dev builds only: `scripts/image_marker.py` appends `HUBBO_REMOTE_LOG=1` to the global CPPDEFINES
  when HUBBO_FW_VERSION is unset (core, a lib, gets it too). `gate fw` builds the plain env with
  HUBBO_FW_VERSION=gate (flag-off, like a release) and _ota flag-on.
- main.cpp (flag only): `PortMuxLogLock` (core's `LogLock`), `RemoteLog` placement-new in PSRAM
  (800 KB) after RunHistory, given to `input.setLog`, `engine.setRemoteLog`, `diag::setRemoteLog`,
  `context.remoteLog`. `bleAdapter` sits on `hid` = `LoggedHidTransport` (flag) or `timedHid`.
- Signal: `RemoteRadio::readSignal()` = `testRPD()` (one register read), once per `drain()` pass
  (RPD latches on the newest packet, so queued payloads share it); Unknown on a non-+ radio and in
  release builds (no read at all).
- RF stall: `diag::loopTick` logs `rf_stall` (ms) for a gap over 5 ms: the first, then within 1 s
  only a longer one (a flash write burst gives ~1 a second).
- BLE: `LoggedHidTransport` logs only a failed send (Action, AdapterFailed, ble, device, ms). A
  failure inside an executed step is also the engine's step entry (two entries, rare); releases
  and poll sends are only ours.
- Flag-off image: no log symbols of ours (nm). Core/api's `sendRemoteLog` route code (1.6 KB) is in
  it though. Release-style 0.0.0: RAM 91,512 B, flash 2,449,827 B. Dev _ota: RAM 102,232 B,
  flash 2,510,207 B.
- .196, idle: loop 2,052 us, 0 over 5 ms. Internal free ~72.4 KB (unchanged). PSRAM free 7.34 MB
  with the log, largest block 7.2 MB (the 2.5 MB download fits).
- Full log (temp console fill, removed): 8,292 appends, 102 dropped. GET /remote/log 680 KB, ~3.3 s
  each, 3 in a row: PSRAM min 6.15 MB (peak ~1.2 MB), internal min 69.1 KB (was 70.8), loop 2,511 us,
  0 over 5 ms. The fill itself (one loop pass) stalled 33 ms, logged as rf_stall.
- Unverified: packet entries from real presses and their signal (no presses on .196; the lead
  reads the media room hub). /simplify done; skipped: a separate dev_build.py, stall coalescing in
  core, a firmware remote_log module, merging the two HID decorators.

## IR (branch `ir`; design `docs/design/ir.md`; wired, built; spike measured on .196)

- Hardware: Adafruit 5639 emitter, signal on GPIO 1 (`board::ir::kPin`, header pin 25), V+ on VSYS.
  README "IR emitter" has the wiring.
- `ir_transmitter.{h,cpp}`: `RmtIrTransmitter` implements core's `integrations::IrTransmitter`
  (send(signal, frames, hold), stop(), busy(), ready()). main.cpp: `IrAdapter irAdapter("ir",
  irTransmitter)` in the AdapterSet (3.2 KB internal .bss, like haAdapter); `irTransmitter.begin()`
  right after the adapters are added; `runtime.setIrTransmitter()` → EngineRuntime reports
  `irIntegrationState/Detail(ready())`. Device availability: "unknown" (as the sim).
  - "ir" task, core 0, **prio 20** (above lwIP 18 / W5500 15: at prio 6, HTTP load delayed a
    hold's next frame 15-23 ms), 4 KB PSRAM stack. It creates the channel, so the RMT ISR is on core 0.
  - send() claims `busy_` with a compare-exchange (the console on httpd and the engine on the loop
    can both call it), fills the mailbox, `xTaskNotifyGive`. Never blocks. Durations are read when
    the task picks the job up (core's storage stays valid).
  - TX channel: 1 MHz ticks, 192 symbols (all 4 TX blocks), queue depth 4, copy encoder,
    `rmt_apply_carrier` (duty 0.33, active high, eot low) only when the carrier changes. Halves over
    32,767 us are split; an odd half count splits the last gap. Symbols in PSRAM (the ISR isn't
    IRAM-safe, so it never runs with the cache off). A one-section code (first == repeat) is built once.
  - Frames: one transaction each; the next is queued 5 ms before the one in flight ends
    (`kQueueLeadUs`), so frames run back to back and a stop() until then ends after the current
    frame. A failed rmt_transmit ends the send after what's in flight (busy clears).
- Console: `ir test [frames]` (`kIrTestCode`: NEC 0x04/0x02, LG volume up), `ir hold [ms]` (an
  esp_timer one-shot calls stop(); `ir stop` cancels it so it can't end a later engine hold),
  `ir send <frames> <pronto>` (URL limit ~90 words), `ir stop`, `ir`. Always runs on httpd (serial
  posts it there), so its PSRAM buffers have one owner. Parses with core's `pronto::parse` /
  `durationUs` / `carrierHz` (per-word rounding, strict spec text), the same as the adapter's
  `toCode`. The serial line buffer (512 B) is in PSRAM. The spike's capture/ahead/stats are gone.
- /simplify done: core's Pronto helpers instead of a console parser; the NEC code a constant;
  `loop()` flattened (`more()`); build scratch local; queue depth 2; one console entry point.
  Skipped: a core `toSignal()` helper shared with `toCode` (first/repeat rule is in two places:
  a core change), a table-driven /debug/run (only `ir` needs arguments today).
- `scripts/core_embeds.py` also puts `core/third_party/ArduinoJson` on the path: core's
  `config/profile.h` now includes `config/json.h`, and the generated builtin_profiles.cpp is built
  before the library finder adds core's flags.
- After /simplify on .196: `ir test 3`, `ir hold 5000` (ends on time), `ir hold 0` + `ir stop` all
  fine; lowercase Pronto refused (strict, like profiles). Loop 1,471 us, 0 over 5 ms; internal
  67.5 KB free, min 65.8 KB; /status `ir` ready. Sizes: default RAM 94,832 B, flash 2,478,919 B;
  `_ota` (OTA password) RAM 105,464 B, flash 2,528,911 B.
- Spike measurements (.196, before the cleanup; same send path):
  - Carrier 38,029 asked → 38,040 Hz (80 MHz / (693+1410)), captured 38,036.8 Hz, duty 32.9%. 16-bit
    high/low counts of 80 MHz: ~0.61 kHz up to MHz (8,972 Hz exact; 518 kHz → 519.5 kHz).
  - Durations: Pronto → us with a rounded running total (≤0.5 us, no drift); RMT exact to the tick.
    Loop-back capture: mark+space pairs exact (the RX demodulator biases marks +5..24 us).
  - Back-to-back frames within 13-14 us; first frame starts ~55 us after the call. rmt_transmit
    33-55 us; build 40 us (NEC) to 0.65 ms (512 pairs). CPU: 0.10% of core 0 during a hold.
  - ≤190 pairs fit in channel RAM; 250 and 512 pairs went out right with ping-pong refills (flash
    stall during one untested).
  - Holds 10-20 s: 0 gaps, also under 240 HTTP GETs (prio 20). Loop 1.5-3.0 ms max gap, 0 over
    5 ms during holds. One 17 ms right after a reboot (not repeated).
- Internal heap on the spike build: 59.8 KB idle (was ~72 KB); 5.6 KB console statics and the 4 KB
  stack have since moved to PSRAM, irAdapter adds 3.2 KB. Sizes now: default RAM 95,328 B, flash
  2,476,423 B; `_ota` RAM 103,544 B, flash 2,498,111 B (no OTA password in these builds).
- On .196 (wired build, 2026-11-24): boot `ir: ready on GPIO 1`; /status lists `ir` ready. Idle
  (HA, MQTT up, ~2 min): internal free 67.1 KB, min 65.2 KB, largest 31.7 KB; loop 1,442 us, 0 over
  5 ms. During `ir hold 5000`: loop 1,501 us, 0 over 5 ms, no send errors. `_ota` with the OTA
  password: RAM 106,000 B, flash 2,529,059 B. Pending: the user's look at the red LED, then the
  board test (docs/design/ir.md).
- Lead review (IR), done:
  - `takeFailed()` (core's port): `failed_` is set at every failure after an accepted send (build:
    zero/odd/over kMaxSymbols; carrier refused; rmt_transmit error), each logged; `fail()` drops
    busy_ at once, an rmt_transmit failure after what's in flight. send() clears it. Not triggerable
    from the console (core's parse refuses such codes); untested on the board.
  - Console: core's `pronto::parse` → `appendIrCode` → `irSignal` (a static vector, PSRAM, kept until
    the next send; httpd only). The first/repeat rule now lives only in core.
  - stop() inside the last 5 ms of a frame still sends the queued one: kept; core's port documents it.
  - A serial line over 511 chars is refused ("console: line too long"); README says so.
- On .196 after the review fixes: ir test 3, hold 5000 (ends on time), hold 0 + stop fine; loop
  2,599 us, 0 over 5 ms; internal 66.9 KB free, min 65.1 KB; /status `ir` ready. Sizes: default
  RAM 95,584 B, flash 2,488,963 B; `_ota` (OTA password) RAM 106,264 B, flash 2,538,303 B.
- Phase gate (fw) passed (2026-11-24): default (version gate) RAM 95,544 B, flash 2,480,643 B (37.9%);
  `_ota` RAM 103,808 B, flash 2,508,587 B (38.3%). Board test passed on the media room hub (.94):
  a Denon's power, volume, mute and inputs over IR (lead).

## Board budget (branch `board-budget`; on .196, not committed, no gate yet)

- `GET /debug/stats` (dev builds): the budget's numbers as JSON. Only `?reset=1` (close the
  window) carries the loop and read->hid fields. The table and the load profile are in
  firmware/README.md "Board budget"; the lead's `bin/hubbo board-check` runs against it.
- Window close (`diag::closeWindow(&out)`, httpd only): bumps `closeRequested` unless one is
  still pending, waits 1 ms polls up to 200 ms for the loop's `closedSerial`; the loop copies the
  window into `closed` and resets it in one pass, never waiting. A timeout answers 503 and the
  close stays pending: the next reset returns that window, so none is lost. The window's
  read->hid is its own LatencyStats (`windowHid`, PSRAM), recorded next to the since-boot one in
  TimedHidTransport; a close never touches what `lat` and /api/v1/status report. No published
  copy any more (publishStats deleted).
- `diag::TaskShares`: one sampler per caller (`tasks`, /debug/stats), deltas by modular uint32
  subtraction (the counters wrap at ~72 min). loopTask's share is unknown (`shareKnown` false,
  `cpu_pct_x100` null, `-` in `tasks`): it never blocks, so its counter is stale. Task arrays are
  on the heap (the loop has ~3 KB stack).
- `image_bytes` is null if the image metadata read fails (a failure isn't cached); board-check
  fails a null.
- RmtIrTransmitter counters: frames, late_frames (a frame queued after the one before it had
  already ended), failures. `bleSendFailures` is counted in TimedHidTransport.
- `remote inject` was deleted (lead, 2026-11-24): it re-implemented the engine's dispatch and was
  the only thing that let /debug/run act. Latency comes from real presses.
- /debug/* handlers return `HttpServer::DebugReply` (body + status): 400 invalid, 409 busy, 503
  the loop didn't answer a close.
- ArduinoJson 7 keeps a `const char[N]` by pointer (as a literal): cast char arrays to
  `const char *` so they're copied (task names came out as garbage otherwise).
- Hub settings: `setCheckUpdates`/`setRadioPower` write the LittleFS record every time, even
  unchanged; board-check's flash window saves `check_updates` with its current value.
- Task-watchdog recorder: `esp_task_wdt_isr_user_handler` saves each core's running task to RTC
  noinit; the next boot logs it only if the reset reason is the task watchdog, and clears it. It doesn't change the watchdog.
- `firmware/budget.json` (mine) holds the limits `bin/hubbo board-check` (the lead's
  bin/board_check.py) judges; the README table mirrors it. Keep both in step; limits change only
  with the user's OK. The check never switches activities; the flash window is two hub saves.
- Measured (settled): idle window max gap 1.5 ms, 0 over 5 ms; internal 66.0 KB free, min 63.7,
  largest 31.7; stacks: loop 2.9 KB, httpd 13.1, ha 4.1, mqtt 6.1, update 3.3, ir 2.3 (after a
  hold), mqtt_task 1.7. IR hold: 186 frames, 0 late. image 2,596,672 B; static RAM 106,800 B.
- Gotchas:
  - The loop never blocks, so its FreeRTOS run-time counter goes stale; its share isn't reported.
  - `esp_ipc_call_blocking(1, ...)` from httpd deadlocks: IPC runs at the caller's priority (5),
    below the loop. Never call it from a task below 16.
  - The since-boot loop gap (`lat`) picks up one stall of about 17 ms in the first minutes after
    boot. Judge the budget on windows after the settle.
- **Open: W5500 starvation (next, own branch; the user decided).** 2–4 parallel HTTP clients
  looping for under 60 s trip the task watchdog. The recorder showed core 0 in `w5500_tsk` and in
  `tiT`. Earlier "other watchdog" resets had dumps with an invalid CRC. Not yet shown whether
  main reproduces it. The loaded window in board-check keeps 4 clients and is expected to fail
  until the fix lands.
  Also on that branch: the last OTAs logged a loop max gap of about 30 s for the run being
  replaced (945 ms on an earlier one). OTA seems to block the loop for the whole upload; check
  whether that's by design.
- Cleanup (2026-11-24, the user: "lean and PURPOSEFUL"): main.cpp is wiring, boot and the
  loop; the serial console is commands.{h,cpp}; the dev console is dev_console.{h,cpp} plus
  board_stats.{h,cpp}, one seam (`dev_console::begin/poll`, empty inlines without
  HUBBO_DEV_CONSOLE). Removed: /debug/bench, /debug/ws-echo, /debug/dns, the http and esp-mqtt
  stack-low log lines, RemoteRadio's oversize counter and default scan timeout (core's
  `api::kRemoteScanTimeoutMs`), flash_fs's FormatRefusedMounted, the IR `sends` counter, and the
  stats fields no rule reads (slot_bytes, loop age/window_ms/since_boot/over_2ms/over_10ms,
  p50/max, task core/prio, radio payloads/bad/unknown, ir ready/busy, ha/mqtt state). The release
  test feed (setTest, versionOverride_) is now dev-build only. max_uri_handlers 11.
- Cleanup, round 2 (lead's decisions): RemoteRadio uses core's `api::RadioState`/`ScanState`
  (no mirrors or mappers); the update owner flag is `update_lock.{h,cpp}` (ota.* is ArduinoOTA
  only); `EspWsTransport::lastError_` deleted; budget rule `integrations.ir.frames` min 1
  (loaded). Kept: BLE link-event log (the Fire TV late-encryption item is open).
- Later (core, through the lead): `utf8Prefix` (hub_name.cpp) is duplicated inline in core's
  ha/services.cpp; it could move to core and serve both.
- State (2026-11-24, after review 2 A–F): board-check --clients 1 passed on .196. Loop max gap
  3.32/3.37/3.32 ms (idle/loaded/flash), 0 over 5 ms; IR 557 frames, 0 late; internal heap
  66.2 KB free, 63.0 min, 31.0 largest; image 2.47 MB, static RAM 102.8 KB; IDLE0 94.0% idle,
  29.7% loaded. Gate fw passed (frozen): default RAM 94,464 B, flash 2,480,219 B; `_ota` RAM
  102,792 B, flash 2,507,731 B. Not committed.
