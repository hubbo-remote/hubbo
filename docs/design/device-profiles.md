# Device profiles and the activity wizard

Approved 2026-11-10. Built and board-tested 2026-11-11 (see "Board test results").

**The goal:**
- **Adding a device is picking a profile** ("Apple TV", "Android TV / Google TV", "Fire TV", and later anything from the catalog).
- **The new-activity wizard builds a starter activity** from the devices you pick: the remote's buttons, the inputs and the power steps. You review it, save it, and then change anything.

The spec and the profiles live in `~/src/hubbo-profiles` (`docs/spec.md`), owned by the spec agent. Research: `research-device-catalog.md`.

## 1. A profile is a device

**What a device holds.** A device on the hub is a profile that's been added: manufacturer, model, kind, platform, commands, power, timing and inputs. It also has its local fields:
- id;
- name;
- integration;
- for Bluetooth, its pairing;
- `profile_id`, `revision` and `sha256` from the profile.

The hub's device record and API field names mirror the profile's (snake_case), so adding a device is close to a copy.

**What's kept and dropped.** The profile's `meta` is dropped. Every command is kept, so any command can be bound later without fetching the profile again.

**Size.** A Bluetooth profile stores at 1–4 KB. IR comes later, in its own phase, which decides how IR codes are stored.

**Home Assistant devices don't change.** They have no profile. Their commands still come from what HA reports (`ha_capabilities`), as today.

**Kinds** mirror Harmony's device types, about 30 of them. The spec agent makes the final list from the archive.
- A kind is only a label, an icon and a browse category.
- An HA device's kind defaults from its HA domain, and you can change it.

**No compatibility:** existing Bluetooth devices have no profile. After this update, the dev board's config is reset and its hosts are paired again.

## 2. Bluetooth commands come from the profile

**What changes.** Today a Bluetooth action is `key_tap` / `key_down` / `key_up` with a `key` from one fixed table in core (`HUBBO_BLE_HID_KEYS`). A Bluetooth host is a keyboard, so any key can be useful: `R` to record or `I` for Info in the Channels app on Apple TV, for example. Hubbo never limits a Bluetooth device to its remote's buttons (user, 2026-11-10). That was one of Harmony's weak spots. After this phase, a Bluetooth device's key picker has two groups:
- **Remote commands:** the device's own commands, from its profile, by name. For example, `Back` on an Apple TV is Consumer 0x223, and on a Shield it's 0x224. These are listed first, and they're what the wizard fills in.
- **All keys:** every keyboard key (0x04–0x65: letters, digits, F-keys, arrows, Enter, Esc and so on) and every named consumer key, on every Bluetooth device, whatever its profile. This is today's key table, made complete.
- **A custom code:** for anything without a name, a raw consumer usage (0x0001–0x03FF), as an advanced option.

"Other" is a profile whose remote commands are empty or minimal. Its devices use All keys.

**What the hub can send.** It sends any page and usage the profile gives, within the HID descriptor:
- keyboard 0x04–0x65;
- consumer 0x0001–0x03FF.

The descriptor doesn't change, so no host needs pairing again for this.

