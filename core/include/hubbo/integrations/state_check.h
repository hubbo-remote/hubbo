#pragma once

#include <string_view>

#include "hubbo/ha/capabilities.h"

// A startup or shutdown step's state check (docs/design/state-checks.md):
// is the entity already in the state this call would set? A generic rule
// from what HA provides, with no per-device table:
//   - <domain>.turn_on: the state is "on", or a media player's playing,
//     paused, idle or buffering; <domain>.turn_off: the state is off or
//     standby. Any other state (a scene's timestamp, a climate's mode) runs
//     the step.
//   - every field the call sends equals the entity's attribute of the same
//     name (strings and bools exactly, numbers within a tiny tolerance).
//   - all or nothing: a field with no such attribute, an attribute that's a
//     list or object, any other service with no fields (a toggle, a step),
//     a Home Assistant action (raw data), or an unavailable or unknown
//     entity: false, the step runs.
// Integration task (it parses `entity`).
namespace hubbo::integrations {

// Whether the rule can ever say yes for `call` (a power service, or one
// that sends fields; never a Home Assistant action): else don't read.
bool checkable(const ha::ServiceCall &call);

// `entity`: HA's compressed state, {"s":<state>,"a":{<attributes>}}
// (HomeAssistantClient::readState). Only the state and the call's fields'
// attributes are parsed.
bool alreadySet(const ha::ServiceCall &call, std::string_view entity);

}  // namespace hubbo::integrations
