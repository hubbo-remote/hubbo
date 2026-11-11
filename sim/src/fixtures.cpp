#include "fixtures.h"

#include <algorithm>

#include "hubbo/domain/limits.h"

namespace hubbo::sim {

ParamInput param(std::string name, ParamLiteral value) { return {std::move(name), std::move(value)}; }
ParamInput param(std::string name, const char *value) { return {std::move(name), ParamLiteral::ofString(value)}; }
ParamInput param(std::string name, int64_t value) { return {std::move(name), ParamLiteral::ofInt(value)}; }
ParamInput param(std::string name, int value) { return {std::move(name), ParamLiteral::ofInt(value)}; }
ParamInput param(std::string name, double value) { return {std::move(name), ParamLiteral::ofNumber(value)}; }
ParamInput param(std::string name, bool value) { return {std::move(name), ParamLiteral::ofBool(value)}; }

ActionSpec action(std::string integration, std::string device, std::string command,
                  std::vector<ParamInput> params) {
    ActionSpec a;
    a.integrationId = std::move(integration);
    if (!device.empty()) a.deviceId = std::move(device);
    a.commandId = std::move(command);
    a.params = std::move(params);
    return a;
}

ActionSpec bleKey(const char *command, const char *key, const char *device) {
    return action("ble", device, command, {param("key", key)});
}

ActionSpec haAction(const char *device, const char *command, std::vector<ParamInput> params) {
    return action("ha_main", device, command, std::move(params));
}

ActionSpec waitMs(int64_t ms) { return action("system", "", "wait", {param("duration_ms", ms)}); }

ActionSpec activateAction(const char *activityId) {
    return action("system", "", "activate", {param("activity_id", activityId)});
}

ActionSpec offAction() { return action("system", "", "off"); }

BindingSpec bind(std::string trigger, std::vector<ActionSpec> actions) {
    return {std::move(trigger), std::move(actions)};
}

DeviceSpec device(std::string id, std::string name, std::string integration, std::string kind,
                  ProviderSpec provider) {
    DeviceSpec d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.integrationId = std::move(integration);
    d.kind = std::move(kind);
    d.provider = std::move(provider);
    d.revision = 1;
    return d;
}

DeviceSpec haDevice(std::string id, std::string name, std::string kind, std::string entityId) {
    return device(std::move(id), std::move(name), "ha_main", std::move(kind),
                  {"home_assistant", std::move(entityId), ""});
}

DeviceSpec bleDevice(std::string id, std::string name, std::string kind) {
    return device(std::move(id), std::move(name), "ble", std::move(kind), {"ble_hid", "", ""});
}

IntegrationSpec integration(std::string id, std::string type, std::string name) {
    IntegrationSpec i;
    i.id = std::move(id);
    i.type = std::move(type);
    i.name = std::move(name);
    i.revision = 1;
    return i;
}

namespace {

// Held remote button = held HID key, exactly as long as the button.
void holdKey(std::vector<BindingSpec> &out, const char *button, const char *key) {
    out.push_back(bind(std::string(button) + ":down", {bleKey("key_down", key)}));
    out.push_back(bind(std::string(button) + ":up", {bleKey("key_up", key)}));
}

ActivitySpec watchAppleTv() {
    ActivitySpec a;
    a.id = "act_watch_apple_tv";
    a.name = "Watch Apple TV";
    a.revision = 1;
    a.startup = {
        haAction("dev_lg_tv", "media_player.turn_on"),
        haAction("dev_denon", "media_player.turn_on"),
        waitMs(1000),
        haAction("dev_lg_tv", "media_player.select_source", {param("source", "HDMI 1")}),
        haAction("dev_denon", "media_player.select_source", {param("source", "Media Player")}),
    };
    a.shutdown = {
        haAction("dev_lg_tv", "media_player.turn_off"),
        haAction("dev_denon", "media_player.turn_off"),
    };
    holdKey(a.bindings, "up", "ARROW_UP");
    holdKey(a.bindings, "down", "ARROW_DOWN");
    holdKey(a.bindings, "left", "ARROW_LEFT");
    holdKey(a.bindings, "right", "ARROW_RIGHT");
    holdKey(a.bindings, "select", "SELECT");
    a.bindings.push_back(bind("menu:tap", {bleKey("key_tap", "MENU")}));
    a.bindings.push_back(bind("back:tap", {bleKey("key_tap", "AC_HOME")}));
    a.bindings.push_back(bind("play:tap", {bleKey("key_tap", "PLAY_PAUSE")}));
    a.bindings.push_back(bind("pause:tap", {bleKey("key_tap", "PLAY_PAUSE")}));
    a.bindings.push_back(bind("info:hold", {bleKey("key_tap", "KEY_I")}));  // family room: Info held = i
    a.bindings.push_back(bind("volume_up:down", {haAction("dev_denon", "media_player.volume_up")}));
    a.bindings.push_back(bind("volume_down:down", {haAction("dev_denon", "media_player.volume_down")}));
    a.bindings.push_back(
        bind("mute:tap", {haAction("dev_denon", "media_player.volume_mute", {param("muted", true)})}));
    return a;
}

ActivitySpec playXbox() {
    ActivitySpec a;
    a.id = "act_play_xbox";
    a.name = "Play Xbox";
    a.revision = 1;
    a.startup = {
        haAction("dev_xbox", "media_player.turn_on"),
        haAction("dev_lg_tv", "media_player.turn_on"),
        haAction("dev_denon", "media_player.turn_on"),
        waitMs(2000),
        haAction("dev_lg_tv", "media_player.select_source", {param("source", "HDMI 2")}),
        haAction("dev_denon", "media_player.select_source", {param("source", "Game")}),
    };
    a.shutdown = {
        haAction("dev_xbox", "media_player.turn_off"),
        haAction("dev_lg_tv", "media_player.turn_off"),
        haAction("dev_denon", "media_player.turn_off"),
    };
    a.bindings.push_back(bind("volume_up:down", {haAction("dev_denon", "media_player.volume_up")}));
    a.bindings.push_back(bind("volume_down:down", {haAction("dev_denon", "media_player.volume_down")}));
    a.bindings.push_back(
        bind("mute:tap", {haAction("dev_denon", "media_player.volume_mute", {param("muted", true)})}));
    return a;
}

}  // namespace

ConfigSpec fixtureConfig() {
    ConfigSpec c;
    c.revision = 1;
    c.globalBindingsRevision = 1;
    c.integrations = {
        integration("system", "system", "System"),
        integration("ble", "ble_hid", "Bluetooth"),
        integration("ha_main", "home_assistant", "Home Assistant"),
    };
    c.integrations[1].deviceName = "Hubbo";
    c.integrations[2].url = "http://homeassistant.local:8123";
    c.devices = {
        bleDevice("dev_apple_tv", "Family Room Apple TV", "streamer"),
        haDevice("dev_lg_tv", "Family Room LG TV", "tv", "media_player.family_room_lg_tv"),
        haDevice("dev_denon", "Family Room Denon", "receiver", "media_player.denon"),
        haDevice("dev_xbox", "Xbox", "console", "media_player.xbox"),
        haDevice("dev_lights", "Family Room Lights", "light", "light.family_room"),
    };
    c.activities = {watchAppleTv(), playXbox()};
    c.globalBindings = {
        bind("off:tap", {offAction()}),
        bind("tv:tap", {activateAction("act_watch_apple_tv")}),
        bind("movie:tap", {activateAction("act_watch_apple_tv")}),
        bind("music:tap", {activateAction("act_play_xbox")}),
        bind("light_1:tap", {haAction("dev_lights", "light.turn_on")}),
        bind("light_2:tap", {haAction("dev_lights", "light.turn_off")}),
    };
    return c;
}

ConfigSpec fixtureConfigWithoutBluetooth() {
    ConfigSpec c = fixtureConfig();
    c.activities.clear();
    c.globalBindings.clear();
    c.devices.erase(std::remove_if(c.devices.begin(), c.devices.end(),
                                   [](const DeviceSpec &d) { return d.provider.type == kBleHidType; }),
                    c.devices.end());
    return c;
}

std::vector<FixtureHaEntity> fixtureHaEntities() {
    // The union of the entities behind fixtureConfig() and the shared API
    // dataset (api/fixtures/admin/backup.json); features and source lists
    // match api/fixtures/devices/*.capabilities.json.
    namespace mp = ha::media_player_feature;
    const uint32_t power = mp::kTurnOn | mp::kTurnOff;
    const uint32_t transport = mp::kPlay | mp::kPause | mp::kNextTrack | mp::kPreviousTrack;
    auto entity = [](std::string id, uint32_t features, std::vector<std::string> sources = {}) {
        ha::EntityInfo e;
        e.entityId = std::move(id);
        e.supportedFeatures = features;
        e.sourceList = std::move(sources);
        return e;
    };

    std::vector<FixtureHaEntity> out;
    out.push_back({entity("media_player.family_room_lg_tv",
                          power | mp::kVolumeStep | mp::kVolumeMute | transport | mp::kStop | mp::kSelectSource,
                          {"HDMI 1", "HDMI 2", "HDMI 3", "Live TV", "Netflix", "YouTube"}),
                   "Family Room LG TV", "webostv"});
    out.push_back({entity("media_player.xbox", power | mp::kVolumeStep | mp::kVolumeMute | transport), "Xbox", "xbox"});
    // fixtureConfig()'s receiver.
    out.push_back({entity("media_player.denon",
                          power | mp::kVolumeSet | mp::kVolumeStep | mp::kVolumeMute | mp::kSelectSource,
                          {"Media Player", "Game", "TV Audio"}),
                   "Denon AVR-X3700H", "denonavr"});
    // The API dataset's receiver and speaker.
    ha::EntityInfo denon = entity("media_player.family_room_denon",
                                  power | mp::kVolumeSet | mp::kVolumeStep | mp::kVolumeMute | mp::kSelectSource |
                                      mp::kSelectSoundMode,
                                  {"Blu-ray", "CBL/SAT", "DVD", "Game", "Media Player", "TV Audio", "Tuner", "Bluetooth"});
    denon.soundModeList = {"MUSIC", "MOVIE", "GAME", "AUTO", "STEREO", "DIRECT"};
    out.push_back({denon, "Family Room Denon", "denonavr"});
    out.push_back({entity("media_player.family_room_sonos",
                          mp::kVolumeSet | mp::kVolumeStep | mp::kVolumeMute | transport | mp::kStop |
                              mp::kSelectSource | mp::kShuffleSet | mp::kRepeatSet,
                          {"TV"}),
                   "Family Room Sonos", "sonos"});
    // Only used by limit tests: an enum option of the maximum length.
    out.push_back({entity("media_player.limits_test", power | mp::kSelectSource,
                          {std::string(limits::kMaxParamStringLength, 's'), "Short"}),
                   "Limits Test", "demo"});
    ha::EntityInfo lights = entity("light.family_room", ha::light_feature::kTransition);
    lights.supportedColorModes = {"color_temp"};
    out.push_back({lights, "Family Room Lights", "hue"});
    return out;
}

std::vector<ha::EntityInfo> fixtureEntities() {
    std::vector<ha::EntityInfo> out;
    for (FixtureHaEntity &e : fixtureHaEntities()) out.push_back(std::move(e.info));
    return out;
}

}  // namespace hubbo::sim
