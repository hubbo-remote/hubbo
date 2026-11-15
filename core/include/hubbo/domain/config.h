#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "hubbo/domain/params.h"
#include "hubbo/domain/smart_buttons.h"
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

// A Home Assistant action on a device (command "call_service",
// docs/design/ha-custom-action.md): one of the device's integration
// services, and its data, sent verbatim. Checked by
// config::decodeHaServiceCall().
struct HaServiceCallSpec {
    std::string service;      // "<domain>.<service>", e.g. "webostv.button"
    std::string data = "{}";  // a JSON object, compact (canonical) text
    bool operator==(const HaServiceCallSpec &o) const { return std::tie(service, data) == std::tie(o.service, o.data); }
    bool operator<(const HaServiceCallSpec &o) const { return std::tie(service, data) < std::tie(o.service, o.data); }
};
// A compiled call: owned by the compiled config or direct command that made
// it, and by any work still carrying it (executor, adapter), so it lives
// exactly as long as something can still send it.
using HaServiceCallRef = std::shared_ptr<const HaServiceCallSpec>;

struct ActionSpec {
    std::string integrationId;            // integration_id
    std::optional<std::string> deviceId;  // device_id; nullopt == JSON null
    std::string commandId;                // command_id
    std::vector<ParamInput> params;       // params (object, in any order)
    std::optional<HaServiceCallSpec> call;  // call_service only (required there)
};

struct BindingSpec {
    std::string trigger;  // "<button>:<event>"; validated, so kept as text
    std::vector<ActionSpec> actions;  // empty == unbound
};

// Smart buttons (docs/design/smart-buttons.md, domain/smart_buttons.h):
// each slot's device id, or none. What a slot does is derived from its
// device at compile (engine/smart_actions.h). A vector, not an array:
// ConfigSpec copies live on the HTTP task's stack.
struct SmartButtonsSpec {
    std::vector<std::optional<std::string>> slots =
        std::vector<std::optional<std::string>>(kSmartSlotCount);  // in SmartSlot order; nullopt == JSON null
    uint32_t revision = 0;

