# A unique default hub name

Every new hub is called "Hubbo" and advertises `hubbo.local`, so two hubs that nobody renames
clash on the LAN. The user approved this fix (2026-11-13): the default name carries a short,
unique suffix that's ugly enough to nudge people to rename it.

## The default name

- **The default name is "Hubbo XXXX"**, where XXXX is the last two bytes of the hub's MAC in
  uppercase hex, e.g. "Hubbo 3F2A". The hostname follows the name the usual way
  (`hostnameFor`), so it's `hubbo-3f2a`.
- **The platform supplies the suffix.** `HubSettings` takes the default name from the platform
  instead of the fixed `kDefaultHubName`:
  - the board uses the MAC it advertises on (Ethernet's);
  - hub-sim uses a fixed suffix, so tests stay stable.
- **Everything that means "not renamed" uses it:** a hub with no saved name, a reset and a wipe.
  A saved name is never touched, so a hub already renamed keeps its name. A hub that's still
  called plain "Hubbo" has that name saved and keeps it too; no migration.
- The Bluetooth integration's own default `deviceName` ("Hubbo") isn't the hub name and stays
  as it is.

## The real hostname after a clash

Two hubs can still pick the same name, e.g. both renamed "Living Room". mDNS then probes, finds
the clash, and advertises a mangled hostname: it adds a numeric suffix or bumps one (`living-room` → `living-room-2`, `room-2` → `room-3`). Today `/status` and Settings
would still say `living-room.local`, which then reaches the wrong hub.

- **The board reports the hostname mDNS actually advertises**, both after probing and after any
  later conflict. `/status` `network.hostname` and `GET /admin/hub`'s `hostname` show that one.
- **Settings and Status show it as is.** No new UI is needed beyond showing the real value. If
  it differs from what the name would give, a light hint can say another hub on the network
  already uses that name.
- hub-sim has no mDNS, so its hostname is always `hostnameFor(name)`.

## Out of scope

- Detecting or resolving clashes in the Bluetooth name.
- Renaming hubs automatically.
