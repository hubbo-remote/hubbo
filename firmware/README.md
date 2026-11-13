# Hubbo firmware

PlatformIO project for the Waveshare ESP32-S3-ETH. It receives the Harmony Companion remote and
runs the portable core on it: `RemoteInput` → `Engine` → `BleHidAdapter` → NimBLE. Config loads
from LittleFS through the core's `ConfigService`. The shared `/api/v1` handlers are served over
Ethernet by `esp_http_server`. A serial console covers bring-up and latency measurement.

```sh
bin/hubbo fw run                     # build (env waveshare_esp32_s3_eth)
bin/hubbo ota <ip>                   # build the dev (OTA) image and upload it over Ethernet
```

Pinned: `espressif32@8.0.0` (arduino-esp32 3.3.12), `nRF24/RF24@1.6.2`,
`h2zero/NimBLE-Arduino@2.5.1`. These are the versions the Harmonizer spike ran on.

## Layout

| File | What |
|---|---|
| `src/board.h` | Board profile: pins and SPI hosts for the Waveshare ESP32-S3-ETH. The only board. |
| `src/remote_radio.*` | nRF24: Hub scan (learns channel + address), listener, endpoint in NVS. Payloads go to a `PayloadSink` unchanged. |
| `src/ethernet_link.*` | W5500 Ethernet with DHCP, non-blocking, logs link changes. |
| `src/ble_hid.*` | NimBLE HID keyboard + consumer control for up to 4 hosts, one per Bluetooth device. It is the core's per-device `HidTransport`: the host table, connect whitelist, pairing window, forget, reports sent by identity. No key state (`BleHidAdapter` owns it). |
| `src/ble_host_runtime.*` | `integrations::BleHostPort` (`ApiContext.bleHosts`): httpd's view of the hosts (a snapshot the loop publishes) and a request queue the loop drains. |
| `include/sdkconfig.h`, `scripts/sdkconfig_shadow.py` | Raise NimBLE's limits (5 connections, 5 bonds, 32 CCCDs) for our sources and every library. |
| `src/http_server.*` | `esp_http_server` → `hubbo::http::Router`: path decoding, body limit from the matched route (`Router::bodyLimit`, 413 before reading) into a per-request PSRAM buffer, headers, authenticator, response. |
| `src/engine_runtime.*` | `api::Runtime`: the HTTP task's bridge to the engine task (`EngineRequest` submit + wait, status snapshot, BLE integration state). |
| `src/flash_fs.*` | LittleFS on the `spiffs` partition at `/fs`. Formats only a positively blank partition (all 0xFF, scanned at boot) or at boot after `fs format`; never while running, never on an unexplained mount failure. |
| `src/littlefs_storage.h` | `storage::Storage` on `/fs` with honest `health()` (Unavailable when not mounted), under core's `SlotRecordStore`. |
| `src/nvs_secret_store.*` | `auth::SecretStore` on NVS. |
| `src/esp_hash.h` | `auth::Hash` with SHA-256 on the hardware accelerator (mbedTLS); HMAC and PBKDF2 stay portable. |
| `src/esp_clock.h`, `src/esp_random_source.h` | `api::Clock` and `auth::RandomSource` ports. The RNG fails closed until BLE (the RF entropy source) is up. |
| `src/diagnostics.*` | Serial log ring, raw payload log, latency probe (`TimedHidTransport`, loop gap). |
| `src/console.*` | `con`: every text output. USB serial is non-blocking (4 KB TX buffer, 0 ms timeout: a slow or absent host never stalls the loop; overflow is dropped). Dev builds keep the last 8 KB for `/debug/log`. |
| `src/latency.*` | Fixed-size latency recorder (p50/p95/p99/max). |
| `src/ota.*` | ArduinoOTA, compiled in only with `HUBBO_OTA_PASSWORD`, and the update owner flag it shares with the web update. |
| `src/factory_reset.*` | `api::FactoryResetPort`: records the request (one NVS flag) and reboots; the wipe (all of NVS, then the fs format) runs early in the next boot. The gate's `HUBBO_WIPE_ON_BOOT` uses the same wipe. |
| `src/hub_name.*` | The hub's name (core's `HubSettings`, a StateStore record): the BLE name, the mDNS instance and hostname, live on a rename (mDNS on httpd, BLE through a mailbox to the loop); DHCP and OTA take the boot hostname. |
| `src/restart.*` | `api::RestartPort` (POST /admin/restart): claims the update-owner flag, then the normal reboot path. |
| `src/firmware_update.*` | `api::FirmwareUpdatePort` over esp_ota: firmware updates from the web admin (see "Web updates"). |
| `src/image_marker.h`, `scripts/image_marker.py` | The `HUBBO-IMAGE/1 board=… layout=… version=<git describe>` marker every image carries; the build fails if the .bin doesn't hold exactly one. Its version is the API's `version`. |
| `src/main.cpp` | Wiring, boot, main loop, serial console. |
| `scripts/drop_unused_idf_libs.py` | Link step: drops the precompiled esp_matter archive. Otherwise ordinary `std::string` calls resolve to Matter's copies and pull in Matter and the Wi-Fi stack (+24 KB RAM, +230 KB flash). |

