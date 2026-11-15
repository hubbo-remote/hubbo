# Home Assistant capabilities

How Hubbo turns a Home Assistant entity into a short list of safe, typed commands. The code is
`core/include/hubbo/ha/capabilities.h` (portable, tested by `core/tests/ha_capabilities_test.cpp`).

## Sources

Every feature bit and service rule below was checked against Home Assistant's own source, at
`home-assistant/core` commit `5a20e53de5a135b3d246a18b8794dace65a8d593` (dev, 2026-11-08):

- `homeassistant/components/media_player/const.py`: `MediaPlayerEntityFeature`, `RepeatMode`.
- `homeassistant/components/media_player/services.py`: which feature each service requires.
- `homeassistant/components/media_player/services.yaml`: `volume_set.volume_level` selector 0–1.
- `homeassistant/components/light/const.py`: `LightEntityFeature`, `ColorMode`,
  `COLOR_MODES_BRIGHTNESS`, `VALID_BRIGHTNESS` (0–255), `VALID_BRIGHTNESS_STEP` (−255–255),
  `VALID_TRANSITION`, `DEFAULT_MIN_KELVIN`/`DEFAULT_MAX_KELVIN` (2000/6535).
- `homeassistant/components/fan/const.py`, `services.py`, `services.yaml` and `entity.py`
  (checked 2026-11-12): `FanEntityFeature`, each service's required features and schema
  (percentages are `Coerce(int)` 0–100), `percentage_step` = 100 / speeds.
- `homeassistant/components/{scene,script,cover,button,input_boolean,humidifier,select}/const.py`
  and `services.py` (dev, checked 2026-11-13): `CoverEntityFeature`, `HumidifierEntityFeature`,
  each service's required features and schema (cover positions and humidity are `Coerce(int)` +
  `Percentage()`; humidifier `min_humidity`/`max_humidity` default 0/100; select
  `select_next`/`select_previous` take an optional `cycle`, default true).
- `homeassistant/components/remote/const.py`, `services.yaml` and `strings.json` (dev, checked
  2026-11-14): `RemoteEntityFeature`, the six services, their fields and English strings.
- `homeassistant/helpers/service.py`: how `required_features` is applied.
- The fixture masks come from the integrations themselves: `denonavr/media_player.py`
  (`SUPPORT_DENON`), `sonos/media_player.py`, `webostv/media_player.py` (`SUPPORT_WEBOSTV`,
  `SUPPORT_WEBOSTV_VOLUME`), `xbox/media_player.py` (`SUPPORT_XBOX`), `hue/v2/light.py`.

Recheck these when bumping the supported HA version. Feature values are part of HA's stored state
and HA doesn't renumber them, but new ones get added.

### How HA gates a service

Entity services are registered with a `required_features` list. `helpers/service.py` lets an
entity through when **any** entry is fully present: `entity.supported_features & f == f`. So a
list `[VOLUME_SET, VOLUME_STEP]` means either one, and a single combined entry `PLAY | PAUSE`
means both. Hubbo uses the same test, so it never offers a command HA would reject with
`ServiceNotSupported`.

## Feature bits

`MediaPlayerEntityFeature`:

| Flag | Value | | Flag | Value |
|---|---|---|---|---|
| PAUSE | 1 | | STOP | 4096 |
| SEEK | 2 | | CLEAR_PLAYLIST | 8192 |
| VOLUME_SET | 4 | | PLAY | 16384 |
| VOLUME_MUTE | 8 | | SHUFFLE_SET | 32768 |
| PREVIOUS_TRACK | 16 | | SELECT_SOUND_MODE | 65536 |
| NEXT_TRACK | 32 | | BROWSE_MEDIA | 131072 |
| TURN_ON | 128 | | REPEAT_SET | 262144 |
| TURN_OFF | 256 | | GROUPING | 524288 |
| PLAY_MEDIA | 512 | | MEDIA_ANNOUNCE | 1048576 |
| VOLUME_STEP | 1024 | | MEDIA_ENQUEUE | 2097152 |
| SELECT_SOURCE | 2048 | | SEARCH_MEDIA | 4194304 |

(64 is unused.)

`LightEntityFeature`: EFFECT 4, FLASH 8, TRANSITION 32. Brightness and colour support come from
the `supported_color_modes` attribute, not from feature bits.

`FanEntityFeature`: SET_SPEED 1, OSCILLATE 2, DIRECTION 4, PRESET_MODE 8, TURN_OFF 16, TURN_ON 32.

