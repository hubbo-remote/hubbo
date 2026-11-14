// The HA WebSocket client against hub-sim's fake HA server: the handshake,
// states and backoff, liveness, commands and chunked discovery.
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "fake_ha.h"
#include "hubbo/integrations/ha_client.h"

using namespace hubbo;
using namespace hubbo::integrations;
using hubbo::sim::FakeHaEntity;
using hubbo::sim::FakeHaServer;
using hubbo::sim::FakeHaTransport;

namespace {

constexpr const char *kUrl = "ws://ha.local:8123/api/websocket";

class ClientTest : public ::testing::Test {
protected:
    // The integration task's loop, in 100 ms steps.
    void run(uint64_t ms, uint64_t step = 100) {
        for (uint64_t t = 0; t < ms; t += step) tick(step);
    }
    void tick(uint64_t step = 100) {
        now += step;
        transport.poll(0);
        client.poll(now);
        transport.poll(0);
    }
    // Runs a discovery to its end (or `ms`); its result, Busy if unfinished.
    ErrorCode discoverNow(HaDiscovery *out = nullptr, uint64_t ms = 40000) {
        ErrorCode result = ErrorCode::Busy;
        EXPECT_EQ(client.discover([&](ErrorCode e, HaDiscovery d) {
                      result = e;
                      if (out != nullptr) *out = std::move(d);
                  }),
                  ErrorCode::Ok);
        for (uint64_t t = 0; t < ms && result == ErrorCode::Busy; t += 100) tick();
        return result;
    }
    void connect() {
        client.configure(kUrl, server.token);
        run(300);
        ASSERT_EQ(client.state(), HaState::Connected);
    }

    FakeHaServer server;
    FakeHaTransport transport{server};
    uint64_t now = 0;
    HomeAssistantClient client{transport, [this] { return now; }};
};

ha::ServiceCall turnOn(const char *entity = "light.family_room") {
    ha::ServiceCall call;
    call.domain = "light";
    call.service = "turn_on";
    call.entityId = entity;
    ha::ServiceField f;
    f.key = "brightness";
    f.value.kind = ha::ServiceValue::Kind::Int;
    f.value.integer = 128;
    call.data.push_back(f);
    return call;
}

}  // namespace

TEST_F(ClientTest, ASlowConnectStillAuthenticates) {
    // A TLS connect that takes 12 s inside transport.poll(), and the
    // handshake spread over the following polls: the auth deadline counts
    // from the clock after the connect, not from the poll before it.
    transport.maxEventsPerPoll = 1;
    transport.beforeConnected = [&] { now += 12000; };
    client.configure(kUrl, server.token);
    run(500);
    EXPECT_EQ(client.state(), HaState::Connected);
    EXPECT_EQ(transport.connectAttempts, 1);
}

TEST_F(ClientTest, VerifyTlsReachesTheTransportAndAChangeReconnects) {
    connect();
    EXPECT_TRUE(transport.lastOptions.verifyTls);
    client.configure(kUrl, server.token, false);
    run(300);
    EXPECT_EQ(client.state(), HaState::Connected);
    EXPECT_FALSE(transport.lastOptions.verifyTls);
    EXPECT_EQ(transport.connectAttempts, 2);
}

TEST_F(ClientTest, NotConfiguredUntilItHasAUrlAndAToken) {
    run(5000);
    EXPECT_EQ(client.state(), HaState::NotConfigured);
    EXPECT_EQ(transport.connectAttempts, 0);
    client.configure(kUrl, "");
    run(5000);
    EXPECT_EQ(client.state(), HaState::NotConfigured);
    EXPECT_STREQ(haStateName(client.state()), "not_configured");
}

TEST_F(ClientTest, AWssUrlIsPassedThroughAsGiven) {
    // An https HA: homeAssistantSocketUrl() gives wss://, the transport does TLS.
    const std::string url = "wss://ha.example.org/api/websocket";
    client.configure(url, server.token);
    run(300);
    EXPECT_EQ(client.state(), HaState::Connected);
    EXPECT_EQ(transport.lastUrl(), url);
}

TEST_F(ClientTest, AuthenticatesAndNeverLeaksTheToken) {
    connect();
    EXPECT_EQ(transport.lastUrl(), kUrl);
    EXPECT_EQ(server.authAttempts(), 1);
    for (const std::string &m : server.received()) {
        EXPECT_EQ(m.find(server.token), std::string::npos) << m;
    }
    EXPECT_NE(server.received().front().find("<redacted>"), std::string::npos);
    EXPECT_STREQ(haStateName(HaState::Connected), "connected");
}

