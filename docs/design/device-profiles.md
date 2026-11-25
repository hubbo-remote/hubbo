# Device profiles and the activity wizard

- **Adding a device is picking a profile**: a built-in one ("Apple TV", "Android TV / Google TV",
  "Fire TV", "Bluetooth keyboard"), one from the catalog, or a profile file.
- **The new-activity wizard builds a starter activity** from the devices you pick: the remote's
  buttons, the inputs and the power steps. You review it, save it, and then change anything.

The profile format is an open spec in [hubbo-profiles](https://hubbo.tv/spec/).

## A profile is a device

**What a device holds.** A device on the hub is a profile that's been added: manufacturer, model,
kind, platform, commands, power, timing and inputs. It also has its local fields: id, name,
integration, and `profile_id`, `revision` and `sha256` from the profile. Bluetooth pairing lives
with the bond, not in the device record ([multi-bluetooth.md](multi-bluetooth.md)).

The hub's device record and API field names mirror the profile's (snake_case), so adding a device is
close to a copy. The profile's `meta` is dropped. Every command is kept, so any command can be bound
later without fetching the profile again.

**Size.** A Bluetooth profile stores at 1–4 KB; an IR profile at about 16 KB ([ir.md](ir.md)).

**Home Assistant devices have no profile.** Their commands come from what HA reports
(`ha_capabilities`, [ha-domains.md](ha-domains.md)).

**Kinds** mirror Harmony's device types, about 30 of them, from the spec's `kinds.json`. A kind is
only a label, an icon and a browse category. An HA device's kind defaults from what HA reports, and
you can change it.

## Bluetooth commands come from the profile

A Bluetooth host is a keyboard, so any key can be useful: `R` to record or `I` for Info in an app,
for example. Hubbo never limits a Bluetooth device to its remote's buttons; that was one of
Harmony's weak spots. A Bluetooth device's key picker has three groups:
- **Remote commands:** the device's own commands, from its profile, by name. `Back` on an Apple TV
  is Consumer 0x223, and on a Shield it's 0x224. These are listed first, and they're what the wizard
  fills in.
- **All keys:** every keyboard key (0x04–0x65: letters, digits, F-keys, arrows, Enter, Esc and so on)
  and every named consumer key, on every Bluetooth device, whatever its profile.
- **A custom code:** for anything without a name, a raw consumer usage (0x0001–0x03FF), as an
  advanced option.

A generic profile ("Bluetooth keyboard") has few or no remote commands; its devices use All keys.

**What the hub can send:** any page and usage the profile gives, within the HID descriptor
(keyboard 0x04–0x65, consumer 0x0001–0x03FF). The descriptor is fixed, so no host needs pairing
again when profiles change.

**Holds.** A profile command with `hold_ms` (for example, the Apple TV's Control Center) is a tap
that holds for that long. Press-and-hold buttons use `key_down` and `key_up`.

## Built-in profiles

The hub ships copies of the generic profiles from hubbo-profiles, in `profiles/`, refreshed by
`bin/hubbo profiles sync` together with the spec's `kinds.json` and `buttons.json`. They're built
into the firmware and hub-sim, and the web admin lists them through the API (`GET /profiles`).

## Adding a device

1. Devices → Add device: search the catalog, pick a built-in profile, import a profile file, or
   pick "From Home Assistant".
2. Name it. The default name is the profile's model.
3. **What happens next depends only on the platform.** Bluetooth goes straight into the pairing
   card. IR is just added. Profiles carry no pairing steps.

## The new-activity wizard

A step-by-step wizard. It ends on a review page, and saving creates an ordinary activity. The answers
aren't stored anywhere; they only build the starter.

1. **Name and devices.** Name the activity and pick its devices, for example a TV, a receiver and a
   streamer.
2. **Watch:** "What are you watching on?" The chosen device gets navigation and playback: the d-pad,
   Select, Menu, Back, Exit, Info and Guide; play, pause, stop, record, skip; the colours and the
   number pad; channels.
3. **Volume:** "What controls the volume?" Volume up, volume down and mute go to that device.
4. **Inputs.** For each device that has inputs: "Which input should the TV use?" Pick one, or
   Don't switch. Inputs come from an HA device's source list (`select_source`) or a profile's
   `inputs`.
5. **Power.** For each device: "Turn on when the activity starts" and "Turn off when it ends". Both
   are on by default. Devices without power commands are left out.
6. **Review.** Each device with its role, input and power; the buttons that will be filled in; the
   startup and shutdown steps. Back goes to any step. Save creates the activity.

After saving, the activity opens in the normal editor.

### How buttons are filled

- For each button in a role, take the first candidate command the device has, using the spec's
  button table (`buttons.json`, shared by the hub and the catalog). A profile's `buttons` overrides
  the table; streamers map Menu to Home, for example. A button with no match stays empty.
- **HA devices:** a small table in the web admin maps HA commands to the same names
  (`media_player` volume up → `VolumeUp`, and so on).
- **Press-and-hold:** buttons that repeat while held, like the d-pad and volume, use `key_down` and
  `key_up` (`press_down`/`press_up` for IR). The rest are taps. Menu is held on Bluetooth devices:
  holding it opens Control Center on an Apple TV.

### How startup and shutdown are built

**Startup, for each device, in the order picked:**
- if it's turned on: its power-on steps, then a wait of its `on_delay_ms`;
- if it has an input: the input, then a wait of its `input_delay_ms`. A device switches input even
  if it isn't turned on;
- a wait of `key_delay_ms` between two steps to the same device.

For HA devices, power on is `turn_on` and the input is `select_source`. An HA device that's turned
on and switches input gets a 3 s wait between the two, since TVs often ignore an input change while
waking. HA mute fills as press = mute, hold = unmute.

**Shutdown:** the power-off steps of each device turned off.

**Switching between activities** runs only the new activity's startup, never the old one's
shutdown (ARCHITECTURE.md). So power-on steps must be safe to repeat:
- **Discrete on** (`PowerOn`) is always safe.
- **Toggle-only devices** get no automatic power step. The review page says so, and you can add one
  by hand.

**Bluetooth streamers:**
- **Power on** waits until the host is connected (`wait_connected`, up to 15 s, without blocking the
  remote), then presses its wake command, e.g. Home on Android TV and Fire TV.
- **Power off** is the profile's off steps, e.g. Sleep (0x34) on Android TV and Fire TV.
- **Apple TV has no power steps.** It wakes on any key and sleeps on its own timer. The only way to
  sleep it over Bluetooth is to hold Home and pick Sleep in Control Center, which depends on
  Control Center's layout and timing; any tvOS update can break it.
- Home as a streamer's power-on never turns it off, but switching between two activities that use
  the same streamer presses Home again, which leaves the open app.
