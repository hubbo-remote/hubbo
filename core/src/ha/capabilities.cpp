#include "hubbo/ha/capabilities.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "hubbo/domain/limits.h"
#include "hubbo/ha/snapshot.h"

namespace hubbo::ha {

// The allowlist: every command this file can offer, with its label and the
// HA service it calls. Command ids are Hubbo's, not HA service names.
struct CommandDef {
    const char *id;
    const char *label;
    const char *domain;   // HA service domain
    const char *service;  // HA service
    // A bool state attribute HA has no toggle for: read at press time
    // (stateAttribute()) and sent back negated under the same key.
    const char *stateAttribute = nullptr;
    const char *group = nullptr;  // CommandSpec::group
};

namespace {

namespace mp = media_player_feature;
namespace fan = fan_feature;

constexpr CommandDef kCommands[] = {
    {"media_player.turn_on", "Power on", "media_player", "turn_on"},
    {"media_player.turn_off", "Power off", "media_player", "turn_off"},
    {"media_player.volume_up", "Volume up", "media_player", "volume_up"},
    {"media_player.volume_down", "Volume down", "media_player", "volume_down"},
    {"media_player.volume_set", "Set volume", "media_player", "volume_set"},
    {"media_player.volume_mute", "Mute", "media_player", "volume_mute"},
    {"media_player.mute_toggle", "Toggle mute", "media_player", "volume_mute", "is_volume_muted"},
    {"media_player.media_play_pause", "Play/pause", "media_player", "media_play_pause"},
    {"media_player.media_play", "Play", "media_player", "media_play"},
    {"media_player.media_pause", "Pause", "media_player", "media_pause"},
    {"media_player.media_stop", "Stop", "media_player", "media_stop"},
    {"media_player.media_next_track", "Next track", "media_player", "media_next_track"},
    {"media_player.media_previous_track", "Previous track", "media_player", "media_previous_track"},
    {"media_player.select_source", "Select source", "media_player", "select_source"},
    {"media_player.select_sound_mode", "Sound mode", "media_player", "select_sound_mode"},
    {"media_player.shuffle_set", "Shuffle", "media_player", "shuffle_set"},
    {"media_player.repeat_set", "Repeat", "media_player", "repeat_set"},
    {"light.turn_on", "Turn on", "light", "turn_on"},
    {"light.turn_off", "Turn off", "light", "turn_off"},
    {"light.brightness_step", "Change brightness", "light", "turn_on"},
    {"switch.turn_on", "Turn on", "switch", "turn_on"},
    {"switch.turn_off", "Turn off", "switch", "turn_off"},
    {"fan.turn_on", "Turn on", "fan", "turn_on"},
    {"fan.turn_off", "Turn off", "fan", "turn_off"},
    {"fan.set_percentage", "Set speed", "fan", "set_percentage"},
    {"fan.increase_speed", "Speed up", "fan", "increase_speed"},
    {"fan.decrease_speed", "Slow down", "fan", "decrease_speed"},
    {"fan.oscillate", "Oscillate", "fan", "oscillate"},
    {"fan.set_direction", "Set direction", "fan", "set_direction"},
    {"fan.set_preset_mode", "Preset mode", "fan", "set_preset_mode"},
    {"scene.turn_on", "Activate", "scene", "turn_on"},
    {"script.turn_on", "Run", "script", "turn_on"},
    {"script.turn_off", "Stop", "script", "turn_off"},
    {"cover.open_cover", "Open", "cover", "open_cover"},
    {"cover.close_cover", "Close", "cover", "close_cover"},
    {"cover.stop_cover", "Stop", "cover", "stop_cover"},
    {"cover.toggle", "Toggle", "cover", "toggle"},
    {"cover.set_cover_position", "Set position", "cover", "set_cover_position"},
    {"cover.open_cover_tilt", "Open tilt", "cover", "open_cover_tilt"},
    {"cover.close_cover_tilt", "Close tilt", "cover", "close_cover_tilt"},
    {"cover.stop_cover_tilt", "Stop tilt", "cover", "stop_cover_tilt"},
    {"cover.set_cover_tilt_position", "Set tilt", "cover", "set_cover_tilt_position"},
    {"button.press", "Press", "button", "press"},
    {"input_boolean.turn_on", "Turn on", "input_boolean", "turn_on"},
    {"input_boolean.turn_off", "Turn off", "input_boolean", "turn_off"},
    {"input_boolean.toggle", "Toggle", "input_boolean", "toggle"},
    {"humidifier.turn_on", "Turn on", "humidifier", "turn_on"},
    {"humidifier.turn_off", "Turn off", "humidifier", "turn_off"},
    {"humidifier.toggle", "Toggle", "humidifier", "toggle"},
    {"humidifier.set_humidity", "Set humidity", "humidifier", "set_humidity"},
    {"humidifier.set_mode", "Set mode", "humidifier", "set_mode"},
    {"select.select_option", "Choose", "select", "select_option"},
    {"select.select_next", "Next", "select", "select_next"},
    {"select.select_previous", "Previous", "select", "select_previous"},
    {"remote.turn_on", "Turn on", "remote", "turn_on"},
    {"remote.turn_off", "Turn off", "remote", "turn_off"},
    {"remote.toggle", "Toggle", "remote", "toggle"},
    // No fixed service: buildCustomCall() takes it from the action.
    {kCallServiceCommandId, "Home Assistant action", "", "", nullptr, "custom"},
};

const CommandDef *findDef(std::string_view id) {
    for (const auto &def : kCommands) {
        if (id == def.id) return &def;
    }
    return nullptr;
}

constexpr const char *kTargetKeys[] = {"entity_id", "device_id", "area_id", "floor_id", "label_id"};
// Raw service payload names; with kTargetKeys, the forbidden param names.
constexpr const char *kPayloadNames[] = {"target", "data", "service", "service_data", "domain"};

bool listed(std::string_view name, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (name == list[i]) return true;
    }
    return false;
}

}  // namespace