TEST_F(ClientTest, ABadTokenIsAuthFailedAndNotRetriedUntilTheConfigChanges) {
    client.configure(kUrl, "wrong");
    run(300);
    EXPECT_EQ(client.state(), HaState::AuthFailed);
    run(10 * 60 * 1000, 1000);
    EXPECT_EQ(server.connections(), 1);  // no retry
    client.configure(kUrl, server.token);
    run(300);
    EXPECT_EQ(client.state(), HaState::Connected);
}

TEST_F(ClientTest, UnreachableRetriesBackOffFrom1sTo60s) {
    server.reachable = false;
    client.configure(kUrl, server.token);
    std::vector<uint64_t> attempts;
    for (int i = 0; i < 3000; i++) {  // 300 s
        const int before = transport.connectAttempts;
        tick();
        if (transport.connectAttempts != before) attempts.push_back(now);
    }
    EXPECT_EQ(client.state(), HaState::Unreachable);
    // First at once, then +1, +2, +4 ... capped at +60 s.
    const std::vector<uint64_t> expected = {100, 1100, 3100, 7100, 15100, 31100, 63100, 123100, 183100, 243100};
    EXPECT_EQ(attempts, expected);
    // Back: it reconnects at the next retry, and the backoff starts over.
    server.reachable = true;
    run(60000);
    EXPECT_EQ(client.state(), HaState::Connected);
}

TEST_F(ClientTest, ADroppedConnectionReconnectsAfterASecond) {
    connect();
    transport.dropConnection();
    tick();
    EXPECT_EQ(client.state(), HaState::Unreachable);
    run(1100);
    EXPECT_EQ(client.state(), HaState::Connected);
    EXPECT_EQ(server.connections(), 2);
}

TEST_F(ClientTest, PingsEvery30sAndAMissingPongMeansUnreachable) {
    connect();
    run(31000);
    int pings = 0;
    for (const std::string &m : server.received()) pings += m.find("\"ping\"") != std::string::npos ? 1 : 0;
    EXPECT_EQ(pings, 1);
    EXPECT_EQ(client.state(), HaState::Connected);  // pong arrived

    // Silence: the next ping (30 s after the last pong) gets no answer, and
    // 10 s later the connection is given up.
    server.answerPings = false;
    const uint64_t lastPong = 30100;  // auth_ok at 100, ping at 30100, pong at once
    while (client.state() == HaState::Connected && now < 200000) tick();
    EXPECT_EQ(now, lastPong + HomeAssistantClient::kPingIntervalMs + HomeAssistantClient::kPongTimeoutMs);
    EXPECT_EQ(client.state(), HaState::Unreachable);
    run(1100);  // the backoff's first retry
    EXPECT_EQ(client.state(), HaState::Connected);
    EXPECT_EQ(server.connections(), 2);
}

TEST_F(ClientTest, CallServiceResults) {
    std::vector<ErrorCode> results;
    auto record = [&](ErrorCode e) { results.push_back(e); };
    EXPECT_EQ(client.callService(turnOn(), record), ErrorCode::AdapterUnavailable);  // not connected
    connect();
    ASSERT_EQ(client.callService(turnOn(), record), ErrorCode::Ok);
    tick();
    server.failing.insert("light.turn_on");
    ASSERT_EQ(client.callService(turnOn(), record), ErrorCode::Ok);
    tick();
    EXPECT_EQ(results, (std::vector<ErrorCode>{ErrorCode::Ok, ErrorCode::AdapterFailed}));
    EXPECT_EQ(server.calls().front(), "light.turn_on light.family_room brightness=128");
}

TEST_F(ClientTest, CallsTimeOutAndAreBounded) {
    connect();
    server.answerCommands = false;
    std::vector<ErrorCode> results;
    for (size_t i = 0; i < HomeAssistantClient::kMaxPendingCommands; i++) {
        ASSERT_EQ(client.callService(turnOn(), [&](ErrorCode e) { results.push_back(e); }), ErrorCode::Ok);
    }
    EXPECT_EQ(client.callService(turnOn(), [&](ErrorCode e) { results.push_back(e); }), ErrorCode::QueueFull);
    run(HomeAssistantClient::kCommandTimeoutMs - 200);
    EXPECT_TRUE(results.empty());
    run(400);
    ASSERT_EQ(results.size(), HomeAssistantClient::kMaxPendingCommands);
    for (ErrorCode e : results) EXPECT_EQ(e, ErrorCode::AdapterTimeout);
}

