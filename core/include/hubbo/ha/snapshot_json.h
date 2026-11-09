#pragma once

#include <ArduinoJson.h>

#include <string>

#include "hubbo/engine/compiler.h"
#include "hubbo/ha/snapshot.h"

// The HaCapabilitySnapshot JSON (api/openapi.yaml), used for the stored
// device's `ha_capabilities` in the config record and in backups.
namespace hubbo::ha {

void encodeSnapshot(const CapabilitySnapshot &snapshot, JsonObject out);

// Strict, like every stored object: unknown fields, wrong types, strings
// and lists over their bounds, bad names and inverted ranges are issues at
// `path` + JSON Pointer. True (and `out` filled) only without issues. What
// the snapshot offers is re-checked again when the device's commands are
// built (EntityCapabilities::fromSnapshot).
bool decodeSnapshot(JsonVariantConst json, CapabilitySnapshot *out, ValidationReport *report,
                    const std::string &path);

}  // namespace hubbo::ha