// Drafts are named in the header (EntityCapabilities::build), so they live
// outside the anonymous namespace.
struct ParamDraft {
    ParamDraft() = default;
    ParamDraft(ParamSpec s, std::string f, std::vector<std::string> o = {})
        : spec(s), field(std::move(f)), options(std::move(o)) {}

    ParamSpec spec{};
    std::string field;                 // HA service data key
    std::vector<std::string> options;  // Enum only
    std::string name, label, unit, description;  // owned (snapshot services); else spec's static strings
    std::string defaultOption;  // Enum with a shown default: resolved to its index after filtering
};

struct CommandDraft {
    std::string id, domain, service;
    std::vector<ParamDraft> params;
    const char *stateAttribute = nullptr;  // allowlist columns (CommandDef)
    const char *group = nullptr;
};

// Collects the commands an entity gets: allowlist ids, then snapshot services.
struct Drafts {
    std::vector<CommandDraft> items;

    CommandDraft &add(const char *id, std::vector<ParamDraft> params = {}) {
        const CommandDef *def = findDef(id);
        items.push_back({def->id, def->domain, def->service, std::move(params), def->stateAttribute, def->group});
        return items.back();
    }
};

namespace {

bool isLowerAlnumUnderscore(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool has(uint32_t features, uint32_t flag) { return (features & flag) == flag; }

// HA's COLOR_MODES_BRIGHTNESS: every valid colour mode except onoff.
bool brightnessMode(const std::string &mode) {
    return mode == "brightness" || mode == "color_temp" || mode == "hs" || mode == "xy" ||
           mode == "rgb" || mode == "rgbw" || mode == "rgbww" || mode == "white";
}

ParamDraft transitionParam() {
    // HA clamps transition to 0..6553 s; its UI selector offers 0..300.
    return {ParamSpec::number("transition", 0.0f, 300.0f, false), "transition", {}};
}

ParamDraft enumParam(const char *name, std::vector<std::string> options) {
    return {ParamSpec::enumeration(name, nullptr, 0), name, std::move(options)};
}

void mediaPlayer(const EntityInfo &e, Drafts &out) {
    const uint32_t f = e.supportedFeatures;
    // Gating mirrors media_player/services.py: a list of flags means "any
    // of"; a combined flag means "all of".
    if (has(f, mp::kTurnOn)) out.add("media_player.turn_on");
    if (has(f, mp::kTurnOff)) out.add("media_player.turn_off");
    if (has(f, mp::kVolumeSet) || has(f, mp::kVolumeStep)) {
        out.add("media_player.volume_up");
        out.add("media_player.volume_down");
    }
    if (has(f, mp::kVolumeSet)) {
        out.add("media_player.volume_set", {{ParamSpec::number("level", 0.0f, 1.0f), "volume_level", {}}});
    }
    if (has(f, mp::kVolumeMute)) {
        out.add("media_player.volume_mute", {{ParamSpec::boolean("muted"), "is_volume_muted", {}}});
        out.add("media_player.mute_toggle");
    }
    if (has(f, mp::kPlay | mp::kPause)) out.add("media_player.media_play_pause");
    if (has(f, mp::kPlay)) out.add("media_player.media_play");
    if (has(f, mp::kPause)) out.add("media_player.media_pause");
    if (has(f, mp::kStop)) out.add("media_player.media_stop");
    if (has(f, mp::kNextTrack)) out.add("media_player.media_next_track");
    if (has(f, mp::kPreviousTrack)) out.add("media_player.media_previous_track");
    if (has(f, mp::kSelectSource) && !e.sourceList.empty()) {
        out.add("media_player.select_source", {enumParam("source", e.sourceList)});
    }
    if (has(f, mp::kSelectSoundMode) && !e.soundModeList.empty()) {
        out.add("media_player.select_sound_mode", {enumParam("sound_mode", e.soundModeList)});
    }
    if (has(f, mp::kShuffleSet)) {
        out.add("media_player.shuffle_set", {{ParamSpec::boolean("shuffle"), "shuffle", {}}});
    }
    if (has(f, mp::kRepeatSet)) {
        // RepeatMode in media_player/const.py.
        out.add("media_player.repeat_set", {enumParam("repeat", {"off", "all", "one"})});
    }
}

void light(const EntityInfo &e, Drafts &out) {
    bool brightness = false;
    bool colorTemp = false;
    for (const auto &mode : e.supportedColorModes) {
        brightness = brightness || brightnessMode(mode);
        colorTemp = colorTemp || mode == "color_temp";
    }
    const bool transition = has(e.supportedFeatures, light_feature::kTransition);

    CommandDraft &on = out.add("light.turn_on");
    if (brightness) {
        on.params.push_back(
            {ParamSpec::integer("brightness", 0, 255, false), "brightness", {}});
    }
    if (colorTemp) {
        int32_t lo = e.minColorTempKelvin;
        int32_t hi = e.maxColorTempKelvin;
        if (lo <= 0 || hi <= 0 || lo > hi) {
            lo = kDefaultMinKelvin;
            hi = kDefaultMaxKelvin;
        }
        on.params.push_back({ParamSpec::integer("color_temp_kelvin", lo, hi, false, lo),
                             "color_temp_kelvin", {}});
    }
    if (transition) on.params.push_back(transitionParam());

    CommandDraft &off = out.add("light.turn_off");
    if (transition) off.params.push_back(transitionParam());

    if (brightness) {
        CommandDraft &step = out.add("light.brightness_step",
                                     {{ParamSpec::integer("step", -255, 255), "brightness_step", {}}});
        if (transition) step.params.push_back(transitionParam());
    }
}

// An int percentage (HA: Coerce(int), 0..100 or the entity's range); a
// fan's `step` from fanSpeedStep().
ParamDraft percentParam(const char *name, bool required, int32_t step = 0, int32_t lo = 0, int32_t hi = 100) {
    return {ParamSpec::integer(name, lo, hi, required).described(nullptr, "%", static_cast<float>(step)), name, {}};
}

void fanEntity(const EntityInfo &e, Drafts &out) {
    const uint32_t f = e.supportedFeatures;
    const bool speed = has(f, fan::kSetSpeed);
    const int32_t step = fanSpeedStep(e.percentageStep);
    // Gating mirrors fan/services.py; set_preset_mode is registered for
    // SET_SPEED or PRESET_MODE, turn_on's preset_mode field for PRESET_MODE.
    if (has(f, fan::kTurnOn)) {
        CommandDraft &on = out.add("fan.turn_on");
        if (speed) on.params.push_back(percentParam("percentage", false, step));
        if (has(f, fan::kPresetMode) && !e.presetModes.empty()) {
            ParamDraft preset = enumParam("preset_mode", e.presetModes);
            preset.spec.required = false;
            on.params.push_back(std::move(preset));
        }
    }
    if (has(f, fan::kTurnOff)) out.add("fan.turn_off");
    if (speed) {
        out.add("fan.set_percentage", {percentParam("percentage", true, step)});
        out.add("fan.increase_speed", {percentParam("percentage_step", false)});
        out.add("fan.decrease_speed", {percentParam("percentage_step", false)});
    }
    if (has(f, fan::kOscillate)) {
        out.add("fan.oscillate", {{ParamSpec::boolean("oscillating"), "oscillating", {}}});
    }
    if (has(f, fan::kDirection)) {
        out.add("fan.set_direction", {enumParam("direction", {"forward", "reverse"})});
    }
    if ((speed || has(f, fan::kPresetMode)) && !e.presetModes.empty()) {
        out.add("fan.set_preset_mode", {enumParam("preset_mode", e.presetModes)});
    }
}

void coverEntity(const EntityInfo &e, Drafts &out) {
    namespace c = cover_feature;
    const uint32_t f = e.supportedFeatures;
    // Gating mirrors cover/services.py (toggle needs OPEN and CLOSE).
    if (has(f, c::kOpen)) out.add("cover.open_cover");
    if (has(f, c::kClose)) out.add("cover.close_cover");
    if (has(f, c::kStop)) out.add("cover.stop_cover");
    if (has(f, c::kOpen | c::kClose)) out.add("cover.toggle");
    if (has(f, c::kSetPosition)) out.add("cover.set_cover_position", {percentParam("position", true)});
    if (has(f, c::kOpenTilt)) out.add("cover.open_cover_tilt");
    if (has(f, c::kCloseTilt)) out.add("cover.close_cover_tilt");
    if (has(f, c::kStopTilt)) out.add("cover.stop_cover_tilt");
    if (has(f, c::kSetTiltPosition)) out.add("cover.set_cover_tilt_position", {percentParam("tilt_position", true)});
}

void humidifier(const EntityInfo &e, Drafts &out) {
    // humidifier/services.py: all but set_mode for every humidifier.
    out.add("humidifier.turn_on");
    out.add("humidifier.turn_off");
    out.add("humidifier.toggle");
    // HA checks the humidity against the entity's range.
    int32_t lo = 0;
    int32_t hi = 100;
    humidityRange(e.minHumidity, e.maxHumidity, &lo, &hi);
    out.add("humidifier.set_humidity", {percentParam("humidity", true, 0, lo, hi)});
    if (has(e.supportedFeatures, humidifier_feature::kModes) && !e.availableModes.empty()) {
        out.add("humidifier.set_mode", {enumParam("mode", e.availableModes)});
    }
}

void selectEntity(const EntityInfo &e, Drafts &out) {
    if (e.options.empty()) return;  // HA requires options; none: nothing to pick
    out.add("select.select_option", {enumParam("option", e.options)});
    out.add("select.select_next");
    out.add("select.select_previous");
}

}  // namespace

int32_t fanSpeedStep(double percentageStep) {
    return percentageStep >= 1.0 && percentageStep <= 100.0 ? static_cast<int32_t>(percentageStep) : 0;
}

bool humidityRange(double min, double max, int32_t *lo, int32_t *hi) {
    if (!(min >= 0.0 && max <= 100.0)) return false;  // NaN fails too
    const double low = std::ceil(min);
    const double high = std::floor(max);
    if (low >= high) return false;
    *lo = static_cast<int32_t>(low);
    *hi = static_cast<int32_t>(high);
    return true;
}

namespace {

// Per-domain facts, in EntityDomain order (kEntityDomains holds the names).
struct DomainTraits {
    const char *kind;          // suggestedKind(); nullptr: by device class and platform
    bool integrationServices;  // offersIntegrationServices()
};
constexpr DomainTraits kDomainTraits[] = {
    {nullptr, true},      // media_player
    {"light", true},      // light
    {"switch", true},     // switch
    {"fan", true},        // fan
    {"scene", false},     // scene
    {"script", false},    // script
    {"cover", false},     // cover
    {"other", false},     // button
    {"other", false},     // input_boolean
    {"climate", false},   // humidifier
    {"other", false},     // select
    {"other", true},      // remote
    {"other", false},     // Unsupported
};
static_assert(std::size(kDomainTraits) == std::size(kEntityDomains) + 1, "one row per EntityDomain");

const DomainTraits &traits(EntityDomain domain) { return kDomainTraits[static_cast<size_t>(domain)]; }

}  // namespace

const char *suggestedKind(std::string_view domain, std::string_view platform, std::string_view deviceClass) {
    if (const char *kind = traits(domainNamed(domain)).kind) return kind;
    // A media player: HA's device_class (tv, speaker, receiver), else what
    // the integration usually is.
    if (deviceClass == "receiver") return "receiver";
    if (deviceClass == "speaker" || platform == "alexa_media" || platform == "group") return "speaker";
    if (platform == "apple_tv") return "streamer";
    return "tv";
}

bool offersIntegrationServices(std::string_view entityId) {
    return traits(entityDomain(entityId)).integrationServices;
}

std::vector<std::string> serviceDomains(std::string_view entityId, std::string_view platform) {
    std::vector<std::string> out;
    if (entityDomain(entityId) == EntityDomain::Remote) out.emplace_back("remote");
    if (!platform.empty() && platform != entityId.substr(0, entityId.find('.'))) out.emplace_back(platform);
    return out;
}

bool isDeviceService(std::string_view entityId, std::string_view platform, std::string_view service) {
    if (!isHaServiceId(service)) return false;
    const std::string_view domain = service.substr(0, service.find('.'));
    for (const std::string &allowed : serviceDomains(entityId, platform)) {
        if (domain == allowed) return true;
    }
    return false;
}

bool isAllowlistedService(std::string_view domain, std::string_view service) {
    for (const auto &def : kCommands) {
        if (domain == def.domain && service == def.service) return true;
    }
    return false;
}

bool isHaName(std::string_view name) {
    if (name.empty() || name.size() > 32 || name[0] < 'a' || name[0] > 'z') return false;
    for (char c : name) {
        if (!isLowerAlnumUnderscore(c)) return false;
    }
    return true;
}

bool isHaServiceId(std::string_view service) {
    const size_t dot = service.find('.');
    if (dot == std::string_view::npos || !isHaName(service.substr(0, dot))) return false;
    const std::string_view name = service.substr(dot + 1);
    if (name.empty() || name.size() > 64 || name[0] < 'a' || name[0] > 'z') return false;
    for (char c : name) {
        if (!isLowerAlnumUnderscore(c)) return false;
    }
    return true;
}

bool filterEnumOptions(std::vector<std::string> &options, size_t *dropped) {
    std::vector<std::string> kept;
    for (auto &option : options) {
        bool duplicate = false;
        for (const auto &seen : kept) duplicate = duplicate || seen == option;
        if (option.empty() || option.size() > limits::kMaxParamStringLength || duplicate ||
            kept.size() >= limits::kMaxEnumOptions) {
            (*dropped)++;
            continue;
        }
        kept.push_back(std::move(option));
    }
    options = std::move(kept);
    return !options.empty();
}

bool isValidEntityId(std::string_view id) {
    if (id.empty() || id.size() > limits::kMaxProviderRefLength) {
        return false;
    }
    const size_t dot = id.find('.');
    if (dot == 0 || dot == std::string_view::npos || dot + 1 == id.size()) {
        return false;
    }
    for (size_t i = 0; i < id.size(); i++) {
        if (i != dot && !isLowerAlnumUnderscore(id[i])) return false;
    }
    const std::string_view objectId = id.substr(dot + 1);
    return objectId != "all" && objectId != "none";
}

EntityDomain entityDomain(std::string_view id) {
    if (!isValidEntityId(id)) return EntityDomain::Unsupported;
    return domainNamed(id.substr(0, id.find('.')));
}

EntityDomain domainNamed(std::string_view domain) {
    for (size_t i = 0; i < std::size(kEntityDomains); i++) {
        if (domain == kEntityDomains[i]) return static_cast<EntityDomain>(i);
    }
    return EntityDomain::Unsupported;
}

std::string entityDomainList() {
    constexpr size_t count = std::size(kEntityDomains);
    std::string out;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) out += i + 1 == count ? " or " : ", ";
        out += kEntityDomains[i];
    }
    return out;
}

