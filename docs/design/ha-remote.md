# Home Assistant remotes

Approved by the user on 2026-11-14. Adds HA's `remote` entity domain, the same way cover, button and
humidifier were added. Nothing else changes.

## What the user decided

- **`remote` is just another domain.** Any `remote.*` entity can be adopted as its own device,
  whatever the integration behind it.
- **Nothing else changes:** no new param types, no change to other domains, to media players or to
  integration services (`webostv.*`, `denonavr.*`, ...), and no change to the Home Assistant action's
  rules for other devices.
- **Built for everyone's HA, not one setup.** Only what HA provides is used, with nothing made up.

## How a remote works

- **Domain:** `remote` joins the entity domains. Its suggested kind is `other`.
- **Typed commands** (the curated allowlist, from HA's `remote/services.yaml`): `remote.turn_on`,
  `remote.turn_off` and `remote.toggle`, with no params. `turn_on`'s `activity` field is text, which
  Hubbo doesn't type, so it isn't a param here.
- **Everything else goes through the Home Assistant action** (`call_service`), exactly as the LG's
  `webostv.button` does. That covers `remote.send_command`, `remote.learn_command`,
  `remote.delete_command`, and `remote.turn_on` with an `activity`, for example
  `{"service": "remote.send_command", "data": {"command": "DPAD_UP"}}`.
  - These are HA's `remote` services that match the entity, read from HA's `get_services` at
    adoption. They're stored as custom services with HA's own field hints (example, description),
    like any integration's services.
  - The remote's own integration's services (the platform's) are offered too, the same way as for
    media players, lights, switches and fans.
- **The one rule change:** the Home Assistant action accepts a service only from the device's
  integration (its platform). On a `remote.*` device, it also accepts services from the `remote`
  domain. A YAML-only remote has no platform, so it gets only the `remote` domain's services.
- **Press behaviour:** every remote command is momentary. One press sends one call.

## Owners

- **api:** the `remote` domain, its traits and allowlist (`ha/` capabilities); the `remote`
  domain's services as custom services on a remote device (`ha/` services, snapshot); the spec,
  fixtures and contract cases (a remote device; `remote.send_command` through the Home Assistant
  action); docs/ha-capabilities.md and docs/agent-api.md.
- **core:** the compiler's service check (`serviceOnDevice`) accepting `remote.*` on a remote device;
  a remote entity in the fake HA, with HA's real `remote` services in its `get_services`; a remote
  in the sim showcase.
- **frontend:** remote entities offered when adding an HA device, and the mock; an e2e test that
  adds a remote and binds `remote.send_command` through the Home Assistant action.
- **lead:** this doc and IMPLEMENTATION_PLAN.md.
