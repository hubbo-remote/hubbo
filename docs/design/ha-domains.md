# Home Assistant domains

Hubbo offers these HA entity domains as devices: `media_player`, `light`, `switch`, `fan`, `scene`,
`script`, `cover`, `button`, `input_boolean`, `humidifier`, `select` and `remote`. Each domain's
commands, feature bits and params are listed in [ha-capabilities.md](../ha-capabilities.md); this
doc is the why.

## Rules

- **Every domain is a curated allowlist** in `core/src/ha/capabilities.cpp`. Each command has a
  Hubbo id, a label and the HA service it calls. Params are typed and checked, and an entity target
  is never a param. Anything past the allowlist goes through the Home Assistant action
  ([ha-custom-action.md](ha-custom-action.md)), which is still scoped to the device.
- **Only what HA provides.** Feature constants are copied from HA's own source
  (`homeassistant/components/<domain>/const.py`) and verified against it. HA's feature bits gate
  commands; where a domain has no feature bits, every command is offered. Enum params (modes,
  options, sources) come from the entity's attributes at registration time, stored in the device's
  capability snapshot. Nothing is invented: no preset lists, no per-brand tables.
- **Built for any HA, not one setup.** A domain works the same whatever integration is behind it.
- **The snapshot stays small** on the board: no per-entity data beyond the attributes a command
  needs.
- **Commands map to buttons** like any other: a momentary command is a Press; a Hold pairs
  on/off-style commands where that makes sense. Nothing new in the bindings model.

## Commands per domain

| Domain | Commands (Hubbo label → HA service) | Gated by / params from |
|---|---|---|
| `media_player` | Power, volume, mute, playback, Select source, Sound mode, shuffle, repeat | `MediaPlayerEntityFeature`; `source_list`, `sound_mode_list` |
| `light` | Turn on / Turn off; Change brightness (`brightness_step`) | `supported_color_modes` |
| `switch` | Turn on / Turn off | always |
| `fan` | Turn on / Turn off, Set speed, Speed up / Slow down, Oscillate, Set direction, Preset mode | `FanEntityFeature`; `preset_modes` |
| `scene` | Activate → `scene.turn_on` | always |
| `script` | Run → `script.turn_on`; Stop → `script.turn_off` | always |
| `cover` | Open, Close, Stop, Toggle, Set position, Change position (`cover.position_step`, see [smart-buttons.md](smart-buttons.md)), Open / Close / Stop tilt, Set tilt | `CoverEntityFeature` |
| `button` | Press → `button.press` | always |
| `input_boolean` | Turn on / Turn off / Toggle | always |
| `humidifier` | Turn on / Turn off / Toggle; Set humidity (min_humidity..max_humidity); Set mode (available_modes) | `HumidifierEntityFeature.MODES` gates Set mode |
| `select` | Choose → `select_option` (options); Next; Previous | always; `options` is the enum |
| `remote` | Turn on / Turn off / Toggle; everything else (`send_command`, `turn_on` with an activity, ...) through the Home Assistant action | always |

A remote's `turn_on` takes an `activity` as free text, which Hubbo doesn't type, so it isn't a
param of the typed command; it's offered through the Home Assistant action instead.

## Device types

**The hub suggests the type.** Discovery candidates carry a `suggested_kind`, picked by one core
function from the domain plus the platform and device class (e.g. an `apple_tv` media player →
streamer). The web admin uses it as the default, and hub-sim's showcase calls the same function, so
the two can't drift. Covers get `cover`, scenes `scene`, scripts `script`, humidifiers `climate`;
buttons, input booleans, selects and remotes `other`. The admin can change it.

## Testing against real data

hub-sim's showcase can register every entity of a supported domain from a capture of an HA
instance (states, `/api/services`, translations) kept under `sim/fixtures/local/`, which is
gitignored. Nothing from a real capture goes into a tracked file; tracked tests and fixtures use
made-up entities for each domain.

## Not offered

- Script fields as params (a script's own inputs). Run/Stop only.
- Locks, climate, alarm panels, cameras, vacuums, valves, water heaters.
- State display (e.g. a cover's current position) in the web admin.