namespace {

void builtins(const EntityInfo &entity, Drafts &drafts) {
    switch (entityDomain(entity.entityId)) {
        case EntityDomain::MediaPlayer: mediaPlayer(entity, drafts); break;
        case EntityDomain::Light: light(entity, drafts); break;
        case EntityDomain::Switch:
            drafts.add("switch.turn_on");
            drafts.add("switch.turn_off");
            break;
        case EntityDomain::Fan: fanEntity(entity, drafts); break;
        case EntityDomain::Scene: drafts.add("scene.turn_on"); break;
        case EntityDomain::Script:
            drafts.add("script.turn_on");
            drafts.add("script.turn_off");
            break;
        case EntityDomain::Cover: coverEntity(entity, drafts); break;
        case EntityDomain::Button: drafts.add("button.press"); break;
        case EntityDomain::InputBoolean:
            drafts.add("input_boolean.turn_on");
            drafts.add("input_boolean.turn_off");
            drafts.add("input_boolean.toggle");
            break;
        case EntityDomain::Humidifier: humidifier(entity, drafts); break;
        case EntityDomain::Select: selectEntity(entity, drafts); break;
        case EntityDomain::Remote:
            // remote/services.yaml: every remote. turn_on's `activity` is text: untyped.
            drafts.add("remote.turn_on");
            drafts.add("remote.turn_off");
            drafts.add("remote.toggle");
            break;
        case EntityDomain::Unsupported: break;
    }
}


// A snapshot service as a draft, or false if it isn't safe to offer: the
// service must belong to the entity's platform (never its domain's curated
// services), and every field must be a valid, non-target-like param name.
// (Decoding already validated types and ranges; this is the safety check.)
// Fills a fresh `draft` in place; the caller drops it on false.
bool serviceDraft(const SnapshotService &service, const CapabilitySnapshot &snapshot, std::string_view entityId,
                  CommandDraft *draft) {
    const size_t dot = service.service.find('.');
    if (dot == std::string::npos) return false;
    const std::string domain = service.service.substr(0, dot);
    const std::string name = service.service.substr(dot + 1);
    if (domain != snapshot.platform || !isHaName(domain) || !isHaName(name)) return false;
    if (entityId.substr(0, entityId.find('.')) == domain) return false;
    if (service.fields.size() > limits::kMaxParams) return false;
    draft->id = service.service;
    draft->domain = domain;
    draft->service = name;
    for (const SnapshotParam &field : service.fields) {
        if (!isHaName(field.name) || isForbiddenParamName(field.name)) return false;
        ParamDraft &param = draft->params.emplace_back();
        param.field = field.name;
        switch (field.type) {
            case SnapshotParamType::Bool:
                param.spec = ParamSpec::boolean(nullptr, field.required, field.defaultNumber != 0);
                break;
            case SnapshotParamType::Int:
                if (field.minimum < -2147483648.0 || field.maximum > 2147483647.0 || field.minimum > field.maximum) {
                    return false;
                }
                param.spec = ParamSpec::integer(
                    nullptr, static_cast<int32_t>(field.minimum), static_cast<int32_t>(field.maximum), field.required,
                    static_cast<int32_t>(field.hasDefault ? field.defaultNumber : field.minimum));
                break;
            case SnapshotParamType::Number:
                if (!(field.minimum <= field.maximum)) return false;
                param.spec = ParamSpec::number(nullptr, static_cast<float>(field.minimum),
                                               static_cast<float>(field.maximum), field.required,
                                               static_cast<float>(field.hasDefault ? field.defaultNumber
                                                                                   : field.minimum));
                break;
            case SnapshotParamType::Enum:
                param.spec = ParamSpec::enumeration(nullptr, nullptr, 0, field.required);
                param.options = field.options;
                param.defaultOption = field.defaultOption;
                break;
        }
        if (field.hasDefault) param.spec = param.spec.shownDefault();
        param.spec.step = static_cast<float>(field.step);
        param.name = field.name;
        param.label = field.label;
        param.unit = field.unit;
        param.description = field.description;
    }
    return true;
}

}  // namespace