**Holds.** A profile command with `hold_ms` (for example, the Apple TV's Control Center) is a tap that holds for that long. Press-and-hold buttons keep working as today, through `key_down` and `key_up`.

**Core decides the exact action shape.** The direction: a command names what to press on this device, and the remote's held-key handling stays as it is.

## 3. Built-in profiles

**What's bundled.** The hub ships copies of the generic profiles from hubbo-profiles: Apple TV, Android TV / Google TV, Fire TV and Other.
- They're kept in the hubbo repo as the same files, under `profiles/`, refreshed by a small sync script.
- They're built into the firmware and hub-sim.
- The web admin lists them through the API.

**When the catalog site exists**, it adds search results to the same list. Adding a device looks the same either way (user, 2026-11-10).

## 4. Adding a device

1. Devices → Add device shows one list with search:
   - the built-in profiles;
   - "From Home Assistant", today's flow, unchanged;
   - later, the catalog.
2. Pick one, then name it. The default name is the profile's model.
3. **What happens next depends only on the platform.** Bluetooth goes straight into the pairing card. IR (later) is just added. Profiles carry no pairing steps.

The limit of 4 Bluetooth devices still applies.

## 5. The new-activity wizard

A step-by-step wizard. It ends on a review page, and saving creates an ordinary activity. The answers aren't stored anywhere; they only build the starter.

1. **Name and devices.** Name the activity and pick its devices, for example LG TV (HA), Denon (HA) and Apple TV.
2. **Watch:** "What are you watching on?" The chosen device gets navigation and playback:
   - the d-pad, Select, Menu, Back, Exit, Info and Guide;
   - play, pause, stop, record, skip;
   - the colours and the number pad;
   - channels.
3. **Volume:** "What controls the volume?" Volume up, volume down and mute go to that device.
4. **Inputs.** For each device that has inputs: "Which input should the LG TV use?" Pick one, or Don't switch. Devices without inputs are skipped. Inputs come from:
   - an HA device's source list (`select_source`), which works today;
   - a profile's `inputs` (IR, later).
5. **Power.** For each device: "Turn on when the activity starts" and "Turn off when it ends". Both are on by default. Devices without power commands are left out.
6. **Review.** One page:
   - each device with its role (watch, volume), input and power;
   - the remote's buttons that will be filled in;
   - the startup and shutdown steps.

   Back goes to any step. Save creates the activity.

**After saving,** the activity opens in the normal editor. Every button and step can be changed, using any device, including ones you didn't pick.

### How buttons are filled

**Button matching:**
- For each button in a role, take the first candidate command the device has, using the spec's button table (spec section 8, "Remote buttons").
- A profile's `buttons` overrides the table. For example, streamers map Menu to Home.
- A button with no match stays empty.

**Where the button table lives.** It moves from prose in the spec into a data file in hubbo-profiles (`buttons.json`). It's bundled like the profiles, so the hub and the catalog read the same table.

**HA devices:** a small hub-side table maps HA commands to the same names (`media_player` volume up → `VolumeUp`, and so on). It's kept with the web admin, next to the wizard.

**Press-and-hold:** buttons that repeat while held, like the d-pad and volume, use `key_down` and `key_up` on Bluetooth devices. The rest are taps.

### How startup and shutdown are built

**Startup, for each device, in the order picked (HA devices first, then Bluetooth):**
- if it's turned on: its power-on steps, then a wait of its `on_delay_ms`;
- if it has an input: the input, then a wait of its `input_delay_ms`. A device switches input even if it isn't turned on.
- a wait of `key_delay_ms` between two steps to the same device.

For HA devices, power on is `turn_on` and the input is `select_source`. An HA device that's turned on and switches input gets a 3 s wait between the two, since TVs often ignore an input change while waking. HA mute fills as press = mute, hold = unmute.

**Shutdown:** the power-off steps of each device turned off.

**Switching between activities** keeps today's rules: going from A to B runs only B's startup, never A's shutdown (ARCHITECTURE.md). So power-on steps must be safe to repeat:
- **Discrete on** (`PowerOn`) is always safe.
- **Toggle-only devices** get no automatic power step. The review page says so, and you can add one by hand.

**Bluetooth streamers:**
- **Power on** means waiting until the host is connected (up to 15 s), then pressing its wake command. For example, Home on Android TV and Fire TV.
- **Power off** is the profile's off steps. For example, Sleep (0x34) on Android TV and Fire TV.
- **Apple TV has no power steps** (user, board test 2026-11-11). It wakes on any key and sleeps on its own timer. The only way to sleep it over Bluetooth is to hold Home, then pick Sleep in Control Center. That depends on Control Center's layout and timing: Logitech's sequence already fails on current tvOS, and the working one needed an extra Down. Any tvOS update could break it again.
- **A new action** lets a startup wait for a Bluetooth device to connect. Core designs it. The startup doesn't block the remote while it waits.

## 6. The spec, hardened by building it

**The spec agent is part of this phase.** The spec isn't final until the hub uses it. What the hub needs is fixed in the spec and the profiles during the phase:
- field names;
- the button table;
- power and input steps;
- the wait-for-connected rule.

**At the end of the phase:**
- hubbo-profiles is committed with everything else;
- the hub's copy of the profiles is synced from it;
- `bin/validate` must pass.

## 7. Firmware

- **Sending:** send any page and usage within the descriptor, not only the old fixed keys.
- **A log line** for each host's connect, encryption result and disconnect, with the time. This is the open item from the Bluetooth phase.

## 8. hub-sim and the mock

- hub-sim lists the built-in profiles and adds devices from them.
- The mock mirrors it.
- **e2e covers:**
  - adding each built-in profile;
  - the whole wizard, with an HA TV, an HA receiver and a Bluetooth streamer;
  - the review page;
  - the saved activity's buttons and steps.

## Ownership

| Agent | Work |
|---|---|
| Spec | `buttons.json`; the final kind list; spec fixes found while building; keeping the profiles valid |
| Core | Device records that carry their profile; Bluetooth commands from the profile; the wait-for-connected action; built-in profiles in hub-sim; kinds |
| API | Listing the profiles, adding a device from one, the device fields, spec, fixtures, contract, docs |
| Firmware | Sending any usage in the descriptor; built-in profiles in the firmware; the link log line |
| Frontend | The add-device list; the wizard with its review page; button filling and the HA name table; mock and e2e |
| Lead | The profile sync script; this doc; relaying spec changes |

## Gate

- The four area gates and `bin/validate` in hubbo-profiles.
- **Board test:**
  1. Add the Apple TV and the Fire TV from their profiles and pair them.
  2. Build "Watch Apple TV" with the wizard (HA TV and receiver plus the Apple TV) and run it.
  3. Check the inputs, the power steps and the buttons.
  4. Check a streamer's power on and off.
  5. Read the link log after a reboot.

## To check on the board

- **Home as a streamer's power-on (Fire TV, Android TV).** It never turns the streamer off, but switching between two activities that use the same streamer presses Home again, which leaves the open app. Watch whether that bothers anyone. A wake key with no visible effect would fix it.
- **The Fire TV's late encryption after a boot.** The link log shows it isn't slow encryption. After the final update, the Fire TV connected at 2.08 s but never started encryption. The link hit its supervision timeout (reason 0x208) at 45.08 s, and the Fire TV reconnected and encrypted at 45.2 s. On an earlier boot it encrypted at 1.7 s, so it doesn't happen every time. The next Bluetooth item for the firmware: if a paired host stays connected but unencrypted for a few seconds, the board starts encryption itself, and failing that drops the link so the host reconnects at once.

## Board test results (2026-11-11)

- **Every Apple TV key arrived.** Arrows went as down/up, and Back, Select, Info and Record as taps. From a remote press to the Bluetooth send took 0.5 ms (median).
- **Menu has to be a held button.** As a tap, holding it never reached the Apple TV. As down/up, holding it opens Control Center. `buttons.json` now marks `menu` as held.
- **The Apple TV has no power steps.** See "Bluetooth streamers" above.
- **The startup waited until connected, then pressed Home.** The Apple TV was already connected, so the wait was instant.
- **Fire TV:** paired alongside the Apple TV. Its keys reached only the Fire TV, and Off sent its Sleep steps.
- **Final build:** both hosts came back after the reboot, and the config loaded with 2 activities and 2 devices.
