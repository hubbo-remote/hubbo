// The HA adapter: catalogs from the device's capabilities snapshot, and the
// engine -> integration task -> HA -> completion path, against hub-sim's
// fake HA server.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fake_ha.h"
#include "fixtures.h"
#include "hubbo/config/json.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/engine/engine.h"
#include "hubbo/integrations/ha_adapter.h"
#include "hubbo/ha/snapshot.h"

using namespace hubbo;
using namespace hubbo::integrations;
using hubbo::sim::FakeHaServer;
using hubbo::sim::FakeHaTransport;

namespace {

DeviceSpec lightDevice(uint32_t features = ha::light_feature::kTransition) {
    DeviceSpec d = sim::haDevice("dev_lights", "Lights", "light", "light.family_room");
    ha::CapabilitySnapshot snapshot;
    snapshot.platform = "demo";
    snapshot.supportedFeatures = features;
    snapshot.supportedColorModes = {"color_temp"};
    d.haCapabilities = snapshot;
    return d;
}

DeviceSpec denonDevice() {
    DeviceSpec d = sim::haDevice("dev_denon", "Denon", "receiver", "media_player.denon");
    ha::CapabilitySnapshot snapshot;
    snapshot.platform = "denonavr";
    snapshot.supportedFeatures = ha::media_player_feature::kVolumeMute | ha::media_player_feature::kVolumeStep;
    d.haCapabilities = snapshot;
    return d;
}

struct Completions : CompletionSink {
    std::vector<std::pair<Ticket, ExecResult>> results;
    void complete(Ticket ticket, ExecResult result, uint32_t) override { results.push_back({ticket, result}); }
};

class AdapterTest : public ::testing::Test {
protected:
    AdapterTest() {
        client.configure("ws://ha.local:8123/api/websocket", server.token);
        integrationTask(300);
    }

    // The integration task: transport, client, adapter.
    void integrationTask(uint64_t ms = 100) {
        for (uint64_t t = 0; t < ms; t += 100) {
            now += 100;
            transport.poll(0);
            client.poll(now);
            adapter.service();
            transport.poll(0);
        }
    }

    ExecRequest request(const CommandCatalog &catalog, const char *command, Ticket ticket,
                        const char *entityId = "light.family_room") {
        const int index = catalog.find(command);
        EXPECT_GE(index, 0) << command;
        // As resolveParams() leaves them: every param typed, none supplied.
        const CommandSpec &spec = catalog.items[index];
        for (uint8_t i = 0; i < spec.paramCount; i++) params_[i] = spec.params[i].defaultValue;
        return ExecRequest{&spec, static_cast<uint16_t>(index), params_, 0, "dev_lights", entityId, ticket, 0, false};
    }

    FakeHaServer server;
    FakeHaTransport transport{server};
    HomeAssistantClient client{transport};
    HomeAssistantAdapter adapter{"ha_main", client};
    Completions sink;
    ParamValue params_[limits::kMaxParams];
    uint64_t now = 0;
};

}  // namespace

TEST_F(AdapterTest, CommandsComeFromTheStoredSnapshot) {
    DeviceSpec none = sim::haDevice("dev_x", "X", "light", "light.x");
    EXPECT_EQ(adapter.commandsFor(none).count, 0u);  // no snapshot: can't be targeted
    const CommandCatalog a = adapter.commandsFor(lightDevice());
    ASSERT_GT(a.count, 0u);
    EXPECT_GE(a.find("light.turn_on"), 0);
    EXPECT_EQ(adapter.commandsFor(lightDevice()).items, a.items);  // same snapshot: same storage
    const CommandCatalog b = adapter.commandsFor(lightDevice(0));
    EXPECT_NE(b.items, a.items);  // changed: new storage...
    EXPECT_GE(a.find("light.turn_on"), 0);  // ...and the old one still valid
    EXPECT_STREQ(adapter.integrationType(), kHomeAssistantType);
}