EntityCapabilities EntityCapabilities::fromEntity(const EntityInfo &entity) {
    Drafts drafts;
    builtins(entity, drafts);
    return build(entity.entityId, std::move(drafts));
}

EntityCapabilities EntityCapabilities::fromSnapshot(std::string_view entityId, const CapabilitySnapshot &snapshot) {
    EntityInfo entity;
    entity.entityId = std::string(entityId);
    entity.supportedFeatures = snapshot.supportedFeatures;
    entity.sourceList = snapshot.sourceList;
    entity.soundModeList = snapshot.soundModeList;
    entity.supportedColorModes = snapshot.supportedColorModes;
    entity.minColorTempKelvin = snapshot.minColorTempKelvin;
    entity.maxColorTempKelvin = snapshot.maxColorTempKelvin;
    entity.presetModes = snapshot.presetModes;
    entity.percentageStep = snapshot.percentageStep;
    entity.minHumidity = snapshot.minHumidity;
    entity.maxHumidity = snapshot.maxHumidity;
    entity.availableModes = snapshot.availableModes;
    entity.options = snapshot.options;
    Drafts drafts;
    builtins(entity, drafts);
    // Re-checked here as well as in offerFor: a stored snapshot can come from a backup.
    if (!offersIntegrationServices(entityId)) return build(entityId, std::move(drafts));
    size_t services = 0;
    for (const SnapshotService &service : snapshot.services) {
        if (services >= kMaxSnapshotServices || findDef(service.service) != nullptr) continue;
        bool duplicate = false;
        for (const auto &existing : drafts.items) duplicate = duplicate || existing.id == service.service;
        if (duplicate) continue;
        if (!serviceDraft(service, snapshot, entityId, &drafts.items.emplace_back())) {
            drafts.items.pop_back();
            continue;
        }
        services++;
    }
    if (!snapshot.customServices.empty()) drafts.add(kCallServiceCommandId);
    return build(entityId, std::move(drafts));
}