The W5500 is on SPI3 (ESP-IDF driver); the nRF24 is on the Arduino `SPI` (FSPI/SPI2) with
its own pins. They never share a bus. The nRF24's IRQ isn't wired, so the radio is polled
every loop iteration.

Code ported from [Harmonizer](https://github.com/soaresbrun0/harmonizer) (AGPL-3.0) is marked
at the top of each file.

## Tasks

| Task | Core | Priority | Runs |
|---|---|---|---|
| `loopTask` (Arduino) | 1 | 16 (raised in `setup()`) | RF + engine: radio FIFO → `RemoteInput` → `Engine` → adapters → NimBLE notify; `engine.poll()`; serial console. Task watchdog on (5 s). |
| `httpd` | 0 | 5 | Every API handler, including `ConfigService` (round-trip, compile, record save, publish) and the active-activity persister. Reaches the engine only through `Engine::submit()` (sole producer, via `EngineRuntime::run`, which `vTaskDelay`-polls `done()`), the `SnapshotStore`, and a status record the loop publishes every 250 ms under a spinlock. |
| `esp_timer` | 0 | framework (22) | `HttpServer::every()`: once a second, decides whether httpd housekeeping is due (an activity change, a pending active-activity write, or a config that is `storage_unavailable` and needs `ConfigService::refresh()`), and if so queues it onto httpd, at most one at a time. The work itself runs on httpd. |
| NimBLE host | 0 | framework | BLE stack. Callbacks only set flags. |
| `tcpip` (lwIP) | 0 | framework (above httpd) | Network. |
| W5500 RX (`w5500_tsk`) | none (runs on 0) | 15 | Reads frames from the W5500. The driver gives it no core affinity, so with loopTask above it, it always runs on core 0. Ethernet is started from a short task pinned to core 0, so the W5500 GPIO and SPI interrupts are on core 0 too. |

`EngineRuntime::run` returns only once the engine has finished the request. The request lives on
the handler's stack, so giving up early would leave the engine writing to freed memory. The engine
bounds the wait: transitions finish on the next poll, executions by the 5 s action timeout.

loopTask runs at priority 16, above every task without a core affinity, so those run on core 0 (an idle LAN otherwise preempted the radio loop for 10–17 ms at a time). It never blocks, so nothing below it runs on core 1. Tasks pinned to core 1 above it (IPC, Arduino network events) still preempt it briefly. The `tasks` command shows each task's core and CPU share.

Core 1 has only the RF/engine loop, so no handler or network burst can delay a radio read. Flash
writes are the exception: with no XIP from PSRAM and no flash auto-suspend, every erase or write
turns the cache off on both cores, so it stalls the loop for its length. A config save (LittleFS)
costs about 2 ms of loop gap (5 saves: max gap 2.1 ms, none over 5 ms). A firmware update erases
about 500 sectors, so the remote may lag while it writes. On core 0 httpd sits below the network and BLE stacks. A slow handler, such as login's
PBKDF2 at about 0.5 s, delays only other HTTP requests. The idle-task watchdog
on core 0 (5 s) bounds any runaway handler.

From the end of `setup()` every `malloc`/`new` tries PSRAM first (`heap_caps_malloc_extmem_enable(0)`).
The prebuilt sdkconfig only sends blocks of 4 KB and up there, and a restore's many small JSON and
config allocations used up the ~100 KB of internal RAM. The next internal allocation anywhere then
aborted (the core dump showed lwIP's mDNS logging an out-of-memory error). Drivers that need
internal or DMA memory ask for it explicitly.

## Storage

| Where | What |
|---|---|
| LittleFS `/fs` (`spiffs` partition, 0xC90000, 3.4 MB) | Core's record store (`docs/design/persistence-and-contracts.md`): `config.a`/`config.b` and `active.a`/`active.b`, each slot a CRC'd header plus payload. A save writes the older slot, fsyncs, reads it back, and only then is acknowledged; load takes the newest intact slot. Written only on the HTTP task, never on button presses. Not mounted means the store is Unavailable: config is `storage_unavailable` and every edit fails. |
| NVS `hubbo-radio` | Learned Harmony Hub endpoint: `channel` (u8), `address` (u64). |
| NVS `hubbo-fs` | `format_req` (bool): `fs format` asked for a format at the next boot. Unreadable counts as "not requested". (No "already formatted" flag: auto-format needs the partition to read as blank.) |
| NVS `hubbo-secret` (chosen over the record store: NVS writes each entry atomically, and login still works if LittleFS breaks, so the hub stays reachable for a restore) | `auth::SecretStore`: `admin_user`, `admin_pw`, `tok_<id>`. Plain NVS: both NVS encryption schemes need eFuse burning (an HMAC key) or flash encryption, which this MVP doesn't do. Someone who dumps the flash can read the password hash and token hashes, but not the plaintext. |
| NVS `nimble_bond` | NimBLE's own bond store (up to 5: 4 hosts plus a re-pair's new bond before the old one is deleted; a full store refuses, never evicts). |
| NVS `hubbo-ble` | `hosts`: device id -> host identity address, 4 records in one blob. Written on httpd when a bond is adopted or forgotten. At boot, entries without a bond are dropped and bonds no device owns are deleted. |

## Network

DHCP on the W5500. Once the hub has an address it starts mDNS as `hubbo-<efuse MAC>.local`
(advertising `_http._tcp`). Requests are plain HTTP (`secure` = false); HTTPS is out of scope.

## OTA

`bin/hubbo ota <ip>` builds the `waveshare_esp32_s3_eth_ota` env and uploads it, with the
password read from a private file (see docs/development.md). A build without a password compiles
OTA out entirely. Only its SHA-256 is compiled in (`scripts/ota_password_hash.py`,
`ArduinoOTA.setPasswordHash`), never the password, and neither is ever printed. ArduinoOTA's
challenge uses that hash as the key, so an OTA-enabled image grants OTA access to boards running
it: treat images and flash dumps as sensitive.
Hostname is `hubbo-<efuse MAC>`.
OTA starts once Ethernet has an address.

ArduinoOTA receives and writes the image inside `handle()` on the main loop. The loop's task
watchdog stays on: each progress callback (one per chunk written) feeds it. A live transfer keeps it
satisfied, and a stalled or trickling one trips it and reboots the hub. ArduinoOTA's receive timeout
(1 s, up to 3 retries) and the 3 s connect timeout keep every gap under the 5 s watchdog. A
transfer gets 120 s in total: after that, progress no longer feeds the watchdog, so a client
trickling bytes can't hold the radio loop (and the remote) hostage. ArduinoOTA's own reboot is turned off:
once the image is written, the hub reboots like any other deliberate reboot.

### Web updates

`POST /api/v1/admin/firmware/update` (admin only) hands the buffered image to
`FirmwareUpdate::install()` on httpd. In order: it claims the owner flag (busy if ArduinoOTA or a
reboot holds it), checks the whole image with core's `api::validateFirmwareImage()` (slot size,
0xE9, ESP32-S3 and chip revision, app description, the Hubbo marker's board and layout, the
appended SHA-256 on the hardware SHA, `esp_hash.h`), copies it to PSRAM, and calls `esp_ota_begin` with
sequential writes, which erases nothing. Then a one-shot `fw_write` task (core 0, priority 3,
internal stack) writes 4 KB at a time through an internal buffer, one sector erase per write, with a
tick's yield between sectors. `esp_ota_end` checks the image again from flash, the boot slot is
set, and `requestReboot` restarts the hub. A failure leaves the running image as the boot image.
The remote may lag for the ~30 s of the write: flash erases stall both cores. There's no
rollback: Arduino marks an image valid before `setup()`.