TEST_F(AdapterTest, ExecuteIsPendingUntilHaAnswers) {
    const CommandCatalog catalog = adapter.commandsFor(lightDevice());
    ASSERT_EQ(client.state(), HaState::Connected);
    EXPECT_EQ(adapter.execute(request(catalog, "light.turn_on", 7)).status, ExecStatus::Pending);
    adapter.poll(0, sink);
    EXPECT_TRUE(sink.results.empty());  // nothing until the integration task ran
    integrationTask();
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 1u);
    EXPECT_EQ(sink.results[0].first, 7u);
    EXPECT_EQ(sink.results[0].second.status, ExecStatus::Accepted);
    ASSERT_EQ(server.calls().size(), 1u);
    EXPECT_EQ(server.calls()[0], "light.turn_on light.family_room");  // the registered entity only

    server.failing.insert("light.turn_on");
    EXPECT_EQ(adapter.execute(request(catalog, "light.turn_on", 8)).status, ExecStatus::Pending);
    integrationTask();
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 2u);
    EXPECT_EQ(sink.results[1].second.status, ExecStatus::Failed);
    EXPECT_EQ(sink.results[1].second.error, ErrorCode::AdapterFailed);
}

TEST_F(AdapterTest, UnavailableAtOnceWhileDisconnectedOrFull) {
    const CommandCatalog catalog = adapter.commandsFor(lightDevice());
    transport.dropConnection();
    integrationTask();
    ASSERT_NE(client.state(), HaState::Connected);
    const ExecResult r = adapter.execute(request(catalog, "light.turn_on", 1));
    EXPECT_EQ(r.status, ExecStatus::Failed);
    EXPECT_EQ(r.error, ErrorCode::AdapterUnavailable);

    integrationTask(1500);  // reconnected
    ASSERT_EQ(client.state(), HaState::Connected);
    for (Ticket t = 1; t <= HomeAssistantAdapter::kQueueDepth; t++) {
        EXPECT_EQ(adapter.execute(request(catalog, "light.turn_on", t)).status, ExecStatus::Pending);
    }
    const ExecResult full = adapter.execute(request(catalog, "light.turn_on", 99));
    EXPECT_EQ(full.status, ExecStatus::Failed);
    EXPECT_EQ(full.error, ErrorCode::AdapterUnavailable);
    integrationTask();
    adapter.poll(0, sink);
    EXPECT_EQ(sink.results.size(), HomeAssistantAdapter::kQueueDepth);
}

TEST_F(AdapterTest, NothingIsReplayedAfterAReconnect) {
    const CommandCatalog catalog = adapter.commandsFor(lightDevice());
    server.answerCommands = false;
    EXPECT_EQ(adapter.execute(request(catalog, "light.turn_on", 5)).status, ExecStatus::Pending);
    integrationTask();  // sent, no answer
    transport.dropConnection();
    integrationTask(1500);
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 1u);
    EXPECT_EQ(sink.results[0].second.error, ErrorCode::AdapterUnavailable);
    server.answerCommands = true;
    integrationTask(1000);
    EXPECT_EQ(server.calls().size(), 1u);  // the one attempt only
}

// End to end through the engine: compile against the snapshot, run a
// startup, the HA result completes the sequence.
TEST_F(AdapterTest, AnActivityDrivesHaThroughTheEngine) {
    AdapterSet adapters;
    ASSERT_EQ(adapters.add(adapter), ErrorCode::Ok);
    Engine::Store store;
    Engine engine{store, adapters};

    ConfigSpec cfg;
    cfg.integrations = {sim::integration("ha_main", kHomeAssistantType, "Home Assistant")};
    cfg.devices = {lightDevice()};
    ActivitySpec a;
    a.id = "act_lights";
    a.name = "Lights";
    a.startup = {sim::haAction("dev_lights", "light.turn_on")};
    cfg.activities = {a};
    ValidationReport report;
    std::unique_ptr<CompiledConfig> compiled = compileConfig(cfg, adapters, &report);
    ASSERT_TRUE(compiled) << (report.ok() ? "" : report.issues()[0].path);
    ASSERT_EQ(store.publish(compiled), ErrorCode::Ok);
    ASSERT_EQ(client.state(), HaState::Connected);

    ASSERT_EQ(engine.activate("act_lights", 1000), ErrorCode::Ok);
    EXPECT_FALSE(engine.executor().idle());  // waiting for HA
    integrationTask();
    engine.poll(1100);
    EXPECT_TRUE(engine.executor().idle());
    EXPECT_EQ(engine.executor().stats().completed, 1u);
    EXPECT_EQ(server.calls(), (std::vector<std::string>{"light.turn_on light.family_room"}));
}