EntityCapabilities EntityCapabilities::build(std::string_view entityId, Drafts drafts) {
    EntityCapabilities caps;
    caps.entityId_ = std::string(entityId);
    // An invalid entity can't be targeted: no commands at all.
    if (!isValidEntityId(entityId)) return caps;

    // Fill the owned storage first; specs hold offsets until the end.
    std::vector<size_t> firstOption;  // per params_ entry, Enum only
    for (auto &draft : drafts.items) {
        bool usable = draft.params.size() <= limits::kMaxParams;
        for (auto &param : draft.params) {
            if (param.spec.type != ParamType::Enum) continue;
            usable = filterEnumOptions(param.options, &caps.droppedOptions_) && usable;
            if (param.spec.hasDefault) {
                // By value: filtering may have moved or dropped the default
                // (the spec's fallback stays option 0).
                const auto at = std::find(param.options.begin(), param.options.end(), param.defaultOption);
                if (at != param.options.end()) {
                    param.spec.defaultValue = ParamValue::ofOption(static_cast<uint16_t>(at - param.options.begin()));
                } else {
                    param.spec.hasDefault = false;
                }
            }
        }
        if (!usable) continue;
        Owned command;
        command.id = caps.keep(std::move(draft.id));
        command.domain = caps.keep(std::move(draft.domain));
        command.service = caps.keep(std::move(draft.service));
        command.stateAttribute = draft.stateAttribute;
        command.group = draft.group;
        caps.owned_.push_back(command);
        caps.firstParam_.push_back(caps.params_.size());
        for (auto &param : draft.params) {
            firstOption.push_back(caps.optionStore_.size());
            param.spec.optionCount = static_cast<uint16_t>(param.options.size());
            for (auto &option : param.options) caps.optionStore_.push_back(std::move(option));
            if (!param.name.empty()) param.spec.name = caps.keep(std::move(param.name));
            if (!param.label.empty()) param.spec.label = caps.keep(std::move(param.label));
            if (!param.unit.empty()) param.spec.unit = caps.keep(std::move(param.unit));
            if (!param.description.empty()) param.spec.description = caps.keep(std::move(param.description));
            caps.params_.push_back(param.spec);
            caps.fields_.push_back(caps.keep(std::move(param.field)));
        }
    }

    // Storage is final: now point into it.
    for (const auto &option : caps.optionStore_) caps.optionPtrs_.push_back(option.c_str());
    for (size_t i = 0; i < caps.params_.size(); i++) {
        if (caps.params_[i].type == ParamType::Enum) {
            caps.params_[i].options = caps.optionPtrs_.data() + firstOption[i];
        }
    }
    for (size_t i = 0; i < caps.owned_.size(); i++) {
        const size_t end = i + 1 < caps.owned_.size() ? caps.firstParam_[i + 1] : caps.params_.size();
        caps.commands_.push_back({caps.owned_[i].id, caps.params_.data() + caps.firstParam_[i],
                                  static_cast<uint8_t>(end - caps.firstParam_[i])});
        caps.commands_.back().group = caps.owned_[i].group;
    }
    return caps;
}