`CoverEntityFeature`: OPEN 1, CLOSE 2, SET_POSITION 4, STOP 8, OPEN_TILT 16, CLOSE_TILT 32,
STOP_TILT 64, SET_TILT_POSITION 128 (SPEED 256 is unused).

`HumidifierEntityFeature`: MODES 1. Scene, script, button, input_boolean and select have no
feature bits.

`RemoteEntityFeature`: LEARN_COMMAND 1, DELETE_COMMAND 2, ACTIVITY 4. Hubbo gates nothing on
them itself; HA's `get_services` filters (turn_on's `activity` field needs ACTIVITY) apply to the
hints.

## The allowlist

A command is offered only if it is in this table **and** the entity has the features. Command IDs
are Hubbo's, not HA service names (`light.brightness_step` calls `light.turn_on`). Param names are
Hubbo's; the HA field is fixed by the table.

### media_player

| Command | HA service | Needs | Params (Hubbo → HA field) |
|---|---|---|---|
| `media_player.turn_on` | `turn_on` | TURN_ON | |
| `media_player.turn_off` | `turn_off` | TURN_OFF | |
| `media_player.volume_up` | `volume_up` | VOLUME_SET or VOLUME_STEP | |
| `media_player.volume_down` | `volume_down` | VOLUME_SET or VOLUME_STEP | |
| `media_player.volume_set` | `volume_set` | VOLUME_SET | `level` number 0–1 → `volume_level` |
| `media_player.volume_mute` | `volume_mute` | VOLUME_MUTE | `muted` bool → `is_volume_muted` |
| `media_player.mute_toggle` (Toggle mute) | `volume_mute` | VOLUME_MUTE | none; sends `is_volume_muted` = the opposite of the entity's current value, read at press time (unknown → `true`) |
| `media_player.media_play_pause` | `media_play_pause` | PLAY and PAUSE | |
| `media_player.media_play` | `media_play` | PLAY | |
| `media_player.media_pause` | `media_pause` | PAUSE | |
| `media_player.media_stop` | `media_stop` | STOP | |
| `media_player.media_next_track` | `media_next_track` | NEXT_TRACK | |
| `media_player.media_previous_track` | `media_previous_track` | PREVIOUS_TRACK | |
| `media_player.select_source` | `select_source` | SELECT_SOURCE and a non-empty `source_list` | `source` enum of `source_list` → `source` |
| `media_player.select_sound_mode` | `select_sound_mode` | SELECT_SOUND_MODE and a non-empty `sound_mode_list` | `sound_mode` enum → `sound_mode` |
| `media_player.shuffle_set` | `shuffle_set` | SHUFFLE_SET | `shuffle` bool → `shuffle` |
| `media_player.repeat_set` | `repeat_set` | REPEAT_SET | `repeat` enum off/all/one → `repeat` |

### light

`brightness` below means: `supported_color_modes` contains any mode in HA's
`COLOR_MODES_BRIGHTNESS` (brightness, color_temp, hs, xy, rgb, rgbw, rgbww, white). A light with
only `onoff` (or no modes reported) gets plain on/off.

| Command | HA service | Needs | Params |
|---|---|---|---|
| `light.turn_on` | `turn_on` | always | optional `brightness` int 0–255 (if brightness), `color_temp_kelvin` int min–max (if `color_temp` mode; range from `min_/max_color_temp_kelvin`, else 2000–6535), `transition` number 0–300 s (if TRANSITION) |
| `light.turn_off` | `turn_off` | always | optional `transition` (if TRANSITION) |
| `light.brightness_step` | `turn_on` with `brightness_step` | brightness | `step` int −255–255; optional `transition` |

Optional params that are left out are left out of the HA call too, so `light.turn_on` with no
params restores the light's last state rather than forcing a brightness.

### switch

`switch.turn_on`, `switch.turn_off`. No params, no feature bits.

### fan

Percentages are int 0–100 with unit `%`. A speed slider's `step` is the entity's
`percentage_step` (100 / speeds) rounded down. HA rounds a percentage up to the next speed, so
every multiple still lands on the speed it means (3 speeds: 33, 66, 99). It's stored in the
snapshot as an integer, 1–100. Below 1 there's no step.

