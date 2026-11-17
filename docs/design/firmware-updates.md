# Firmware updates: releases, and updating from the web admin

Approved by the user on 2026-11-16: the repo goes public, stable channel only, the dev board keeps
`bin/hubbo ota`.

## The idea

Anyone running Hubbo can update it from the web admin without building anything. A tagged commit
is built by CI into a signed release on GitHub. The hub checks for it, shows what's new, and
installs it when the admin says so. If the new firmware doesn't come up healthy, the board goes
back to the old one by itself.

Uploading a `.bin` by hand (`POST /admin/firmware/update`) stays, unchanged.

## What the user decided

- **The repo goes public**, so releases are plain GitHub Releases that anyone can download.
- **Stable only.** One channel: the latest release.
- **Nothing installs by itself.** The hub only checks and tells; the admin chooses Install.
- **The dev board is left alone.** It keeps `bin/hubbo ota` and development builds.

## Releases

- **A tag `vX.Y.Z` on `main` is a release.** A GitHub Actions workflow builds it with the same
  toolchain image as `bin/hubbo` (web admin, then firmware with `HUBBO_FW_VERSION=X.Y.Z`), and
  publishes a Release with three assets:
  - `hubbo-X.Y.Z.bin`: the app image, the same file the manual upload takes.
  - `manifest.json`: `{"version","board","layout","size","sha256","min_version"}`.
    `min_version` is the oldest version that can install it directly (normally absent). There are
    no notes in it: the web admin links to the release on GitHub.
  - `manifest.sig`: the manifest's signature.
- **The hub's URL is fixed:**
  `https://github.com/<owner>/hubbo/releases/latest/download/manifest.json` (and `.sig`). GitHub
  answers with a redirect to its download host; both are HTTPS with public certificates.
- **The release build has no secrets in it:** CI builds the non-OTA environment, so there's no OTA
  password to leak. The tag's annotation is the release notes. CI checks the image's marker
  carries the tag's version and verifies the signature against `firmware/release-key.pub.pem`
  before publishing.

## Signing

- **ECDSA P-256 over the manifest bytes.** mbedTLS on the board verifies it with no new library;
  CI signs with `openssl`. The manifest carries the image's sha256, so one signature covers both.
- **The public key is built into the firmware** from `firmware/release-key.pub.pem`. The private
  key lives in the `RELEASE_SIGNING_KEY` Actions secret and in `~/.config/hubbo/release-key.pem`
  (mode 600). Only the lead provisions it.
- A manifest whose signature doesn't verify is ignored and reported as `bad_signature`; nothing
  is downloaded.

## Checking

- **The hub checks** at boot (once the network is up, after a 1 minute delay) and every 24 hours
  of uptime, and when the admin presses Check now. A setting, "Check for updates", is on by
  default; off, only Check now checks.
- **What it compares:** the manifest's `board` and `layout` must match this board, and `version`
  must be newer than the running one by semver. A development build (anything not a plain
  `X.Y.Z`) never sees an update as newer, so the dev board is never prompted.
- **What it remembers:** the latest manifest, in RAM only, and when it was checked.
- **Where it runs:** the HA task's pattern, a low-priority task on core 0 that never blocks the RF
  loop. One HTTPS connection at a time, with a 10 s connect and 30 s total limit for the
  manifest.

## Installing

- **Install** downloads the image over HTTPS into PSRAM (the same buffer the upload path uses),
  checks its size and sha256 against the signed manifest, then hands it to the existing
  `FirmwareUpdate::install()`, which runs every check the manual upload runs and writes it. So
  there's one write path.
- **Progress:** `GET /admin/firmware` gains a `downloading` state before `writing`.
- **Refused** while an update or reboot is running, as now.

## Rollback

- **The new image boots on trial.** The firmware overrides Arduino's `verifyRollbackLater()` so
  the image isn't marked valid before `setup()`.
- **Healthy means:** Ethernet has an address, the config loaded (ok or blocked; a blocked config
  is the admin's to fix, not a bad image), and the web server answers its own request. Not HA or
  BLE: those depend on other devices.
- **Healthy within 2 minutes:** the firmware marks the image valid. Otherwise it reboots, and the
  bootloader goes back to the previous image. The watchdog firing during the trial also rolls
  back.
- **The web admin says so** after a rollback: `GET /admin/firmware` reports
  `rolled_back_from: "X.Y.Z"` until the next update.
- This applies to every update, manual uploads included.

**What the spike measured on the dev board:**
- The bootloader we build has rollback on. A new image boots on trial, and an unmarked image goes
  back after a restart or a watchdog reset.
- A board whose bootloader lacks rollback skips the trial quietly. The bootloader can't change by
  an OTA update.
- Updates are refused during the trial. `bin/hubbo ota` still works then.
- One HTTPS fetch takes about 2 KB of internal RAM (mbedTLS memory is in PSRAM). The radio loop
  stays under 3 ms.
- The image downloads at about 250 KB/s, so roughly 10 s.
- GitHub's latest-release URL redirects twice, to its download host, with a URL of about 900
  characters.
- Verifying the signature takes about 0.4 s of ECC. It runs on the update task, not the web
  server.
- The version being installed is saved before the reboot and compared at boot, which gives
  `rolled_back_from`.

## The API

- `GET /admin/firmware` gains:
  - `available`: `{version}` or null.
  - `checked_ago_ms` and `check_error` (`unreachable`, `bad_signature`, `bad_manifest` or null).
  - `rolled_back_from`.
  - the `downloading` state.
- `POST /admin/firmware/check` runs a check now: `202`, then poll.
- `POST /admin/firmware/install` with `{version}` installs that available version: `202`, or `409`
  if it's no longer the available one or an update is running.
- The "Check for updates" setting sits with the hub settings.

## The web admin

Shaped with the user on hub-sim:
- The card leads with the running version and the board, like an About panel.
- When a newer release is available, a block that stands out says "Hubbo 1.4.0 is available",
  links to What's new on GitHub (no notes or size in the card), and has Install. Install asks
  first, in plain words: the remote won't work for about a minute, and you'll sign in again.
- During an update, the same block shows Downloading, Installing and Restarting with progress.
  An update from a file uses the same block.
- One alert, only when there's something to say: a failure, a rollback ("The update to 1.5.0
  didn't start, so Hubbo went back to 1.4.0.") or "Updated to Hubbo 1.4.0".
- One group holds the "Check for updates daily" switch, Check now (its arrows spin while
  checking; no "checking" text) and Choose file, with "Up to date · checked 3h ago" under them.
  The status line never repeats what the update block shows.

## Before the repo goes public

The lead scans the whole history for secrets and for anything the rules keep out of the repo
(IPs, MACs, room and entity names, HA data), and the user decides what to do about any finding
before flipping it public.

## Owners

- **lead:** this doc, the release workflow in `.github/workflows/release.yml`, the release key,
  the history scan, ARCHITECTURE.md and IMPLEMENTATION_PLAN.md.
- **firmware:** the spike, the HTTPS client, the check task, download into PSRAM, the signature
  check (mbedTLS), rollback and the health mark.
- **core:** the manifest parser, semver compare and the update-check state, host-tested; hub-sim
  serving a fake release feed so the flow runs end to end without GitHub.
- **api:** the routes and fields, the setting, spec, fixtures, contract, docs/agent-api.md and
  docs/security.md (the signing key, what a LAN attacker can and can't do).
- **frontend:** the Firmware card, the mock, e2e.