const char *EntityCapabilities::keep(std::string text) {
    strings_.push_back(std::move(text));
    return strings_.back().c_str();
}

CommandCatalog EntityCapabilities::catalog() const {
    return {commands_.data(), static_cast<uint16_t>(commands_.size())};
}

ErrorCode EntityCapabilities::buildCustomCall(std::string_view service, std::string_view dataJson,
                                              ServiceCall *out) const {
    if (catalog().find(kCallServiceCommandId) < 0 || !isHaServiceId(service)) return ErrorCode::UnknownCommand;
    const size_t dot = service.find('.');
    ServiceCall call;
    call.domain = std::string(service.substr(0, dot));
    call.service = std::string(service.substr(dot + 1));
    call.entityId = entityId_;
    call.rawData = dataJson.empty() ? std::string("{}") : std::string(dataJson);
    *out = std::move(call);
    return ErrorCode::Ok;
}

const char *EntityCapabilities::stateAttribute(uint16_t index) const {
    return index < owned_.size() ? owned_[index].stateAttribute : nullptr;
}

ErrorCode EntityCapabilities::buildServiceCall(uint16_t index, const ParamValue *params, uint8_t present,
                                               ServiceCall *out, const ServiceValue *state) const {
    // An invalid entity id produces an empty catalog, so this also covers it.
    if (index >= commands_.size()) {
        return ErrorCode::UnknownCommand;
    }
    const CommandSpec &command = commands_[index];
    if (command.id == std::string_view(kCallServiceCommandId)) return ErrorCode::UnknownCommand;  // buildCustomCall()
    if (command.paramCount > 0 && params == nullptr) {
        return ErrorCode::MissingParam;
    }

    ServiceCall call;
    call.domain = owned_[index].domain;
    call.service = owned_[index].service;
    call.entityId = entityId_;
    call.data.reserve(command.paramCount);
    for (uint8_t i = 0; i < command.paramCount; i++) {
        const ParamSpec &spec = command.params[i];
        const ParamValue &value = params[i];
        if (value.type != spec.type) {
            return ErrorCode::InvalidParamType;
        }
        if (!spec.required && (present & (1u << i)) == 0) {
            continue;
        }
        ServiceField field;
        field.key = fields_[firstParam_[index] + i];
        switch (spec.type) {
            case ParamType::Bool:
                field.value.kind = ServiceValue::Kind::Bool;
                field.value.boolean = value.asBool();
                break;
            case ParamType::Int:
                field.value.kind = ServiceValue::Kind::Int;
                field.value.integer = value.integer;
                break;
            case ParamType::Number:
                field.value.kind = ServiceValue::Kind::Number;
                field.value.number = value.number;
                break;
            case ParamType::Enum:
                // The index indexes our own storage: always bounds-check it.
                if (value.asOption() >= spec.optionCount) {
                    return ErrorCode::InvalidEnumValue;
                }
                field.value.kind = ServiceValue::Kind::String;
                field.value.string = spec.options[value.asOption()];
                break;
            case ParamType::ActivityRef:
                return ErrorCode::InvalidParamType;
        }
        call.data.push_back(std::move(field));
    }
    if (const char *attribute = owned_[index].stateAttribute) {
        // A toggle: the opposite of the current value. Unknown or not a bool
        // counts as false, so the first press turns it on (mutes).
        const bool current = state != nullptr && state->kind == ServiceValue::Kind::Bool && state->boolean;
        ServiceField field;
        field.key = attribute;
        field.value.kind = ServiceValue::Kind::Bool;
        field.value.boolean = !current;
        call.data.push_back(std::move(field));
    }
    *out = std::move(call);
    return ErrorCode::Ok;
}

const char *commandLabel(std::string_view commandId) {
    const CommandDef *def = findDef(commandId);
    return def != nullptr ? def->label : nullptr;
}

bool isTargetKey(std::string_view name) { return listed(name, kTargetKeys, std::size(kTargetKeys)); }

bool isForbiddenParamName(std::string_view name) {
    return isTargetKey(name) || listed(name, kPayloadNames, std::size(kPayloadNames));
}

}  // namespace hubbo::ha