| Command | HA service | Needs | Params |
|---|---|---|---|
| `fan.turn_on` | `turn_on` | TURN_ON | optional `percentage` (if SET_SPEED), optional `preset_mode` enum of `preset_modes` (if PRESET_MODE and the list isn't empty) |
| `fan.turn_off` | `turn_off` | TURN_OFF | |
| `fan.set_percentage` | `set_percentage` | SET_SPEED | `percentage` |
| `fan.increase_speed` | `increase_speed` | SET_SPEED | optional `percentage_step` int 0–100 % (HA uses the fan's own step when omitted) |
| `fan.decrease_speed` | `decrease_speed` | SET_SPEED | optional `percentage_step` |
| `fan.oscillate` | `oscillate` | OSCILLATE | `oscillating` bool |
| `fan.set_direction` | `set_direction` | DIRECTION | `direction` enum forward/reverse |
| `fan.set_preset_mode` | `set_preset_mode` | SET_SPEED or PRESET_MODE, and a non-empty `preset_modes` | `preset_mode` enum |

Typical masks: 49 for speed only, 57 with presets, and 61 with presets and direction.

### scene, script, button, input_boolean

No feature bits and no params; HA registers these services for every entity.

| Command | HA service |
|---|---|
| `scene.turn_on` (Activate) | `scene.turn_on` |
| `script.turn_on` (Run), `script.turn_off` (Stop) | `script.turn_on`, `script.turn_off` |
| `button.press` (Press) | `button.press` |
| `input_boolean.turn_on`, `.turn_off`, `.toggle` | the same |

A script's own fields (`variables`) aren't offered: Run and Stop only.

### cover

Positions are int 0–100 with unit `%`. Gating mirrors `cover/services.py`.

| Command | HA service | Needs | Params |
|---|---|---|---|
| `cover.open_cover` (Open) | `open_cover` | OPEN | |
| `cover.close_cover` (Close) | `close_cover` | CLOSE | |
| `cover.stop_cover` (Stop) | `stop_cover` | STOP | |
| `cover.toggle` (Toggle) | `toggle` | OPEN and CLOSE | |
| `cover.set_cover_position` (Set position) | `set_cover_position` | SET_POSITION | `position` |
| `cover.open_cover_tilt`, `.close_cover_tilt`, `.stop_cover_tilt` | the same | OPEN_TILT, CLOSE_TILT, STOP_TILT | |
| `cover.set_cover_tilt_position` (Set tilt) | `set_cover_tilt_position` | SET_TILT_POSITION | `tilt_position` |

The open/close `speed` field isn't offered (free text).

### humidifier

| Command | HA service | Needs | Params |
|---|---|---|---|
| `humidifier.turn_on`, `.turn_off`, `.toggle` | the same | | |
| `humidifier.set_humidity` (Set humidity) | `set_humidity` | | `humidity` int, unit `%`, the entity's `min_humidity`..`max_humidity` (whole percent inside it; 0–100 if not reported) |
| `humidifier.set_mode` (Set mode) | `set_mode` | MODES, and a non-empty `available_modes` | `mode` enum |

`humidityRange()` is the one rule for the snapshot and the command.

### select

`select.select_option` (Choose, `option` enum of `options`), `select.select_next` (Next),
`select.select_previous` (Previous). A select with no options gets nothing. `cycle` is left to
HA's default (true).

### remote

| Command | HA service | Needs | Params |
|---|---|---|---|
| `remote.turn_on` (Turn on), `.turn_off` (Turn off), `.toggle` (Toggle) | the same | | |

Every remote, no params: turn_on's `activity` is text, which Hubbo doesn't type. Everything else
goes through the Home Assistant action: HA's other `remote` services that match the entity
(`remote.send_command`, `remote.learn_command`, `remote.delete_command`, and `remote.turn_on`
with an `activity`), as custom services; see "Integration-specific services". Every command is
momentary: one press, one call.

### Suggested kind

Discovery candidates carry `suggested_kind`, from `ha::suggestedKind(domain, platform,
device_class)`: light, switch, fan, cover, scene and script are their own kinds (garage doors and
gates too are `cover`); humidifier is `climate`; button, input_boolean, select and remote
are `other`. A media player is `receiver` or `speaker` by its device class (or `speaker` for the alexa_media and group
platforms), `streamer` for apple_tv, and otherwise `tv`. hub-sim's showcase uses the same
function.

## Deliberately not offered

- **toggle** (media_player, light, switch, fan, script): the architecture prefers explicit
  on/off. Toggles break as soon as the hub and the device disagree. Covers, humidifiers, input
  booleans and remotes get one too, as HA has it for them.
- **Mute toggle is the one exception, and it doesn't guess.** HA has no mute toggle
  (`volume_mute` needs `is_volume_muted`), and a remote's mute key toggles. So
  `media_player.mute_toggle` sits next to Mute/Unmute: `EntityCapabilities::stateAttribute()`
  names the attribute it reads (`is_volume_muted`); the adapter reads the one entity's state
  (`subscribe_entities`) just before sending and passes the value to `buildServiceCall()`, which
  sends the opposite. HA leaves the attribute out when the player is off (or reports None);
  unknown, or not a bool, counts as false, so the press mutes. Two presses faster than HA
  updates its state can both send the same value. The wizard binds the remote's mute button to
  it; activities and sequences keep the explicit Mute/Unmute.
- **play_media, browse/search, announce, enqueue**: take free-form content IDs or URLs.
- **join / unjoin** (GROUPING): take other entity IDs.
- **media_seek, clear_playlist**: no remote use, and clear_playlist is destructive.
- **light effect, flash, rgb/hs/xy colour**: possible later. Effect would be an enum from
  `effect_list`; colours need array params, which the param model doesn't have.
- **Integration-specific services** (`webostv.button`, `sonos.*`, `denonavr.*`): not hand-curated.
  They're resolved from HA's own service metadata; see below.
- **Domains** climate, lock, alarm_control_panel, camera, vacuum, valve, water_heater,
  the other input_*, group, automation and everything else: no commands until added here with
  tests.
- **Typed `remote.send_command`**: its `command` is free text or a list, so it's a Home Assistant
  action (docs/design/ha-remote.md, 2026-11-14), not a typed command.
- **Integration services for the newer domains** (scene through select): only media players,
  lights, switches, fans and remotes get them (`offersIntegrationServices()`).

## Safety rules (enforced in code)

1. **Target is the registered entity, always.** `buildServiceCall()` sets the target to the
   device's stored `entity_id` and nothing else. The API never accepts an entity ID in an
   execution request.
2. **No broad targets.** `isValidEntityId()` accepts only `<domain>.<object_id>` with
   `[a-z0-9_]`, at most 96 chars (`limits::kMaxProviderRefLength`), rejects `all`/`none` object IDs and anything with commas or spaces. No area,
   device, floor or label targets exist in the call model.
3. **Keys from the table, values from the user.** Service data keys come only from the static
   table above. Values are typed and range-checked by core `resolveParams()` against the
   catalog, then checked again by `buildServiceCall()`.
4. **No nesting.** Parameter values are scalars (`bool`, number, string-from-enum). The OpenAPI
   schema rejects objects and arrays, and the core's `ParamLiteral` cannot represent them.
5. **Target-like names are refused by name** (`entity_id`, `device_id`, `area_id`, `floor_id`,
   `label_id`, `target`, `data`, `service`, `service_data`, `domain`): `forbidden_param`. The
   spec's `ParamName` lists the same names; `api/lint.sh` checks they match.
6. **Enum options are the entity's own.** `source_list` and friends are captured at
   normalization. Options over 64 bytes (`limits::kMaxParamStringLength`), empty or duplicate
   options, and anything past 128 are dropped and counted (`droppedOptions()`). A picker with no
   usable options is dropped.

The device registry is an application boundary. The HA token Hubbo holds can still do anything
that token's HA user can do (see docs/security.md).

## Integration with the core

- The HA adapter keeps one `EntityCapabilities` per registered HA device and returns
  `catalog()` from `IntegrationAdapter::commandsFor()`. The catalog points into the object, so
  it must outlive any compiled config that uses it. Rebuild it when HA reports new attributes
  (e.g. a changed `source_list`), then recompile the config.
- Omitted optional params: core `resolveParams()` reports which params were supplied as a bit
  mask, carried to the adapter as `ExecRequest.paramsPresent`. The adapter passes it to
  `buildServiceCall()`, which leaves unsupplied optional params out of the HA call.
- WebSocket message shape (when the transport lands):
  `{"type":"call_service","domain":<domain>,"service":<service>,"target":{"entity_id":<entity_id>},"service_data":{<data>}}`.
  Format floats with about 6 significant digits; `level` is stored as a float, so 0.35 is
  0.3499999940395355 as a double.
- `supported_features` changes at runtime for some integrations (webOS adds volume
  features only while on, and drops VOLUME_SET with external speakers). Normalize from the
  state seen at registration/discovery, refresh on reconnect, and report a command that later
  fails with `ServiceNotSupported` as `failed`, not as a crash.

## Open questions

- **Volume repeat on hold:** decided, not for now (2026-11-08). An HA command fires once per
  trigger.

## Integration-specific services

Built in Phase 3 (`core/include/hubbo/ha/services.h`, `snapshot.h`). Resolved from Home
Assistant rather than curated per integration. What Hubbo can't type is still callable as a
Home Assistant action (below).

- `get_services` (WebSocket) returns every service with its `target` filter and its `fields`
  with selectors. For example, `sonos.set_sleep_timer` targets `entity: {integration: sonos,
  domain: media_player}`, and its `sleep_time` field is a `number` selector. Older
  services.yaml files name the target as an `entity_id` field with an `entity` selector instead
  (`webostv.button`, `sonos.snapshot`): that field counts as the target, not a field.
  `webostv.button`'s `button` field is a free `text` selector (HA documents INFO, HOME, ... but
  sends any string), so it's a custom service, not a typed command.
- The entity registry gives each entity's `platform` (integration), e.g. `webostv`. A YAML-only
  entity has no registry entry, so no platform and no integration services; its domain commands
  still work.
- For an entity of platform P, a service `P.S` is offered when its `target` has an `entity`
  filter matching the entity: the filter's integration (if any) is P, its domain list (if any)
  has the entity's domain, and one of its `supported_features` entries (if any) is fully
  supported. Only P's own services are considered (never the entity domain's, which are the
  allowlist above), and never when P is the entity's domain. Services with no entity target
  (domain-wide, device-only) are never offered.