    std::optional<std::string> &at(SmartSlot slot) { return slots[static_cast<size_t>(slot)]; }
    const std::optional<std::string> &at(SmartSlot slot) const { return slots[static_cast<size_t>(slot)]; }
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
constexpr const char *kMqttType = "mqtt";

// The built-in BLE HID integration's id (always present, like "system").
constexpr const char *kBleIntegrationId = "ble";
// The built-in Home Assistant integration's id (always present; no URL, so
// not_configured, until its settings are set).
constexpr const char *kHomeAssistantIntegrationId = "ha_main";
// The built-in MQTT integration's id (always present, disabled until the
// admin turns it on): Hubbo as a device in HA (docs/design/mqtt-discovery.md).
constexpr const char *kMqttIntegrationId = "mqtt_main";

// Index of the item with this id in items[0, end), or -1. Cold path.
template <typename T>
int indexOfId(const std::vector<T> &items, std::string_view id, size_t end = static_cast<size_t>(-1)) {
    const size_t n = end < items.size() ? end : items.size();
    for (size_t i = 0; i < n; i++) {
        if (items[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// ---- device profiles (~/src/hubbo-devices/docs/spec.md) -------------------
// A Bluetooth device carries the profile it was added from (everything but
// `meta`), so any of its commands can be bound without the profile. Field
// names mirror the spec; optional fields stay optional so they round-trip.

enum class HidPage : uint8_t { Keyboard, Consumer };  // HID pages 0x07, 0x0C

// One command: a page and usage (limits::kHid*), optionally held.
struct ProfileCommand {
    std::string name;   // 1-32 letters and digits, unique ignoring case
    std::string label;  // "" when absent
    HidPage page = HidPage::Consumer;
    uint16_t usage = 0;
    std::optional<uint32_t> holdMs;  // hold_ms: a tap is a press this long
};

// A power or input step: a command (optionally held), or a delay.
struct ProfileStep {
    std::string command;             // "" for a delay step
    std::optional<uint32_t> holdMs;  // command steps only
    uint32_t delayMs = 0;            // delay steps only (1-60000)
};

// buttons: a remote button name -> a command, or null (leave it empty).
struct ProfileButton {
    std::string button;
    std::optional<std::string> command;
};

struct ProfilePower {
    std::string type;  // "discrete", "toggle" or "none"
    std::vector<ProfileStep> on, off, toggle;
    std::optional<uint32_t> onDelayMs;
};

struct ProfileTiming {
    std::optional<uint32_t> keyDelayMs;
    std::optional<uint32_t> inputDelayMs;
};

struct ProfileInput {
    std::string name;
    std::vector<ProfileStep> steps;
};

struct ProfileChannels {
    std::optional<uint8_t> digits;  // 1-4
    std::string start, finish;      // "" when absent
};

struct DeviceProfile {
    // Identity, as `profile: {id, revision, sha256}`.
    std::string id;       // "<source>:<slug>"
    uint32_t revision = 0;
    std::string sha256;   // 64 lowercase hex, of the profile as published
    // The profile itself, as top-level device fields.
    std::string manufacturer;
    std::string model;
    std::vector<ProfileCommand> commands;
    std::vector<ProfileButton> buttons;
    std::optional<ProfilePower> power;
    std::optional<ProfileTiming> timing;
    std::vector<ProfileInput> inputs;
    std::optional<ProfileChannels> channels;

    const ProfileCommand *findCommand(std::string_view name) const;  // exact name
};

// Where a device lives in its integration. `type` matches the integration
// type: "home_assistant" (entityId, optional uniqueId) or "ble_hid"
// (nothing else: the device is its own host, bonded by the radio).
struct ProviderSpec {
    std::string type;
    std::string entityId;
    std::string uniqueId;
};

struct DeviceSpec {
    std::string id;
    std::string name;
    std::string integrationId;  // integration_id
    std::string kind = "other"; // UI hint, one of kinds.json (config/profile.h)
    ProviderSpec provider;
    // home_assistant devices: what HA reported at registration or refresh;
    // the device's commands come from it (works while HA is offline).
    // Stored as `ha_capabilities`. Absent: the device has no commands.
    std::optional<ha::CapabilitySnapshot> haCapabilities;
    // ble_hid devices: the profile it was added from (required). Other
    // integrations: none.
    std::optional<DeviceProfile> profile;
    uint32_t revision = 0;
};

// The target an adapter executes against (never accepted from the API):
// the HA entity id, or the BLE peer ("bonded_host" when unnamed).
std::string providerRef(const DeviceSpec &device);

// Non-secret integration settings. The HA access token lives in protected
// storage, not here.
struct IntegrationSpec {
    std::string id;
    std::string type;  // "system", "ble_hid", "home_assistant", "mqtt"
    std::string name;
    bool enabled = true;
    std::string url;          // home_assistant
    bool verifyTls = true;    // home_assistant
    std::string deviceName;   // ble_hid: advertised name
    std::string host;         // mqtt: the broker, an IP or a hostname ("" = not configured)
    uint16_t port = 1883;     // mqtt
    std::string username;     // mqtt ("" = none; the password is a secret, not here)
    uint32_t revision = 0;
};

// The integrations' count once ensureBuiltinIntegrations() has added the
// missing built-ins (system, ble, ha_main, mqtt_main). Checked against
// limits::kMaxIntegrations, so a stored list can never grow past the cap.
size_t integrationCountWithBuiltins(const std::vector<IntegrationSpec> &integrations);

struct ConfigSpec;
// Adds the built-in integrations ("system", "ble", "ha_main", "mqtt_main") if they're missing, so
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
    SmartButtonsSpec smartButtons;
    uint32_t revision = 0;  // whole config; bumped by every saved change
};

// The remote's Off press as a trigger ("off:tap", what the web admin's
// "Press" writes).
constexpr const char *kOffTrigger = "off:tap";
// A fresh hub's config (bootHub()'s defaults on the board's first boot and
// after a factory reset): the built-in integrations, and the Off button
// bound globally to system.off. An ordinary binding the user can change or
// remove; a stored config is never touched by it.
ConfigSpec factoryDefaultConfig();

}  // namespace hubbo
