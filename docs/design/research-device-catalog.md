# Research: Bluetooth hosts, the Harmony archive and a device catalog

Research from 2026-11-10, for discussion. Nothing here is decided yet. Sources: the firmware agent
(BLE HID), a scan of the whole Harmony archive (`logitech-harmony-ir-archive-main/`, CC0) and
hosting research. The archive data and the full working notes stay outside the repo.

## 1. Bluetooth hosts

**Our firmware can already send what every host needs.** The HID descriptor has a keyboard
(usages 0–0x65) and Consumer Control (0–0x3FF), and the Harmonizer spike ran it on an Apple TV.
New keys are only entries in core's key table, so no host needs re-pairing. Changing the descriptor
later, for example to add System Control power keys, would mean re-pairing every host. If we ever do
it, do it once, early.

**Sources ranked:** Logitech's own Bluetooth profiles in the archive, then what the spike ran on
hardware, then the Linux and Android mappings (from memory, unchecked).

| | Apple TV | Android TV / Shield | Fire TV |
|---|---|---|---|
| D-pad, Select | C:0x42–0x45, C:0x41 | same | same, or keyboard keys (Logitech used keypad Enter 0x58 for OK) |
| Back | C:0x223 (AC Home; tvOS treats it as Back) | C:0x224 AC Back | C:0x224 |
| Home | C:0x40 (hold for Control Center) | C:0x223 | C:0x223 (hold for the quick menu) |
| Play/Pause | C:0xCD | C:0xCD | C:0xCD |
| FF / Rew / Stop | C:0xB3 / 0xB4 / 0xB7 (untested) | same | same |
| Volume, Mute | C:0xE9/0xEA/0xE2, via the TV over CEC | same | same |
| Search / Assistant | none known | C:0x221 | none (Alexa can't be triggered) |
| Power on | Home, then about 6 s | any key, likely Home | Home |
| Power off | hold Home 2 s, wait 200 ms, Select | C:0x34 Sleep | C:0x34 Sleep |
| Confidence | high (Logitech plus the spike) | medium (a Logitech test device plus Android) | medium |

Missing from our key table today: FF, Rew, Stop, AC Back, Search, Exit, channel up/down, Info,
Sleep, keypad Enter, Backspace, Esc and a few F-keys.

Waking: every host wakes on a key from a bonded keyboard, but it has to reconnect first. So "on"
means press Home, then wait until the host is connected.

Board checks to add to `multi-bluetooth.md`: FF/Rew/Stop on the Apple TV, the Fire TV's OK as
Consumer Select or keyboard Enter, and Sleep 0x34 on the Shield.

## 2. What the Harmony archive gives us

- **Size:** 276,236 devices from 7,889 manufacturers, in 338,935 files. It's 1.28 GB with codesets
  shared (as stored) and 6.6 GB with them inlined. One inlined device averages 22 KB, or 1–3 KB
  gzipped.
- **Per device:**
  - the IR codes as Pronto strings (all valid raw `0000` format);
  - power (toggle on 64%, discrete on 23%, with on/off steps);
  - timing (power-on delay with a 1.5 s median, delay between keys, IR repeat count);
  - inputs;
  - how channel numbers are dialled.
- **There's no transport field.** Bluetooth shows only as the `HID 16 Bit` protocol, whose keycode
  holds the HID page and usage, which maps straight onto our `HidKey{page, usage}`. There are just 4
  such profiles: BT Apple TV 5 (good, Logitech's own), an Android TV box (a good template), a thin
  Fire TV test and a generic keyboard map. Logitech's main Bluetooth library isn't in the archive.
- **Placeholders:** the Shield, Fire TV sticks and Cube, Chromecast with Google TV, PS4, Mi Box and
  others have 3 fake commands (all one placeholder code). Their power and timing data are still
  real. About 7.8% of devices are empty or nearly empty.
- **Real IR exists for:** Apple TV 4K, Xbox One and Series X, older Rokus, PS2 and PS3.
  **IP-only:** newer Rokus and Sonos, which aren't usable from the archive.
- **Names:** command names are Logitech's vocabulary, and the top 100 cover 77% of all commands.
  The ambiguity is in meaning, not spelling: Exit vs Return vs Back, Menu vs Home vs Setup, Select vs
  OK vs Enter. We'd map remote buttons onto a small hub vocabulary with ordered fallbacks.
- **Search data:**
  - 1,583 "Zone 2" variants;
  - regional SKUs listed side by side;
  - about 450 duplicate groups once punctuation is stripped;
  - no Apple A-numbers (an alias table is needed).
  - The data also needs cleaning: some index entries have null models or leading spaces.
- **For IR later:** an ESP32 can send almost everything from the Pronto string alone. The catches:
  - 14% of commands use a toggle bit, all stored with the bit at 0. The catalog build could render
    the second version.
  - Some bursts are longer than the S3's transmit buffer, so the firmware needs chunked sending.

## 3. A device catalog

The idea: search "Apple TV 4K" and get one document saying how to drive it: IR codes, Bluetooth
key map, power on/off steps, timing and inputs. The hub stores a copy of each device it uses.

**Recommended shape:**
1. **Source:** a public `hubbo-catalog` repo, CC0 like the archive, kept apart from the AGPL
   firmware. It holds the schema, our hand-written documents (Bluetooth profiles for Apple TV,
   Android TV/Shield and Fire TV first) and a build script. The archive isn't committed; the build
   reads it from its release tarball.
2. **Build:** a GitHub Action, on each tag, cleans and merges the data, builds a search index, and
   uploads everything to Cloudflare R2.
3. **Serve:** a small Cloudflare Worker returns one merged document (device plus codeset) per
   request, with CORS `*`. That's $0 at hobby traffic, with no file-count limits. Other hosts don't
   fit: GitHub Pages caps a site at 1 GB, raw GitHub is rate-limited, and Pages/Netlify/Vercel cap
   file counts. S3 plus CloudFront also works, for cents a month.
4. **Search in the browser:**
   - a manufacturer list (about 60 KB gzipped) with typo-tolerant matching;
   - then that manufacturer's models, matched with case and punctuation ignored;
   - the whole index is about 2.2 MB gzipped, too big for the hub, but fine fetched in pieces;
   - there's no search server.
5. **The hub never talks to the cloud.** The web admin searches the catalog from the browser and
   posts the chosen document to the hub, which stores it. Nothing depends on the cloud at runtime.
   An http page fetching an https catalog is allowed, and Chrome's local-network prompts don't apply
   to that direction.
6. **Versioning:** each document carries `schemaVersion`, `id`, `source`, `revision` and a sha256.
   The hub keeps them, so the UI can show "update available". Signing is overkill at this scale.

**Precedents:** LIRC's `irdb-get` (an index, then fetch one file) is the same model. SmartIR,
Flipper-IRDB and ESPHome keep a git repo as the source of truth, take contributions by PR and
validate files in CI.

**Risks:**
- The hub's Content-Security-Policy (`connect-src 'self'`) blocks catalog fetches today. Firmware
  would allow the catalog's address, which then becomes part of the firmware.
- Vendor limits keep changing (GitHub, D1 and Netlify all did in 2025–26). Keep the build output a
  plain file tree that can move hosts.
- R2 has no hard spending cap, so set a billing alert.
- Search quality depends on cleaning the Harmony data.

## 4. Decisions so far (user, 2026-11-10)

1. **Bluetooth first.** Multi-host Bluetooth with built-in profiles for Apple TV, Android TV/Shield
   and Fire TV comes next. The catalog comes later.
2. **One shape for device information.** The built-in profiles use the same format the catalog will
   use later: key map, power steps and timing. That way catalog devices drop in without rework (the
   lead's call).
3. **Pre-filled buttons: open.** When an activity uses a device that has a profile, should the
   remote's buttons be filled in from that profile (Up sends the device's Up, Back its Back, and so
   on), leaving the user to change only what differs?
4. **Power steps are optional.** A profile carries its on/off steps (for example, Apple TV sleep:
   hold Home 2 s, then Select). The user can add them to an activity's startup or shutdown.
5. **Hosting: later.** It can carry community traffic: adding a device is a few requests; the free
   tier allows about 100k requests a day, and $5/month allows 10M a month.
6. **Contributions: yes, lightly.** Profiles live in hubbo-profiles. People can improve them, or add specific devices as complete profiles, with pull requests or feedback.
7. **A profile is a device.** The hub's device record mirrors the profile, and both use the hub's platforms (`ble_hid`, later `ir`). No inheritance and no aliases. The generic Bluetooth profiles (Apple TV, Android TV / Google TV, Fire TV, Other) are ordinary profiles, and the hub bundles copies.

## 5. Notes from discussion (2026-11-10)

- **Device profile spec:** there's no industry standard, so we write our own. It covers identity,
  commands with normalized names and how each is sent (Bluetooth key, IR, later network), power (on,
  off or toggle, with steps and delays), inputs and channel entry. The hub accepts only this format,
  and the catalog converts Harmony data into it.
- **Pre-filled buttons** work because commands have normalized names. Remote buttons use the same
  names, with fallbacks such as Back → Exit → Return.
- **Hosting through the cache:**
  - The build prebuilds every document and index piece as plain files in R2.
  - They're served through Cloudflare's cache on a custom domain, not through a Worker. Cache hits
    cost nothing and use no quota.
  - The r2.dev URL is for development only and isn't cached, so a domain on Cloudflare is needed.
