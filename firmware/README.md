# Hubbo firmware

PlatformIO project for the Waveshare ESP32-S3-ETH. For now it is **bring-up firmware**: it
receives the Harmony Companion remote, runs the portable core's `RemoteInput`, brings up
Ethernet and the BLE HID keyboard, and logs events, status and latency over USB serial.
The activity engine replaces `src/main.cpp` later.

```sh
bin/hubbo fw run                     # build (env waveshare_esp32_s3_eth)
bin/hubbo fw run -e waveshare_esp32_s3_eth_ota -t upload --upload-port <ip>   # network upload
```

Pinned: `espressif32@8.0.0` (arduino-esp32 3.3.12), `nRF24/RF24@1.6.2`,
`h2zero/NimBLE-Arduino@2.5.1`. These are the versions the Harmonizer spike ran on.

## Layout

| File | What |
|---|---|
| `src/board.h` | Board profile: pins and SPI hosts for the Waveshare ESP32-S3-ETH. The only board. |
| `src/remote_radio.*` | nRF24: Hub scan (learns channel + address), listener, endpoint in NVS (`hubbo-radio`). Payloads go to a `PayloadSink` unchanged. |
| `src/ethernet_link.*` | W5500 Ethernet with DHCP, non-blocking, logs link changes. |
| `src/ble_hid.*` | NimBLE HID keyboard + consumer control: link, pairing window, forget, state, and `sendKeyboard`/`sendConsumer` reports. No key state; it is shaped to back core's `HidTransport`, and core's `BleHidAdapter` tracks held keys. |
| `src/latency.*` | Fixed-size latency recorder (p50/p95/p99/max). |
| `src/ota.*` | ArduinoOTA, compiled in only with `HUBBO_OTA_PASSWORD`. |
| `src/main.cpp` | Temporary bring-up main and serial console. |

The W5500 is on SPI3 (ESP-IDF driver); the nRF24 is on the Arduino `SPI` (FSPI/SPI2) with
its own pins. They never share a bus. The nRF24's IRQ isn't wired, so the radio is polled
every loop iteration.

Code ported from [Harmonizer](https://github.com/soaresbrun0/harmonizer) (AGPL-3.0) is marked
at the top of each file.

## OTA

Set `HUBBO_OTA_PASSWORD` in the environment **at build time and at upload time**. Unset or
empty compiles OTA out entirely. The password is baked into the image; it's never printed.
It's passed as a `-D` string, so avoid `"` and `\` in it. Hostname is `hubbo-<efuse MAC>`.
OTA starts once Ethernet has an address.

## Serial console

115200 baud over USB CDC. Type `help`. Status prints every 30 s.

| Command | |
|---|---|
| `status`, `mem` | Radio / Ethernet / BLE state, heap and PSRAM, latency summary. |
| `scan [seconds]` / `scan stop` | Learn a Harmony Hub's address (default 120 s). Saved to NVS on success. |
| `radio forget` | Erase the saved endpoint. |
| `pair [seconds]` / `pair stop` | BLE pairing window (default 120 s). Forgets the current host first. |
| `ble forget`, `ble release` | Delete bonds; release all keys. |
| `ble tap <key>` | Tap a key from core's verified HID key table for 100 ms, e.g. `ble tap SELECT`. |
| `raw on\|off` | Print raw payloads. |
| `lat`, `lat reset` | Latency summary / clear it. |
| `wdt` | Hang the loop; the task watchdog should reboot the board in ~5 s. |
| `reboot` | |

Event lines look like `event 81234 up down (42 us)`: ms timestamp, button, kind, `timedOut` if the
release was synthesized after 500 ms of silence, and FIFO-read-to-event latency. Events from
`poll()` (hold, timed-out release) have no latency figure.

### Latency measurement (Phase 0.3)

- `read->event`: `micros()` right after the payload leaves the nRF24 FIFO, to the `Down`/`Up`
  event reaching the sink. Pure decode cost. RF-to-HID latency gets measured once the engine
  drives BLE.
- `loop`: longest main-loop iteration and count over 5 ms. The radio is polled, so a long
  iteration delays the FIFO read; that delay is **not** in `read->event`.

Not measured here: air time from remote to radio, and BLE link to the host. Events are buffered
and printed after the hot path, so serial output doesn't inflate the numbers (`raw on` does).
