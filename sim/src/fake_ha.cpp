#include "fake_ha.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "fakes.h"
#include "fixtures.h"
#include "hubbo/ha/services.h"

namespace hubbo::sim {

namespace {

std::string serialize(const JsonDocument &doc) {
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string result(uint32_t id, const std::string &resultJson) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"result\",\"success\":true,\"result\":" + resultJson + "}";
}

std::string error(uint32_t id, const std::string &code) {
    return "{\"id\":" + std::to_string(id) + ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"" +
           code + "\",\"message\":\"" + code + "\"}}";
}

std::string valueText(JsonVariantConst v) {
    if (v.is<bool>()) return v.as<bool>() ? "true" : "false";
    if (v.is<long long>()) return std::to_string(v.as<long long>());
    if (v.is<double>()) return formatNumber(v.as<double>());  // as FakeHomeAssistant logs them
    if (v.is<const char *>()) return quoteIfSpaced(v.as<const char *>());
    return serialize(JsonDocument(v));
}

}  // namespace

FakeHaServer::FakeHaServer() { reset(); }

void FakeHaServer::reset() {
    reachable = true;
    answerPings = true;
    answerCommands = true;
    rejectAuth = false;
    stallConnect = false;
    failing.clear();
    fillerEntities = 0;
    entities_.clear();
    for (FixtureHaEntity &fixture : fixtureHaEntities()) {
        FakeHaEntity e;
        e.uniqueId = "uid_" + fixture.info.entityId;
        e.info = std::move(fixture.info);
        e.friendlyName = std::move(fixture.name);
        e.platform = std::move(fixture.platform);
        addEntity(std::move(e));
    }
    // A template switch from configuration.yaml: a state, no registry entry.
    FakeHaEntity yaml;
    yaml.info.entityId = kYamlOnlyEntity;
    yaml.friendlyName = "Patio Fountain";
    yaml.registered = false;
    yaml.platform.clear();
    addEntity(std::move(yaml));
}

void FakeHaServer::removeEntity(std::string_view entityId) {
    entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                   [&](const FakeHaEntity &e) { return e.info.entityId == entityId; }),
                    entities_.end());
}

void FakeHaServer::addEntity(FakeHaEntity entity) {
    for (auto &e : entities_) {
        if (e.info.entityId == entity.info.entityId) {
            e = std::move(entity);
            return;
        }
    }
    entities_.push_back(std::move(entity));
}

std::vector<std::string> FakeHaServer::onOpen() {
    connections_++;
    subscriptions_.clear();  // a new connection has none
    authed_ = false;
    return {"{\"type\":\"auth_required\",\"ha_version\":\"2026.11.0\"}"};
}

std::vector<std::string> FakeHaServer::onMessage(const std::string &text) {
    JsonDocument doc;
    if (deserializeJson(doc, text)) return {};
    const std::string type = doc["type"] | "";
    if (type == "auth") {
        authAttempts_++;
        JsonDocument logged = doc;
        logged["access_token"] = "<redacted>";
        received_.push_back(serialize(logged));
        const std::string given = doc["access_token"] | "";
        authed_ = !rejectAuth && given == token;
        return {authed_ ? "{\"type\":\"auth_ok\",\"ha_version\":\"2026.11.0\"}"
                        : "{\"type\":\"auth_invalid\",\"message\":\"Invalid access token or password\"}"};
    }
    received_.push_back(text);
    if (!authed_) return {};
    const uint32_t id = doc["id"] | 0u;
    if (type == "ping") {
        if (!answerPings) return {};
        return {"{\"id\":" + std::to_string(id) + ",\"type\":\"pong\"}"};
    }
    if (type == "call_service") {
        const std::string service = std::string(doc["domain"] | "") + "." + std::string(doc["service"] | "");
        std::string line = service + " " + std::string(doc["target"]["entity_id"] | "");
        for (JsonPairConst field : doc["service_data"].as<JsonObjectConst>()) {
            line += " " + std::string(field.key().c_str()) + "=" + valueText(field.value());
        }
        calls_.push_back(line);  // received, answered or not
        if (!answerCommands) return {};
        // As real HA: an entity it doesn't have still gets success.
        if (failing.count(service) != 0) return {error(id, "service_validation_error")};
        return {result(id, "{\"context\":{\"id\":\"ctx\"},\"response\":null}")};
    }
    if (!answerCommands) return {};
    if (type == "config/entity_registry/list_for_display") return {result(id, displayJson())};
    if (type == "config/entity_registry/get") {
        const std::string entityId = doc["entity_id"] | "";
        for (const FakeHaEntity &e : entities_) {
            if (e.registered && e.info.entityId == entityId) return {result(id, registryEntry(e))};
        }
        return {error(id, "not_found")};
    }
    if (type == "subscribe_entities") {
        // As HA: the result, then at once an event with every current state.
        subscriptions_.insert(id);
        std::vector<std::string> ids;
        for (JsonVariantConst v : doc["entity_ids"].as<JsonArrayConst>()) ids.emplace_back(v | "");
        return {result(id, "null"), "{\"id\":" + std::to_string(id) + ",\"type\":\"event\",\"event\":{\"a\":" +
                                              compressedStates(ids) + "}}"};
    }
    if (type == "unsubscribe_events") {
        if (subscriptions_.erase(doc["subscription"] | 0u) == 0) return {error(id, "not_found")};
        return {result(id, "null")};
    }
    if (type == "get_services") return {result(id, servicesJson())};
    return {error(id, "unknown_command")};
}

