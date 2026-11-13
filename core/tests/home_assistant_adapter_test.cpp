// The HA adapter: catalogs from the device's capabilities snapshot, and the
// engine -> integration task -> HA -> completion path, against hub-sim's
// fake HA server.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fake_ha.h"
#include "fixtures.h"
#include "hubbo/config/json.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/engine/engine.h"
#include "hubbo/integrations/ha_adapter.h"

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

    ExecRequest request(const CommandCatalog &catalog, const char *command, Ticket ticket) {
        const int index = catalog.find(command);
        EXPECT_GE(index, 0) << command;
        // As resolveParams() leaves them: every param typed, none supplied.
        const CommandSpec &spec = catalog.items[index];
        for (uint8_t i = 0; i < spec.paramCount; i++) params_[i] = spec.params[i].defaultValue;
        return ExecRequest{&spec, static_cast<uint16_t>(index), params_, 0, "dev_lights", "light.family_room",
                           ticket, 0, false};
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
