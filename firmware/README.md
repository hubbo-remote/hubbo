# Hubbo firmware

PlatformIO project for the Waveshare ESP32-S3-ETH (16 MB flash, 8 MB OPI PSRAM, W5500 Ethernet,
PoE). It receives the Harmony Companion remote over an nRF24 and runs the portable core:
`RemoteInput` → `Engine` → adapters (Bluetooth HID, IR, Home Assistant, MQTT). The `/api/v1`
handlers and the embedded web admin are served over Ethernet by `esp_http_server`.

```sh
bin/hubbo fw run                     # build the release env (waveshare_esp32_s3_eth)
bin/hubbo ota <board ip>             # build the dev env (_ota) and upload it over Ethernet
```

| Env | What |
|---|---|
| `waveshare_esp32_s3_eth` | The release image. OTA is compiled in only if `HUBBO_OTA_PASSWORD` is set. |
| `waveshare_esp32_s3_eth_ota` | Dev builds: adds the dev console over HTTP (`/debug/*`) and uploads with espota. |

The first install is over USB (see docs/development.md). After that a hub updates itself from
GitHub releases, from an uploaded image in the web admin, or (dev builds) with `bin/hubbo ota`.

Pinned exactly: `espressif32@8.0.0` (arduino-esp32 3.3.12, ESP-IDF 5.5, GCC 14.2) and its
packages, `nRF24/RF24@1.6.2`, `h2zero/NimBLE-Arduino@2.5.1`. Builds use `-O2`. `web/dist` is
embedded, so `bin/hubbo fw run` builds the web admin first when it's stale.

## Layout