TEST_F(ClientTest, ADropFailsCallsInFlight) {
    connect();
    server.answerCommands = false;
    std::vector<ErrorCode> results;
    ASSERT_EQ(client.callService(turnOn(), [&](ErrorCode e) { results.push_back(e); }), ErrorCode::Ok);
    transport.dropConnection();
    tick();
    EXPECT_EQ(results, (std::vector<ErrorCode>{ErrorCode::AdapterUnavailable}));
}

TEST_F(ClientTest, DiscoveryWalksTheResultsInStepsAndKeepsOnlySupportedDomains) {
    server.fillerEntities = 600;  // sensors: not offered, but parsed past
    sim::FakeHaEntity unregistered;
    unregistered.info.entityId = "switch.porch";
    unregistered.friendlyName = "Porch";
    unregistered.registered = false;
    server.addEntity(unregistered);
    sim::FakeHaEntity disabled;
    disabled.info.entityId = "light.attic";
    disabled.disabled = true;
    server.addEntity(disabled);
    sim::FakeHaEntity named;
    named.info.entityId = "media_player.den";
    named.friendlyName = "Den speaker";
    named.name = "Den";
    named.platform = "sonos";
    named.uniqueId = "RINCON_1";
    server.addEntity(named);
    connect();

    int yields = 0;
    client.setYield([&] { yields++; });
    bool done = false;
    ErrorCode result = ErrorCode::Internal;
    HaDiscovery discovery;
    ASSERT_EQ(client.discover([&](ErrorCode e, HaDiscovery d) {
                  done = true;
                  result = e;
                  discovery = std::move(d);
              }),
              ErrorCode::Ok);
    EXPECT_EQ(client.discover(nullptr), ErrorCode::Busy);
    int polls = 0;
    while (!done && polls < 1000) {
        tick();
        polls++;
    }
    ASSERT_TRUE(done);
    ASSERT_EQ(result, ErrorCode::Ok);
    EXPECT_GT(polls, 2 * 600 / static_cast<int>(HomeAssistantClient::kParseStepEntities) - 2);  // in steps
    EXPECT_GT(yields, 10);

    std::vector<std::string> ids;
    for (const HaCandidate &c : discovery.candidates) ids.push_back(c.entityId);
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
    EXPECT_EQ(std::count(ids.begin(), ids.end(), "light.attic"), 0);
    EXPECT_EQ(std::count_if(ids.begin(), ids.end(), [](const std::string &s) { return s.rfind("sensor.", 0) == 0; }), 0);
    EXPECT_EQ(discovery.candidates.size(), server.entities().size() - 1);  // all but the disabled one
    EXPECT_EQ(discovery.truncated, 0u);
    EXPECT_GT(discovery.elapsedMs, 0u);
    for (const HaCandidate &c : discovery.candidates) {
        if (c.entityId == "media_player.den") {
            EXPECT_EQ(c.name, "Den speaker");  // friendly_name first
            EXPECT_EQ(c.platform, "sonos");
            EXPECT_EQ(c.domain, "media_player");
            EXPECT_EQ(c.state, "on");
        }
        if (c.entityId == "switch.porch") {  // YAML-only: no registry entry
            EXPECT_EQ(c.platform, "");
            EXPECT_EQ(c.name, "Porch");
        }
        if (c.entityId == "media_player.family_room_lg_tv") {
            EXPECT_FALSE(c.info.sourceList.empty());
            EXPECT_NE(c.info.supportedFeatures, 0u);
            EXPECT_EQ(c.platform, "webostv");
        }
    }
    EXPECT_EQ(server.subscriptions(), 0u);  // the state snapshot's subscription was ended
    // What it asked for: the compact registry and the states, no services.
    std::string asked;
    for (const std::string &m : server.received()) asked += m;
    EXPECT_NE(asked.find("list_for_display"), std::string::npos);
    EXPECT_NE(asked.find("subscribe_entities"), std::string::npos);
    EXPECT_EQ(asked.find("get_services"), std::string::npos);
}

