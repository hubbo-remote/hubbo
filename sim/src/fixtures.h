#pragma once

// Sample configuration shared by hub-sim and the tests: the family-room
// setup with two activities, "Watch Apple TV" and "Play Xbox", plus small
// builders for writing configs in C++ until the JSON config service exists.

#include <initializer_list>
#include <string>
#include <vector>

#include "hubbo/domain/config.h"
#include "hubbo/ha/capabilities.h"

namespace hubbo::sim {

ParamInput param(std::string name, ParamLiteral value);
ParamInput param(std::string name, const char *value);
ParamInput param(std::string name, int64_t value);
ParamInput param(std::string name, int value);
ParamInput param(std::string name, double value);
ParamInput param(std::string name, bool value);

ActionSpec action(std::string integration, std::string device, std::string command,
                  std::vector<ParamInput> params = {});
ActionSpec bleKey(const char *command, const char *key, const char *device = "dev_apple_tv");
ActionSpec haAction(const char *device, const char *command, std::vector<ParamInput> params = {});
ActionSpec waitMs(int64_t ms);
ActionSpec activateAction(const char *activityId);
ActionSpec offAction();
BindingSpec bind(std::string trigger, std::vector<ActionSpec> actions);
BindingSpec macro(std::string trigger, std::vector<ActionSpec> actions);  // a binding marked a macro
DeviceSpec device(std::string id, std::string name, std::string integration, std::string kind,
                  ProviderSpec provider);
DeviceSpec haDevice(std::string id, std::string name, std::string kind, std::string entityId);
// A Bluetooth device added from a built-in profile (default: the Apple TV).
DeviceSpec bleDevice(std::string id, std::string name, std::string kind, const char *profileId = "hubbo:apple-tv");
IntegrationSpec integration(std::string id, std::string type, std::string name);

// Devices: dev_apple_tv (ble), dev_lg_tv, dev_denon, dev_xbox, dev_lights
// (ha_main). Activities: act_watch_apple_tv, act_play_xbox.
// Global bindings (as in api/fixtures): off:tap -> system.off, tv:tap and
// movie:tap -> act_watch_apple_tv, music:tap -> act_play_xbox,
// light_1:tap / light_2:tap -> lights on/off.
ConfigSpec fixtureConfig();
// fixtureConfig() with no Bluetooth devices (and no activities or global
// bindings, which use them).
ConfigSpec fixtureConfigWithoutBluetooth();

// The Home Assistant entities behind the fixture's ha_main devices, as HA
// would describe them (feature bits, source lists), with the name and
// integration (registry platform) HA reports. FakeHomeAssistant builds their
// command catalogs with the real ha::EntityCapabilities; the fake HA server
// serves all of it.
struct FixtureHaEntity {
    ha::EntityInfo info;
    std::string name;
    std::string platform;
    std::string state;        // "" = the fake's default ("on" for a player, else "off")
    std::string deviceClass;  // "" = none
};
std::vector<FixtureHaEntity> fixtureHaEntities();
std::vector<ha::EntityInfo> fixtureEntities();  // fixtureHaEntities()' infos

}  // namespace hubbo::sim