- **The remote exception** (docs/design/ha-remote.md): a `remote.*` entity also gets HA's
  `remote` services whose target matches it; of the allowlisted ones only turn_on, and only
  when it has fields left for the entity (`activity`, with ACTIVITY 4). They're always custom
  services (never typed, never in `skipped_services`), ahead of P's with `remote.send_command`
  first, so a long list of P's hints can't push it out of the 2048-byte budget. A YAML-only
  remote (no platform) gets only these. `ha::serviceDomains(entity_id, platform)` (`remote`
  first for a remote, then P) is the one rule: the `get_services` domains to parse, the order
  offered, and what `ha::isDeviceService` lets an action send. Translations are fetched for
  `remote` too.
- Field selectors map to typed params; a field with its own feature filter the entity doesn't
  meet is dropped:
  - `boolean` → `bool`
  - `number` with `min` and `max` → `int` (integral step and bounds) or `number` (`step: any`
    or fractional)
  - `select` (single, string or `{value}` options) → `enum` (options over the length limit, or
    past the count limit, are dropped and counted in `dropped_options`)
  - anything else (`text` is deferred, decided 2026-11-08; `entity`, `object`, `template`,
    `target`, `time`, multiple selects, ...) → the whole service is skipped, as it is for more
    than 4 fields, a field name that isn't a valid param name or names a target (`entity_id`,
    `area_id`, `device_id`, ...), or a service/platform name outside `^[a-z][a-z0-9_]{0,31}$`.
