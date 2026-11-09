#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/domain/params.h"
#include "hubbo/ha/snapshot.h"

// Source (uncompiled) configuration model. Field names mirror the public
// JSON (api/openapi.yaml, ARCHITECTURE.md "Data model"):
//
//   Action       {integration_id, device_id|null, command_id, params}
//   Activity     {id, name, startup: Action[], shutdown: Action[],
//                 bindings: {"<button>:<event>": Action[]}, revision}
//   Device       {id, name, integration_id, kind, provider, revision}
//   Integration  {id, type, name, enabled, settings (non-secret), revision}
//
// These are plain cold-path structs (they allocate). config/json.h converts
// them to and from JSON; the compiler validates them and produces the
// allocation-free runtime tables (engine/compiled.h). Nothing here is ever
// executed directly. Secrets (HA access token, BLE bonds) are never here.
namespace hubbo {

struct ActionSpec {
    std::string integrationId;            // integration_id
    std::optional<std::string> deviceId;  // device_id; nullopt == JSON null
    std::string commandId;                // command_id
    std::vector<ParamInput> params;       // params (object, in any order)
};

struct BindingSpec {
    std::string trigger;  // "<button>:<event>"; validated, so kept as text
    std::vector<ActionSpec> actions;  // empty == unbound
};

struct ActivitySpec {
    std::string id;
    std::string name;
    std::vector<ActionSpec> startup;
    std::vector<ActionSpec> shutdown;
    std::vector<BindingSpec> bindings;
    uint32_t revision = 0;
};

// Integration types (IntegrationSpec::type, ProviderSpec::type).
constexpr const char *kSystemType = "system";
constexpr const char *kBleHidType = "ble_hid";
constexpr const char *kHomeAssistantType = "home_assistant";

// The built-in BLE HID integration's id (always present, like "system").
constexpr const char *kBleIntegrationId = "ble";
// The built-in Home Assistant integration's id (always present; no URL, so
// not_configured, until its settings are set).
constexpr const char *kHomeAssistantIntegrationId = "ha_main";

// Index of the item with this id in items[0, end), or -1. Cold path.
template <typename T>
int indexOfId(const std::vector<T> &items, std::string_view id, size_t end = static_cast<size_t>(-1)) {
    const size_t n = end < items.size() ? end : items.size();
    for (size_t i = 0; i < n; i++) {
        if (items[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// Where a device lives in its integration. `type` matches the integration
// type: "home_assistant" (entityId, optional uniqueId) or "ble_hid"
// (optional peerName).
struct ProviderSpec {
    std::string type;
    std::string entityId;
    std::string uniqueId;
    std::string peerName;
};

struct DeviceSpec {
    std::string id;
    std::string name;
    std::string integrationId;  // integration_id
    std::string kind = "other"; // UI hint: tv, receiver, speaker, streamer, console, light, switch, other
    ProviderSpec provider;
    // home_assistant devices: what HA reported at registration or refresh;
    // the device's commands come from it (works while HA is offline).
    // Stored as `ha_capabilities`. Absent: the device has no commands.
    std::optional<ha::CapabilitySnapshot> haCapabilities;
    uint32_t revision = 0;
};

// The target an adapter executes against (never accepted from the API):
// the HA entity id, or the BLE peer ("bonded_host" when unnamed).
std::string providerRef(const DeviceSpec &device);

// Non-secret integration settings. The HA access token lives in protected
// storage, not here.
struct IntegrationSpec {
    std::string id;
    std::string type;  // "system", "ble_hid", "home_assistant"
    std::string name;
    bool enabled = true;
    std::string url;          // home_assistant
    bool verifyTls = true;    // home_assistant
    std::string deviceName;   // ble_hid: advertised name
    uint32_t revision = 0;
};

struct ConfigSpec;
// Adds the built-in integrations ("system", "ble", "ha_main") if they're missing, so
// config.integrations is always complete. Idempotent.
void ensureBuiltinIntegrations(ConfigSpec *config);

struct ConfigSpec {
    std::vector<IntegrationSpec> integrations;
    std::vector<DeviceSpec> devices;
    std::vector<ActivitySpec> activities;
    // Bindings that apply whatever activity is active (including none), when
    // the active activity does not bind the same trigger. This is where the
    // remote's activity buttons and Off live (system.activate / system.off).
    std::vector<BindingSpec> globalBindings;
    uint32_t globalBindingsRevision = 0;
    uint32_t revision = 0;  // whole config; bumped by every saved change
};

}  // namespace hubbo
