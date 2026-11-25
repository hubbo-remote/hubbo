# A Home Assistant action on a device

Hubbo's typed commands cover the common services of each HA domain ([ha-domains.md](ha-domains.md)).
Plenty of useful services sit outside them: an integration's own services, such as
`webostv.button` with `{"button": "INFO"}`, or a remote's `remote.send_command`. The Home Assistant
action reaches those, scoped to one registered device.

## Rules

- **Device-scoped only.** The action targets a registered HA device. There's no device-less form, and no
  free-text service name. Scripts and scenes are supported natively as devices.
- **Pick, don't type.** The service is chosen from the options presented for that device. Those are the
  callable services the hub doesn't offer as typed commands, read from HA's `get_services` at
  registration or re-link. They're stored in the device's HA snapshot as `custom_services` and served
  on the `call_service` command's `services` in capabilities, with HA's own field hints (example,
  description). Only the data is typed, as JSON, pre-filled from HA's example.
- **Anyone authenticated can set it up.** The admin or an API token, the same as other bindings and
  activities. The docs say plainly that this can reach any service the device offers.

## Which services a device offers

- **Its integration's services** (its platform: `webostv.*` only on a webOS TV, `sonos.*` on a
  Sonos), for any domain. A device with no platform (a YAML-only entity) gets none of these.
- **A remote's own domain.** A `remote.*` device also offers HA's `remote` services that match it:
  `remote.send_command` (listed first), `remote.learn_command`, `remote.delete_command`, and
  `remote.turn_on` with an `activity`.
- Only services whose target matches the entity (domain, integration and supported features) are
  offered, and response-only services are left out. Older `services.yaml` entries with no `target`,
  only an `entity_id` field with an entity selector (webostv, some sonos services), count that field
  as the target; that's what makes webOS services show at all.

## Shape

The action is a pseudo-command of the HA integration. The Action has an optional `call` field, which
is allowed only with this command:

```json
{"integration_id": "ha_main", "device_id": "dev_tv", "command_id": "call_service", "params": {},
 "call": {"service": "webostv.button", "data": {"button": "INFO"}}}
```

- `device_id` is required, and it must be an HA device. The target is that device's entity, with the
  usual rename following, stale refusal and "Used by".
- **`service`** matches `^[a-z][a-z0-9_]{0,31}\.[a-z][a-z0-9_]{0,63}$`, and its domain must be one the
  device offers (above). Anything else, such as `homeassistant.stop`, is 422 `forbidden_param` at
  `.../call/service`.
- **`data`** is a JSON object:
  - at most 1024 bytes serialized, nested at most 4 levels (the data object is level 1, and each
    nested object or array adds a level);
  - no top-level target keys (`entity_id`, `device_id`, `area_id`, `floor_id`, `label_id`), refused
    with 422 `forbidden_param`;
  - no `{{` or `{%`, refused with 422 `template_not_supported`;
  - `{}` by default.
- The data is sent verbatim with `call_service` and no `return_response`. Every call is momentary:
  one press sends one call.
- Validation lives in core's config decode (`config::decodeHaServiceCall`) and compile, so it also
  covers backup and restore. The server checks the service's shape and domain; restricting it to the
  picker's exact choices is the UI's job.
- The call is owned by the compiled config or the direct execute that made it, shared with the
  executor and adapter while in flight, and freed with them. There is no global store, and the engine
  task never allocates for it.

## Limits

- At most 16 actions with a `call` per config; one more is 422 `capacity_exceeded` at `.../call`.
- Service hints: at most 16 services of 8 fields each, 2048 bytes per device and 16384 per config.
  Registration and re-link trim a new device's hints to what's left, so adding a device never fails
  over hints. A config over the total is refused with `capacity_exceeded` at
  `/devices/N/ha_capabilities/custom_services`.

## In the web admin

"Home Assistant action…" under More in an HA device's command picker: a service select over the
command's `services` (no free text) and a JSON data box, pre-filled from the hints and checked as you
type. Test and Apply wait until the call is valid. The binding reads "webostv.button · button: INFO".