// Each new domain end to end (the fake's made-up entities): the snapshot
// registration stores, through a saved config and back, then a command with
// params to the right HA service with the right data.
TEST_F(AdapterTest, TheNewDomainsRoundTripAndCallTheRightServices) {
    ASSERT_EQ(client.state(), HaState::Connected);
    ConfigSpec cfg;
    cfg.revision = 1;
    cfg.globalBindingsRevision = 1;
    cfg.integrations = {sim::integration("ha_main", kHomeAssistantType, "Home Assistant")};
    struct Case {
        const char *entityId;
        const char *command;
        std::vector<ParamInput> params;
        const char *call;  // what HA was asked
    };
    const auto text = [](const char *name, const char *value) { return ParamInput{name, ParamLiteral::ofString(value)}; };
    const auto number = [](const char *name, double value) { return ParamInput{name, ParamLiteral::ofNumber(value)}; };
    const std::vector<Case> cases = {
        {"scene.movie_night", "scene.turn_on", {}, "scene.turn_on scene.movie_night"},
        {"script.goodnight", "script.turn_on", {}, "script.turn_on script.goodnight"},
        {"script.goodnight", "script.turn_off", {}, "script.turn_off script.goodnight"},
        {"cover.office_blinds", "cover.stop_cover", {}, "cover.stop_cover cover.office_blinds"},
        {"cover.office_blinds", "cover.set_cover_position", {number("position", 40)},
         "cover.set_cover_position cover.office_blinds position=40"},
        {"button.restart_router", "button.press", {}, "button.press button.restart_router"},
        {"input_boolean.guest_mode", "input_boolean.toggle", {}, "input_boolean.toggle input_boolean.guest_mode"},
        {"humidifier.bedroom_humidifier", "humidifier.set_humidity", {number("humidity", 45)},
         "humidifier.set_humidity humidifier.bedroom_humidifier humidity=45"},
        {"humidifier.bedroom_humidifier", "humidifier.set_mode", {text("mode", "eco")},
         "humidifier.set_mode humidifier.bedroom_humidifier mode=eco"},
        {"select.dryer_cycle", "select.select_option", {text("option", "towels")},
         "select.select_option select.dryer_cycle option=towels"},
        {"select.dryer_cycle", "select.select_next", {}, "select.select_next select.dryer_cycle"},
    };
    for (const Case &c : cases) {
        if (std::any_of(cfg.devices.begin(), cfg.devices.end(),
                        [&](const DeviceSpec &d) { return d.provider.entityId == c.entityId; }))
            continue;
        const std::string id = c.entityId;
        cfg.devices.push_back(sim::haDevice("dev_" + id.substr(id.find('.') + 1), "Test", "other", id));
    }
    ASSERT_EQ(cfg.devices.size(), 7u);  // one per new domain
    sim::addFakeHaSnapshots(&cfg, server);

    ConfigSpec loaded;
    const std::string saved = config::serializeConfig(cfg);
    const config::ParseResult parsed = config::parseConfig(saved, &loaded);
    ASSERT_EQ(parsed.code, ErrorCode::Ok)
        << (parsed.report.ok() ? "" : parsed.report.issues()[0].path + " " + errorCodeName(parsed.report.issues()[0].code));
    ASSERT_EQ(loaded.devices.size(), cfg.devices.size());
    for (size_t i = 0; i < cfg.devices.size(); i++) {
        ASSERT_TRUE(loaded.devices[i].haCapabilities) << cfg.devices[i].id;
        EXPECT_EQ(*loaded.devices[i].haCapabilities, *cfg.devices[i].haCapabilities) << cfg.devices[i].id;
    }

    Ticket ticket = 100;
    for (const Case &c : cases) {
        SCOPED_TRACE(c.command);
        const auto device = std::find_if(loaded.devices.begin(), loaded.devices.end(),
                                         [&](const DeviceSpec &d) { return d.provider.entityId == c.entityId; });
        ASSERT_NE(device, loaded.devices.end());
        const CommandCatalog catalog = adapter.commandsFor(*device);
        const int index = catalog.find(c.command);
        ASSERT_GE(index, 0);
        ParamValue values[limits::kMaxParams];
        std::string bad;
        uint8_t present = 0;
        ASSERT_EQ(resolveParams(catalog.items[index], c.params, values, &bad, nullptr, &present), ErrorCode::Ok) << bad;
        const ExecRequest request{&catalog.items[index], static_cast<uint16_t>(index), values, present,
                                  device->id.c_str(), c.entityId, ++ticket, 0, false};
        ASSERT_EQ(adapter.execute(request).status, ExecStatus::Pending);
        integrationTask();
        adapter.poll(0, sink);
        ASSERT_FALSE(sink.results.empty());
        EXPECT_EQ(sink.results.back().first, ticket);
        EXPECT_EQ(sink.results.back().second.status, ExecStatus::Accepted);
        ASSERT_FALSE(server.calls().empty());
        EXPECT_EQ(server.calls().back(), c.call);
    }
}

