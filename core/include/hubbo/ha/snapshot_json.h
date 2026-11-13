#pragma once

#include <ArduinoJson.h>

#include <string>

#include "hubbo/engine/compiler.h"
#include "hubbo/ha/snapshot.h"

// The HaCapabilitySnapshot JSON (api/openapi.yaml), used for the stored
// device's `ha_capabilities` in the config record and in backups.
namespace hubbo::ha {

void encodeSnapshot(const CapabilitySnapshot &snapshot, JsonObject out);

// HA's `default` for a typed param: kept (hasDefault, defaultNumber or
// defaultOption set) only when it fits the type, range and options. The one
// rule for both parsing HA's services and decoding a stored snapshot.
bool applyDefault(JsonVariantConst value, SnapshotParam *param);

// Strict, like every stored object: unknown fields, wrong types, strings
// and lists over their bounds, bad names and inverted ranges are issues at
// `path` + JSON Pointer. True only without issues. Decodes in place into
// a fresh (default) `out`, with no copy on the HTTP task's small stack, so
// on false `out` holds a partial snapshot the caller drops. What the snapshot offers is re-checked
// again when the device's commands are built (EntityCapabilities::fromSnapshot).
bool decodeSnapshot(JsonVariantConst json, CapabilitySnapshot *out, ValidationReport *report,
                    const std::string &path);

}  // namespace hubbo::ha