- A response-only service (`response: {optional: false}`, e.g. `sonos.get_queue`) is skipped:
  it only returns data, which Hubbo can't use.
- Skipped services are listed in the entity details' `skipped_services` as `{service, reason}`:
  `unsupported_field`, `too_many_fields`, `response_only` or `too_many_services`.
- **Names come from HA's translations** (2026-11-12). Real HA's `get_services` carries no
  `name`; the English strings come from WS `frontend/get_translations` `{language: en,
  category: services, integration: [P]}`, fetched with the details: `component.P.services.S.name`
  is the command's label and `....fields.F.name` the param's (48 chars at most). Without one, the
  description's own `name`, else the id made readable (`set_sleep_timer` → "Set sleep timer",
  `sleep_time` → "Sleep time"); a client never sees a raw id as a label. Command descriptions
  aren't kept.
- **Field descriptions** become the param's `description`, shown as a tooltip:
  `....fields.F.description` from the translations, else the field's own `description`.
  Whitespace is collapsed, and anything over 160 bytes is cut at a word boundary and ends in "…".
  Absent when HA has none. Stored in the snapshot and re-checked on load (about 4.6 KB across the
  29-device showcase).
- **Defaults and units.** A field's `default` is kept when it fits its type and range (a bool for
  `bool`, a number in range for `int`/`number`, integral for `int`, one of the options for
  `enum`), and shown as the param's `default`. A number's `unit_of_measurement` becomes its
  `unit`, shortened where there's a common form (`seconds` → `s`, `milliseconds` → `ms`,
  `minutes` → `min`, `hours` → `h`) and dropped if still over 16 chars. All of it is stored in
  the snapshot and re-checked when it's loaded (a default that doesn't fit is refused).
