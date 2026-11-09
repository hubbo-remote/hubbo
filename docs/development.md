# Development

Everything builds inside Docker; nothing gets installed on the host. `bin/hubbo` is the only
entry point you need.

```sh
bin/hubbo image          # build the toolchain image (first time, or after changing it)
bin/hubbo test           # build the portable core natively and run its tests
bin/hubbo fw run         # cross-compile the ESP32 firmware (any `pio` args work)
bin/hubbo sim run sim/scenarios/*.hubbo   # run simulator scenario scripts
bin/hubbo sim serve [--data DIR] [--reset] [--seed FILE] [--auth]   # hub-sim HTTP on 127.0.0.1:8780
bin/hubbo web            # web admin dev server on :5180 (Vite + HMR, mock API)
bin/hubbo web check      # web typecheck + lint + tests (also: build, test, gen:api)
api/lint.sh              # validate OpenAPI spec, fixtures and role policy (`gen` regenerates fixtures)
bin/hubbo tsan           # native tests under ThreadSanitizer (one-off container, seccomp unconfined)
bin/hubbo asan           # native tests under AddressSanitizer + UBSan (same container setup)
bin/hubbo shell          # shell inside the toolchain
```

Each command runs in a fresh container, so it always sees your current environment. `docker
compose up` (the `dev` launcher's logs window) runs just the web dev server.

### Web admin

`bin/hubbo web` serves on :5180 (set `HUBBO_WEB_PORT`; 5173 is taken on this host). The API comes
from the TypeScript mock in `web/mock`, seeded from `api/fixtures`. Point `HUBBO_API_TARGET` at
`http://127.0.0.1:8780` for hub-sim, or at a board URL. Mock options: `HUBBO_MOCK_LATENCY` (ms),
`HUBBO_MOCK_FIRST_RUN=1` (setup wizard), `HUBBO_MOCK_AUTH=1` (require login). After any change to
`api/openapi.yaml`, run `bin/hubbo web gen:api`. `bin/hubbo web build` writes `web/dist` and prints
gzipped sizes.

### Simulator

`hub-sim` runs the real core with fake adapters and deterministic time. `sim serve` serves the
real `/api/v1` handlers. Without `--auth` it runs as AUTH DISABLED (simulator only); with `--auth`
it uses the production authenticator. It seeds from `api/fixtures/admin/backup.json`, the same
data the web mock uses (`--seed FILE` overrides). With `--data DIR` the config is saved there, so
restarting the sim acts as a reboot; `--reset` goes back to the seed.

Under `--data`, every saved record is a pair of slot files (`config.a/.b`, `active_activity.a/.b`,
`secrets.a/.b`), in the same format the board uses. See
`docs/design/persistence-and-contracts.md`. To test the Blocked state, write an intact but
unusable config and then start the sim without `--reset`:

```sh
bin/hubbo sim write-record --data build/sim-x --name config --file sim/fixtures/config.newer-schema.json
bin/hubbo sim serve --data build/sim-x
```

`sim/fixtures/` also has `config.does-not-compile.json`. `--reset` or `POST /sim/reset` leaves
Blocked. The scenario script syntax
is documented in `sim/src/scenario.h`, and each script in `sim/scenarios/` is also a ctest. The
no-auth bypass is compile-time and host-only: an ESP32 build that defines `HUBBO_SIMULATOR_NO_AUTH`
fails with `#error`.

### Firmware bring-up

See `firmware/README.md` for the serial console (`status`, `scan`, `pair`, `lat`, `raw`, ...), the
latency method and the hardware checklist. Network flashing:
`HUBBO_OTA_PASSWORD=... bin/hubbo fw run -e waveshare_esp32_s3_eth_ota -t upload --upload-port <dev board IP>`.
Never point it at the deployed Harmonizer boards.

## Toolchain

`docker/toolchain.Dockerfile`: Debian bookworm, CMake + Ninja + GoogleTest, and PlatformIO 6.1.18.
The container runs as your host uid/gid, so build output in the repo stays yours.

- PlatformIO packages live in the `platformio` named volume and persist between runs.
- The firmware is pinned to `espressif32@8.0.0`, which is arduino-esp32 3.3.12 on GCC 14.2. That's
  the same core the Harmonizer spike shipped on.
- The container uses host networking. Network (OTA) flashing needs this because espota makes the
  board connect back to the uploading machine.
- USB flashing from the container needs the board's serial device passed through. It's not set up
  yet. The first flash may be easier from a machine with the board attached, or later with the
  browser installer (Phase 6.2).

## Layout

| Path | What |
|---|---|
| `core/` | Portable C++17 core. **No Arduino, ESP-IDF or FreeRTOS headers.** Built natively by CMake and into the firmware as a PlatformIO library (`core/library.json`). |
| `core/tests/` | GoogleTest unit tests. |
| `firmware/` | PlatformIO project for the Waveshare ESP32-S3-ETH. Platform adapters live here. |
| `sim/` | `hub-sim`: native simulator, scenario scripts, fake adapters, loopback HTTP. Shared test fixtures. |
| `api/` | `openapi.yaml` (source of truth for `/api/v1`), generated `fixtures/`, `lint.sh`. |
| `web/` | React + TypeScript admin. The mock API in `web/mock` is seeded from `api/fixtures`. |
| `docker/` | Toolchain image and web image (`web.Dockerfile`). |
| `docs/` | Developer, security, agent-API and HA-capability docs. |

Watch for Arduino's macros: `bit`, `min`, `max`, `abs` and friends are `#define`s in `Arduino.h`.
Core code must not use those names. The ESP32 cross-build catches it; the native build doesn't.

## Reference material (local only, not committed)

- `Harmonizer/`: the working spike (RF, Ethernet, BLE HID to Apple TV, HA over MQTT). Read
  `Harmonizer/HANDOFF.md`. Its git repo is separate; don't commit it here.
- `logitech-harmony-ir-archive-main/`: the full Harmony device database (1.6 GB, CC0). `index.json`
  lists brands, `devices/<Brand>/<Model>.json` holds power on/off sequences, inputs and timing, and
  `codesets/xx/<id>.json` holds the commands (IR and BLE HID). Use it as ground truth for codes.
  Hubbo will eventually query a hosted service built from it.

## Remote input semantics

The remote sends "everything held" per button group, not press/release events. `RemoteInput`
turns that into `down` / `up` / `tap` / `hold` per button, and releases everything after radio
silence. The semantics are documented in `core/include/hubbo/remote/input.h`, and the wire format
in `packet.h`.