std::string FakeHaServer::compressedStates(const std::vector<std::string> &ids) const {
    const auto wanted = [&](const std::string &entityId) {
        return ids.empty() || std::find(ids.begin(), ids.end(), entityId) != ids.end();
    };
    JsonDocument doc;
    JsonObject states = doc.to<JsonObject>();
    for (const FakeHaEntity &e : entities_) {
        if (e.disabled || !wanted(e.info.entityId)) continue;  // HA has no state for a disabled entity
        JsonObject s = states[e.info.entityId].to<JsonObject>();
        const bool player = ha::entityDomain(e.info.entityId) == ha::EntityDomain::MediaPlayer;
        s["s"] = player ? "on" : "off";
        JsonObject a = s["a"].to<JsonObject>();
        a["friendly_name"] = e.friendlyName;
        if (player) {
            a["volume_level"] = 0.35;
            a["is_volume_muted"] = false;
            if (!e.info.sourceList.empty()) a["source"] = e.info.sourceList.front();
        }
        a["supported_features"] = e.info.supportedFeatures;
        auto list = [&](const char *key, const std::vector<std::string> &items) {
            if (items.empty()) return;
            JsonArray arr = a[key].to<JsonArray>();
            for (const auto &item : items) arr.add(item);
        };
        list("source_list", e.info.sourceList);
        list("sound_mode_list", e.info.soundModeList);
        list("supported_color_modes", e.info.supportedColorModes);
        if (e.info.minColorTempKelvin != 0) a["min_color_temp_kelvin"] = e.info.minColorTempKelvin;
        if (e.info.maxColorTempKelvin != 0) a["max_color_temp_kelvin"] = e.info.maxColorTempKelvin;
        s["c"] = "01JC0000000000000000000000";
        s["lc"] = 1762646400.0;
    }
    for (size_t i = 0; i < fillerEntities; i++) {
        const std::string entityId = "sensor.filler_" + std::to_string(i);
        if (!wanted(entityId)) continue;
        JsonObject s = states[entityId].to<JsonObject>();
        s["s"] = "21.5";
        JsonObject a = s["a"].to<JsonObject>();
        a["friendly_name"] = "Filler sensor with a \"quoted\" [bracketed] {braced} name " + std::to_string(i);
        a["unit_of_measurement"] = "°C";
    }
    return serialize(doc);
}

std::string FakeHaServer::displayJson() const {
    // list_for_display: enabled registry entries only, compact keys.
    JsonDocument doc;
    doc["entity_categories"]["0"] = "config";
    doc["entity_categories"]["1"] = "diagnostic";
    JsonArray entries = doc["entities"].to<JsonArray>();
    for (const FakeHaEntity &e : entities_) {
        if (!e.registered || e.disabled) continue;
        JsonObject r = entries.add<JsonObject>();
        r["ei"] = e.info.entityId;
        r["pl"] = e.platform;
        if (!e.name.empty()) r["en"] = e.name;
        r["di"] = "dev_" + e.uniqueId;
    }
    for (size_t i = 0; i < fillerEntities; i++) {
        JsonObject r = entries.add<JsonObject>();
        r["ei"] = "sensor.filler_" + std::to_string(i);
        r["pl"] = "demo";
        r["ec"] = 1;
    }
    return serialize(doc);
}

std::string FakeHaServer::registryEntry(const FakeHaEntity &e) const {
    JsonDocument doc;
    doc["entity_id"] = e.info.entityId;
    doc["unique_id"] = e.uniqueId;
    doc["platform"] = e.platform;
    doc["name"] = e.name.empty() ? nullptr : e.name.c_str();  // null: HA's own
    doc["original_name"] = e.friendlyName;
    doc["disabled_by"] = e.disabled ? "user" : nullptr;
    return serialize(doc);
}

