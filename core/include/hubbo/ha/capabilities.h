#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
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
//   join/unjoin (take other entity ids), browse/search, or anything taking
//   free text or URLs. Toggle only where HA's own UI leans on it (covers,
//   humidifiers, input booleans, remotes), plus mute: HA has none, so mute_toggle
//   reads is_volume_muted at press time and sends the opposite.
//
// Feature bits, attribute names and service gating were checked against
// HA's dev branch: homeassistant/components/<domain>/const.py and
// services.py (2026-11-13). docs/ha-capabilities.md has the tables.
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

// FanEntityFeature, homeassistant/components/fan/const.py.
namespace fan_feature {
constexpr uint32_t kSetSpeed = 1;
constexpr uint32_t kOscillate = 2;
constexpr uint32_t kDirection = 4;
constexpr uint32_t kPresetMode = 8;
constexpr uint32_t kTurnOff = 16;
constexpr uint32_t kTurnOn = 32;
}  // namespace fan_feature

// CoverEntityFeature, homeassistant/components/cover/const.py.
namespace cover_feature {
constexpr uint32_t kOpen = 1;
constexpr uint32_t kClose = 2;
constexpr uint32_t kSetPosition = 4;
constexpr uint32_t kStop = 8;
constexpr uint32_t kOpenTilt = 16;
constexpr uint32_t kCloseTilt = 32;
constexpr uint32_t kStopTilt = 64;
constexpr uint32_t kSetTiltPosition = 128;
}  // namespace cover_feature

// HumidifierEntityFeature, homeassistant/components/humidifier/const.py.
namespace humidifier_feature {
constexpr uint32_t kModes = 1;
}  // namespace humidifier_feature

// HA's default colour temperature range when the entity reports none
// (DEFAULT_MIN_KELVIN / DEFAULT_MAX_KELVIN in light/const.py).
constexpr int32_t kDefaultMinKelvin = 2000;
constexpr int32_t kDefaultMaxKelvin = 6535;

enum class EntityDomain : uint8_t {
    MediaPlayer, Light, Switch, Fan, Scene, Script, Cover, Button, InputBoolean, Humidifier, Select, Remote,
    Unsupported
};

// The entity domains with a curated allowlist, in EntityDomain order. The
// spec's discovery `domain` enum matches it (api/tools/validate.py checks).
constexpr const char *kEntityDomains[] = {"media_player", "light",         "switch",     "fan",
                                          "scene",        "script",        "cover",      "button",
                                          "input_boolean", "humidifier",   "select",     "remote"};
static_assert(sizeof(kEntityDomains) / sizeof(kEntityDomains[0]) == static_cast<size_t>(EntityDomain::Unsupported),
              "one name per EntityDomain");
// "media_player, light, ... or remote", for messages.
std::string entityDomainList();
EntityDomain domainNamed(std::string_view domain);  // "fan" -> Fan; Unsupported if none

// `<domain>.<object_id>`, both [a-z0-9_]+, at most
// limits::kMaxProviderRefLength chars (it is stored as the device's provider
// ref), and not a broadcast value ("all", "none"). Commas/whitespace (lists) are rejected.
bool isValidEntityId(std::string_view entityId);
EntityDomain entityDomain(std::string_view entityId);  // Unsupported if invalid
// The text before the first '.' ("light" for "light.lamp"), any domain,
// unchecked; the whole id if it has no dot.
std::string_view entityDomainName(std::string_view entityId);

// Integration services (e.g. sonos.*) are offered only for media players,
// lights, switches, fans and remotes; the other newer domains get their
// allowlist alone. A remote also gets HA's own `remote` services beyond the
// allowlist (send_command, ...) as custom services: isDeviceService().
bool offersIntegrationServices(std::string_view entityId);

// A fan's speed-slider step: HA's percentage_step (100 / speeds) rounded down
// to 1..100, so every multiple still lands on the speed it means (HA rounds a
// percentage up to the next speed). 0: none (not reported, or under 1).
int32_t fanSpeedStep(double percentageStep);

// The HA service domains whose services an entity can be offered beyond the
// allowlist, in offer (and hint budget) order: "remote" first for a remote
// (docs/design/ha-custom-action.md: the one exception), then its platform unless
// that's empty or the entity's own domain. Also the get_services domains
// ServiceDirectory::parse() must keep.
std::vector<std::string> serviceDomains(std::string_view entityId, std::string_view platform);

// A service a Home Assistant action may send to this entity: isHaServiceId,
// with its domain in serviceDomains(). The one rule for custom services'
// snapshot decode and core's compiler.
bool isDeviceService(std::string_view entityId, std::string_view platform, std::string_view service);

// An HA service some allowlisted command calls (`remote`, `turn_on`).
bool isAllowlistedService(std::string_view domain, std::string_view service);

// A humidifier's reported range as whole percent within 0..100, lo < hi:
// the one rule for the snapshot and the command. False: not reported or
// unusable (HA's default 0..100 applies).
bool humidityRange(double min, double max, int32_t *lo, int32_t *hi);

// The device kind (a DeviceKind, GET /kinds) a new device of this entity
// starts with: by `domain` ("media_player"), and for a media player by its
// device_class or platform (apple_tv -> streamer). Never nullptr: "other"
// when nothing fits.
const char *suggestedKind(std::string_view domain, std::string_view platform, std::string_view deviceClass);

