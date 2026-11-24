#pragma once

// A profile's device fields (manufacturer, model, commands, buttons, power,
// timing, inputs, channels), read from and written to the object that holds
// them: a stored device or a profile document. Internal to config.

#include <string>

#include "json_fields.h"

namespace hubbo::config::detail {

// Reads the fields through `f` (so the caller's unknown-field check covers
// them), with the platform's command shape (ir: Pronto codes, else
// Bluetooth's page and usage). `out` keeps its id, revision and sha256.
bool decodeProfileFields(Fields &f, bool ir, DeviceProfile *out, ValidationReport *report);
void encodeProfileFields(const DeviceProfile &profile, bool ir, JsonObject out, bool withCommands = true);

// `profile: {id, revision, sha256}` on a stored device.
bool decodeProfileRef(JsonVariantConst json, DeviceProfile *out, ValidationReport *report, const std::string &path);
void encodeProfileRef(const DeviceProfile &profile, JsonObject out);

}  // namespace hubbo::config::detail
