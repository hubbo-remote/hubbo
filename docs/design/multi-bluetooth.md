# Several Bluetooth devices

Today Hubbo pairs with one Bluetooth host. A home can have several: an Apple TV, an NVIDIA Shield, a
Fire TV Stick, each used by its own activities (user, 2026-11-10). This phase lets the hub pair with
up to four hosts and stay connected to all of them at once, so switching activities is instant. That
goes further than the Harmony Hub, which reconnects on each switch.

This phase comes before polish round 2 and Phase 6. Device profiles, pre-filled buttons and the
catalog come later, after a spec phase (`research-device-catalog.md`); this phase doesn't wait for
them.

## 0. More keys

Add the keys the research found missing from core's key table. They fit the current HID descriptor,
so no host needs re-pairing:
- Consumer page: FF 0xB3, Rew 0xB4, Stop 0xB7, AC Back 0x224, Search 0x221, Exit 0x46, Ch +/−
  0x9C/0x9D, Info 0x1BD, Sleep 0x34.
- Keyboard page: keypad Enter 0x58, Backspace 0x2A, Esc 0x29.

Don't change the descriptor in this phase.

## 1. Model

- **The `ble` integration is the radio.** It has one name that hosts see ("Hubbo" by default), and
  its health is the radio's: working or not. It no longer has a bond, a peer or a pairing state.
- **Each host is a device** under `ble`, for example `dev_apple_tv`. Adding one is the same as any
  device: Devices → Add device → Bluetooth → name. No discovery, no pairing at this point. A new
  device shows "Not paired".
- **At most 4 Bluetooth devices** (user decision). Adding a fifth is refused with a short message.
- **Pairing is per device.** On the device's page, Pair opens a window for that device. The host
  that bonds in the window becomes that device's host. Pairing an already-paired device replaces only
  its own bond. Forget removes only its own bond. Deleting the device forgets it.
- **What's stored:** for each paired device, the host's identity address, next to the bond in NVS.
  It's written once at pairing. The device record in config holds no pairing data. Backups don't
  carry bonds (as before).
- **Pairings follow the config.** At boot and after every config change (create, delete, restore,
  reset), the hub forgets any pairing whose device isn't in the config. A restored device whose id
  still has a pairing on this hub keeps it. After a factory reset, or on another board, it shows
  "Not paired".

Never shipped, so no migration: the integration-level bond and the `bonded` and `peer_name`
settings go away. After the update, the dev board's Apple TV is re-paired from its device page.

## 2. Staying connected

- The hub keeps every paired host connected. While any paired host isn't connected, it advertises
  with a whitelist of the paired hosts' identities only (connect filter), so unknown hosts can't
  connect and bonded hosts can come back on their own.
- Up to 4 connections at once. Raise NimBLE's bond and connection limits to fit, and override its
  bond-store-full behaviour (which deletes the oldest bond) so it refuses instead.
- An activity start does nothing Bluetooth-specific. Hosts are already connected, or reconnect when
  they wake.

## 3. Sending keys: no "current host"

There is no active-host state.

- A Bluetooth action already names its device (`device_id`). The key goes to that device's host.
- At send time the firmware asks the Bluetooth stack for the live connection to that host's
  identity. It doesn't keep its own table of connection handles.
- If that host isn't connected, the action is `unavailable` and nothing is sent anywhere. No
  queueing (as for HA).
- A hold's down and up are on the same device (the compiler already checks this). When a host
  disconnects, or the activity changes, any key held on it is released.

A key can only reach the wrong host if a binding names the wrong device, which the editor shows.

## 4. Pairing window

- `POST /admin/devices/{device_id}/pair` opens the window for a Bluetooth device. `GET` shows its
  state, `DELETE` cancels. `DELETE /admin/devices/{device_id}/pairing` forgets the host. The
  integration-level `/admin/integrations/{id}/pair` goes away.
- One window at a time (409 `busy`).
- While the window is open, the whitelist is off, so a new host can connect. A host already paired
  to another device stays tied to that device. The first new bond goes to the window's device.
- A full bond store (4) refuses new bonds, so the window ends as failed.

## 5. Status

| Where | Shows |
|---|---|
| Integrations → Bluetooth | The radio: working, or "Bluetooth isn't working" |
| Devices list and device page | "Not paired", "Ready to pair · 87 s", "Connected", "Not connected" |

Core owns one function from the per-device pairing and connection state to these, used by hub-sim
and the firmware (as `ble_state` does today).

## 6. hub-sim and the mock

hub-sim's fake Bluetooth gets several hosts that can be paired, connected and dropped, with
`/sim/...` controls. The mock mirrors it. The e2e tests cover add, pair, forget, the limit of four,
and a key going to the right host.

## 7. Board tests

Physical steps (pairing on each host) are the user's.

1. Two hosts paired and both connected. Keys go only to the named host, and the remote's latency
   is unchanged.
2. The whitelist rejects an unknown host, and paired hosts using private addresses still
   reconnect.
3. A host that sleeps or turns off comes back on its own, and keys for it are `unavailable` until
   then.
4. Four hosts paired, and a fifth bond is refused.
5. Pairing a new host while others are connected.
6. A day with one host turned off, then turned on.
7. The loop's max gap stays under 5 ms with all connections up and while hosts reconnect.
8. The new keys: FF/Rew/Stop on the Apple TV, the Fire TV's OK as Consumer 0x41 or keypad Enter
   0x58, and Sleep 0x34 on the Shield (where those hosts are available).

## Ownership

| Agent | Work |
|---|---|
| Core | Device-level BLE pairing state and its status function; the limit of 4; removing the integration bond; hub-sim's fake hosts; compiler and engine handling of an unavailable host |
| API | Device pair and forget routes, removing the integration pair route, the port with a device id, spec, fixtures, contract, docs |
| Firmware | Several bonds and connections, the whitelist, the store-full override, send by identity, release per host, board measurements |
| Frontend | Add a Bluetooth device, pair and forget on the device page, per-device status, the radio-only integration card, mock and e2e |

## Gate

The four area gates, then the board tests above.

## Board test results (2026-11-10)

A Fire TV and an Apple TV were paired and connected at the same time, and keys followed activities. The Apple TV came back on its own after a reboot. Pair then Cancel kept the Fire TV's pairing. FF, Rew and Stop work on the Apple TV, and OK works on the Fire TV. After a power cycle of the board, both hosts came back paired. Latency: read→HID median 0.67 ms, max 1.45 ms.

**Accepted:** a loop gap of about 10 ms, now and then, while the flash erases. It comes from saves on core 0 (the active activity, about 5 s after a switch), which pause the other core for up to `CONFIG_SPI_FLASH_ERASE_YIELD_DURATION_MS` (10). It dates from Phase 2 and nobody can feel it (user, 2026-11-10). If presses ever seem dropped, lower that setting to about 2 ms.

**Known:** after the board reboots, the Fire TV reconnects at once but takes about 40 s to encrypt the link. Until then its keys are `unavailable`; then they work. Accepted (user, 2026-11-10). Retrying security or dropping the link was judged riskier than the delay. Next firmware work adds a log line for each connect, encryption result and disconnect.
