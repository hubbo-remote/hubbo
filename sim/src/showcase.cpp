#include "showcase.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include "fixtures.h"
#include "hubbo/domain/ids.h"
#include "hubbo/ha/services.h"
#include "hubbo/integrations/ha_client.h"

namespace hubbo::sim {

namespace {

constexpr const char *kSamplePath = HUBBO_SIM_FIXTURES_DIR "/local/real-ha-sample.json";
constexpr const char *kTranslationsPath = HUBBO_SIM_FIXTURES_DIR "/local/real-ha-translations.json";

[[noreturn]] void fail(const std::string &why) {
    std::fprintf(stderr, "hub-sim showcase: %s\n", why.c_str());
    std::abort();
}

JsonDocument readFixture(const char *path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    JsonDocument doc;
    const DeserializationError parsed =
        deserializeJson(doc, text.str(), DeserializationOption::NestingLimit(32));  // HA's selectors nest deep
    if (!in || parsed != DeserializationError::Ok) fail(std::string("can't read ") + path + ": " + parsed.c_str());
    return doc;
}

// sim/fixtures/local/real-ha-sample.json: {"entities": [{entity_id, state,
// platform (null: no registry entry), attributes}], "services": HA's
// /api/services ([{domain, services}])}.
struct Sample {
    std::vector<FakeHaEntity> entities;
    std::string services;  // get_services' shape
    std::vector<std::string> deviceClasses;  // per entity, "" if none
};

const Sample &sample() {
    static const Sample loaded = [] {
        const JsonDocument doc = readFixture(kSamplePath);
        Sample s;
        for (JsonObjectConst e : doc["entities"].as<JsonArrayConst>()) {
            FakeHaEntity entity;
            entity.info.entityId = e["entity_id"] | "";
            entity.state = e["state"] | "";
            JsonObjectConst attrs = e["attributes"];
            serializeJson(attrs, entity.attributesJson);
            entity.friendlyName = attrs["friendly_name"] | "";
            entity.registered = !e["platform"].isNull();
            entity.platform = e["platform"] | "";
            entity.uniqueId = entity.registered ? "uid_" + entity.info.entityId : "";
            s.deviceClasses.emplace_back(attrs["device_class"] | "");
            s.entities.push_back(std::move(entity));
        }
        JsonDocument services;
        for (JsonObjectConst d : doc["services"].as<JsonArrayConst>()) services[d["domain"] | ""] = d["services"];
        serializeJson(services, s.services);
        if (s.entities.empty()) fail(std::string("no entities in ") + kSamplePath);
        return s;
    }();
    return loaded;
}

// The real HA client over the fake HA, driven to completion in-process.
class Fetcher {
public:
    explicit Fetcher(FakeHaServer &server) : transport_(server), client_(transport_) {
        client_.configure("ws://homeassistant.local:8123/api/websocket", server.token, false);
        pumpUntil([&] { return client_.state() == integrations::HaState::Connected; });
    }

    // What registration stores: the entity's details, normalized.
    ha::CapabilitySnapshot snapshot(const std::string &entityId) {
        bool done = false;
        ErrorCode result = ErrorCode::Internal;
        integrations::HaEntityDetails details;
        const ErrorCode started =
            client_.entityDetails(entityId, [&](ErrorCode r, integrations::HaEntityDetails d) {
                result = r;
                details = std::move(d);
                done = true;
            });
        if (started != ErrorCode::Ok) fail(entityId + ": " + errorCodeName(started));
        pumpUntil([&] { return done; });
        if (result != ErrorCode::Ok) fail(entityId + ": " + errorCodeName(result));
        const ha::ServiceDirectory services =
            ha::ServiceDirectory::parse(details.services, {details.platform}, details.translations);
        return ha::makeSnapshot(details.info, details.platform, services);
    }

private:
    template <typename Done>
    void pumpUntil(Done finished) {
        for (int i = 0; i < 10000 && !finished(); i++) {
            transport_.poll(0);
            client_.poll(nowMs_++);
        }
        if (!finished()) fail("the fake HA didn't answer");
    }

