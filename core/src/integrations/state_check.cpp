#include "hubbo/integrations/state_check.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>

namespace hubbo::integrations {

namespace {

bool isOff(std::string_view state) { return state == "off" || state == "standby"; }

// Only real power states count as on: a scene's state is when it last ran,
// a climate's is its mode; anything but these runs the step.
bool isOn(std::string_view domain, std::string_view state) {
    if (state == "on") return true;
    return domain == "media_player" &&
           (state == "playing" || state == "paused" || state == "idle" || state == "buffering");
}

bool equal(const ha::ServiceValue &value, JsonVariantConst attribute) {
    switch (value.kind) {
        case ha::ServiceValue::Kind::Bool:
            return attribute.is<bool>() && attribute.as<bool>() == value.boolean;
        case ha::ServiceValue::Kind::String:
            return attribute.is<const char *>() && value.string == attribute.as<const char *>();
        case ha::ServiceValue::Kind::Int:
        case ha::ServiceValue::Kind::Number: {
            if (attribute.is<bool>() || !(attribute.is<double>() || attribute.is<long long>())) return false;
            const double want = value.kind == ha::ServiceValue::Kind::Int ? value.integer : value.number;
            const double have = attribute.as<double>();
            // HA rounds some values: equal within a tiny tolerance.
            return std::fabs(want - have) <= 1e-6 * std::max({1.0, std::fabs(want), std::fabs(have)});
        }
    }
    return false;
}

}  // namespace

bool checkable(const ha::ServiceCall &call) {
    if (!call.rawData.empty()) return false;  // a Home Assistant action: never checked
    return call.service == "turn_on" || call.service == "turn_off" || !call.data.empty();
}

bool alreadySet(const ha::ServiceCall &call, std::string_view entity) {
    if (!checkable(call)) return false;  // no state it sets (a toggle, a step)
    // Only the state and the attributes the fields name: a receiver's
    // attributes run to KBs (source lists).
    JsonDocument filter;
    filter["s"] = true;
    for (const ha::ServiceField &field : call.data) filter["a"][field.key] = true;
    JsonDocument doc;
    if (deserializeJson(doc, entity.data(), entity.size(), DeserializationOption::Filter(filter))) return false;
    const std::string_view state = doc["s"] | "";
    if (state.empty() || state == "unavailable" || state == "unknown") return false;
    if (call.service == "turn_on" && !isOn(call.domain, state)) return false;
    if (call.service == "turn_off" && !isOff(state)) return false;
    JsonObjectConst attributes = doc["a"];
    for (const ha::ServiceField &field : call.data) {
        if (!equal(field.value, attributes[field.key])) return false;
    }
    return true;
}

}  // namespace hubbo::integrations