// ---- a command that reads the current state first (mute toggle) ----------

namespace {

constexpr const char *kDenon = "media_player.denon";
constexpr const char *kMute = "media_player.volume_mute media_player.denon is_volume_muted=true";
constexpr const char *kUnmute = "media_player.volume_mute media_player.denon is_volume_muted=false";

std::vector<std::string> muteCalls(const FakeHaServer &server) {
    std::vector<std::string> out;
    for (const std::string &c : server.calls()) {
        if (c.rfind("media_player.volume_mute", 0) == 0) out.push_back(c);
    }
    return out;
}

}  // namespace

TEST_F(AdapterTest, MuteToggleReadsTheStateThenSendsTheOpposite) {
    const CommandCatalog catalog = adapter.commandsFor(denonDevice());
    ASSERT_GE(catalog.find("media_player.mute_toggle"), 0);
    ASSERT_EQ(server.muted(kDenon), false);

    EXPECT_EQ(adapter.execute(request(catalog, "media_player.mute_toggle", 1, kDenon)).status, ExecStatus::Pending);
    integrationTask(300);  // read, then call
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 1u);
    EXPECT_EQ(sink.results[0].first, 1u);
    EXPECT_EQ(sink.results[0].second.status, ExecStatus::Accepted);
    EXPECT_EQ(muteCalls(server), (std::vector<std::string>{kMute}));
    EXPECT_EQ(server.muted(kDenon), true);  // the fake applied it

    // Muted now: the next press unmutes, and the one after mutes again.
    EXPECT_EQ(adapter.execute(request(catalog, "media_player.mute_toggle", 2, kDenon)).status, ExecStatus::Pending);
    integrationTask(300);
    EXPECT_EQ(adapter.execute(request(catalog, "media_player.mute_toggle", 3, kDenon)).status, ExecStatus::Pending);
    integrationTask(300);
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 3u);
    EXPECT_EQ(sink.results[2].second.status, ExecStatus::Accepted);
    EXPECT_EQ(muteCalls(server), (std::vector<std::string>{kMute, kUnmute, kMute}));
    EXPECT_EQ(server.subscriptions(), 0u);  // every read ended its subscription

    // A command without state never reads.
    const size_t received = server.received().size();
    EXPECT_EQ(adapter.execute(request(catalog, "media_player.volume_up", 4, kDenon)).status, ExecStatus::Pending);
    integrationTask();
    for (size_t i = received; i < server.received().size(); i++) {
        EXPECT_EQ(server.received()[i].find("subscribe_entities"), std::string::npos);
    }
}

TEST_F(AdapterTest, MuteToggleWithTheAttributeMissingMutes) {
    // A player that is off: HA leaves is_volume_muted out.
    for (sim::FakeHaEntity &e : server.entities()) {
        if (e.info.entityId != kDenon) continue;
        e.state = "off";
        e.attributesJson = R"({"friendly_name":"Denon","supported_features":1036})";
    }
    const CommandCatalog catalog = adapter.commandsFor(denonDevice());
    EXPECT_EQ(adapter.execute(request(catalog, "media_player.mute_toggle", 1, kDenon)).status, ExecStatus::Pending);
    integrationTask(300);
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 1u);
    EXPECT_EQ(sink.results[0].second.status, ExecStatus::Accepted);
    EXPECT_EQ(muteCalls(server), (std::vector<std::string>{kMute}));
}