    FakeHaTransport transport_;
    integrations::HomeAssistantClient client_;
    uint64_t nowMs_ = 1;
};

// A device kind for a captured entity: its domain, and for a media player
// its device_class or platform.
const char *kindFor(const FakeHaEntity &e, const std::string &deviceClass) {
    switch (ha::entityDomain(e.info.entityId)) {
        case ha::EntityDomain::Light: return "light";
        case ha::EntityDomain::Switch: return "switch";
        case ha::EntityDomain::Fan: return "climate";  // no "fan" kind; fans and purifiers
        default: break;
    }
    if (deviceClass == "receiver") return "receiver";
    if (deviceClass == "speaker" || e.platform == "alexa_media" || e.platform == "group") return "speaker";
    if (e.platform == "apple_tv") return "streamer";
    return "tv";
}

std::string trimmed(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.substr(0, limits::kMaxNameLength);
}

}  // namespace

std::string showcaseMissing() {
    for (const char *path : {kSamplePath, kTranslationsPath}) {
        if (!std::ifstream(path)) return path;
    }
    return "";
}

const std::vector<std::pair<std::string, std::string>> &realHaTranslations() {
    static const std::vector<std::pair<std::string, std::string>> loaded = [] {
        const JsonDocument doc = readFixture(kTranslationsPath);
        std::vector<std::pair<std::string, std::string>> out;
        for (JsonPairConst kv : doc.as<JsonObjectConst>()) out.emplace_back(kv.key().c_str(), kv.value() | "");
        return out;
    }();
    return loaded;
}

const std::vector<FakeHaEntity> &showcaseHaEntities() { return sample().entities; }

const std::string &showcaseServicesJson() { return sample().services; }

ConfigSpec showcaseConfig() {
    ConfigSpec c = fixtureConfig();
    FakeHaServer server;
    server.enableShowcase();
    const Sample &real = sample();

    // The fixture's HA devices stay unless the capture has their entity.
    const auto captured = [&](const std::string &entityId) {
        return std::any_of(real.entities.begin(), real.entities.end(),
                           [&](const FakeHaEntity &e) { return e.info.entityId == entityId; });
    };
    c.devices.erase(std::remove_if(c.devices.begin(), c.devices.end(),
                                   [&](const DeviceSpec &d) { return captured(d.provider.entityId); }),
                    c.devices.end());
    for (size_t i = 0; i < real.entities.size(); i++) {
        const FakeHaEntity &e = real.entities[i];
        const std::string objectId = e.info.entityId.substr(e.info.entityId.find('.') + 1);
        std::string id = ("dev_" + objectId).substr(0, limits::kMaxIdLength);
        while (!id.empty() && id.back() == '_') id.pop_back();
        if (indexOfId(c.devices, id) >= 0) id = id.substr(0, limits::kMaxIdLength - 2) + "_2";
        const std::string name = e.friendlyName.empty() ? objectId : e.friendlyName;
        c.devices.push_back(haDevice(id, trimmed(name), kindFor(e, real.deviceClasses[i]), e.info.entityId));
    }
    c.devices.push_back(bleDevice("dev_fire_tv_stick", "Fire TV Stick", "streamer", "hubbo:fire-tv"));
    c.devices.push_back(bleDevice("dev_shield", "SHIELD TV", "streamer", "hubbo:android-tv"));

    // Every HA device's snapshot, as registration would build it.
    Fetcher fetcher(server);
    for (DeviceSpec &d : c.devices) {
        if (d.provider.type == kHomeAssistantType) d.haCapabilities = fetcher.snapshot(d.provider.entityId);
    }

    // One activity, on the first captured player with sources (none: no activity).
    c.globalBindings = {
        bind("off:tap", {offAction()}),
        bind("light_1:tap", {haAction("dev_lights", "light.turn_on")}),
        bind("light_2:tap", {haAction("dev_lights", "light.turn_off")}),
    };
    c.activities.clear();
    const auto tv = std::find_if(c.devices.begin(), c.devices.end(), [&](const DeviceSpec &d) {
        return captured(d.provider.entityId) && d.haCapabilities && !d.haCapabilities->sourceList.empty() &&
               ha::entityDomain(d.provider.entityId) == ha::EntityDomain::MediaPlayer;
    });
    if (tv != c.devices.end()) {
        const char *id = tv->id.c_str();
        ActivitySpec watch;
        watch.id = kShowcaseActivityId;
        watch.name = "Watch TV";
        watch.revision = 1;
        watch.startup = {
            haAction(id, "media_player.turn_on"),
            haAction(id, "media_player.select_source", {param("source", tv->haCapabilities->sourceList.front().c_str())}),
        };
        watch.shutdown = {haAction(id, "media_player.turn_off")};
        watch.bindings = {
            bind("volume_up:tap", {haAction(id, "media_player.volume_up")}),
            bind("volume_down:tap", {haAction(id, "media_player.volume_down")}),
            bind("mute:tap", {haAction(id, "media_player.volume_mute", {param("muted", true)})}),
        };
        c.activities.push_back(std::move(watch));
        c.globalBindings.push_back(bind("tv:tap", {activateAction(kShowcaseActivityId)}));
    }
    return c;
}

}  // namespace hubbo::sim
