#include "hubbo/ha/capabilities.h"

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
};

namespace {

namespace mp = media_player_feature;

constexpr CommandDef kCommands[] = {
    {"media_player.turn_on", "Power on", "media_player", "turn_on"},
    {"media_player.turn_off", "Power off", "media_player", "turn_off"},
    {"media_player.volume_up", "Volume up", "media_player", "volume_up"},
    {"media_player.volume_down", "Volume down", "media_player", "volume_down"},
    {"media_player.volume_set", "Set volume", "media_player", "volume_set"},
    {"media_player.volume_mute", "Mute", "media_player", "volume_mute"},
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
};

const CommandDef *findDef(std::string_view id) {
    for (const auto &def : kCommands) {
        if (id == def.id) return &def;
    }
    return nullptr;
}

constexpr const char *kForbiddenNames[] = {
    "entity_id", "device_id", "area_id", "floor_id", "label_id",
    "target",    "data",      "service", "service_data", "domain",
};

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
    std::string name, label, unit;     // owned (snapshot services); else spec's static strings
};

struct CommandDraft {
    std::string id, domain, service;
    std::vector<ParamDraft> params;
};

// Collects the commands an entity gets: allowlist ids, then snapshot services.
struct Drafts {
    std::vector<CommandDraft> items;

    CommandDraft &add(const char *id, std::vector<ParamDraft> params = {}) {
        const CommandDef *def = findDef(id);
        items.push_back({def->id, def->domain, def->service, std::move(params)});
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


}  // namespace

bool isHaName(std::string_view name) {
    if (name.empty() || name.size() > 32 || name[0] < 'a' || name[0] > 'z') return false;
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
    const std::string_view domain = id.substr(0, id.find('.'));
    if (domain == "media_player") return EntityDomain::MediaPlayer;
    if (domain == "light") return EntityDomain::Light;
    if (domain == "switch") return EntityDomain::Switch;
    return EntityDomain::Unsupported;
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
        case EntityDomain::Unsupported: break;
    }
}


// A snapshot service as a draft, or false if it isn't safe to offer: the
// service must belong to the entity's platform (never its domain's curated
// services), and every field must be a valid, non-target-like param name.
// (Decoding already validated types and ranges; this is the safety check.)
bool serviceDraft(const SnapshotService &service, const CapabilitySnapshot &snapshot, std::string_view entityId,
                  CommandDraft *out) {
    const size_t dot = service.service.find('.');
    if (dot == std::string::npos) return false;
    const std::string domain = service.service.substr(0, dot);
    const std::string name = service.service.substr(dot + 1);
    if (domain != snapshot.platform || !isHaName(domain) || !isHaName(name)) return false;
    if (entityId.substr(0, entityId.find('.')) == domain) return false;
    if (service.fields.size() > limits::kMaxParams) return false;
    CommandDraft draft{service.service, domain, name, {}};
    for (const SnapshotParam &field : service.fields) {
        if (!isHaName(field.name) || isForbiddenParamName(field.name)) return false;
        ParamDraft param;
        param.field = field.name;
        switch (field.type) {
            case SnapshotParamType::Bool: param.spec = ParamSpec::boolean(nullptr, field.required); break;
            case SnapshotParamType::Int:
                if (field.minimum < -2147483648.0 || field.maximum > 2147483647.0 || field.minimum > field.maximum) {
                    return false;
                }
                param.spec = ParamSpec::integer(nullptr, static_cast<int32_t>(field.minimum),
                                                static_cast<int32_t>(field.maximum), field.required,
                                                static_cast<int32_t>(field.minimum));
                break;
            case SnapshotParamType::Number:
                if (!(field.minimum <= field.maximum)) return false;
                param.spec = ParamSpec::number(nullptr, static_cast<float>(field.minimum),
                                               static_cast<float>(field.maximum), field.required,
                                               static_cast<float>(field.minimum));
                break;
            case SnapshotParamType::Enum:
                param.spec = ParamSpec::enumeration(nullptr, nullptr, 0, field.required);
                param.options = field.options;
                break;
        }
        param.spec.step = static_cast<float>(field.step);
        param.name = field.name;
        param.label = field.label;
        param.unit = field.unit;
        draft.params.push_back(std::move(param));
    }
    *out = std::move(draft);
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
    Drafts drafts;
    builtins(entity, drafts);
    size_t services = 0;
    for (const SnapshotService &service : snapshot.services) {
        CommandDraft draft;
        if (services >= kMaxSnapshotServices || findDef(service.service) != nullptr ||
            !serviceDraft(service, snapshot, entityId, &draft)) {
            continue;
        }
        bool duplicate = false;
        for (const auto &existing : drafts.items) duplicate = duplicate || existing.id == draft.id;
        if (duplicate) continue;
        drafts.items.push_back(std::move(draft));
        services++;
    }
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
            if (param.spec.type == ParamType::Enum) {
                usable = filterEnumOptions(param.options, &caps.droppedOptions_) && usable;
            }
        }
        if (!usable) continue;
        Owned command;
        command.id = caps.keep(std::move(draft.id));
        command.domain = caps.keep(std::move(draft.domain));
        command.service = caps.keep(std::move(draft.service));
        caps.owned_.push_back(command);
        caps.firstParam_.push_back(caps.params_.size());
        for (auto &param : draft.params) {
            firstOption.push_back(caps.optionStore_.size());
            param.spec.optionCount = static_cast<uint16_t>(param.options.size());
            for (auto &option : param.options) caps.optionStore_.push_back(std::move(option));
            if (!param.name.empty()) param.spec.name = caps.keep(std::move(param.name));
            if (!param.label.empty()) param.spec.label = caps.keep(std::move(param.label));
            if (!param.unit.empty()) param.spec.unit = caps.keep(std::move(param.unit));
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

ErrorCode EntityCapabilities::buildServiceCall(uint16_t index, const ParamValue *params, uint8_t present,
                                               ServiceCall *out) const {
    // An invalid entity id produces an empty catalog, so this also covers it.
    if (index >= commands_.size()) {
        return ErrorCode::UnknownCommand;
    }
    const CommandSpec &command = commands_[index];
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
    *out = std::move(call);
    return ErrorCode::Ok;
}

const char *commandLabel(std::string_view commandId) {
    const CommandDef *def = findDef(commandId);
    return def != nullptr ? def->label : nullptr;
}

bool isForbiddenParamName(std::string_view name) {
    for (const char *forbidden : kForbiddenNames) {
        if (name == forbidden) return true;
    }
    return false;
}

}  // namespace hubbo::ha
