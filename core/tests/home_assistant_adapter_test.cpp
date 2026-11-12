// The HA adapter: catalogs from the device's capabilities snapshot, and the
// engine -> integration task -> HA -> completion path, against hub-sim's
// fake HA server.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fake_ha.h"
#include "fixtures.h"
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
