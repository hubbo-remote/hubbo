# More Home Assistant domains

Today Hubbo offers four HA domains: `media_player`, `light`, `switch` and `fan`. The user wants seven
more (2026-11-13): `scene`, `script`, `cover`, `button`, `input_boolean`, `humidifier`
and `select`. Locks, remotes and climate are dropped (decided 2026-11-13). Once they're built, the showcase hub-sim gets fake versions of
them, based on a fresh local capture of the user's HA, so the user can review how the editors
handle them.

## Rules that stay

- **Every domain is a curated allowlist** in `core/src/ha/capabilities.cpp`. Each command has a
  Hubbo id, a label and the HA service it calls. Hubbo never sends a raw service call. Params are
  typed and checked, and an entity target is never a param.
- **HA's feature bits gate commands.** Feature constants are copied from HA's own source
  (`homeassistant/components/<domain>/const.py`) and verified against it, the way the fan pass
  did. Enum params (modes, options, activities) come from the entity's attributes at
  registration time, stored in the device's capability snapshot.
- **Integration services** (e.g. `sonos.*`) stay as they are: for media players only. None of
  the new domains gets them in this pass.
- **The snapshot limits hold.** This must stay small on the board; no new per-entity data beyond
  the attributes a command needs.

## Commands per domain

Each feature bit and attribute name below is to be verified against HA's source. Where HA has no
feature bits for a domain, every command is always offered.

| Domain | Commands (Hubbo label → HA service) | Gated by / params from |
|---|---|---|
| `scene` | Activate → `scene.turn_on` | always |
| `script` | Run → `script.turn_on`; Stop → `script.turn_off` | always |
| `cover` | Open → `open_cover`, Close → `close_cover`, Stop → `stop_cover`, Toggle → `toggle`, Set position → `set_cover_position` (0–100 %), Open tilt / Close tilt / Stop tilt, Set tilt → `set_cover_tilt_position` | `CoverEntityFeature`: OPEN 1, CLOSE 2, SET_POSITION 4, STOP 8, OPEN_TILT 16, CLOSE_TILT 32, STOP_TILT 64, SET_TILT_POSITION 128 |
| `button` | Press → `button.press` | always |
| `input_boolean` | Turn on / Turn off / Toggle | always |
| `humidifier` | Turn on / Turn off / Toggle; Set humidity → `set_humidity` (min_humidity..max_humidity); Set mode → `set_mode` (available_modes) | `HumidifierEntityFeature.MODES` 1 gates Set mode |
| `select` | Choose → `select_option` (options); Next → `select_next`; Previous → `select_previous` | always; `options` attribute is the enum |

Remote buttons map to these like any other command: a momentary command is a Press; a Hold
pairs on/off-style commands where that already makes sense. Nothing new in the bindings model.

## Device types

New HA devices need a sensible default type. Three new kinds in `kinds.json` (hubbo-spec):
`cover` (blinds, shades, garage doors, gates: every HA cover, as HA groups them), `scene` and `script`. Humidifiers use `climate`. Buttons, input booleans and selects use `other`.

**The hub suggests the type.** Instead of the web admin guessing from names and hub-sim keeping
its own mapping (they had drifted), discovery candidates get a `suggested_kind`. One core
function picks it from the domain plus the platform and device class, using what hub-sim's
`kindFor` already does well (e.g. `apple_tv` → streamer). The web admin uses it as the default,
and hub-sim's showcase calls the same function.

## The showcase

- The lead takes a fresh capture of the user's HA (states, `/api/services`, translations) that
  includes the new domains, into `sim/fixtures/local/` (gitignored). No entity id, name or value
  from it goes into any tracked file, tests included.
- The showcase registers every captured entity of a supported domain, the way it does today.
- Tracked tests and fixtures use made-up entities for each new domain.

## Out of scope

- Script fields as params (a script's own inputs). Run/Stop only for now.
- Locks, remotes, climate, alarm panels, cameras, vacuums, valves, water heaters.
- Integration services for the new domains.
- State display (e.g. a cover's current position) in the web admin.