TEST_F(ClientTest, AnOversizedMessageFailsTheDiscoveryAndTheClientRecovers) {
    server.fillerEntities = 200;
    connect();
    transport.maxMessageBytes = 8 * 1024;  // the platform's cap, scaled down
    EXPECT_EQ(discoverNow(nullptr, 500), ErrorCode::AdapterUnavailable);
    transport.maxMessageBytes = 4 * 1024 * 1024;
    run(1500);
    EXPECT_EQ(client.state(), HaState::Connected);
}

TEST_F(ClientTest, ADiscoveryThatGetsNoAnswerTimesOut) {
    connect();
    server.answerCommands = false;
    server.answerPings = true;
    EXPECT_EQ(discoverNow(nullptr, HomeAssistantClient::kDiscoveryTimeoutMs + 200), ErrorCode::AdapterTimeout);
}

TEST_F(ClientTest, TheDiscoveryMailboxWorksFromAnyTask) {
    HaDiscovery out;
    ErrorCode error = ErrorCode::Ok;
    EXPECT_EQ(client.requestDiscovery(), ErrorCode::AdapterUnavailable);
    connect();
    ASSERT_EQ(client.requestDiscovery(), ErrorCode::Ok);
    EXPECT_EQ(client.requestDiscovery(), ErrorCode::Busy);
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Running);
    run(2000);
    ASSERT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Done);
    EXPECT_FALSE(out.candidates.empty());
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Idle);

    // Cancel: before it starts, and while it runs.
    ASSERT_EQ(client.requestDiscovery(), ErrorCode::Ok);
    client.cancelDiscovery();
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Idle);
    run(2000);
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Idle);

    server.fillerEntities = 600;
    ASSERT_EQ(client.requestDiscovery(), ErrorCode::Ok);
    tick();
    tick();
    client.cancelDiscovery();
    run(5000);
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Idle);
    EXPECT_FALSE(client.hasWork());

    // A failure is reported once.
    server.answerCommands = false;
    ASSERT_EQ(client.requestDiscovery(), ErrorCode::Ok);
    run(HomeAssistantClient::kDiscoveryTimeoutMs + 500);
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Failed);
    EXPECT_EQ(error, ErrorCode::AdapterTimeout);
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Idle);
    EXPECT_STREQ(discoveryStatusName(DiscoveryStatus::Failed), "failed");
}

TEST_F(ClientTest, OnlyTheCurrentGenerationIsPublished) {
    connect();
    HaDiscovery out;
    ErrorCode error = ErrorCode::Ok;
    const uint32_t first = client.discoveryGeneration();
    ASSERT_EQ(client.requestDiscovery(), ErrorCode::Ok);
    EXPECT_NE(client.discoveryGeneration(), first);
    // Run up to the last command: its answer is queued, not yet delivered.
    const auto servicesSent = [&] { return server.received().back().find("subscribe_entities") != std::string::npos; };
    for (int i = 0; i < 400 && !servicesSent(); i++) {
        now += 100;
        transport.poll(0);
        client.poll(now);
    }
    ASSERT_TRUE(servicesSent());
    client.cancelDiscovery();  // from another task, before that answer arrives
    run(1000);
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Idle);

    // The next one publishes, tagged with its generation.
    ASSERT_EQ(client.requestDiscovery(), ErrorCode::Ok);
    run(2000);
    ASSERT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Done);
    EXPECT_EQ(out.generation, client.discoveryGeneration());

    // New settings (maybe another HA): a finished result is thrown away and
    // the generation moves on, so a cache of `out` is stale.
    ASSERT_EQ(client.requestDiscovery(), ErrorCode::Ok);
    run(2000);
    client.configure("ws://other.local:8123/api/websocket", server.token);
    EXPECT_NE(out.generation, client.discoveryGeneration());
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Idle);
    // A running one is dropped too.
    run(300);
    ASSERT_EQ(client.requestDiscovery(), ErrorCode::Ok);
    tick();
    client.reconnect();
    run(2000);
    EXPECT_EQ(client.takeDiscovery(&out, &error), DiscoveryStatus::Idle);
}