std::string FakeHaServer::servicesJson() const {
    // Domain services (the hub's own allowlist decides those), plus
    // integration services shaped like real HA's, one per selector kind the
    // hub maps (boolean, number, select) and one it skips (text).
    const auto service = [](const char *name, const char *integration, const char *fields) {
        return std::string("\"") + name + "\":{\"target\":{\"entity\":[{\"integration\":\"" + integration +
               "\",\"domain\":[\"media_player\"]}]},\"fields\":{" + fields + "}}";
    };
    return R"({"media_player":{"turn_on":{"fields":{}},"turn_off":{"fields":{}},)"
           R"("volume_set":{"fields":{"volume_level":{"required":true,"selector":{"number":{"min":0,"max":1,"step":0.01}}}}},)"
           R"("select_source":{"fields":{"source":{"required":true,"selector":{"text":{}}}}}},)"
           R"("light":{"turn_on":{"fields":{"brightness":{"selector":{"number":{"min":0,"max":255}}}}},"turn_off":{"fields":{}}},)"
           R"("switch":{"turn_on":{"fields":{}},"turn_off":{"fields":{}}},)"
           R"("denonavr":{)" +
           service("set_dynamic_eq", "denonavr", R"("dynamic_eq":{"required":true,"selector":{"boolean":{}}})") + "," +
           service("update_audyssey", "denonavr", "") + "," +
           service("get_command", "denonavr", R"("command":{"required":true,"selector":{"text":{}}})") +
           R"(},"sonos":{)" +
           service("set_sleep_timer", "sonos",
                   R"("sleep_time":{"selector":{"number":{"min":0,"max":7200,"step":1,"unit_of_measurement":"s"}}})") +
           "," +
           service("play_queue", "sonos", R"("queue_position":{"selector":{"number":{"min":0,"max":10000000,"mode":"box"}}})") +
           "," +
           service("set_option", "sonos",
                   R"("night_sound":{"selector":{"boolean":{}}},"speech_enhance":{"selector":{"boolean":{}}})") +
           R"(},"webostv":{)" +
           service("select_sound_output", "webostv",
                   R"("sound_output":{"required":true,"selector":{"select":{"options":)"
                   R"(["tv_speaker","external_arc","external_optical","bt_soundbar"]}}})") +
           "}}";
}

// ---- transport ---------------------------------------------------------

bool FakeHaTransport::connect(const std::string &url, const integrations::ConnectOptions &options) {
    if (open_) return false;
    connectAttempts++;
    lastOptions = options;
    if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) return false;
    url_ = url;
    open_ = true;
    closing_ = false;
    if (!server_.reachable) {
        events_.push_back({Event::Closed, {}});
        closing_ = true;
        return true;
    }
    if (server_.stallConnect) {
        stalled_ = true;  // poll() completes it once the stall is lifted
    } else {
        openLink();
    }
    return true;
}

void FakeHaTransport::openLink() {
    events_.push_back({Event::Connected, {}});
    for (std::string &reply : server_.onOpen()) events_.push_back({Event::Text, std::move(reply)});
}

bool FakeHaTransport::send(std::string_view text) {
    if (!open_ || closing_) return false;
    for (std::string &reply : server_.onMessage(std::string(text))) {
        if (reply.size() > maxMessageBytes) {  // the platform's cap: drop it and close
            events_.push_back({Event::Closed, {}});
            closing_ = true;
            break;
        }
        events_.push_back({Event::Text, std::move(reply)});
    }
    return true;
}

void FakeHaTransport::close() {
    if (!open_ || closing_) return;
    events_.push_back({Event::Closed, {}});
    closing_ = true;
}

void FakeHaTransport::dropConnection() { close(); }

void FakeHaTransport::poll(uint32_t timeoutMs) {
    (void)timeoutMs;  // never waits: everything is already queued
    if (!server_.reachable) close();  // the network went away: the link drops
    if (stalled_ && !closing_ && !server_.stallConnect) {
        stalled_ = false;
        openLink();
    }
    for (size_t n = 0; n < maxEventsPerPoll && !events_.empty(); n++) {
        Queued q = std::move(events_.front());
        events_.pop_front();
        if (q.event == Event::Closed) {
            // Anything queued after the close is gone with the connection.
            events_.clear();
            open_ = false;
            closing_ = false;
            stalled_ = false;
            if (listener_ != nullptr) listener_->onClosed();
            return;
        }
        if (listener_ == nullptr) continue;
        if (q.event == Event::Connected) {
            if (beforeConnected) beforeConnected();
            listener_->onConnected();
        } else {
            listener_->onText(std::move(q.text));
        }
    }
}

void addFakeHaSnapshots(ConfigSpec *config, const FakeHaServer &server) {
    std::vector<std::string> platforms;
    for (const FakeHaEntity &e : server.entities()) platforms.push_back(e.platform);
    const ha::ServiceDirectory services = ha::ServiceDirectory::parse(server.servicesJson(), platforms);
    for (DeviceSpec &device : config->devices) {
        if (device.provider.type != kHomeAssistantType || device.haCapabilities) continue;
        for (const FakeHaEntity &e : server.entities()) {
            if (e.registered && e.info.entityId == device.provider.entityId) {
                device.haCapabilities = ha::makeSnapshot(e.info, e.platform, services);
            }
        }
    }
}

}  // namespace hubbo::sim
