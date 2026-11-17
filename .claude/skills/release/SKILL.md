---
name: release
description: Cut a Hubbo firmware release (tag vX.Y.Z on main, watch CI, verify the published image and signature, optionally install it on the dev board). Use when the user says "release 0.x.y" or asks to ship a version.
---

# Release Hubbo X.Y.Z

How releases work is in `docs/development.md` (Releasing) and `docs/design/firmware-updates.md`.
This is the checklist. Stop and tell the user at the first failed check; never work around one.

The argument is the version (`0.2.0` or `v0.2.0`). If none was given, ask.

## 1. Before tagging

- The version is plain `X.Y.Z` and newer than the latest release
  (`gh release view --json tagName -q .tagName`).
- `main` is checked out, clean, and equal to `origin/main` (`git fetch` first).
- The dev board has run this commit: ask the user if it isn't clear from the session.
- Notes: draft them from the commits since the last tag (`git log <last>..main --oneline`), in
  plain user-facing language (features and fixes people notice, no internals), and show them to
  the user. Tag only once they approve the notes.

## 2. Tag and push

```sh
git tag -a vX.Y.Z -F <notes file in the scratchpad>
git push origin vX.Y.Z
```

The user asking for the release is the request to push the tag.

## 3. Watch CI

Find the run (`gh run list --workflow release --limit 1`) and wait on
`gh run watch <id> --exit-status` in the background. On failure, show the failing step's log.

## 4. Verify what was published

- From the run log (`gh run view <id> --log`):
  - `framework-arduinoespressif32@<version>` matches the pin in `firmware/platformio.ini`.
  - The `RAM:` and `Flash:` lines equal a local `HUBBO_FW_VERSION=X.Y.Z bin/hubbo fw run -e
    waveshare_esp32_s3_eth`. Run that build if there isn't one for this commit. A difference
    means something isn't pinned: stop.
- Download `manifest.json` and `manifest.sig` anonymously from
  `https://github.com/<owner>/hubbo/releases/latest/download/` into a new scratchpad directory:
  - `version` is X.Y.Z, and `openssl dgst -sha256 -verify firmware/release-key.pub.pem
    -signature manifest.sig manifest.json` says `Verified OK`.
  - The image's size and sha256 match the manifest.

## 5. Optionally, install it on the dev board

Only if the user wants it. Hand it to hubbo-firmware: make the dev board treat the release as newer
(the dev console's release test, `running=0.0.1`), check, install, watch the trial pass, then put
the dev build back by web upload of the OTA build. Never touch any other board.

## 6. Report

One short message: the release link, that the image matches the local build and the signature
verifies, and the board result if step 5 ran.

## A bad release

If nothing can have installed it yet: `gh release delete vX.Y.Z --cleanup-tag`, `git tag -d
vX.Y.Z`, fix, and release the same version again. Otherwise ship a newer version.