TEST_F(AdapterTest, MuteToggleWhenTheReadTimesOutStillMutes) {
    // Already muted, but the read gets no answer: unknown, so it mutes.
    ASSERT_TRUE(server.setMuted(kDenon, true));
    const CommandCatalog catalog = adapter.commandsFor(denonDevice());
    server.answerCommands = false;
    EXPECT_EQ(adapter.execute(request(catalog, "media_player.mute_toggle", 1, kDenon)).status, ExecStatus::Pending);
    integrationTask(HomeAssistantClient::kStateReadTimeoutMs - 100);
    EXPECT_TRUE(muteCalls(server).empty());  // still reading
    integrationTask(200);
    // Sent within the executor's 5 s bound (the fake records it unanswered).
    EXPECT_EQ(muteCalls(server), (std::vector<std::string>{kMute}));
    // The read and the call share kCommandTimeoutMs (the executor's 5 s):
    // the call gets what the read left.
    integrationTask(HomeAssistantClient::kCommandTimeoutMs - HomeAssistantClient::kStateReadTimeoutMs - 100);
    adapter.poll(0, sink);
    EXPECT_TRUE(sink.results.empty());  // the call's own result is still to come
    integrationTask(100);  // 5 s since the read began
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 1u);  // one result for the ticket: the call's
    EXPECT_EQ(sink.results[0].second.error, ErrorCode::AdapterTimeout);
}

TEST_F(AdapterTest, MuteToggleFailures) {
    const CommandCatalog catalog = adapter.commandsFor(denonDevice());

    // HA has no state for the entity: EntityNotFound, nothing sent.
    server.removeEntity(kDenon);
    EXPECT_EQ(adapter.execute(request(catalog, "media_player.mute_toggle", 1, kDenon)).status, ExecStatus::Pending);
    integrationTask(300);
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 1u);
    EXPECT_EQ(sink.results[0].second.status, ExecStatus::Failed);
    EXPECT_EQ(sink.results[0].second.error, ErrorCode::EntityNotFound);
    EXPECT_TRUE(muteCalls(server).empty());

    // The connection drops during the read: unavailable, nothing sent.
    server.reset();
    server.answerCommands = false;
    EXPECT_EQ(adapter.execute(request(catalog, "media_player.mute_toggle", 2, kDenon)).status, ExecStatus::Pending);
    integrationTask();
    transport.dropConnection();
    integrationTask();
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 2u);
    EXPECT_EQ(sink.results[1].second.error, ErrorCode::AdapterUnavailable);
    EXPECT_TRUE(muteCalls(server).empty());

    // HA refuses the call itself: AdapterFailed, as any call.
    server.answerCommands = true;
    integrationTask(1500);
    ASSERT_EQ(client.state(), HaState::Connected);
    server.failing.insert("media_player.volume_mute");
    EXPECT_EQ(adapter.execute(request(catalog, "media_player.mute_toggle", 3, kDenon)).status, ExecStatus::Pending);
    integrationTask(300);
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 3u);
    EXPECT_EQ(sink.results[2].second.error, ErrorCode::AdapterFailed);
    EXPECT_EQ(server.muted(kDenon), false);  // refused: not applied
}

// ---- a Home Assistant action (call_service) --------------------------------

namespace {

constexpr const char *kLgTv = "media_player.family_room_lg_tv";

DeviceSpec lgTvDevice() {
    DeviceSpec d = sim::haDevice("dev_lg_tv", "LG TV", "tv", kLgTv);
    ha::CapabilitySnapshot snapshot;
    snapshot.platform = "webostv";
    snapshot.supportedFeatures = ha::media_player_feature::kTurnOn | ha::media_player_feature::kTurnOff;
    ha::SnapshotCustomService button;
    button.service = "webostv.button";
    button.label = "Button";
    snapshot.customServices = {button};
    d.haCapabilities = snapshot;
    return d;
}

// The LG TV with one activity, "act_tv", whose startup is `startup`.
ConfigSpec tvConfig(std::vector<ActionSpec> startup) {
    ConfigSpec cfg;
    cfg.integrations = {sim::integration("ha_main", kHomeAssistantType, "Home Assistant")};
    cfg.devices = {lgTvDevice()};
    ActivitySpec a;
    a.id = "act_tv";
    a.name = "TV";
    a.startup = std::move(startup);
    cfg.activities = {a};
    return cfg;
}

ActionSpec infoButton(const char *data = R"({"button":"INFO"})") {
    ActionSpec a = sim::haAction("dev_lg_tv", ha::kCallServiceCommandId);
    a.call = HaServiceCallSpec{"webostv.button", data};
    return a;
}

}  // namespace