TEST_F(ClientTest, EntityDetailsAreFetchedForOneEntity) {
    connect();
    const auto fetch = [&](const char *entityId, HaEntityDetails *out) {
        ErrorCode result = ErrorCode::Busy;
        EXPECT_EQ(client.entityDetails(entityId, [&](ErrorCode e, HaEntityDetails d) {
                      result = e;
                      *out = std::move(d);
                  }),
                  ErrorCode::Ok);
        for (int i = 0; i < 100 && result == ErrorCode::Busy; i++) tick();
        return result;
    };
    HaEntityDetails d;
    ASSERT_EQ(fetch("media_player.family_room_sonos", &d), ErrorCode::Ok);
    EXPECT_EQ(d.entityId, "media_player.family_room_sonos");
    EXPECT_EQ(d.uniqueId, "uid_media_player.family_room_sonos");
    EXPECT_EQ(d.platform, "sonos");
    EXPECT_EQ(d.name, "Family Room Sonos");
    EXPECT_NE(d.info.supportedFeatures, 0u);
    EXPECT_EQ(d.info.sourceList, std::vector<std::string>{"TV"});
    EXPECT_EQ(d.services.front(), '{');
    EXPECT_NE(d.services.find("\"sonos\""), std::string::npos);
    EXPECT_EQ(server.subscriptions(), 0u);
    // Its integration's service names, as HA's UI shows them; only its own.
    EXPECT_EQ(d.translations.front(), '{');
    EXPECT_NE(d.translations.find("\"component.sonos.services.set_sleep_timer.name\":\"Set timer\""),
              std::string::npos);
    EXPECT_EQ(d.translations.find("component.denonavr."), std::string::npos);
    EXPECT_NE(server.received().back().find(R"("integration":["sonos"])"), std::string::npos);

    // YAML-only: no registry entry, so no unique_id or platform.
    ASSERT_EQ(fetch(FakeHaServer::kYamlOnlyEntity, &d), ErrorCode::Ok);
    EXPECT_EQ(d.uniqueId, "");
    EXPECT_EQ(d.platform, "");
    EXPECT_EQ(d.name, "Patio Fountain");
    EXPECT_FALSE(d.services.empty());
    EXPECT_EQ(d.translations, "");  // no integration: nothing asked
    EXPECT_EQ(server.received().back().find("get_translations"), std::string::npos);

    // Translations are optional: unanswered, details still arrive, without them.
    server.answerTranslations = false;
    ErrorCode slow = ErrorCode::Busy;
    ASSERT_EQ(client.entityDetails("media_player.denon", [&](ErrorCode e, HaEntityDetails x) {
                  slow = e;
                  d = std::move(x);
              }),
              ErrorCode::Ok);
    run(HomeAssistantClient::kCommandTimeoutMs + 200);
    EXPECT_EQ(slow, ErrorCode::Ok);
    EXPECT_EQ(d.platform, "denonavr");
    EXPECT_EQ(d.translations, "");

    // So is a connection drop while they're asked: what was gathered stays.
    ErrorCode dropped = ErrorCode::Busy;
    ASSERT_EQ(client.entityDetails("media_player.denon", [&](ErrorCode e, HaEntityDetails x) {
                  dropped = e;
                  d = std::move(x);
              }),
              ErrorCode::Ok);
    run(500);
    ASSERT_NE(server.received().back().find("get_translations"), std::string::npos);
    EXPECT_EQ(dropped, ErrorCode::Busy);
    transport.dropConnection();
    tick();
    EXPECT_EQ(dropped, ErrorCode::Ok);
    EXPECT_EQ(d.platform, "denonavr");
    EXPECT_FALSE(d.services.empty());
    EXPECT_EQ(d.translations, "");
    run(1100);  // reconnected
    ASSERT_EQ(client.state(), HaState::Connected);
    server.answerTranslations = true;

    EXPECT_EQ(fetch("light.nowhere", &d), ErrorCode::EntityNotFound);
    EXPECT_EQ(client.entityDetails("sensor.temperature", nullptr), ErrorCode::InvalidId);
    EXPECT_EQ(client.entityDetails("not an id", nullptr), ErrorCode::InvalidId);
    server.answerCommands = false;
    ErrorCode late = ErrorCode::Busy;
    ASSERT_EQ(client.entityDetails("light.family_room", [&](ErrorCode e, HaEntityDetails) { late = e; }), ErrorCode::Ok);
    run(HomeAssistantClient::kDiscoveryTimeoutMs + 200);
    EXPECT_EQ(late, ErrorCode::AdapterTimeout);
}

