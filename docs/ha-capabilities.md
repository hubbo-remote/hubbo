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

## Deliberately not offered

- **toggle** (media_player, light, switch): the architecture prefers explicit on/off. Toggles
  break as soon as the hub and the device disagree.
- **play_media, browse/search, announce, enqueue**: take free-form content IDs or URLs.
- **join / unjoin** (GROUPING): take other entity IDs.
- **media_seek, clear_playlist**: no remote use, and clear_playlist is destructive.
- **light effect, flash, rgb/hs/xy colour**: possible later. Effect would be an enum from
  `effect_list`; colours need array params, which the param model doesn't have.
- **Integration-specific services** (`webostv.button`, `sonos.*`, `denonavr.*`): not hand-curated.
  They're resolved from HA's own service metadata; see below.
- **Domains** remote, scene, script, cover, fan, button, input_*, group, automation and
  everything else: no commands until added here with tests.
  - `remote.send_command` takes arbitrary command strings. If it's ever added it needs a
    per-device allowlist of command names chosen by the admin.
  - `scene.turn_on` and `script.turn_on` are entity-specific, but a scene or script can do
    anything in the house, so registering one should be a deliberate admin choice. They're the
    likely next additions.
  - `cover` (OPEN 1, CLOSE 2, SET_POSITION 4, STOP 8, ...) and `fan` (SET_SPEED 1, ...) are
    straightforward when needed. Verify their bits against HA's source at that time.

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
   normalization. Options over 32 bytes (`limits::kMaxParamStringLength`), empty or duplicate
   options, and anything past 128 are dropped and counted (`droppedOptions()`). A picker with no
   usable options is dropped.

The device registry is an application boundary. The HA token Hubbo holds can still do anything
that token's HA user can do (see docs/security.md).

## Integration with the core

- The HA adapter keeps one `EntityCapabilities` per registered HA device and returns
  `catalog()` from `IntegrationAdapter::commandsFor()`. The catalog points into the object, so
  it must outlive any compiled config that uses it. Rebuild it when HA reports new attributes
  (e.g. a changed `source_list`), then recompile the config.
- Omitted optional params: core `resolveParams()` fills them with `defaultValue`. HA optional
  params use an out-of-range default (`kOmittedInt` = INT32_MIN, or NaN) that
  `buildServiceCall()` treats as "leave out". A user can't send those values because they're
  outside every accepted range.
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

## Integration-specific services (decided 2026-11-08, not built yet)

Resolve them from Home Assistant rather than curating per integration. Never take free-text
service calls: the user asked for that as a fallback, but HA's metadata covers the need.

- `get_services` (WebSocket) returns every service with its `target` filter and its `fields`
  with selectors. For example, `webostv.button` targets `entity: {integration: webostv, domain:
  media_player}`, and its `button` field is a `select` selector listing `INFO`, `HOME`, ....
- `config/entity_registry/get` gives an entity's `platform` (integration), e.g. `webostv`.
- For a registered entity, offer each non-core-domain service whose target filter matches its
  integration, domain and supported features. Map field selectors to typed params:
  - `select` with options → `enum`
  - `number` → `int` or `number` with min/max/step
  - `boolean` → `bool`
  - `text` → a bounded string param
  - `entity`, `object`, `template`, `target` and other selector types → skip the service.
- Same safety rules as the allowlist: the target is always the registered entity, only declared
  fields can be sent, and nothing can override `entity_id`, `area_id` or `device_id`.
- The capability list is fetched when a device is registered or refreshed and stored with the
  device, so it's available offline and never looked up on the hot path. The core needs a string
  param type for `text` selectors.
- **Hue rate.** Hue handles about one command per second (Harmonizer/HANDOFF.md). Bursts of
  `light.brightness_step` should be coalesced by the HA adapter's queue rather than sent one by
  one.