TEST_F(AdapterTest, AHomeAssistantActionIsSentAsWritten) {
    const CommandCatalog catalog = adapter.commandsFor(lgTvDevice());
    ASSERT_GE(catalog.find(ha::kCallServiceCommandId), 0);
    auto call = std::make_shared<const HaServiceCallSpec>(
        HaServiceCallSpec{"webostv.button", R"({"button":"INFO","n":{"x":[1]}})"});
    const std::weak_ptr<const HaServiceCallSpec> watch = call;
    ExecRequest r = request(catalog, ha::kCallServiceCommandId, 1, kLgTv);
    r.call = call;
    EXPECT_EQ(adapter.execute(r).status, ExecStatus::Pending);
    // Whoever compiled it lets go (a direct execute's request is freed, a
    // config replaced): the adapter's copy still sends it, then lets go too.
    r.call.reset();
    call.reset();
    EXPECT_FALSE(watch.expired());
    integrationTask();
    EXPECT_TRUE(watch.expired());
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 1u);
    EXPECT_EQ(sink.results[0].second.status, ExecStatus::Accepted);
    EXPECT_EQ(server.lastCallJson(), R"({"service":"webostv.button","entity_id":"media_player.family_room_lg_tv",)"
                                     R"("data":{"button":"INFO","n":{"x":[1]}}})");
    // The device's entity is the only target.
    EXPECT_NE(server.received().back().find(R"("target":{"entity_id":"media_player.family_room_lg_tv"})"),
              std::string::npos)
        << server.received().back();

    // A stale entity is refused unsent, as any command.
    server.removeEntity(kLgTv);
    bool discovered = false;
    ASSERT_EQ(client.discover([&](ErrorCode, HaDiscovery) { discovered = true; }), ErrorCode::Ok);
    for (int i = 0; i < 50 && !discovered; i++) integrationTask();
    ASSERT_TRUE(client.entityStale(kLgTv));
    const size_t calls = server.calls().size();
    r.ticket = 2;
    r.call = std::make_shared<const HaServiceCallSpec>(HaServiceCallSpec{"webostv.button", R"({"button":"INFO"})"});
    EXPECT_EQ(adapter.execute(r).status, ExecStatus::Pending);
    integrationTask();
    adapter.poll(0, sink);
    ASSERT_EQ(sink.results.size(), 2u);
    EXPECT_EQ(sink.results[1].second.error, ErrorCode::EntityNotFound);
    EXPECT_EQ(server.calls().size(), calls);
}

TEST_F(AdapterTest, AHomeAssistantActionCompilesAndRunsThroughTheEngine) {
    AdapterSet adapters;
    ASSERT_EQ(adapters.add(adapter), ErrorCode::Ok);
    Engine::Store store;
    Engine engine{store, adapters};
    const ConfigSpec cfg = tvConfig({infoButton()});
    ValidationReport report;
    std::unique_ptr<CompiledConfig> compiled = compileConfig(cfg, adapters, &report);
    ASSERT_TRUE(compiled) << (report.ok() ? "" : report.issues()[0].path);
    ASSERT_NE(compiled->actions.at(0).call, nullptr);
    EXPECT_EQ(compiled->actions.at(0).call->data, R"({"button":"INFO"})");
    // The compiled config owns its calls: a dry run's go with it.
    std::weak_ptr<const HaServiceCallSpec> dryRun = compileConfig(cfg, adapters, &report)->actions.at(0).call;
    EXPECT_TRUE(dryRun.expired());
    ASSERT_EQ(store.publish(compiled), ErrorCode::Ok);

    ASSERT_EQ(engine.activate("act_tv", 1000), ErrorCode::Ok);
    integrationTask();
    engine.poll(1100);
    EXPECT_TRUE(engine.executor().idle());
    EXPECT_EQ(engine.executor().stats().completed, 1u);
    EXPECT_EQ(server.calls(), (std::vector<std::string>{"webostv.button media_player.family_room_lg_tv button=INFO"}));

    // Directly, as the execute route compiles it.
    CompiledCommand direct;
    ValidationReport issues;
    const HaServiceCallSpec back{"webostv.button", R"({"button":"BACK"})"};
    ASSERT_EQ(compileDeviceCommand(cfg, adapters, "dev_lg_tv", ha::kCallServiceCommandId, {}, &direct, &issues, &back),
              ErrorCode::Ok);
    ASSERT_NE(direct.action.call, nullptr);
    EXPECT_EQ(*direct.action.call, back);
}

