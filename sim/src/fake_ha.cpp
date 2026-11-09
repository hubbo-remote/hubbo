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

std::string result(uint32_t id, bool success, const std::string &resultJson) {
    std::string out = "{\"id\":" + std::to_string(id) + ",\"type\":\"result\",\"success\":" +
                      (success ? "true" : "false") + ",\"result\":" + resultJson;
    if (!success) out += ",\"error\":{\"code\":\"service_validation_error\",\"message\":\"refused\"}";
    return out + "}";
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
    failing.clear();
    fillerEntities = 0;
    entities_.clear();
    for (const ha::EntityInfo &info : fixtureEntities()) {
        FakeHaEntity e;
        e.info = info;
        e.friendlyName = info.entityId;
        e.uniqueId = "uid_" + info.entityId;
        addEntity(std::move(e));
    }
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
        authed_ = given == token;
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
        const bool ok = failing.count(service) == 0;
        return {result(id, ok, ok ? "{\"context\":{\"id\":\"ctx\"},\"response\":null}" : "null")};
    }
    if (!answerCommands) return {};
    if (type == "get_states") return {result(id, true, statesJson())};
    if (type == "config/entity_registry/list") return {result(id, true, registryJson())};
    if (type == "get_services") return {result(id, true, servicesJson())};
    return {"{\"id\":" + std::to_string(id) +
            ",\"type\":\"result\",\"success\":false,\"error\":{\"code\":\"unknown_command\"}}"};
}

std::string FakeHaServer::statesJson() const {
    JsonDocument doc;
    JsonArray states = doc.to<JsonArray>();
    for (const FakeHaEntity &e : entities_) {
        if (e.disabled) continue;  // HA has no state for a disabled entity
        JsonObject s = states.add<JsonObject>();
        s["entity_id"] = e.info.entityId;
        s["state"] = "off";
        JsonObject a = s["attributes"].to<JsonObject>();
        a["friendly_name"] = e.friendlyName;
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
        s["last_changed"] = "2026-11-09T00:00:00+00:00";
    }
    for (size_t i = 0; i < fillerEntities; i++) {
        JsonObject s = states.add<JsonObject>();
        s["entity_id"] = "sensor.filler_" + std::to_string(i);
        s["state"] = "21.5";
        JsonObject a = s["attributes"].to<JsonObject>();
        a["friendly_name"] = "Filler sensor with a \"quoted\" [bracketed] {braced} name " + std::to_string(i);
        a["unit_of_measurement"] = "°C";
    }
    return serialize(doc);
}

std::string FakeHaServer::registryJson() const {
    JsonDocument doc;
    JsonArray entries = doc.to<JsonArray>();
    for (const FakeHaEntity &e : entities_) {
        if (!e.registered) continue;
        JsonObject r = entries.add<JsonObject>();
        r["entity_id"] = e.info.entityId;
        r["unique_id"] = e.uniqueId;
        r["platform"] = e.platform;
        if (e.name.empty()) {
            r["name"] = nullptr;
        } else {
            r["name"] = e.name;
        }
        r["original_name"] = e.friendlyName;
        if (e.disabled) {
            r["disabled_by"] = "user";
        } else {
            r["disabled_by"] = nullptr;
        }
    }
    for (size_t i = 0; i < fillerEntities; i++) {
        JsonObject r = entries.add<JsonObject>();
        r["entity_id"] = "sensor.filler_" + std::to_string(i);
        r["unique_id"] = "filler_" + std::to_string(i);
        r["platform"] = "demo";
        r["disabled_by"] = nullptr;
    }
    return serialize(doc);
}

std::string FakeHaServer::servicesJson() const {
    return R"({"media_player":{"turn_on":{"fields":{}},"turn_off":{"fields":{}},)"
           R"("volume_set":{"fields":{"volume_level":{"required":true,"selector":{"number":{"min":0,"max":1,"step":0.01}}}}},)"
           R"("select_source":{"fields":{"source":{"required":true,"selector":{"text":{}}}}}},)"
           R"("light":{"turn_on":{"fields":{"brightness":{"selector":{"number":{"min":0,"max":255}}}}},"turn_off":{"fields":{}}},)"
           R"("switch":{"turn_on":{"fields":{}},"turn_off":{"fields":{}}}})";
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
    events_.push_back({Event::Connected, {}});
    for (std::string &reply : server_.onOpen()) events_.push_back({Event::Text, std::move(reply)});
    return true;
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
    for (size_t n = 0; n < maxEventsPerPoll && !events_.empty(); n++) {
        Queued q = std::move(events_.front());
        events_.pop_front();
        if (q.event == Event::Closed) {
            // Anything queued after the close is gone with the connection.
            events_.clear();
            open_ = false;
            closing_ = false;
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