- Same safety rules as the allowlist: the target is always the registered entity, only declared
  fields can be sent, and nothing can override `entity_id`, `area_id` or `device_id`. (A Home
  Assistant action sends data the user wrote, but keeps the target rule.)
- **Discovery is lean** (Phase 4, decided 2026-11-09): `POST`/`GET
  /admin/integrations/ha_main/discover` reads the compact entity registry and all current states
  (about 0.8 MB, ~7 s on the board), never `get_services` (about 230 KB) or the full registry.
  Each candidate's `command_ids` is a preview from its domain alone. `GET` filters by `q`,
  `domain` and `available=true` (supported and not registered) before paging, so every page is
  full until the last.
- **The capabilities snapshot comes from the entity's details.** Picking an entity loads its
  details (`POST`/`GET /admin/integrations/ha_main/entities/{entity_id}/details`, about 2 s):
  fresh attributes (supported features, source and sound mode lists, colour modes, kelvin
  range, preset modes, available modes, options, humidity range), platform, `unique_id` and `get_services`, normalized into the snapshot with the offered
  services (at most 32). Registering (`POST /admin/devices`) or re-linking/refreshing (`PUT
  /admin/devices/{id}` with a `provider`) copies that snapshot into the device
  (`DeviceSpec.haCapabilities`, stored and backed up as `ha_capabilities`); without finished
  details for that entity from the current HA settings it's `409 details_required`. The device's commands are compiled from it, so they work offline and HA is
  never asked on the hot path. The API never accepts a snapshot from a client and never returns
  one; a stored snapshot is re-validated when it's loaded (`ha/snapshot_json.h`), so a
  hand-edited backup can't smuggle in a service of another integration or a target param.
- **Home Assistant action** (`call_service`, docs/design/ha-custom-action.md; the user's
  decision 2026-11-13, reversing "never free-text service calls"). The integration's services
  that match the entity but can't be typed (`unsupported_field` or `too_many_fields`, never
  `response_only`) are kept in the snapshot as `custom_services`, with HA's field hints (name,
  required, example as JSON of at most 64 bytes, translated description). A device with any gets
  the command `call_service` "Home Assistant action" (group `custom`), whose capabilities entry
  lists them as `services`. An action picks one and writes its `data` as a JSON object, sent
  verbatim (`ha::EntityCapabilities::buildCustomCall`, `ServiceCall::rawData`):
  - the target is always the device's entity; the service must be `^[a-z][a-z0-9_]{0,31}\.[a-z][a-z0-9_]{0,63}$`
    (`ha::isHaServiceId`), and its domain the snapshot's platform (`webostv.*` on a webOS TV),
    or on a remote also `remote` (`{"service": "remote.send_command", "data": {"command":
    "DPAD_UP"}}`); else `forbidden_param` at `.../call/service`, checked when core compiles the
    action. A device with no snapshot takes none, and one with no platform takes none unless
    it's a remote. One rule, `ha::isDeviceService(entity_id, platform, service)`, serves the
    compiler and the snapshot decoder (custom services). Any service of that integration passes,
    not only the listed ones; the UI offers the list;
  - `data`: at most 1024 bytes compact and 4 levels; no top-level `entity_id`, `device_id`,
    `area_id`, `floor_id`, `label_id` (`forbidden_param`); no `{{` or `{%` anywhere
    (`template_not_supported`: HA's call_service sends templates as plain text);
  - at most 16 such actions per config (`capacity_exceeded`);
  - hints: up to 16 services and 8 fields, 2048 bytes per device and 16384 per config
    (`ha::kMaxCustomHintBytes`, `kMaxConfigHintBytes`); registration and re-link keep what fits.
  An action can reach anything HA allows for a service of the device's own integration (and, on
  a remote, of the `remote` domain), so it's the one place the "only allowlisted calls" rule doesn't hold; security.md says so.
- **Hue rate.** Hue handles about one command per second (Harmonizer/HANDOFF.md). Bursts of
  `light.brightness_step` should be coalesced by the HA adapter's queue rather than sent one by
  one.