struct EntityInfo {
    std::string entityId;
    uint32_t supportedFeatures = 0;
    std::vector<std::string> sourceList;           // media_player source_list
    std::vector<std::string> soundModeList;        // media_player sound_mode_list
    std::vector<std::string> supportedColorModes;  // light supported_color_modes
    int32_t minColorTempKelvin = 0;                // light; 0 = not reported
    int32_t maxColorTempKelvin = 0;
    std::vector<std::string> presetModes;          // fan preset_modes
    double percentageStep = 0.0;                   // fan percentage_step (100 / speeds); 0 = not reported
    // humidifier min_humidity / max_humidity; not reported (min >= max):
    // HA's 0..100.
    double minHumidity = 0.0;
    double maxHumidity = 0.0;
    std::vector<std::string> availableModes;       // humidifier available_modes
    std::vector<std::string> options;              // select options
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
    // A Home Assistant action (call_service): the service_data as a JSON
    // object's text, sent verbatim instead of `data`. "" for every other call.
    std::string rawData;
};

// The Home Assistant action: an HA device's command for any of its custom
// services (CapabilitySnapshot::customServices) with data the user writes.
// Built with buildCustomCall(), never buildServiceCall().
constexpr const char *kCallServiceCommandId = "call_service";

// ^[a-z][a-z0-9_]{0,31}\.[a-z][a-z0-9_]{0,63}$: a Home Assistant action's
// service ("webostv.button").
bool isHaServiceId(std::string_view service);

// How a command uses the state attribute it reads at press time
// (stateAttribute()). Toggle: a bool HA has no toggle for, sent back negated
// under the same key (mute_toggle). Step: a number the command's one param
// is added to, clamped to 0..100 and sent under the param's data key
// (cover.position_step).
enum class StateUse : uint8_t { Toggle, Step };

struct CommandDef;  // allowlist entry (id, label, HA service); capabilities.cpp
struct CapabilitySnapshot;  // snapshot.h
struct Drafts;              // capabilities.cpp

// Commands for one entity. Owns its option strings and spec arrays; the
// catalog points into them, so keep the object alive (and don't copy it)
// while any compiled config uses the catalog. Moving is fine.
class EntityCapabilities {
public:
    static EntityCapabilities fromEntity(const EntityInfo &entity);
    // A registered device's commands, from its stored snapshot: the
    // allowlist for its attributes plus the snapshot's integration services
    // (re-checked: they must belong to the snapshot's platform, with valid,
    // non-target-like field names; others are left out). No HA needed.
    static EntityCapabilities fromSnapshot(std::string_view entityId, const CapabilitySnapshot &snapshot);

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
    //
    // `state`: the value of stateAttribute(index) in the entity's current
    // state attributes, read just before sending; nullptr when it wasn't read,
    // the entity left it out (HA drops it when a player is off or reports
    // None) or it was null/an object/an array. Commands with no
    // stateAttribute ignore it. A toggle sends the opposite; an unknown or
    // non-bool state counts as false (mute_toggle then mutes). A step
    // (cover.position_step) sends its param plus the state, clamped to
    // 0..100; with no numeric state it sends nothing: StateUnavailable.
    ErrorCode buildServiceCall(uint16_t index, const ParamValue *params, uint8_t present,
                               ServiceCall *out, const ServiceValue *state = nullptr) const;

    // The Home Assistant action for this entity: `service`
    // (isHaServiceId) with `dataJson`, a JSON object's text checked by the
    // config decoder ("" = {}), sent verbatim. UnknownCommand when the
    // catalog has no call_service (no custom services) or the service isn't
    // shaped right.
    ErrorCode buildCustomCall(std::string_view service, std::string_view dataJson, ServiceCall *out) const;

    // The state attribute catalog command `index` reads at press time
    // ("is_volume_muted" for media_player.mute_toggle, "current_position"
    // for cover.position_step). nullptr: it needs no
    // state, or the index is out of range. Valid while this object is.
    const char *stateAttribute(uint16_t index) const;

private:
    EntityCapabilities() = default;
    static EntityCapabilities build(std::string_view entityId, Drafts drafts);
    const char *keep(std::string text);  // owned, address-stable string

    struct Owned {
        const char *id = nullptr;
        const char *domain = nullptr;  // HA service domain
        const char *service = nullptr;
        const char *stateAttribute = nullptr;  // static (CommandDef), or nullptr
        const char *group = nullptr;           // static (CommandDef), or nullptr
        StateUse stateUse = StateUse::Toggle;  // CommandDef::stateUse
    };

    std::string entityId_;
    std::deque<std::string> strings_;  // ids, labels, names, data keys (stable addresses)
    std::vector<std::string> optionStore_;
    std::vector<const char *> optionPtrs_;
    std::vector<ParamSpec> params_;
    std::vector<const char *> fields_;  // HA data key per params_ entry
    std::vector<CommandSpec> commands_;
    std::vector<Owned> owned_;              // per command
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
// list (and HaServiceCall's data keys, for isTargetKey) against these.
bool isForbiddenParamName(std::string_view name);

// The target keys (entity_id, device_id, area_id, floor_id, label_id): never
// in a Home Assistant action's data, since the target is always the device.
// A subset of the forbidden param names.
bool isTargetKey(std::string_view name);

// ^[a-z][a-z0-9_]{0,31}$: an HA integration, service or field name as Hubbo
// accepts it (get_services parsing, stored snapshots, the safety re-check).
bool isHaName(std::string_view name);

// Keeps usable enum options (non-empty, within kMaxParamStringLength, no
// duplicates, at most kMaxEnumOptions) and counts the rest in `dropped`.
// False if none are left.
bool filterEnumOptions(std::vector<std::string> &options, size_t *dropped);

}  // namespace hubbo::ha
