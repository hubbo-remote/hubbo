# A Home Assistant action on a device

Approved by the user on 2026-11-13. This is an escape hatch for HA services Hubbo doesn't type, such as
`webostv.button` with `{"button": "INFO"}`. The design comes from hubbo-api's investigation (see
docs/team/hubbo-api.md). Remote-key tables per integration are a later phase.

## What the user decided

- **Device-scoped only.** The action targets a registered HA device. There's no device-less form, and no
  free-text service name. Scripts and scenes are already supported natively.
- **Pick, don't type.** The service is chosen from the options presented for that device. Those are the
  callable integration services the hub doesn't offer as typed commands. They're stored in the device's
  HA snapshot as `custom_services` at registration or re-link, and served on the `call_service`
  command's `services` in capabilities, with field hints. Only the data is typed, as JSON, pre-filled
  from HA's example. An existing device gets them after one re-link.
- **Anyone authenticated can set it up.** The admin or an API token, the same as other bindings and
  activities, with no new auth rule. The docs say plainly that this can reach any service the device
  offers.
- This reverses the earlier documented rule "never free-text service calls".

## Shape

The action is a pseudo-command of the HA integration. The Action gains an optional `call` field, which
is allowed only with this command:

```json
{"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "call_service", "params": {},
 "call": {"service": "webostv.button", "data": {"button": "INFO"}}}
```

- `device_id` is required, and it must be an HA device. The target is that device's entity, with the
  usual rename following, stale refusal and "Used by".
- **`service`** matches `^[a-z][a-z0-9_]{0,31}\.[a-z][a-z0-9_]{0,63}$`, and its domain must be the
  device's own HA integration (its snapshot platform: `webostv.*` only on a webOS TV). Anything else,
  such as `homeassistant.stop`, is 422 `forbidden_param` at `.../call/service`. A device with no
  platform accepts no call.
- **`data`** is a JSON object:
  - at most 1024 bytes serialized, nested at most 4 levels;
  - no top-level target keys (`entity_id`, `device_id`, `area_id`, `floor_id`, `label_id`), refused
    with 422 `forbidden_param`;
  - no `{{` or `{%`, refused with 422 `template_not_supported`;
  - `{}` by default.
- Depth counts the data object as level 1, and each nested object or array adds a level.
- The data is sent verbatim with `call_service` and no `return_response`.
- Validation lives in core's config decode (`config::decodeHaServiceCall`) and compile, so it also
  covers backup and restore. The server checks the service's shape and domain. Restricting it to the
  picker's exact choices is the UI's job.
- The call is owned by the compiled config or the direct execute that made it, shared with the
  executor and adapter while in flight, and freed with them. There is no global store.
- Older `services.yaml` entries with no `target`, only an `entity_id` field with an entity selector
  (webostv, some sonos services), count that field as the target. This is what makes webOS services
  show at all.

## Limits

- At most 16 actions with a `call` per config; one more is 422 `capacity_exceeded` at `.../call`.
- Service hints: at most 16 services of 8 fields each, 2048 bytes per device and 16384 per config.
  Registration and re-link trim a new device's hints to what's left, so adding a device never fails
  over hints. A config over the total is refused with `capacity_exceeded` at
  `/devices/N/ha_capabilities/custom_services`.

## Owners

- **core:**
  - ActionSpec `call` (service, plus data as canonical JSON text), with decode, encode and the rules
    above;
  - compile;
  - the engine carrying the call to the adapter without allocating on the engine task;
  - the adapter's `call_service` with raw data, keeping the stale check;
  - the fake HA accepting any service and recording `last_call`;
  - the size budgets (actions and hint total).
- **api:**
  - `Action.call` in the spec, with its errors and the executeCommand body;
  - `call_service` "Home Assistant action" in an HA device's catalog (group `custom`);
  - `custom_services` in the snapshot, and `services` on `call_service` in capabilities (at most 16
    services and 8 fields each, response-only services left out);
  - fixtures, including an LG `webostv.button` INFO binding;
  - contract cases for bind, execute and `last_call`, plus the 422s;
  - doc fixes: ha-capabilities.md, agent-api.md and security.md.
- **frontend:**
  - "Home Assistant action…" under More in an HA device's command picker;
  - a service select over the command's `services` (no free text) and a JSON data box, pre-filled
    from the hints and checked as you type, inline in the command picker (not a nested dialog);
    Test and Apply wait until the call is valid;
  - the label "webostv.button · button: INFO";
  - the mock and an e2e test.
