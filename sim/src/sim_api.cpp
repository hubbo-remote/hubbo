#include "sim_api.h"

#include <ArduinoJson.h>

#include "hubbo/api/agent_docs.h"
#include "hubbo/api/transitions.h"

#include <cstdlib>
#include <sstream>
#include <string>

namespace hubbo::sim {

namespace {

using http::jsonString;
using http::Method;

std::string logJson(const std::vector<LogLine> &log, size_t limit) {
    std::string out = "[";
    const size_t start = log.size() > limit ? log.size() - limit : 0;
    for (size_t i = start; i < log.size(); i++) {
        if (i != start) out += ",";
        out += "{\"at_ms\":" + std::to_string(log[i].atMs) + ",\"text\":" + jsonString(log[i].text) + "}";
    }
    return out + "]";
}

api::HubInfo hubInfo(bool authEnabled) {
    api::HubInfo info;
    info.version = "0.0.0-sim";
    info.build = "hub-sim";
    info.board = "simulator";
    info.simulatorNoAuth = !authEnabled;
    return info;
}

api::ApiContext context(auth::Credentials &credentials, auth::SessionStore &sessions, const api::Clock &clock,
                        api::Runtime &runtime, bool authEnabled, SimHub &hub, auth::SecretStore *secrets) {
    return api::ApiContext{credentials,
                           sessions,
                           clock,
                           hub.configService(),
                           runtime,
                           hubInfo(authEnabled),
                           &hub.remote(),
                           &hub.haClient(),
                           secrets,
                           nullptr,
                           &hub.bleHosts(),
                           &hub.firmware(),
                           &hub.factoryResetPort(),
                           &hub.hubSettings(),
                           &hub.restartPort(),
                           &hub.mqtt()};
}

// "channel=76&address=e7e7e7e7e7" (hex, 40 bits).
bool parseEndpoint(const http::Request &r, api::RemoteEndpoint *out) {
    const std::string channel(http::queryValue(r.query, "channel"));
    if (channel.empty() || channel.size() > 3) return false;
    char *end = nullptr;
    const unsigned long ch = std::strtoul(channel.c_str(), &end, 10);
    if (*end != '\0' || ch > api::kMaxRemoteChannel) return false;
    out->channel = static_cast<uint8_t>(ch);
    return api::parseRemoteAddress(http::queryValue(r.query, "address"), &out->address) &&
           api::isValidRemoteEndpoint(*out);
}

}  // namespace

SimApi::SimApi(ScenarioRunner &runner, bool authEnabled, auth::Credentials &credentials, SwitchableStorage *storage,
               auth::SecretStore *secrets)
    : runner_(runner),
      authEnabled_(authEnabled),
      storage_(storage),
      credentials_(credentials),
      sessions_(random_),
      clock_(runner.hub()),
      runtime_(runner.hub()),
      secrets_(secrets),
      api_(context(credentials_, sessions_, clock_, runtime_, authEnabled, runner.hub(), secrets)),
      productionAuth_(credentials_, sessions_, clock_) {
    runner.hub().onFirmwareRebooted = [this] { sessions_.clear(); };  // as the board's restart
    runner.hub().restartPort().restarted = [this] { sessions_.clear(); };  // nothing else reboots
    runner.hub().onFactoryReset = [this] { eraseEverything(); };
    api::registerApiRoutes(router_, api_);

    // As the firmware does at boot: the "ha_main" URL and the stored token,
    // and "mqtt_main" with its password.
    api::applyHomeAssistantSettings(runner.hub().configService(), secrets, runner.hub().haClient());
    (void)api::applyMqttSettings(runner.hub().configService(), secrets, runner.hub().mqtt());
    // HA's activity choices reach the engine as the API's do.
    runner.hub().runTransition = [this](const std::string &activityId) {
        (void)api::runTransition(runner_.hub().configService(), runtime_, activityId);
    };

    // The fake MQTT broker: ?reachable=0|1 (0 drops the link: the will
    // fires) and ?auth=reject|ok. GET shows what the hub published.
    host(Method::Post, "/sim/mqtt", [this](auto &r, auto &, auto &out) {
        const std::string reachable = http::queryValue(r.query, "reachable");
        const std::string auth = http::queryValue(r.query, "auth");
        const bool reachableOk = reachable.empty() || reachable == "0" || reachable == "1";
        const bool authOk = auth.empty() || auth == "reject" || auth == "ok";
        if ((reachable.empty() && auth.empty()) || !reachableOk || !authOk) {
            http::jsonError(out, 400, "bad_request", "need reachable=0|1 and/or auth=reject|ok");
            return;
        }
        FakeMqttBroker &broker = runner_.hub().mqttBroker();
        if (!reachable.empty()) broker.reachable = reachable == "1";
        if (!auth.empty()) broker.refuseAuth = auth == "reject";
        runner_.hub().serviceIntegrations();
        out.json(200, mqttJson());
    });
    host(Method::Get, "/sim/mqtt", [this](auto &, auto &, auto &out) { out.json(200, mqttJson()); });
    // ?topic=&payload=&retain=0|1: a message from "HA" to the hub.
    host(Method::Post, "/sim/mqtt/publish", [this](auto &r, auto &, auto &out) {
        const std::string topic = http::queryValue(r.query, "topic");
        const std::string retain = http::queryValue(r.query, "retain");
        if (topic.empty() || !(retain.empty() || retain == "0" || retain == "1")) {
            http::jsonError(out, 400, "bad_request", "need topic= (and retain=0|1)");
            return;
        }
        runner_.hub().mqttBroker().publish(topic, http::queryValue(r.query, "payload"), retain == "1");
        runner_.hub().advance(1);  // delivered, and a posted transition run
        out.json(200, "{}");
    });

    // The remote radio's pair scan: a Hub answers (?channel=N&address=HEX).
    host(Method::Post, "/sim/radio/found", [this](auto &r, auto &, auto &out) {
        api::RemoteEndpoint endpoint;
        if (!parseEndpoint(r, &endpoint)) {
            http::jsonError(out, 400, "bad_request", "need channel=0..124 and a valid 40-bit hex address");
            return;
        }
        runner_.hub().remote().found(endpoint);
        out.json(200, simTime());
    });

    // The fake Home Assistant: ?reachable=0|1 (also drops the connection when
    // 0), ?commands=0|1 (answer service calls or let them time out),
    // ?fail=<domain.service> (HA refuses it; fail=none clears), and
    // ?remove=<entity_id> (HA forgets the entity: calls get not_found, the
    // next discovery misses it), and ?muted=<entity_id>:0|1 (a media
    // player's is_volume_muted, as a volume_mute call sets it; 400 unless
    // it names a media player the fake has). The answer: the client's
    // state, the number of calls and `last_call` (FakeHaServer::lastCallJson(),
    // null until a call; /sim/reset clears it).
    host(Method::Post, "/sim/ha", [this](auto &r, auto &, auto &out) {
        const std::string reachable = http::queryValue(r.query, "reachable");
        const std::string commands = http::queryValue(r.query, "commands");
        SimHub &hub = runner_.hub();
        if (!reachable.empty()) hub.haServer().reachable = reachable == "1";
        if (!commands.empty()) hub.haServer().answerCommands = commands == "1";
        const std::string fail(http::queryValue(r.query, "fail"));
        if (fail == "none") {
            hub.haServer().failing.clear();
        } else if (!fail.empty()) {
            hub.haServer().failing.insert(fail);
        }
        const std::string muted(http::queryValue(r.query, "muted"));
        if (!muted.empty()) {
            const size_t colon = muted.rfind(':');
            const std::string flag = colon == std::string::npos ? "" : muted.substr(colon + 1);
            if ((flag != "0" && flag != "1") || !hub.haServer().setMuted(muted.substr(0, colon), flag == "1")) {
                http::jsonError(out, 400, "bad_request", "muted needs <media player entity_id>:0|1");
                return;
            }
        }
        const std::string remove(http::queryValue(r.query, "remove"));
        if (!remove.empty()) hub.haServer().removeEntity(remove);
        // auth=reject: auth_failed; connect=stall: held in connecting. Reconnect
        // so the change shows at once (auth_failed never retries by itself).
        const std::string auth = http::queryValue(r.query, "auth");
        const std::string connect = http::queryValue(r.query, "connect");
        if (!auth.empty()) hub.haServer().rejectAuth = auth == "reject";
        if (!connect.empty()) hub.haServer().stallConnect = connect == "stall";
        if (!auth.empty() || !connect.empty()) hub.haClient().reconnect();
        hub.serviceIntegrations();
        out.json(200, "{\"state\":" + jsonString(integrations::haStateName(hub.haClient().state())) +
                          ",\"calls\":" + std::to_string(hub.haServer().calls().size()) +
                          ",\"last_call\":" + hub.haServer().lastCallJson() + "}");
    });

    // A host bonds in ?device=<id>'s open pairing window (POST
    // /admin/devices/{id}/pair) and is connected; ?host=<name> names it
    // (default: one of its own). 409 when that device has no open window, or
    // the host is bonded to another device; a full bond store closes the
    // window (also 409).
    host(Method::Post, "/sim/ble/bond", [this](auto &r, auto &, auto &out) {
        const std::string device = http::queryValue(r.query, "device");
        std::string name = http::queryValue(r.query, "host");
        if (device.empty()) {
            http::jsonError(out, 400, "bad_request", "device is required");
            return;
        }
        if (name.empty()) name = "Host of " + device;
        SimulatedBleHosts &hosts = runner_.hub().bleHosts();
        if (!hosts.status(device).pairing || !hosts.bonded(name)) {
            http::jsonError(out, 409, "conflict", "no pairing window for this device took the host");
            return;
        }
        out.json(200, "{}");
    });

    // ?device=<id>&up=0|1: that device's host disconnects or reconnects.
    host(Method::Post, "/sim/ble/link", [this](auto &r, auto &, auto &out) {
        const std::string device = http::queryValue(r.query, "device");
        const std::string up = http::queryValue(r.query, "up");
        if (device.empty() || (up != "0" && up != "1")) {
            http::jsonError(out, 400, "bad_request", "need device and up=0|1");
            return;
        }
        if (!runner_.runLine(std::string("ble ") + (up == "1" ? "connected " : "disconnected ") + device)) {
            http::jsonError(out, 404, "not_found", runner_.error());
            return;
        }
        out.json(200, std::string("{\"up\":") + (up == "1" ? "true" : "false") + "}");
    });

    // The radio: ?ready=0|1 (0: "Bluetooth isn't working", pairing refused).
    host(Method::Post, "/sim/ble/radio", [this](auto &r, auto &, auto &out) {
        const std::string ready = http::queryValue(r.query, "ready");
        if (ready != "0" && ready != "1") {
            http::jsonError(out, 400, "bad_request", "ready must be 0 or 1");
            return;
        }
        runner_.hub().bleHosts().ready = ready == "1";
        out.json(200, std::string("{\"ready\":") + (ready == "1" ? "true" : "false") + "}");
    });

    // Firmware update faults: ?fail=1|0 (writes end in Failed) and/or
    // ?ready=0|1 (install answers Unavailable). /sim/reset restores both.
    host(Method::Post, "/sim/firmware", [this](auto &r, auto &, auto &out) {
        const std::string fail = http::queryValue(r.query, "fail");
        const std::string ready = http::queryValue(r.query, "ready");
        const auto flag = [](const std::string &v) { return v.empty() || v == "0" || v == "1"; };
        if (!flag(fail) || !flag(ready) || (fail.empty() && ready.empty())) {
            http::jsonError(out, 400, "bad_request", "fail and ready must be 0 or 1 (at least one)");
            return;
        }
        api::SimulatedFirmwareUpdate &firmware = runner_.hub().firmware();
        if (!fail.empty()) firmware.failWrites = fail == "1";
        if (!ready.empty()) firmware.ready = ready == "1";
        out.json(200, std::string("{\"fail\":") + (firmware.failWrites ? "true" : "false") +
                          ",\"ready\":" + (firmware.ready ? "true" : "false") + "}");
    });

    // Factory reset availability: ?ready=0|1 (0: POST /admin/factory-reset
    // answers 503). /sim/reset restores it.
    host(Method::Post, "/sim/factory-reset", [this](auto &r, auto &, auto &out) {
        const std::string ready = http::queryValue(r.query, "ready");
        if (ready != "0" && ready != "1") {
            http::jsonError(out, 400, "bad_request", "ready must be 0 or 1");
            return;
        }
        runner_.hub().factoryResetPort().ready = ready == "1";
        out.json(200, std::string("{\"ready\":") + (ready == "1" ? "true" : "false") + "}");
    });

    host(Method::Post, "/sim/remote/{button}/{action}", [this](auto &, auto &p, auto &out) {
        const std::string action(p[1]);
        if (action != "press" && action != "release" && action != "tap") {
            http::jsonError(out, 400, "bad_request", "action must be press, release or tap");
            return;
        }
        if (!runner_.runLine(action + " " + std::string(p[0]))) {
            http::jsonError(out, 400, "bad_request", runner_.error());
            return;
        }
        out.json(200, simTime());
    });

    host(Method::Post, "/sim/advance", [this](auto &r, auto &, auto &out) {
        if (!runner_.runLine("advance " + http::queryValue(r.query, "ms"))) {
            http::jsonError(out, 400, "bad_request", runner_.error());
            return;
        }
        out.json(200, simTime());
    });

    // hub-sim has no web admin at /: an agent pointed at the bare URL gets
    // what /llms.txt says (on the board, / is the web admin).
    host(Method::Get, "/", [](auto &, auto &, auto &out) { api::docs::sendLlmsTxt(out); }, auth::Requirement::Public);

    host(Method::Get, "/sim/log", [this](auto &, auto &, auto &out) {
        SimHub &hub = runner_.hub();
        out.json(200, "{\"commands\":" + logJson(hub.commands(), 200) + ",\"events\":" + logJson(hub.events(), 200) +
                          "}");
    });

    // Back to the seed config, nothing active, faults cleared (persisted);
    // see SimHub::factoryReset().
    host(Method::Post, "/sim/reset", [this](auto &, auto &, auto &out) {
        runner_.runLine("reset");
        out.json(200, simTime());
    });

    // Storage health: ?health=available|unavailable|read_only. Edits then
    // fail (storage_unavailable) while the remote keeps working.
    host(Method::Post, "/sim/storage", [this](auto &r, auto &, auto &out) {
        const std::string wanted = http::queryValue(r.query, "health");
        storage::Health health;
        if (wanted == "available") {
            health = storage::Health::Available;
        } else if (wanted == "unavailable") {
            health = storage::Health::Unavailable;
        } else if (wanted == "read_only") {
            health = storage::Health::ReadOnly;
        } else {
            http::jsonError(out, 400, "bad_request", "health must be available, unavailable or read_only");
            return;
        }
        if (storage_ == nullptr) {
            http::jsonError(out, 409, "not_available", "this hub-sim has no switchable storage");
            return;
        }
        storage_->setHealth(health);
        out.json(200, "{\"health\":" + jsonString(storage::healthName(storage_->health())) + "}");
    });

    // Body: a scenario script, run against the live hub.
    host(Method::Post, "/sim/script", [this](auto &r, auto &, auto &out) {
        std::istringstream script{std::string(r.body)};
        if (!runner_.run(script, "request")) {
            out.json(422, "{\"ok\":false,\"error\":" + jsonString(runner_.error()) + "}");
            return;
        }
        out.json(200, "{\"ok\":true,\"sim_time_ms\":" + std::to_string(runner_.hub().nowMs()) + "}");
    });
}

http::RequestAuthenticator &SimApi::authenticator() {
    if (authEnabled_) return productionAuth_;
    return noAuth_;
}

void SimApi::host(Method method, const char *pattern, Fn::F f, auth::Requirement access) {
    handlers_.push_back(std::make_unique<Fn>(std::move(f)));
    router_.addHostRoute(method, pattern, *handlers_.back(), access);
}

// The board's wiped restart: every secret (the login, tokens, the HA token)
// and session goes, the hub wipes and boots (reloading the credentials), and
// the HA client is set up again from the now tokenless settings.
void SimApi::eraseEverything() {
    if (secrets_ != nullptr) (void)auth::eraseAllSecrets(*secrets_);
    sessions_.clear();
    runner_.hub().wipe(&credentials_);
    api::applyHomeAssistantSettings(runner_.hub().configService(), secrets_, runner_.hub().haClient());
    (void)api::applyMqttSettings(runner_.hub().configService(), secrets_, runner_.hub().mqtt());
}

std::string SimApi::mqttJson() const {
    const FakeMqttBroker &broker = runner_.hub().mqttBroker();
    JsonDocument doc;
    doc["connected"] = broker.connected();
    doc["client_id"] = broker.clientId();
    JsonObject retained = doc["retained"].to<JsonObject>();
    for (const auto &[topic, payload] : broker.retained()) retained[topic] = payload;
    JsonArray published = doc["published"].to<JsonArray>();
    for (const FakeMqttBroker::Message &m : broker.published()) {
        JsonObject o = published.add<JsonObject>();
        o["topic"] = m.topic;
        o["payload"] = m.payload;
        o["retained"] = m.retained;
    }
    JsonArray subscriptions = doc["subscriptions"].to<JsonArray>();
    for (const std::string &filter : broker.subscriptions()) subscriptions.add(filter);
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string SimApi::simTime() const {
    return "{\"sim_time_ms\":" + std::to_string(runner_.hub().nowMs()) + "}";
}

}  // namespace hubbo::sim
