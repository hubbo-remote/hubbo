# Bluetooth hosts

A home can have several Bluetooth streamers: an Apple TV, an NVIDIA Shield, a Fire TV, each used by
its own activities. The hub pairs with up to four hosts and stays connected to all of them at once,
so switching activities is instant. That goes further than the Harmony Hub, which reconnects on
each switch.

## Model

- **The `ble` integration is the radio.** It has one name that hosts see ("Hubbo" by default), and
  its health is the radio's: working or not. It has no bond, peer or pairing state of its own.
- **Each host is a device** under `ble`, added from a profile ([device-profiles.md](device-profiles.md)).
  A new device shows "Not paired".
- **At most 4 Bluetooth devices.** Adding a fifth is refused with a short message.
- **Pairing is per device.** On the device's page, Pair opens a window for that device. The host
  that bonds in the window becomes that device's host. Pairing an already-paired device replaces only
  its own bond. Forget removes only its own bond. Deleting the device forgets it.
- **What's stored:** for each paired device, the host's identity address, in NVS next to the bond.
  It's written once at pairing. The device record in config holds no pairing data, and backups
  don't carry bonds.
- **Pairings follow the config.** At boot and after every config change (create, delete, restore,
  reset), the hub forgets any pairing whose device isn't in the config. A restored device whose id
  still has a pairing on this hub keeps it. After a factory reset, or on another board, it shows
  "Not paired".

## Staying connected

- The hub keeps every paired host connected. While any paired host isn't connected, it advertises
  with a whitelist of the paired hosts' identities only (connect filter), so unknown hosts can't
  connect and bonded hosts can come back on their own.
- Up to 4 connections at once. NimBLE's bond and connection limits are raised to fit, and its
  bond-store-full behaviour (which deletes the oldest bond) is overridden to refuse instead.
- An activity start does nothing Bluetooth-specific. Hosts are already connected, or reconnect when
  they wake. A startup can wait for one with `wait_connected`.

## Sending keys: no "current host"

There is no active-host state.

- A Bluetooth action names its device (`device_id`). The key goes to that device's host.
- At send time the firmware asks the Bluetooth stack for the live connection to that host's
  identity. It doesn't keep its own table of connection handles.
- If that host isn't connected, the action is `unavailable` and nothing is sent anywhere. Nothing
  is queued.
- A hold's down and up are on the same device (the compiler checks this). When a host disconnects,
  or the activity changes, any key held on it is released.

A key can only reach the wrong host if a binding names the wrong device, which the editor shows.

## Pairing window

- `POST /admin/devices/{device_id}/pair` opens the window for a Bluetooth device. `GET` shows its
  state, `DELETE` cancels. `DELETE /admin/devices/{device_id}/pairing` forgets the host.
- One window at a time (409 `busy`).
- While the window is open, the whitelist is off, so a new host can connect. A host already paired
  to another device stays tied to that device. The first new bond goes to the window's device.
- A full bond store refuses new bonds, so the window ends as failed.

## Status

| Where | Shows |
|---|---|
| Integrations → Bluetooth | The radio: working, or "Bluetooth isn't working" |
| Devices list and device page | "Not paired", "Ready to pair · 87 s", "Connected", "Not connected" |

Core owns one function from the per-device pairing and connection state to these, used by hub-sim
and the firmware. hub-sim's fake Bluetooth has several hosts that can be paired, connected and
dropped through `/sim/...` controls.

## Known behaviour

- Some hosts reconnect at once after the hub reboots but take a while to encrypt the link (a Fire
  TV has taken about 40 s). Until then their keys are `unavailable`. Retrying security or dropping
  the link was judged riskier than the delay. The firmware logs each host's connect, encryption
  result and disconnect.
