# Firmware updates: releases, and updating from the web admin

Anyone running Hubbo can update it from the web admin without building anything. A tagged commit
is built by CI into a signed release on GitHub. The hub checks for it, shows what's new, and
installs it when the admin says so. If the new firmware doesn't come up healthy, the board goes
back to the old one by itself.

Uploading a `.bin` by hand (`POST /admin/firmware/update`) works too, through the same install path.
The release steps themselves are in [development.md](../development.md#releasing).

## Rules

- **Releases are plain GitHub Releases** that anyone can download.
- **Stable only.** One channel: the latest release.
- **Nothing installs by itself.** The hub only checks and tells; the admin chooses Install.
- **Development builds are never offered an update**, so a dev board keeps `bin/hubbo ota`.

## Releases

- **A tag `vX.Y.Z` on `main` is a release.** A GitHub Actions workflow
  (`.github/workflows/release.yml`) builds it with the same toolchain image as `bin/hubbo` (web
  admin, then firmware with `HUBBO_FW_VERSION=X.Y.Z`), and publishes a Release with three assets:
  - `hubbo-X.Y.Z.bin`: the app image, the same file the manual upload takes.
  - `manifest.json`: `{"version","board","layout","size","sha256","min_version"}`.
    `min_version` is the oldest version that can install it directly (normally absent). There are
    no notes in it: the web admin links to the release on GitHub.
  - `manifest.sig`: the manifest's signature.
- **The hub's URL is fixed:**
  `https://github.com/hubbo-remote/hubbo/releases/latest/download/manifest.json` (and `.sig`).
  GitHub answers with a redirect to its download host; both are HTTPS with public certificates.
- **The release build has no secrets in it:** CI builds the non-OTA environment, so there's no OTA
  password to leak. The tag's annotation is the release notes. CI checks the image's marker
  carries the tag's version and verifies the signature against `firmware/release-key.pub.pem`
  before publishing.

## Signing

- **ECDSA P-256 over the manifest bytes.** mbedTLS on the board verifies it with no new library;
  CI signs with `openssl`. The manifest carries the image's sha256, so one signature covers both.
- **The public key is built into the firmware** from `firmware/release-key.pub.pem`. The private
  key lives in the `RELEASE_SIGNING_KEY` Actions secret, with a copy kept off the repo.
- A manifest whose signature doesn't verify is ignored and reported as `bad_signature`; nothing
  is downloaded.

## Checking

- **The hub checks** at boot (once the network is up, after a 1 minute delay) and every 24 hours
  of uptime, and when the admin presses Check now. A setting, "Check for updates", is on by
  default; off, only Check now checks.
- **What it compares:** the manifest's `board` and `layout` must match this board, and `version`
  must be newer than the running one by semver. A development build (anything not a plain
  `X.Y.Z`) never sees an update as newer.
- **What it remembers:** the latest manifest, in RAM only, and when it was checked.
- **Where it runs:** a low-priority task on core 0 that never blocks the RF loop. One HTTPS
  connection at a time, with a 10 s connect and 30 s total limit for the manifest.

## Installing

- **Install** downloads the image over HTTPS into PSRAM (the same buffer the upload path uses),
  checks its size and sha256 against the signed manifest, then hands it to
  `FirmwareUpdate::install()`, which runs every check the manual upload runs and writes it. So
  there's one write path.
- **Progress:** `GET /admin/firmware` has a `downloading` state before `writing`.
- **Refused** while an update or reboot is running.

## Rollback

- **The new image boots on trial.** The firmware overrides Arduino's `verifyRollbackLater()` so
  the image isn't marked valid before `setup()`.
- **Healthy means:** Ethernet has an address, the config loaded (ok or blocked; a blocked config
  is the admin's to fix, not a bad image), and the web server answers its own request. Not HA or
  Bluetooth: those depend on other devices.
- **Healthy within 2 minutes:** the firmware marks the image valid. Otherwise it reboots, and the
  bootloader goes back to the previous image. The watchdog firing during the trial also rolls
  back.
- **The web admin says so** after a rollback: `GET /admin/firmware` reports
  `rolled_back_from: "X.Y.Z"` until the next update. The version being installed is saved before
  the reboot and compared at boot.
- This applies to every update, manual uploads included. Updates are refused during the trial.
- A board whose bootloader lacks rollback skips the trial quietly, and the bootloader can't change
  by an OTA update. The bootloader Hubbo builds has rollback on.

**Measured on the board:** one HTTPS fetch takes about 2 KB of internal RAM (mbedTLS memory is in
PSRAM) and the radio loop stays under 3 ms. The image downloads at about 250 KB/s, so roughly 10 s.
Verifying the signature takes about 0.4 s of ECC, on the update task, not the web server.

## The API

- `GET /admin/firmware`: `available` (`{version}` or null), `checked_ago_ms`, `check_error`
  (`unreachable`, `bad_signature`, `bad_manifest` or null), `rolled_back_from`, and the state.
- `POST /admin/firmware/check` runs a check now: `202`, then poll.
- `POST /admin/firmware/install` with `{version}` installs that available version: `202`, or `409`
  if it's no longer the available one or an update is running.
- The "Check for updates" setting sits with the hub settings (`/admin/hub`).

## The web admin

- The card leads with the running version and the board, like an About panel.
- When a newer release is available, a block that stands out says "Hubbo 1.4.0 is available",
  links to What's new on GitHub, and has Install. Install asks first, in plain words: the remote
  won't work for about a minute, and you'll sign in again.
- During an update, the same block shows Downloading, Installing and Restarting with progress.
  An update from a file uses the same block.
- One alert, only when there's something to say: a failure, a rollback ("The update to 1.5.0
  didn't start, so Hubbo went back to 1.4.0.") or "Updated to Hubbo 1.4.0".
- One group holds the "Check for updates daily" switch, Check now and Choose file, with
  "Up to date · checked 3h ago" under them.