TEST_F(ClientTest, TheEntityDetailsMailboxWorksFromAnyTask) {
    HaEntityDetails out;
    ErrorCode error = ErrorCode::Ok;
    EXPECT_EQ(client.requestEntityDetails("light.family_room"), ErrorCode::AdapterUnavailable);
    connect();
    EXPECT_EQ(client.requestEntityDetails("sensor.x"), ErrorCode::InvalidId);
    server.answerCommands = false;  // so it stays in flight
    ASSERT_EQ(client.requestEntityDetails("light.family_room"), ErrorCode::Ok);
    EXPECT_EQ(client.requestEntityDetails("light.family_room"), ErrorCode::Busy);
    EXPECT_EQ(client.takeEntityDetails(&out, &error), DiscoveryStatus::Running);
    run(500);
    EXPECT_EQ(client.takeEntityDetails(&out, &error), DiscoveryStatus::Running);  // in flight
    // A new pick replaces it at once.
    server.answerCommands = true;
    client.cancelEntityDetails();
    ASSERT_EQ(client.requestEntityDetails("media_player.denon"), ErrorCode::Ok);
    run(1000);
    ASSERT_EQ(client.takeEntityDetails(&out, &error), DiscoveryStatus::Done);
    EXPECT_EQ(out.entityId, "media_player.denon");
    EXPECT_EQ(out.platform, "denonavr");
    EXPECT_EQ(client.takeEntityDetails(&out, &error), DiscoveryStatus::Idle);
    EXPECT_EQ(server.subscriptions(), 0u);

    ASSERT_EQ(client.requestEntityDetails("light.nowhere"), ErrorCode::Ok);
    run(1000);
    EXPECT_EQ(client.takeEntityDetails(&out, &error), DiscoveryStatus::Failed);
    EXPECT_EQ(error, ErrorCode::EntityNotFound);

    // New settings drop a finished fetch.
    ASSERT_EQ(client.requestEntityDetails("light.family_room"), ErrorCode::Ok);
    run(1000);
    client.reconnect();
    EXPECT_EQ(client.takeEntityDetails(&out, &error), DiscoveryStatus::Idle);
}

TEST_F(ClientTest, FetchedDetailsProveAnEntityIsntStale) {
    connect();
    ASSERT_EQ(discoverNow(), ErrorCode::Ok);
    sim::FakeHaEntity added;
    added.info.entityId = "light.new_lamp";
    added.friendlyName = "New lamp";
    server.addEntity(added);
    EXPECT_TRUE(client.entityStale("light.new_lamp"));  // not in that discovery
    ErrorCode result = ErrorCode::Busy;
    ASSERT_EQ(client.entityDetails("light.new_lamp", [&](ErrorCode e, HaEntityDetails) { result = e; }), ErrorCode::Ok);
    run(1000);
    ASSERT_EQ(result, ErrorCode::Ok);
    EXPECT_FALSE(client.entityStale("light.new_lamp"));  // registered from details: usable at once
    server.removeEntity("light.new_lamp");
    ASSERT_EQ(discoverNow(), ErrorCode::Ok);
    EXPECT_TRUE(client.entityStale("light.new_lamp"));  // until a discovery says otherwise
}

TEST_F(ClientTest, AnEntityMissingFromDiscoveryIsStale) {
    connect();
    EXPECT_FALSE(client.entityStale("light.family_room"));  // nothing known yet
    server.removeEntity("light.family_room");
    // As real HA: a call to an entity it doesn't have still succeeds, so
    // calls never tell us; discovery does.
    ErrorCode result = ErrorCode::Internal;
    ASSERT_EQ(client.callService(turnOn(), [&](ErrorCode e) { result = e; }), ErrorCode::Ok);
    run(200);
    EXPECT_EQ(result, ErrorCode::Ok);
    EXPECT_FALSE(client.entityStale("light.family_room"));

    ASSERT_EQ(discoverNow(), ErrorCode::Ok);
    EXPECT_TRUE(client.entityStale("light.family_room"));
    EXPECT_FALSE(client.entityStale("media_player.denon"));
    EXPECT_TRUE(client.entityStale("light.never_existed"));

    // Back in HA: the next discovery clears it; new settings forget it.
    FakeHaEntity light;
    light.info.entityId = "light.family_room";
    server.addEntity(light);
    ASSERT_EQ(discoverNow(), ErrorCode::Ok);
    EXPECT_FALSE(client.entityStale("light.family_room"));
    server.removeEntity("light.family_room");
    ASSERT_EQ(discoverNow(), ErrorCode::Ok);
    ASSERT_TRUE(client.entityStale("light.family_room"));
    client.reconnect();
    tick();
    EXPECT_FALSE(client.entityStale("light.family_room"));
}