| File | What |
|---|---|
| `src/main.cpp` | Wiring, boot and the main loop only. |
| `src/board.h` | Pins and SPI hosts for the Waveshare ESP32-S3-ETH. The only board. |
| `src/remote_radio.*` | nRF24: Hub scan (learns channel + address), listener, endpoint in NVS. Payloads go to a `PayloadSink` unchanged. |
| `src/remote_runtime.*` | `api::RemotePort`: httpd's view of the radio (a copy the loop refreshes) and a one-slot mailbox the loop takes. |
| `src/ethernet_link.*` | W5500 Ethernet with DHCP, non-blocking; logs link changes. |
| `src/ble_hid.*` | NimBLE HID keyboard + consumer control for up to 4 hosts, one per Bluetooth device. The core's per-device `HidTransport`: host table, connect whitelist, pairing window, forget. No key state (`BleHidAdapter` owns it). |
| `src/ble_host_runtime.*` | `integrations::BleHostPort`: httpd's view of the hosts (a snapshot the loop publishes) and a request queue the loop drains. |
| `src/ir_transmitter.*` | `integrations::IrTransmitter` on RMT TX with a hardware carrier (GPIO 1); core's `IrAdapter` drives it. |
| `src/integration_task.*` | A core-0 worker task (priority 4, PSRAM stack) with a job queue. Three run: `ha`, `mqtt` and `update`. |
| `src/ws_transport.*` | `WebSocketTransport` on `esp_transport_ws` (ws:// or wss://, verified against the IDF certificate bundle). The Home Assistant connection. |
| `src/mqtt_transport.*` | `MqttTransport` on esp-mqtt: MQTT 3.1.1 over TCP, QoS 0. |
| `src/tls_memory.*` | mbedTLS allocates from PSRAM first; long handshakes yield a tick every 20 ms so IDLE0 (the watchdog) still runs. |
| `src/http_server.*` | `esp_http_server` → `hubbo::http::Router`: path decoding, per-route body limit (413 before reading) into a PSRAM buffer, auth, response; serves the web admin. |
| `src/web_assets.h`, `scripts/web_assets.py` | The web admin from `web/dist`, gzipped into flash at build time. A missing `web/dist` fails the build. |
| `src/engine_runtime.*` | `api::Runtime`: httpd's bridge to the engine (`EngineRequest` submit + wait, status snapshot). |
| `src/flash_fs.*` | LittleFS on the `spiffs` partition at `/fs`. Formats only a blank partition (all 0xFF) or at boot after `fs format`; never while running, never after an unexplained mount failure. |
| `src/littlefs_storage.h` | `storage::Storage` on `/fs`, under core's `SlotRecordStore`. `health()` is Unavailable when not mounted. |
| `src/nvs_secret_store.*` | `auth::SecretStore` on NVS. |
| `src/esp_hash.h`, `src/esp_clock.h`, `src/esp_random_source.h` | Hardware SHA-256, clock and RNG ports. The RNG fails closed until BLE (the RF entropy source) is up. |
| `src/hub_name.*` | The hub's name: the BLE name, mDNS instance and hostname, live on a rename. DHCP and OTA take the boot hostname. |
| `src/firmware_update.*` | `api::FirmwareUpdatePort` over esp_ota: uploaded images and downloaded releases. |
| `src/firmware_releases.*`, `src/esp_release_feed.*`, `src/release_key.h` | Online updates: the signed GitHub release feed, downloads, and the trial a new image boots on. |
| `src/image_marker.h`, `scripts/image_marker.py` | The `HUBBO-IMAGE/1 board=… layout=… version=…` marker every image carries. `HUBBO_FW_VERSION` sets the version, else `git describe`. |
| `src/update_lock.*` | The one-update-at-a-time owner flag that ArduinoOTA, web updates, factory reset and restart claim. |
| `src/ota.*` | ArduinoOTA, compiled in only with `HUBBO_OTA_PASSWORD`. |
| `src/factory_reset.*`, `src/restart.*` | `POST /admin/factory-reset` (wipes NVS and formats the filesystem at the next boot) and `POST /admin/restart`. |
| `src/console.*` | Every text output. USB serial is non-blocking (4 KB TX buffer, 0 ms timeout, overflow dropped). Dev builds keep the last 8 KB for `/debug/log`. |
| `src/commands.*` | The serial console's commands (all builds). |
| `src/diagnostics.*`, `src/latency.*` | Log ring, raw payload log, latency probe, loop gap, task CPU shares, the task-watchdog record. |
| `src/dev_console.*`, `src/board_stats.*` | Dev builds only: `/debug/*` and `/debug/stats`. Release builds get empty inlines. |
| `include/sdkconfig.h`, `scripts/sdkconfig_shadow.py` | Raise NimBLE's limits (5 connections, 5 bonds, 32 CCCDs) for our sources and every library. |
| `scripts/core_embeds.py` | Embeds the built-in device profiles and the agent docs with core's generators. |
| `scripts/ota_password_hash.py` | Compiles in SHA-256 of `HUBBO_OTA_PASSWORD`, never the password. |
| `scripts/release_key.py` | Builds in `release-key.pub.pem`, the key release manifests are checked against. |
| `scripts/wipe_on_boot.py` | `HUBBO_WIPE_ON_BOOT=1`: an image that factory-resets on every boot (`bin/hubbo ota --wipe` only). |
| `scripts/drop_unused_idf_libs.py` | Drops the precompiled esp_matter archive from the link (it otherwise pulls in Matter and Wi-Fi: +24 KB RAM, +230 KB flash). |

Code ported from [Harmonizer](https://github.com/soaresbrun0/harmonizer) (AGPL-3.0) is marked at
the top of each file.

## Hardware

The W5500 is on SPI3 (SCLK 13, MISO 12, MOSI 11, CS 14, INT 10, RST 9, ESP-IDF driver). The
nRF24 is on the Arduino `SPI` (SPI2) with its own pins; the two never share a bus.

### nRF24

- SCK 15, MISO 16, MOSI 17, CSN 18, CE 38. IRQ isn't used: the radio is polled every loop pass.
- A plain module runs from 3V3 with a 10 µF capacitor across VCC/GND.
- A PA+LNA module (SMA antenna) draws too much for the 3V3 rail. Fit it on a socket adapter with
  its own 3.3 V regulator (AMS1117-3.3), adapter VCC to VSYS (pin 39). VBUS is live only on USB
  power. Never wire 3V3 or 5V straight to the module.

The radio power setting (low, medium, high or max, the default) applies live.

### IR emitter

GPIO 1 (header pin 25), fixed. Use an active-high emitter module whose input switches its LEDs
through a transistor, such as Adafruit's High Power IR LED Emitter (product 5639): signal to
GPIO 1, V+ to VSYS (pin 39; each pulse draws about 400 mA), GND to GND. A bare IR LED needs a
transistor and resistor.

The RMT peripheral makes the carrier (the code's Pronto frequency, 33% duty) and the marks and
spaces to the microsecond, on core 0. A hold's repeat frames go back to back until the release,
and the frame in flight always finishes. `ir test` on the console is a quick check.

### Power

Never connect USB and PoE at the same time.

## Tasks

| Task | Core | Priority | Runs |
|---|---|---|---|
| `loopTask` (Arduino) | 1 | 16 | RF + engine: radio FIFO → `RemoteInput` → `Engine` → adapters → NimBLE notify; `engine.poll()`; serial console. Never blocks. Task watchdog on (5 s). |
| `tiT` (lwIP) | 0 | 18 | Network. |
| `ir` | 0 | 20 | Builds each IR code's RMT symbols and queues its frames (about 50 µs a frame). Above lwIP and W5500 RX so a hold's frames have no gaps. |
| W5500 RX (`w5500_tsk`) | any (runs on 0) | 15 | Ethernet frames. Ethernet starts from a task pinned to core 0, so its interrupts are on core 0. |
| NimBLE host | 0 | framework | BLE stack. Callbacks only set flags. |
| `httpd` | 0 | 5 | Every API handler, `ConfigService` (save, compile, publish), the active-activity persister, NVS and LittleFS work. Reaches the engine only through `Engine::submit()`, the `SnapshotStore` and a status record the loop publishes every 250 ms. |
| `ha`, `mqtt`, `update` | 0 | 4 | Home Assistant, the MQTT bridge, online update checks and downloads. Below httpd: a TLS handshake is seconds of CPU. They never touch flash; store writes are posted to httpd. |
| `mqtt_task` (esp-mqtt) | any | framework | esp-mqtt's own client task. |
| `fw_write` | 0 | 3 | One-shot: writes a firmware image (see "Firmware updates"). |
| `esp_timer` | 0 | 22 | Once a second, queues httpd housekeeping when it's due. |

loopTask at 16 outranks every unpinned task, so those run on core 0, and core 1 has only the
RF/engine loop: no handler or network burst can delay a radio read. Flash writes are the
exception: every erase or write turns the cache off on both cores. A config save costs about
2 ms of loop gap; a firmware update's write lags the remote for its ~30 s. The `tasks` command
shows each task's core and CPU share.

`EngineRuntime::run` returns only once the engine has finished a request, because the request
lives on the handler's stack. The engine bounds the wait: transitions finish on the next poll,
executions by the 5 s action timeout.

## Memory

From the end of `setup()` every `malloc`/`new` tries PSRAM first
(`heap_caps_malloc_extmem_enable(0)`); the boot config parse runs under the same rule. Internal
RAM (about 100 KB) is kept for drivers that ask for internal or DMA memory and for lwIP, which
can't send large frames once it runs low. Every `mem:` line warns under 20 KB internal free.

## Storage

| Where | What |
|---|---|
| LittleFS `/fs` (`spiffs` partition, 0xC90000, 3.4 MB) | Core's record store (`docs/design/persistence-and-contracts.md`): config, active activity and hub state, each as two CRC'd slots. A save writes the older slot, fsyncs and reads it back before it's acknowledged; a load takes the newest intact slot. Written only on httpd. Unmounted means Unavailable: config is `storage_unavailable` and edits fail. |
| NVS `hubbo-radio` | The Hub endpoint: `channel` (u8), `address` (u64). |
| NVS `hubbo-fs` | `format_req`: `fs format` asked for a format at the next boot. |
| NVS `hubbo-secret` | `auth::SecretStore`: admin record, API tokens, the HA token. Plain NVS (no NVS encryption): a flash dump shows the password hash and token hashes, not the plaintext. |
| NVS `nimble_bond` | NimBLE's bonds, up to 5 (4 hosts plus a re-pair's new bond). A full store refuses, never evicts. |
| NVS `hubbo-ble` | Device id → host identity address, 4 records. At boot, entries without a bond are dropped and bonds no device owns are deleted. |

The partition table is `default_16MB.csv`: two 6.25 MB app slots (0x10000, 0x650000), the
`spiffs` data partition and a core dump partition.

## Network

DHCP on the W5500. Once it has an address the hub starts mDNS with the hostname that follows its
name: `hubbo-xxxx.local` for the default "Hubbo XXXX" (the Ethernet MAC's last two bytes;
`docs/design/hub-name.md`), advertising `_http._tcp`. Plain HTTP only.

## Firmware updates

**Online** (`docs/design/firmware-updates.md`): the `update` task reads the latest GitHub
release's `manifest.json`, checks `manifest.sig` (ECDSA P-256) against the built-in key, and
downloads `hubbo-<version>.bin`. Only plain `X.Y.Z` builds are offered updates.

**Install:** uploads and downloads both go through `FirmwareUpdate`. It claims the owner flag,
checks the whole image with core's `api::validateFirmwareImage()` (slot size, chip and revision,
the Hubbo marker's board and layout, the appended SHA-256), then `fw_write` writes it 4 KB at a
time, one sector erase per write, yielding between sectors. `esp_ota_end` checks it again from
flash, the boot slot is set and the hub restarts. A failure leaves the running image in place.

**Trial:** a new image boots pending verification. If within 2 minutes Ethernet is up, the config
has loaded and the web server answers, it's marked valid. Otherwise the hub restarts and the
bootloader goes back to the previous image, which then reports the version it rolled back from.
Updates are refused during a trial.

**ArduinoOTA** (`bin/hubbo ota`, images built with `HUBBO_OTA_PASSWORD`): only the password's
SHA-256 is compiled in, and it is the key ArduinoOTA's challenge uses, so treat such images and
flash dumps as sensitive. It runs inside `handle()` on the main loop; each chunk written feeds the
watchdog, a transfer gets 120 s in total, and a stalled one trips the watchdog. While another
update holds the owner flag, OTA packets are dropped.

## Deliberate reboots

`reboot`, `fs format`, restart, factory reset and a finished update are requests. The loop never
waits on another task: it releases held keys, stops reading the radio and posts "flush and
restart" to httpd, which writes out the debounced active activity, then restarts. Just before,
the loop line (`loop: max gap ...`) is kept in RTC memory and the next boot logs it as
`previous run (restart: <why>): ...`. After a task-watchdog reset, the next boot logs which task
each core was running.

## Board budget

What a dev board must hold to, as `GET /debug/stats` reports it. The limits live in
`firmware/budget.json`, which `bin/hubbo board-check <board ip>` reads; this table mirrors it.
"Now" is a dev-board measurement (2026-11-24, `_ota` build).

The check waits until 2 minutes after boot, then runs three windows (`?reset=1` ends each):

- **Idle:** 60 s with nothing going on.
- **Loaded:** 60 s with an IR hold and one nonstop HTTP client cycling `/api/v1/status`, `/` and
  `/api/v1/openapi.json`. The read→HID rules need presses of a bound button; without them
  they're skipped.
- **Flash:** two hub-settings saves (real LittleFS writes, no state change), then 15 s.

In budget.json each rule's `max`/`min` maps a window to its limit, tasks are addressed by name
(`tasks.loopTask.stack_free_min`), and `report` rules are printed without judging. "End" rows
read the last window's document. If the loop doesn't answer a close within 200 ms, `?reset=1`
answers 503, a failure in itself.

| Metric | Field | Windows | Budget (fail) | Now |
|---|---|---|---|---|
| Loop max gap | `loop.window.max_gap_us` | idle, loaded | ≤ 4 ms | 1.5 ms idle, 3.0 ms loaded |
| Loop iterations over 5 ms | `loop.window.over_5ms` | idle, loaded | 0 | 0 |
| Loop max gap / over 5 ms, flash | `loop.window` | flash | ≤ 12 ms / ≤ 3 | |
| read→HID p95 / p99 | `latency.read_to_hid` | loaded, when it has samples | < 2 ms / < 5 ms | |
| Queue overflows | `radio.queue_overflows` | end | 0 | 0 |
| IR late frames | `integrations.ir.late_frames` | loaded | 0 | 0 (186 frames) |
| IR frames sent (a hold ran) | `integrations.ir.frames` | loaded | ≥ 1 | 186 |
| IR failures | `integrations.ir.failures` | end | 0 | 0 |
| Internal heap free | `heap.internal.free` | end | ≥ 55 KB | 66.0 KB |
| Internal heap min | `heap.internal.min_free` | end | ≥ 50 KB | 63.7 KB |
| Internal heap largest block | `heap.internal.largest` | end | ≥ 24 KB | 31.7 KB |
| PSRAM free / largest | `heap.psram` | end | ≥ 5 MB / ≥ 4 MB | 7.33 MB / 7.21 MB |
| Image | `image_bytes` | end | ≤ 2.9 MB | 2,596,672 B |
| Static internal RAM | `static_ram_bytes` | end | ≤ 112 KB | 102,832 B |
| Loop stack (`loopTask`) | `tasks[].stack_free_min` | end | ≥ 2 KB | 2.9 KB |
| httpd stack | | end | ≥ 4 KB | 13.1 KB |
| esp-mqtt stack (`mqtt_task`) | | end | ≥ 1 KB | 1.7 KB |
| `ha` / `mqtt` / `update` stacks | | end | ≥ 2 KB | 4.1 / 6.1 / 3.3 KB |
| `ir` stack | | end | ≥ 1 KB | 2.3 KB (after a hold) |
| IDLE0 share | `tasks[].cpu_pct_x100` | idle, loaded | report only | 92% idle |
| BLE send failures | `integrations.ble.send_failures` | end | report only | 0 |

One nonstop client is about 36 requests a second, far above real use. Two or more nonstop
clients saturate core 0 (about 1–2 ms of CPU per Ethernet frame) and the task watchdog resets
the board: a known limit.

## Dev console over HTTP (dev builds)

The `_ota` env adds read-only plain-text GETs outside the API and its auth, for a board on PoE
with no serial attached:

| URL | |
|---|---|
| `/debug/log` | The last 8 KB of console output. |
| `/debug/run?cmd=status` | Queues one console command on the loop; read the result in `/debug/log`. Allowed: `status`, `mem`, `lat`, `lat reset`, `tasks`, `help`, and `ir ...` (test IR). 400 for anything else, 409 if one is already queued. |
| `/debug/stats` | The board budget's numbers as JSON. `?reset=1` closes the window and starts the next. |
| `/debug/coredump` | The last panic's core dump (raw partition). Decode it with esp-coredump against that build's `firmware.elf`. |
| `/debug/release?op=...` | Online updates against a local test feed: `op=test&base=<url>&running=<X.Y.Z>`, `op=check`, `op=install&version=X`, `op=crash` (restart with no flush, to try the rollback). |

## Serial console

115200 baud over the USB-C port (USB CDC). Type `help`. `status` also prints every 30 s.

| Command | |
|---|---|
| `status` | Radio, engine, Ethernet + HTTP, BLE, memory, latency. |
| `mem` | Memory, then filesystem usage (printed by httpd). |
| `tasks` | Each task's core, priority and CPU share. |
| `scan [seconds]` / `scan stop` | Learn a Harmony Hub's endpoint (default 120 s; press the Hub's pair button). Saved to NVS. |
| `radio set <channel> <address>` | Use a known endpoint instead: channel 0–124 (decimal), 40-bit address (hex, `0x` optional). |
| `radio forget` | Erase the saved endpoint. |
| `ble pair <device_id>` / `ble cancel <device_id>` | A 120 s pairing window for that Bluetooth device. Its old host stays until a new bond completes. |
| `ble forget <device_id>` | Delete that device's bond. |
| `raw on\|off` | Print raw payloads. |
| `ir test [frames]` | Send a NEC test code (LG TV volume up), 1 frame by default. |
| `ir hold [ms]` | The same code held (default 3000 ms). |
| `ir send <frames> <pronto>` | Send a raw Pronto (`0000`) code. A serial line holds 511 characters; over `/debug/run` the URL limits it to about 90 words. |
| `ir stop`, `ir` | End a hold / show whether the emitter is ready. |
| `lat`, `lat reset` | Latency summary / clear it. |
| `fs format` | Erase the config filesystem: records the request and reboots; the format runs at boot. |
| `wdt` | Hang the loop; the task watchdog reboots the board in about 5 s. |
| `reboot` | |

The log shows each remote event and what the engine did with it:

```
event 81234 up down
action 81234 ble dev_apple key_down -> accepted
event 81402 up up
action 81402 ble dev_apple key_up -> accepted
activity 90001 watch_tv
```

`timedOut` marks a release synthesized after 500 ms of radio silence. Lines are formatted into a
ring after the hot path and printed later.

### Latency

- `read->event`: from the payload leaving the nRF24 FIFO to the `Down`/`Up` event reaching the
  engine (decode cost).
- `read->hid`: same start, to the HID report being queued to NimBLE. Target p95 < 2 ms,
  p99 < 5 ms. Only reports caused directly by a radio payload count.
- `loop`: the longest loop pass and the count over 5 ms. A long pass delays the FIFO read; that
  delay isn't in `read->*`.

Not measured: air time from the remote, and the BLE link to the host (15 ms connection interval
requested).

## Boot log

What a healthy boot shows, in order:

- reset reason, 16 MB flash, about 8 MB PSRAM;
- `fs: blank partition, formatted` on the very first boot only;
- `radio: nRF24L01+ ready`, `ble: started, …`;
- `config: fresh` (first boot) or `config: ok, generation N`;
- `auth: fresh, …` (no admin yet: first-run setup open) or the saved state;
- `http: listening on port 80`, then `eth: up, IP …`;
- the `mem:` line.
