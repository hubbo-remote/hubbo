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
latency method and the hardware checklist. Network flashing: `bin/hubbo ota <board ip>` builds the
dev firmware and uploads it, reading the OTA password from `~/.config/hubbo/ota-password` (mode 600;
it is never printed). Point it only at your own dev board, never at a hub in daily use. Firmware builds (`fw`, `ota`) hold a lock, so only one runs at a time.

## First install (USB)

A new board gets Hubbo over USB once. After that it updates itself from releases, or from an image
uploaded in Hubbo. An update over the network can't write the bootloader or the partition table, so
the first install is always a full flash.

1. **Get the install image.** Download `hubbo-install.bin` from the
   [latest release](https://github.com/hubbo-remote/hubbo/releases/latest). It's one file for
   address 0x0: the bootloader, the partition table and that release's firmware.
2. **Flash it** from a machine with the board on USB (`bin/hubbo` can't reach USB from its
   container yet). Unplug PoE first: never connect USB and PoE together. Then either:
   - Chrome with <https://espressif.github.io/esptool-js/>: connect, Erase Flash, then program
     `hubbo-install.bin` at 0x0.
   - esptool: `esptool --chip esp32s3 erase-flash`, then
     `esptool --chip esp32s3 write-flash 0x0 hubbo-install.bin`.

   Erase first: Hubbo formats its storage only when the storage partition is blank. If the board
   won't connect, hold BOOT, tap RESET, release BOOT, and try again.
3. **First boot.** Press RESET, unplug USB, and connect Ethernet (or PoE). The hub is named
   "Hubbo XXXX" from the end of its Ethernet MAC. Open `http://hubbo-xxxx.local/` or its IP, create
   your username and password, then pair your remote.

**Building the install image from source.** Check out the release tag, build it with that version
(a build with any other version is never offered updates), then merge it into one file the way the
release workflow does:

```sh
git checkout vX.Y.Z
HUBBO_FW_VERSION=X.Y.Z bin/hubbo fw run -e waveshare_esp32_s3_eth
bin/hubbo run bash -c 'B=firmware/.pio/build/waveshare_esp32_s3_eth; mkdir -p build;
  /platformio/packages/tool-esptool/esptool --chip esp32s3 merge-bin -o build/hubbo-install.bin \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 $B/bootloader.bin 0x8000 $B/partitions.bin \
  0xe000 /platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 $B/firmware.bin'
```

Then flash `build/hubbo-install.bin` as in step 2.

## Releasing

A tag `vX.Y.Z` on `main` is a release. `.github/workflows/release.yml` builds it and publishes a
GitHub Release, and every hub offers it at its next check (`docs/design/firmware-updates.md`).

1. **Test first.** Flash the commit you mean to ship to the dev board and use it.
2. **Tag `main`.** The tag's message is the release notes on GitHub:
   ```sh
   git tag -a v0.2.0 -F notes.txt   # or -m "…"
   git push origin v0.2.0
   ```
3. **CI builds and publishes it** (about 8 minutes). It builds the web admin and the firmware
   with `HUBBO_FW_VERSION=0.2.0`, checks the image's marker carries that version, writes
   `manifest.json`, signs it, verifies the signature against `firmware/release-key.pub.pem`, then
   publishes `hubbo-0.2.0.bin`, `manifest.json` and `manifest.sig`.
4. **Check it.** The build log's RAM and Flash numbers should equal a local
   `HUBBO_FW_VERSION=0.2.0 bin/hubbo fw run -e waveshare_esp32_s3_eth`. If they differ, a package
   isn't pinned: every PlatformIO package in `firmware/platformio.ini` is pinned exactly, because a
   fresh install otherwise picks newer patches that change the build.

Things to know:
- **Versions:** plain `X.Y.Z`. Any other version, like a local build's `v0.1.0-3-gabc123-dev`, is
  a development build and is never offered an update. Bump the last number for fixes, the middle
  one for features.
- **One channel:** every release becomes the latest, and hubs only see the latest.
- **A bad release:** delete it with `gh release delete vX.Y.Z --cleanup-tag`, delete the local
  tag, and tag again. Once a hub may have installed it, ship a newer version instead.
- **The signing key** is ECDSA P-256. The private key is the `RELEASE_SIGNING_KEY` Actions secret,
  with a copy kept off the repo (mode 600). Hubs only accept manifests signed by it, so losing
  both copies means existing hubs can't take updates; back it up.

## Toolchain

`docker/toolchain.Dockerfile`: Debian bookworm, CMake + Ninja + GoogleTest, and PlatformIO 6.1.18.
The container runs as your host uid/gid, so build output in the repo stays yours.

- PlatformIO packages live in the `platformio` named volume and persist between runs.
- The firmware is pinned to `espressif32@8.0.0`, which is arduino-esp32 3.3.12 on GCC 14.2. That's
  the same core the Harmonizer spike shipped on. Its packages are pinned to exact versions too, so
  CI and a fresh volume build the same image (see Releasing).
- The container uses host networking. Network (OTA) flashing needs this because espota makes the
  board connect back to the uploading machine.
- USB flashing from the container needs the board's serial device passed through. It's not set up
  yet; see First install for flashing from a machine with the board attached.

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
| `docs/` | Developer, API, security and HA-capability docs; `docs/design/` holds the design notes. |

Watch for Arduino's macros: `bit`, `min`, `max`, `abs` and friends are `#define`s in `Arduino.h`.
Core code must not use those names. The ESP32 cross-build catches it; the native build doesn't.

## Reference material

- [soaresbrun0/Harmonizer](https://github.com/soaresbrun0/harmonizer): the RF protocol work Hubbo
  builds on.
- The Logitech Harmony device archive (CC0): the source of the IR and Bluetooth codes in the
  [device catalog](https://hubbo.tv/devices/), converted in
  [hubbo-profiles](https://github.com/hubbo-remote/hubbo-profiles).

## Remote input semantics

The remote sends "everything held" per button group, not press/release events. `RemoteInput`
turns that into `down` / `up` / `tap` / `hold` per button, and releases everything after radio
silence. The semantics are documented in `core/include/hubbo/remote/input.h`, and the wire format
in `packet.h`.