TEST_F(ClientTest, NewSettingsReconnectAtOnceNotAfterTheBackoff) {
    connect();
    const int before = server.connections();
    client.configure("wss://other.example/api/websocket", server.token);
    tick();  // close, and the close arrives
    tick();  // connect, auth
    tick();
    EXPECT_EQ(client.state(), HaState::Connected);
    EXPECT_EQ(server.connections(), before + 1);
    EXPECT_EQ(transport.lastUrl(), "wss://other.example/api/websocket");
}

TEST_F(ClientTest, ABigHomeFitsAndOverTheCapIsReportedNotSilent) {
    // More switches than kMaxCandidates.
    const size_t extra = HomeAssistantClient::kMaxCandidates + 10;
    for (size_t i = 0; i < extra; i++) {
        FakeHaEntity e;
        e.info.entityId = "switch.s" + std::to_string(10000 + i);
        server.addEntity(e);
    }
    connect();
    HaDiscovery discovery;
    ASSERT_EQ(discoverNow(&discovery), ErrorCode::Ok);
    EXPECT_EQ(discovery.candidates.size(), HomeAssistantClient::kMaxCandidates);
    EXPECT_EQ(discovery.candidates.size() + discovery.truncated, server.entities().size());
    // Left out of the list is not the same as gone from HA.
    for (const FakeHaEntity &e : server.entities()) {
        EXPECT_FALSE(client.entityStale(e.info.entityId)) << e.info.entityId;
    }
}

// The attributes the new domains' commands need, read from HA's states, and
// the device_class for the suggested kind.
TEST_F(ClientTest, TheNewDomainsAttributesAreRead) {
    connect();  // the fake's made-up entities, one per new domain
    HaDiscovery found;
    ASSERT_EQ(discoverNow(&found), ErrorCode::Ok);
    const auto candidate = [&](const char *id) {
        const auto it = std::find_if(found.candidates.begin(), found.candidates.end(),
                                     [&](const HaCandidate &c) { return c.entityId == id; });
        EXPECT_NE(it, found.candidates.end()) << id;
        return it != found.candidates.end() ? *it : HaCandidate{};
    };
    EXPECT_EQ(candidate("cover.office_blinds").domain, "cover");
    EXPECT_EQ(candidate("cover.office_blinds").deviceClass, "blind");
    EXPECT_EQ(candidate("cover.office_blinds").state, "open");
    EXPECT_EQ(candidate("scene.movie_night").domain, "scene");
    EXPECT_EQ(candidate("scene.movie_night").deviceClass, "");
    const HaCandidate h = candidate("humidifier.bedroom_humidifier");
    EXPECT_EQ(h.info.availableModes, (std::vector<std::string>{"normal", "eco", "sleep"}));
    EXPECT_EQ(h.info.minHumidity, 30);
    EXPECT_EQ(h.info.maxHumidity, 80);
    EXPECT_EQ(candidate("select.dryer_cycle").info.options,
              (std::vector<std::string>{"normal", "delicate", "towels"}));
}

// ---- one attribute, read fresh (a toggle's current state) ----------------

namespace {

struct Read {
    bool done = false;
    ErrorCode result = ErrorCode::Busy;
    std::optional<ha::ServiceValue> value;
    // The readAttribute callback that fills this in.
    HomeAssistantClient::ReadDone sink() {
        return [this](ErrorCode e, std::optional<ha::ServiceValue> v) {
            done = true;
            result = e;
            value = std::move(v);
        };
    }
};

}  // namespace