While the web update holds the owner flag, the loop never runs ArduinoOTA's `handle()` and
`ota_watch` drops OTA packets, so `bin/hubbo ota` just gets no answer. ArduinoOTA claims the flag
for each `handle()` call, which holds its whole transfer. Once a reboot is pending, nobody can
claim it.

## Deliberate reboots

`reboot`, `fs format` and a finished OTA are requests. The main loop never waits on another task,
because it alone polls the engine and httpd may be waiting on the engine. It releases held keys
itself, stops reading the radio, and posts "flush and restart" to httpd. httpd writes out the
debounced active activity, then restarts. The engine keeps being polled until then. Just before
any deliberate restart, the loop line (`loop: max gap ...`) is printed and kept in RTC memory, and
the next boot logs it as `previous run (restart: <why>): ...`, so a stall before a reboot (a
firmware update's write) can still be read from `/debug/log`.

## Dev console over HTTP (dev builds)

The `waveshare_esp32_s3_eth_ota` env adds `HUBBO_DEV_CONSOLE`, because the dev board runs on PoE with
no serial attached. Plain-text GETs outside the API and its auth, read-only by design:

| URL | |
|---|---|
| `/debug/log` | The last 8 KB of console output. |
| `/debug/run?cmd=status` | Queues one console command on the loop task; read the result in `/debug/log`. Allowed: `status`, `mem`, `lat`, `lat reset`, `tasks`, `help`. Nothing that changes state. |
| `/debug/bench?iterations=N` | Times PBKDF2-HMAC-SHA256 on httpd at N iterations (default: a login's cost, 10,000; at most 20,000). |
| `/debug/coredump` | The core dump the last panic left in flash (raw partition image). Decode it with esp-coredump against that build's `firmware.elf`. |

## Serial console

115200 baud over USB CDC. Type `help`. Status prints every 30 s.

| Command | |
|---|---|
| `status` | Radio, engine, Ethernet + HTTP, BLE, heap/PSRAM/loop stack, latency. Printed every 30 s. |
| `mem` | Memory, then filesystem usage (printed by httpd: it walks the filesystem, which the RF task never does). |
| `scan [seconds]` / `scan stop` | Learn a Harmony Hub's address (default 120 s). Saved to NVS on success. |
| `radio set <ch> <addr>` | Use a known Hub endpoint instead of scanning: channel 0–124 (decimal), 40-bit address (hex, `0x` optional). Validated, saved like a scanned one, listened on at once. |
| `radio forget` | Erase the saved endpoint. |
| `ble pair <device_id>` / `ble cancel <device_id>` | A pairing window for that Bluetooth device (120 s). Its old host stays until a new bond completes. |
| `ble forget <device_id>` | Delete that device's bond. |
| `raw on\|off` | Print raw payloads. |
| `lat`, `lat reset` | Latency summary / clear it. |
| `fs format` | Erase the config filesystem: records the request and reboots; the format runs at boot, before anything uses the filesystem. |
| `wdt` | Hang the loop; the task watchdog should reboot the board in ~5 s. |
| `reboot` | |

The log shows each remote event and then what the engine did with it:

```
event 81234 up down
action 81234 ble dev_apple key_down -> accepted
event 81402 up up
action 81402 ble dev_apple key_up -> accepted
activity 90001 watch_tv
```

`timedOut` marks a release synthesized after 500 ms of radio silence. Lines are formatted on the
RF task into a ring and printed after the hot path, and an event's line is formatted only after
the engine has dispatched it.

### Latency measurement (Phase 0.3 / 1.5)

- `read->event`: `micros()` right after the payload leaves the nRF24 FIFO, to the `Down`/`Up`
  event reaching the engine. Decode cost.
- `read->hid`: same start, to the HID report being queued to the NimBLE host by the engine's
  BLE adapter. This is the full internal dispatch path (ARCHITECTURE target: p95 < 10 ms,
  p99 < 25 ms). Only reports caused directly by a radio payload count, not timed-out releases.
- `loop`: longest main-loop iteration and count over 5 ms. The radio is polled, so a long
  iteration delays the FIFO read; that delay is **not** in `read->*`.

Not measured: air time from remote to radio, and the BLE link to the host (connection interval,
15 ms requested).

## Hardware bring-up checklist

Dedicated dev board only. USB power only, never USB and PoE together.

1. Fit the 10 µF capacitor across the nRF24's VCC/GND. Wire SCK 15, MISO 16, MOSI 17, CSN 18, CE 38.
2. Flash over USB and open the serial monitor at 115200. In the boot log, check:
   - reset reason `power-on`, 16 MB flash, about 8 MB PSRAM;
   - `fs: blank partition, formatted` the first time, then plain `fs: mounted, …` on later boots;
   - `radio: nRF24L01+ ready`, `ble: started as "Hubbo"`;
   - `config: fresh` and `config: no saved config; starting empty` (first boot),
     `auth: fresh, secrets available (no admin yet: first-run setup open)`,
     `activity: none saved`, `http: listening on port 80`;
   - the `mem:` line. Save it as the memory baseline.
3. Plug in Ethernet. Expect `eth: link up`, then `eth: up, IP …`. From a LAN machine:
   - `curl -i http://<ip>/api/v1/status` and `curl -i http://hubbo-<mac>.local/api/v1/status`
     should both answer, with the status JSON or 401 as `api/openapi.yaml` says.
   - The console's `status` shows the request count.
   - First-run setup (creating the admin through the API) and a login prove NVS secrets and RNG
     entropy. Whoever owns the credentials does it, once.
4. Give the radio its Hub endpoint. If it's known, use `radio set <channel> <address>`: the guest
   room Hub is `radio set 5 0xFC5C97550E`, the family room Hub `radio set 74 0xFC5C97550F`. If there's
   a Harmony Hub to scan instead, type `scan` and press its pair button (`found Harmony Hub`).
   Either way expect `radio: listening on channel …`. Reboot: it listens again from NVS.
5. Press, hold and release buttons. With an empty config every event is unbound: you see `event`
   lines, and `status` shows them as `unbound`. A held button gives `hold` at 800 ms. No `up`
   should carry `timedOut`.
6. Through the API, register the Apple TV as a `ble` device. Type `ble pair <device_id>`, and on
   the Apple TV pick "Hubbo" (Settings > Remotes and Devices > Bluetooth).
   Expect `ble: <device_id> paired with …`; `status` lists it as connected.
7. Through the API, create an activity that binds
   the d-pad to `key_down`/`key_up`, then activate it through the API. Hold Up. Expect `action … key_down -> accepted`,
   tvOS repeating, and `key_up` on release. `lat` gives `read->hid` p50/p95/p99. Record them.
8. Hold a d-pad button and turn the Apple TV off or walk out of range. After it reconnects
   no key may be stuck.
9. With RF in use, drive HTTP traffic (for example `curl` in a loop). `loop: max gap` and
   `read->hid` should not move.
10. Type `wdt` and expect a reboot with reset reason `task watchdog`.
11. Leave it running for 30 minutes with Ethernet up. Minimum free heap should be stable and
    bad checksums flat.
12. Reboot. The boot log shows `config: ok, generation N`, and `GET /api/v1/activities` lists the
    same activities.
13. Activate an activity and wait more than 5 s. Reboot (`reboot`, or power cycle). The boot log
    shows `activity: restored <id> (no startup)`, and there are no `action` lines for its startup.
    The remote's bindings work straight away. Then turn it off (API or the remote), wait over 5 s,
    reboot, and expect `activity: none saved`.
14. Switch activity, then immediately reboot via OTA or `reboot` (within the 5 s debounce). The
    pending write is flushed first, so the new activity is restored. A power cut inside the window
    may still restore the previous one; that's the debounce.
15. Pull power during a save (repeat it several times), for example while a script PUTs an activity
    in a loop. Every boot must show `config: ok` with either the old or the new generation, never
    `blocked` or `fresh`. An edit the API acknowledged must never be lost.
16. Corrupt one slot: with the board off, flash garbage over part of the `spiffs` partition (or
    truncate `config.a` in a test build). Boot falls back to the other intact slot. With both
    slots corrupt, expect `config: fresh`. Corrupting the whole filesystem gives
    `fs: MOUNT FAILED` and `config: storage_unavailable`; edits return errors; `fs format` recovers.
17. Restore a config record with a newer `schema_version` (written with `hub-sim write-record` and
    copied over). Expect `config: blocked, unsupported_schema`, nothing active, and every edit
    refused until a restore or factory reset.
18. Unreadable records are errors, not "missing". With a slot file that exists but can't be read
    (corrupt LittleFS metadata from a power cut, or a debug build that makes `config.a`
    unreadable), boot must show `config: storage_unavailable` and never `fresh`. esp_littlefs must
    report anything other than a truly missing file as an error (only ENOENT counts as NotFound).
    Core's `failReads` suite proves the shared code on the host; only the board proves what
    esp_littlefs returns.
19. Storage recovery. Boot with LittleFS unreadable (mount works but the slots can't be read,
    e.g. a flaky flash or an injected read fault in a debug build): expect
    `config: storage_unavailable`, nothing active, edits refused. When the medium reads again,
    the 1 Hz httpd tick's `refresh()` logs `config: storage back, now ok` with the same generation,
    and nothing was rewritten (slot files unchanged). A filesystem that didn't mount at all stays
    unavailable until `fs format`: the firmware doesn't remount while running.
20. Secrets the same way: erase or corrupt the `hubbo-secret` NVS namespace partially (or fill
    NVS). Boot must show `auth: unavailable` or `corrupt` with login and setup refused, never
    `fresh`. On a factory-fresh board, `auth: fresh` with setup open.
21. Blank partition: erase the `spiffs` partition (`esptool erase_region 0xC90000 0x360000`) and
    boot. Expect `fs: blank partition, formatted` once, then `config: fresh`. The next boot shows a
    plain mount.
22. Damaged, non-blank partition: write a few KB of garbage at the start of the partition so
    LittleFS can't mount it. Boot must show `fs: MOUNT FAILED; not blank, so not formatted` and
    `config: storage_unavailable`. The data is untouched (read the partition back to check), and
    it stays that way across reboots until `fs format`.
23. A format request that can't be cleared. In a debug build, make clearing `hubbo-fs/format_req`
    fail (or fill NVS so the write fails), then run `fs format`. The boot must log
    `fs: !!! FORMAT REQUESTED BUT NOT DONE`, keep the filesystem and its records intact, and do
    the same on every later boot. It must never format in a loop.
24. Reboots never block the radio loop: run `reboot` while an API request is in flight (for
    example a slow login). Expect `reboot requested (console)`, then `restarting` from httpd after
    the active activity is flushed. Also try `fs format`, and an OTA upload (the board restarts
    after `ota: update written`, through the same path). An OTA upload longer than 5 s must not
    trip the task watchdog. Kill espota mid-transfer: expect `ota: error …` and the hub carries on
    (radio and API working). Pause espota mid-transfer (`kill -STOP`): within about 5 s,
    ArduinoOTA gives up (`ota: error`) or the watchdog reboots the hub (reset reason
    `task watchdog`). It must never hang.
25. The RF task never touches NVS or the filesystem while running. During a burst of API writes
    (create a token, change the password, PUT activities in a loop), hold a d-pad button and run
    `status`, `mem`, `scan`, `radio forget`, `ble forget` and `ble pair`. `loop: max gap` must stay
    small: NVS and LittleFS work, and the `mem` filesystem usage, run on httpd. (OTA's image
    write is the one exception; it happens inside `handle()` by design.)
26. Restore a backup over 40 KB (16 activities with about 20 bindings each) through the API. It
    must succeed, and anything over the restore route's limit must get 413 before the body is read.
