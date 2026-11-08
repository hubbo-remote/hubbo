#include "fixtures.h"

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
    c.devices = {
        {"dev_apple_tv", "Family Room Apple TV", "ble", "bonded_host"},
        {"dev_lg_tv", "Family Room LG TV", "ha_main", "media_player.family_room_lg_tv"},
        {"dev_denon", "Family Room Denon", "ha_main", "media_player.denon"},
        {"dev_xbox", "Xbox", "ha_main", "media_player.xbox"},
        {"dev_lights", "Family Room Lights", "ha_main", "light.family_room"},
        {"dev_projector", "Projector", "ir", "codeset_placeholder"},
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

std::vector<ha::EntityInfo> fixtureEntities() {
    namespace mp = ha::media_player_feature;
    const uint32_t power = mp::kTurnOn | mp::kTurnOff;
    ha::EntityInfo tv;
    tv.entityId = "media_player.family_room_lg_tv";
    tv.supportedFeatures = power | mp::kVolumeStep | mp::kVolumeMute | mp::kSelectSource;
    tv.sourceList = {"HDMI 1", "HDMI 2", "HDMI 3"};

    ha::EntityInfo receiver;
    receiver.entityId = "media_player.denon";
    receiver.supportedFeatures = power | mp::kVolumeSet | mp::kVolumeStep | mp::kVolumeMute | mp::kSelectSource;
    receiver.sourceList = {"Media Player", "Game", "TV Audio"};

    ha::EntityInfo xbox;
    xbox.entityId = "media_player.xbox";
    xbox.supportedFeatures = power | mp::kPlay | mp::kPause;

    ha::EntityInfo lights;
    lights.entityId = "light.family_room";
    lights.supportedColorModes = {"brightness"};

    return {tv, receiver, xbox, lights};
}

}  // namespace hubbo::sim