TEST_F(ClientTest, ReadAttributeReadsOneEntityAndUnsubscribes) {
    connect();
    FakeHaEntity denon;
    for (const FakeHaEntity &e : server.entities()) {
        if (e.info.entityId == "media_player.denon") denon = e;
    }
    ASSERT_FALSE(denon.info.entityId.empty());
    denon.state = "on";
    denon.attributesJson =
        R"({"is_volume_muted":true,"volume_level":0.4,"sound_mode_raw":7,"source":"TV","gone":null,"list":[1]})";
    server.addEntity(denon);

    const auto read = [&](const char *attribute) {
        Read r;
        EXPECT_EQ(client.readAttribute("media_player.denon", attribute, r.sink()), ErrorCode::Ok);
        run(200);
        EXPECT_TRUE(r.done);
        return r;
    };
    Read r = read("is_volume_muted");
    EXPECT_EQ(r.result, ErrorCode::Ok);
    ASSERT_TRUE(r.value);
    EXPECT_EQ(r.value->kind, ha::ServiceValue::Kind::Bool);
    EXPECT_TRUE(r.value->boolean);
    // Only that entity was asked for, and the subscription was ended.
    EXPECT_NE(server.received().back().find("unsubscribe_events"), std::string::npos);
    bool askedForOne = false;
    for (const std::string &m : server.received()) {
        askedForOne |= m.find(R"("type":"subscribe_entities","entity_ids":["media_player.denon"])") != std::string::npos;
    }
    EXPECT_TRUE(askedForOne);
    EXPECT_EQ(server.subscriptions(), 0u);

    r = read("volume_level");
    ASSERT_TRUE(r.value);
    EXPECT_EQ(r.value->kind, ha::ServiceValue::Kind::Number);
    EXPECT_NEAR(r.value->number, 0.4, 1e-6);
    r = read("sound_mode_raw");
    ASSERT_TRUE(r.value);
    EXPECT_EQ(r.value->kind, ha::ServiceValue::Kind::Int);
    EXPECT_EQ(r.value->integer, 7);
    r = read("source");
    ASSERT_TRUE(r.value);
    EXPECT_EQ(r.value->kind, ha::ServiceValue::Kind::String);
    EXPECT_EQ(r.value->string, "TV");
    // Left out, null, or not a plain value: Ok, unknown.
    for (const char *unknown : {"missing", "gone", "list"}) {
        r = read(unknown);
        EXPECT_EQ(r.result, ErrorCode::Ok) << unknown;
        EXPECT_FALSE(r.value) << unknown;
    }
    EXPECT_EQ(server.subscriptions(), 0u);
}

TEST_F(ClientTest, ReadAttributeFailures) {
    Read r;
    const HomeAssistantClient::ReadDone done = r.sink();
    EXPECT_EQ(client.readAttribute("media_player.denon", "is_volume_muted", done), ErrorCode::AdapterUnavailable);
    connect();
    EXPECT_EQ(client.readAttribute("sensor.x", "a", done), ErrorCode::InvalidId);
    EXPECT_FALSE(r.done);

    // HA has no state for it.
    server.removeEntity("media_player.denon");
    ASSERT_EQ(client.readAttribute("media_player.denon", "is_volume_muted", done), ErrorCode::Ok);
    run(200);
    EXPECT_TRUE(r.done);
    EXPECT_EQ(r.result, ErrorCode::EntityNotFound);
    EXPECT_EQ(server.subscriptions(), 0u);

    // No answer: times out after kStateReadTimeoutMs, and ends the subscription.
    r = Read();
    server.answerCommands = false;
    ASSERT_EQ(client.readAttribute("media_player.family_room_sonos", "is_volume_muted", done), ErrorCode::Ok);
    run(HomeAssistantClient::kStateReadTimeoutMs - 200);
    EXPECT_FALSE(r.done);
    run(300);
    EXPECT_TRUE(r.done);
    EXPECT_EQ(r.result, ErrorCode::AdapterTimeout);
    EXPECT_FALSE(r.value);
    EXPECT_NE(server.received().back().find("unsubscribe_events"), std::string::npos);

    // A drop fails a read in flight.
    r = Read();
    ASSERT_EQ(client.readAttribute("media_player.family_room_sonos", "is_volume_muted", done), ErrorCode::Ok);
    tick();
    transport.dropConnection();
    tick();
    EXPECT_TRUE(r.done);
    EXPECT_EQ(r.result, ErrorCode::AdapterUnavailable);
}

TEST_F(ClientTest, AReadRunsAlongsideEntityDetailsWithoutBeingDropped) {
    // Details finishing (forgetPending) must not end a read in flight.
    connect();
    Read r;
    ErrorCode details = ErrorCode::Busy;
    ASSERT_EQ(client.entityDetails("media_player.denon", [&](ErrorCode e, HaEntityDetails) { details = e; }),
              ErrorCode::Ok);
    ASSERT_EQ(client.readAttribute("media_player.denon", "is_volume_muted", r.sink()), ErrorCode::Ok);
    run(1000);
    EXPECT_EQ(details, ErrorCode::Ok);
    EXPECT_TRUE(r.done);
    EXPECT_EQ(r.result, ErrorCode::Ok);
    ASSERT_TRUE(r.value);
    EXPECT_FALSE(r.value->boolean);
}
