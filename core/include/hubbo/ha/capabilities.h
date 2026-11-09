#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/domain/error.h"
#include "hubbo/domain/params.h"

// Home Assistant capability normalization (docs/ha-capabilities.md).
//
// Input: one HA entity's id, supported_features and the attributes that
// shape parameters (source_list, supported_color_modes, ...). Output: the
// allowlisted Hubbo commands for it, as a core CommandCatalog the HA adapter
// returns from commandsFor(), plus the one way to turn a resolved command
// into an HA service call.
//
// Safety rules enforced here (not by callers):
// - The service call target is always exactly the registered entity_id.
//   No area/device/label/floor targets, no "all", no target in the data.
// - Service data keys come only from this file's static tables. Users supply
//   values, never keys, and values are already typed and range-checked by
//   core resolveParams() against the catalog.
// - Commands are offered only when HA itself would accept them for the
//   entity (same supported_features test as HA's entity service
//   registration), and only from a curated allowlist: no play_media,
//   join/unjoin (take other entity ids), toggle (we prefer explicit power),
//   browse/search, or anything taking free text or URLs.
namespace hubbo::ha {

// MediaPlayerEntityFeature, homeassistant/components/media_player/const.py.
namespace media_player_feature {
constexpr uint32_t kPause = 1;
constexpr uint32_t kSeek = 2;
constexpr uint32_t kVolumeSet = 4;
constexpr uint32_t kVolumeMute = 8;
constexpr uint32_t kPreviousTrack = 16;
constexpr uint32_t kNextTrack = 32;
constexpr uint32_t kTurnOn = 128;
constexpr uint32_t kTurnOff = 256;
constexpr uint32_t kPlayMedia = 512;
constexpr uint32_t kVolumeStep = 1024;
constexpr uint32_t kSelectSource = 2048;
constexpr uint32_t kStop = 4096;
constexpr uint32_t kClearPlaylist = 8192;
constexpr uint32_t kPlay = 16384;
constexpr uint32_t kShuffleSet = 32768;
constexpr uint32_t kSelectSoundMode = 65536;
constexpr uint32_t kBrowseMedia = 131072;
constexpr uint32_t kRepeatSet = 262144;
constexpr uint32_t kGrouping = 524288;
constexpr uint32_t kMediaAnnounce = 1048576;
constexpr uint32_t kMediaEnqueue = 2097152;
constexpr uint32_t kSearchMedia = 4194304;
}  // namespace media_player_feature

// LightEntityFeature, homeassistant/components/light/const.py.
namespace light_feature {
constexpr uint32_t kEffect = 4;
constexpr uint32_t kFlash = 8;
constexpr uint32_t kTransition = 32;
}  // namespace light_feature

// HA's default colour temperature range when the entity reports none
// (DEFAULT_MIN_KELVIN / DEFAULT_MAX_KELVIN in light/const.py).
constexpr int32_t kDefaultMinKelvin = 2000;
constexpr int32_t kDefaultMaxKelvin = 6535;

enum class EntityDomain : uint8_t { MediaPlayer, Light, Switch, Unsupported };

// `<domain>.<object_id>`, both [a-z0-9_]+, at most
// limits::kMaxProviderRefLength chars (it is stored as the device's provider
// ref), and not a broadcast value ("all", "none"). Commas/whitespace (lists) are rejected.
bool isValidEntityId(std::string_view entityId);
EntityDomain entityDomain(std::string_view entityId);  // Unsupported if invalid

struct EntityInfo {
    std::string entityId;
    uint32_t supportedFeatures = 0;
    std::vector<std::string> sourceList;           // media_player source_list
    std::vector<std::string> soundModeList;        // media_player sound_mode_list
    std::vector<std::string> supportedColorModes;  // light supported_color_modes
    int32_t minColorTempKelvin = 0;                // light; 0 = not reported
    int32_t maxColorTempKelvin = 0;
};

struct ServiceValue {
    enum class Kind : uint8_t { Bool, Int, Number, String };
    Kind kind = Kind::Int;
    bool boolean = false;
    int32_t integer = 0;
    double number = 0.0;
    std::string string;
};

struct ServiceField {
    std::string key;
    ServiceValue value;
};

// One HA service call: {"domain", "service", "target": {"entity_id"},
// "service_data": {fields}} on the WebSocket API's call_service.
struct ServiceCall {
    std::string domain;
    std::string service;
    std::string entityId;
    std::vector<ServiceField> data;
};

struct CommandDef;  // allowlist entry (id, label, HA service); capabilities.cpp

// Commands for one entity. Owns its option strings and spec arrays; the
// catalog points into them, so keep the object alive (and don't copy it)
// while any compiled config uses the catalog. Moving is fine.
class EntityCapabilities {
public:
    static EntityCapabilities fromEntity(const EntityInfo &entity);

    EntityCapabilities(EntityCapabilities &&) = default;
    EntityCapabilities &operator=(EntityCapabilities &&) = default;
    EntityCapabilities(const EntityCapabilities &) = delete;
    EntityCapabilities &operator=(const EntityCapabilities &) = delete;

    CommandCatalog catalog() const;
    bool empty() const { return commands_.empty(); }
    const std::string &entityId() const { return entityId_; }

    // Enum options left out because they exceed limits::kMaxParamStringLength
    // or limits::kMaxEnumOptions. Shown in discovery so the admin knows.
    size_t droppedOptions() const { return droppedOptions_; }

    // The HA call for catalog command `index` with `params` from
    // resolveParams() (catalog order; ranges already checked there) and its
    // `present` mask (bit i = param i was supplied). Optional params that
    // weren't supplied are left out of the call, so HA keeps its own
    // behaviour (e.g. light.turn_on restores the last brightness). Errors:
    // UnknownCommand (bad index or invalid entity), InvalidParamType /
    // InvalidEnumValue (values not from this catalog).
    ErrorCode buildServiceCall(uint16_t index, const ParamValue *params, uint8_t present,
                               ServiceCall *out) const;

private:
    EntityCapabilities() = default;

    std::string entityId_;
    std::vector<std::string> optionStore_;
    std::vector<const char *> optionPtrs_;
    std::vector<ParamSpec> params_;
    std::vector<const char *> fields_;  // HA data key per params_ entry
    std::vector<CommandSpec> commands_;
    std::vector<const CommandDef *> defs_;  // per command
    std::vector<size_t> firstParam_;        // per command, index into params_/fields_
    size_t droppedOptions_ = 0;
};

// Display label for a command id this file can produce ("Set volume"), or
// nullptr.
const char *commandLabel(std::string_view commandId);

// Names that look like targets or raw service payloads (exact, lowercase:
// param names are ^[a-z][a-z0-9_]*$). They never appear in a catalog, so
// resolveParams() already rejects them as unknown; the API layer uses this to
// report `forbidden_param` instead. api/lint.sh checks the spec's ParamName
// list against this one.
bool isForbiddenParamName(std::string_view name);

}  // namespace hubbo::ha