TEST_F(AdapterTest, AHomeAssistantActionStaysOnItsDevice) {
    // Only the device's own integration's services (webostv.* on a webostv
    // TV): nothing aimed at HA as a whole, in a config or run directly.
    AdapterSet adapters;
    ASSERT_EQ(adapters.add(adapter), ErrorCode::Ok);
    for (const char *service : {"homeassistant.stop", "hassio.host_shutdown", "webostvx.button", "light.turn_on"}) {
        ActionSpec action = infoButton();
        action.call->service = service;
        const ConfigSpec cfg = tvConfig({action});
        ValidationReport report;
        EXPECT_FALSE(compileConfig(cfg, adapters, &report)) << service;
        EXPECT_TRUE(report.has(ErrorCode::ForbiddenParam, "/activities/0/startup/0/call/service")) << service;

        CompiledCommand direct;
        ValidationReport issues;
        const HaServiceCallSpec call{service, "{}"};
        EXPECT_EQ(compileDeviceCommand(cfg, adapters, "dev_lg_tv", ha::kCallServiceCommandId, {}, &direct, &issues,
                                       &call),
                  ErrorCode::ValidationFailed)
            << service;
        EXPECT_TRUE(issues.has(ErrorCode::ForbiddenParam, "/call/service")) << service;
    }
}

TEST_F(AdapterTest, HomeAssistantActionsHaveASizeBudget) {
    AdapterSet adapters;
    ASSERT_EQ(adapters.add(adapter), ErrorCode::Ok);
    ConfigSpec cfg = tvConfig(std::vector<ActionSpec>(limits::kMaxHaServiceCalls, infoButton()));
    ValidationReport report;
    EXPECT_TRUE(compileConfig(cfg, adapters, &report)) << (report.ok() ? "" : report.issues()[0].path);
    cfg.activities[0].shutdown = {infoButton()};  // one over, anywhere in the config
    report = ValidationReport();
    EXPECT_FALSE(compileConfig(cfg, adapters, &report));
    EXPECT_TRUE(report.has(ErrorCode::CapacityExceeded, "/activities/0/shutdown/0/call"));

    // Not an HA device (no call_service in its catalog): unknown_command.
    ActionSpec onTheLights = infoButton();
    onTheLights.deviceId = "dev_lights";
    cfg.devices.push_back(lightDevice());
    cfg.activities[0].shutdown = {onTheLights};
    report = ValidationReport();
    EXPECT_FALSE(compileConfig(cfg, adapters, &report));
    EXPECT_TRUE(report.has(ErrorCode::UnknownCommand, "/activities/0/shutdown/0/command_id"));

    // The devices' custom service hints together: ha::kMaxConfigHintBytes.
    cfg.activities[0].shutdown.clear();
    cfg.devices = {};
    size_t total = 0;
    size_t crossing = 0;
    for (size_t i = 0; total <= ha::kMaxConfigHintBytes; i++) {
        DeviceSpec d = lgTvDevice();
        d.id = "dev_tv_" + std::to_string(i);
        d.haCapabilities->customServices[0].label = std::string(1500, 'h');
        total += ha::customHintBytes(d.haCapabilities->customServices[0]);
        crossing = i;
        cfg.devices.push_back(std::move(d));
    }
    cfg.activities[0].startup.clear();
    report = ValidationReport();
    EXPECT_FALSE(compileConfig(cfg, adapters, &report));
    EXPECT_TRUE(report.has(ErrorCode::CapacityExceeded,
                           "/devices/" + std::to_string(crossing) + "/ha_capabilities/custom_services"));
    EXPECT_EQ(report.issues().size(), 1u);
    cfg.devices.pop_back();  // at or under the budget
    report = ValidationReport();
    EXPECT_TRUE(compileConfig(cfg, adapters, &report)) << (report.ok() ? "" : report.issues()[0].path);
}
