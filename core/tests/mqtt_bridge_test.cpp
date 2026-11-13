// The MQTT bridge (docs/design/mqtt-discovery.md) over hub-sim's fake broker:
// discovery, labels, commands, the Power switch, resends, the last will and
// the integration states.
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fake_mqtt.h"
#include "hubbo/domain/limits.h"
#include "hubbo/integrations/mqtt_bridge.h"

using namespace hubbo;
using namespace hubbo::integrations;
using hubbo::sim::FakeMqttBroker;
using hubbo::sim::FakeMqttTransport;

namespace {

constexpr const char *kNode = "hubbo_0200000000aa";

MqttDeviceInfo deviceInfo() {
    MqttDeviceInfo info;
    info.version = "1.2.3";
    info.board = "test-board";
    info.ip = "10.0.0.5";
    info.mac = "02:00:00:00:00:aa";
    info.hostname = "hubbo-00aa";
    info.uptimeS = 42;
    return info;
}

// A hub around one bridge: the hooks read and record plain fields.
struct Harness {
    FakeMqttBroker broker;
    FakeMqttTransport transport{broker};
    uint32_t changes = 1;
    std::string active;
    MqttDeviceInfo info = deviceInfo();
    std::vector<std::string> transitions;
    int restarts = 0;
    uint64_t now = 0;
    MqttBridge bridge{transport, kNode, hooks()};

    Harness() {
        bridge.setHubName("Den");
        bridge.setActivities({{"act_tv", "Watch TV"}, {"act_music", "Listen to Music"}});
    }

    MqttBridge::Hooks hooks() {
        MqttBridge::Hooks h;
        h.activityChanges = [this] { return changes; };
        h.activeActivityId = [this] { return active; };
        h.liveInfo = [this] { return info; };
        h.transition = [this](const std::string &id) { transitions.push_back(id); };
        h.restart = [this] { restarts++; };
        return h;
    }

    // As the mqtt task's loop: the transport, then the bridge.
    void step(uint32_t ms = 50) {
        now += ms;
        transport.poll(0);
        bridge.poll(now);
    }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 50) step();
    }
    void enable(bool runStates = true) {
        MqttSettings s;
        s.enabled = true;
        s.host = "broker.test";
        s.username = "hubbo";
        s.password = "not-a-real-password";
        bridge.configure(s);
        step();  // applies the settings: connect
        step();  // the link comes up: subscribe, discovery, online
        if (runStates) run(MqttBridge::kStatesDelayMs + 100);
    }
    void setActive(const std::string &id) {
        active = id;
        changes++;
    }
    // A live message from "HA".
    void send(const std::string &leaf, const std::string &payload) {
        broker.publish(std::string("hubbo/") + kNode + "/" + leaf, payload, false);
        step();
    }
    std::string retained(const std::string &leaf) const {
        const auto it = broker.retained().find(std::string("hubbo/") + kNode + "/" + leaf);
        return it == broker.retained().end() ? std::string() : it->second;
    }
    std::string discovery() const {
        const auto it = broker.retained().find(std::string("homeassistant/device/") + kNode + "/config");
        return it == broker.retained().end() ? std::string() : it->second;
    }
    // How many times the client published `topic` (of the last 100).
    int publishedTo(const std::string &topic) const {
        int n = 0;
        for (const auto &m : broker.published()) n += m.topic == topic;
        return n;
    }
    int publishes(const std::string &leaf) const { return publishedTo(std::string("hubbo/") + kNode + "/" + leaf); }
    int discoveries() const { return publishedTo(std::string("homeassistant/device/") + kNode + "/config"); }
};

std::vector<std::string> options(const std::string &discovery) {
    JsonDocument doc;
    EXPECT_FALSE(deserializeJson(doc, discovery));
    std::vector<std::string> out;
    for (JsonVariantConst o : doc["cmps"]["activity"]["options"].as<JsonArrayConst>()) out.push_back(o.as<std::string>());
    return out;
}

}  // namespace

TEST(MqttBridge, NodeIdIsTheMac) {
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x53, 0x4D};
    EXPECT_EQ(mqttNodeId(mac), "hubbo_02000000534d");
    EXPECT_EQ(mqttMacText(mac), "02:00:00:00:53:4d");
}

TEST(MqttBridge, LabelsAreUniqueAndNeverPowerOff) {
    EXPECT_EQ(activityLabels({{"a", "TV"}, {"b", "TV"}, {"c", "Music"}, {"d", "TV"}}),
              (std::vector<std::string>{"TV", "TV (2)", "Music", "TV (3)"}));
    EXPECT_EQ(activityLabels({{"a", "PowerOff"}, {"b", "PowerOff (2)"}}),
              (std::vector<std::string>{"PowerOff (2)", "PowerOff (2) (2)"}));
    EXPECT_TRUE(activityLabels({}).empty());
}

// The whole message, pinned: HA's abbreviations where its table has one.
TEST(MqttBridge, DiscoveryGolden) {
    MqttDeviceInfo device = deviceInfo();
    device.name = "Den";
    const std::string expected =
        R"({"dev":{"ids":["hubbo_0200000000aa"],"name":"Den","mf":"Hubbo","mdl":"test-board","sw":"1.2.3",)"
        R"("cns":[["mac","02:00:00:00:00:aa"]],"cu":"http://hubbo-00aa.local/"},"o":{"name":"Hubbo","sw":"1.2.3"},)"
        R"("avty_t":"hubbo/hubbo_0200000000aa/availability","cmps":{)"
        R"("activity":{"p":"select","name":"Activity","uniq_id":"hubbo_0200000000aa_activity",)"
        R"("stat_t":"hubbo/hubbo_0200000000aa/activity","cmd_t":"hubbo/hubbo_0200000000aa/cmd/activity",)"
        R"("options":["PowerOff","Watch TV"],"ic":"mdi:remote-tv"},)"
        R"("power":{"p":"switch","name":"Power","uniq_id":"hubbo_0200000000aa_power",)"
        R"("stat_t":"hubbo/hubbo_0200000000aa/power","cmd_t":"hubbo/hubbo_0200000000aa/cmd/power"},)"
        R"("restart":{"p":"button","name":"Restart","uniq_id":"hubbo_0200000000aa_restart",)"
        R"("cmd_t":"hubbo/hubbo_0200000000aa/cmd/restart","dev_cla":"restart","ent_cat":"config"},)"
        R"("version":{"p":"sensor","name":"Firmware version","uniq_id":"hubbo_0200000000aa_version",)"
        R"("stat_t":"hubbo/hubbo_0200000000aa/diag","val_tpl":"{{ value_json.version }}","ent_cat":"diagnostic"},)"
        R"("board":{"p":"sensor","name":"Board","uniq_id":"hubbo_0200000000aa_board",)"
        R"("stat_t":"hubbo/hubbo_0200000000aa/diag","val_tpl":"{{ value_json.board }}","ent_cat":"diagnostic"},)"
        R"("ip":{"p":"sensor","name":"IP address","uniq_id":"hubbo_0200000000aa_ip",)"
        R"("stat_t":"hubbo/hubbo_0200000000aa/diag","val_tpl":"{{ value_json.ip }}","ent_cat":"diagnostic"},)"
        R"("mac":{"p":"sensor","name":"MAC address","uniq_id":"hubbo_0200000000aa_mac",)"
        R"("stat_t":"hubbo/hubbo_0200000000aa/diag","val_tpl":"{{ value_json.mac }}","ent_cat":"diagnostic"},)"
        R"("hostname":{"p":"sensor","name":"Hostname","uniq_id":"hubbo_0200000000aa_hostname",)"
        R"("stat_t":"hubbo/hubbo_0200000000aa/diag","val_tpl":"{{ value_json.hostname }}","ent_cat":"diagnostic"},)"
        R"("booted":{"p":"sensor","name":"Booted","uniq_id":"hubbo_0200000000aa_booted",)"
        R"("stat_t":"hubbo/hubbo_0200000000aa/diag",)"
        R"("val_tpl":"{{ (now() - timedelta(seconds=value_json.uptime_s)).replace(microsecond=0).isoformat() }}",)"
        R"("dev_cla":"timestamp","ent_cat":"diagnostic"}}})";
    EXPECT_EQ(mqttDiscoveryPayload(kNode, device, {"Watch TV"}), expected);
}

// Every activity at the longest name, all the same (so the longest dedupe
// suffixes), and the longest hub name (32 four-byte characters) and
// hostname. Names full of characters JSON escapes (" and \) can go past it.
TEST(MqttBridge, DiscoveryFitsItsBound) {
    std::vector<MqttActivity> activities;
    for (size_t i = 0; i < limits::kMaxActivities; i++) {
        activities.push_back({"act_" + std::to_string(i), std::string(limits::kMaxNameLength, 'x')});
    }
    MqttDeviceInfo device = deviceInfo();
    device.name.clear();
    for (int i = 0; i < 32; i++) device.name += "\xF0\x9F\x8E\xAC";
    device.version = std::string(32, '9');
    device.ip = "255.255.255.255";
    device.hostname = std::string(63, 'h');
    const std::string payload = mqttDiscoveryPayload(kNode, device, activityLabels(activities));
    EXPECT_LE(payload.size(), kMaxDiscoveryBytes) << payload.size();
}

TEST(MqttBridge, ConnectAnnouncesThenStates) {
    Harness h;
    h.enable(/*runStates=*/false);
    EXPECT_EQ(h.broker.clientId(), kNode);
    EXPECT_EQ(h.transport.lastOptions().username, "hubbo");
    EXPECT_EQ(h.transport.lastOptions().will.topic, std::string("hubbo/") + kNode + "/availability");
    EXPECT_EQ(h.broker.subscriptions(),
              (std::vector<std::string>{std::string("hubbo/") + kNode + "/cmd/+", "homeassistant/status"}));
    EXPECT_EQ(options(h.discovery()), (std::vector<std::string>{"PowerOff", "Watch TV", "Listen to Music"}));
    EXPECT_EQ(h.retained("availability"), "online");
    EXPECT_EQ(h.retained("activity"), "");  // after kStatesDelayMs
    h.run(MqttBridge::kStatesDelayMs + 100);
    EXPECT_EQ(h.retained("activity"), "PowerOff");
    EXPECT_EQ(h.retained("power"), "OFF");
    const auto &last = h.broker.published().back();
    EXPECT_EQ(last.topic, std::string("hubbo/") + kNode + "/diag");
    EXPECT_FALSE(last.retained);
    JsonDocument diag;
    ASSERT_FALSE(deserializeJson(diag, last.payload));
    EXPECT_EQ(diag["hostname"], "hubbo-00aa");
    EXPECT_EQ(diag["uptime_s"], 42);
    EXPECT_EQ(std::string(h.bridge.state()), "connected");
    EXPECT_EQ(h.bridge.detail(), "Connected to the broker");
}

TEST(MqttBridge, ActivityChangesPublishState) {
    Harness h;
    h.enable();
    h.setActive("act_music");
    h.step();
    EXPECT_EQ(h.retained("activity"), "Listen to Music");
    EXPECT_EQ(h.retained("power"), "ON");
    h.setActive("");
    h.step();
    EXPECT_EQ(h.retained("activity"), "PowerOff");
    EXPECT_EQ(h.retained("power"), "OFF");
}

TEST(MqttBridge, SelectCommands) {
    Harness h;
    h.bridge.setActivities({{"act_a", "PowerOff"}, {"act_b", "TV"}, {"act_c", "TV"}});
    h.enable();
    h.send("cmd/activity", "TV (2)");
    h.send("cmd/activity", "PowerOff (2)");
    h.send("cmd/activity", "PowerOff");
    EXPECT_EQ(h.transitions, (std::vector<std::string>{"act_c", "act_a", ""}));

    // An unknown label does nothing but put HA's select back.
    h.setActive("act_b");
    h.step();
    const int before = h.publishes("activity");
    h.send("cmd/activity", "Nope");
    EXPECT_EQ(h.transitions.size(), 3u);
    EXPECT_EQ(h.publishes("activity"), before + 1);
    EXPECT_EQ(h.retained("activity"), "TV");
}

// A retained command is old news: the broker's copy, sent on subscribe.
TEST(MqttBridge, RetainedCommandsAreIgnored) {
    Harness h;
    h.broker.publish(std::string("hubbo/") + kNode + "/cmd/activity", "Watch TV", true);
    h.broker.publish(std::string("hubbo/") + kNode + "/cmd/restart", "PRESS", true);
    h.broker.publish("homeassistant/status", "online", true);  // a stored birth isn't a restart
    h.enable();
    EXPECT_TRUE(h.transitions.empty());
    EXPECT_EQ(h.restarts, 0);
    EXPECT_EQ(h.discoveries(), 1);
}

TEST(MqttBridge, PowerSwitch) {
    Harness h;
    h.enable();
    // Nothing ever ran: ON has nothing to start, and HA's switch goes back off.
    const int before = h.publishes("power");
    h.send("cmd/power", "ON");
    EXPECT_TRUE(h.transitions.empty());
    EXPECT_EQ(h.publishes("power"), before + 1);
    EXPECT_EQ(h.retained("power"), "OFF");

    h.setActive("act_music");
    h.step();
    h.send("cmd/power", "OFF");
    EXPECT_EQ(h.transitions, (std::vector<std::string>{""}));
    h.setActive("");
    h.step();
    h.send("cmd/power", "ON");  // the last activity
    EXPECT_EQ(h.transitions, (std::vector<std::string>{"", "act_music"}));

    // The last activity was deleted: ON does nothing.
    h.bridge.setActivities({{"act_tv", "Watch TV"}});
    h.step();
    h.send("cmd/power", "ON");
    EXPECT_EQ(h.transitions.size(), 2u);
    h.send("cmd/power", "OFF");  // already off
    EXPECT_EQ(h.transitions.size(), 2u);
}

TEST(MqttBridge, RestartButton) {
    Harness h;
    h.enable();
    h.send("cmd/restart", "nope");
    EXPECT_EQ(h.restarts, 0);
    h.send("cmd/restart", "PRESS");
    EXPECT_EQ(h.restarts, 1);
}

// HA's birth message means it restarted and forgot the device.
TEST(MqttBridge, ResendsOnHaBirth) {
    Harness h;
    h.enable();
    const int before = h.discoveries();
    h.broker.publish("homeassistant/status", "offline", false);
    h.step();
    EXPECT_EQ(h.discoveries(), before);
    h.broker.publish("homeassistant/status", "online", false);
    h.step();
    EXPECT_EQ(h.discoveries(), before + 1);
}

TEST(MqttBridge, WillAndReconnect) {
    Harness h;
    h.enable();
    h.broker.reachable = false;
    h.step();
    EXPECT_EQ(h.retained("availability"), "offline");  // the will
    EXPECT_EQ(std::string(h.bridge.state()), "unreachable");
    EXPECT_EQ(h.bridge.detail(), "Can't reach the broker");

    h.setActive("act_tv");  // while down
    h.step();
    const int before = h.discoveries();
    h.broker.reachable = true;
    h.step();
    EXPECT_EQ(std::string(h.bridge.state()), "connected");
    EXPECT_EQ(h.discoveries(), before + 1);
    EXPECT_EQ(h.retained("availability"), "online");
    EXPECT_EQ(h.broker.subscriptions().size(), 2u);  // a clean session: subscribed again
    h.run(MqttBridge::kStatesDelayMs + 100);
    EXPECT_EQ(h.retained("activity"), "Watch TV");
}

// The API caps detail at 120 characters; the longest host doesn't reach it.
TEST(MqttBridge, DetailStaysShort) {
    Harness h;
    MqttSettings s;
    s.enabled = true;
    s.host = std::string(limits::kMaxHostLength, 'h');
    h.bridge.configure(s);
    h.step();
    EXPECT_EQ(h.bridge.detail(), "Connecting to the broker");
    h.broker.refuseAuth = true;
    h.step();
    EXPECT_EQ(std::string(h.bridge.state()), "auth_failed");
    EXPECT_LE(h.bridge.detail().size(), 120u);
    EXPECT_EQ(h.bridge.detail().find("hhhh"), std::string::npos);
}

// connect() refusing to start is unreachable, and it's tried again.
TEST(MqttBridge, ARefusedConnectIsRetried) {
    Harness h;
    h.transport.refuseConnect = true;
    h.enable(false);
    EXPECT_EQ(std::string(h.bridge.state()), "unreachable");
    EXPECT_EQ(h.bridge.detail(), "Can't reach the broker");
    EXPECT_EQ(h.transport.connectCalls, 1);
    h.run(MqttBridge::kConnectRetryMs);
    EXPECT_EQ(h.transport.connectCalls, 2);
    h.transport.refuseConnect = false;
    h.run(MqttBridge::kConnectRetryMs + 100);
    EXPECT_EQ(h.transport.connectCalls, 3);
    EXPECT_EQ(std::string(h.bridge.state()), "connected");
}

TEST(MqttBridge, AuthFailed) {
    Harness h;
    h.broker.refuseAuth = true;
    h.enable(false);
    EXPECT_EQ(std::string(h.bridge.state()), "auth_failed");
    EXPECT_EQ(h.bridge.detail().find("not-a-real-password"), std::string::npos);
    h.broker.refuseAuth = false;
    h.step();
    EXPECT_EQ(std::string(h.bridge.state()), "connected");
}

TEST(MqttBridge, DisabledAndNotConfigured) {
    Harness h;
    EXPECT_EQ(std::string(h.bridge.state()), "disabled");
    h.enable();
    MqttSettings off;  // disabled: offline first, then a clean disconnect
    off.host = "broker.test";
    h.bridge.configure(off);
    h.step();
    EXPECT_EQ(std::string(h.bridge.state()), "disabled");
    EXPECT_FALSE(h.broker.connected());
    EXPECT_EQ(h.retained("availability"), "offline");
    MqttSettings empty;
    empty.enabled = true;
    h.bridge.configure(empty);
    h.step();
    EXPECT_EQ(std::string(h.bridge.state()), "not_configured");
    EXPECT_EQ(h.bridge.detail(), "");
}

TEST(MqttBridge, NewSettingsReconnect) {
    Harness h;
    h.enable();
    MqttSettings s;
    s.enabled = true;
    s.host = "other.test";
    s.port = 8883;
    h.bridge.configure(s);
    h.step();
    h.step();
    EXPECT_EQ(h.broker.connections(), 2);
    EXPECT_EQ(h.transport.lastOptions().host, "other.test");
    EXPECT_EQ(h.transport.lastOptions().port, 8883);
    EXPECT_EQ(h.retained("availability"), "online");
}

// Every integration save re-applies the settings: the same ones keep the
// connection (HA would see Hubbo blip offline).
TEST(MqttBridge, SameSettingsKeepTheConnection) {
    Harness h;
    h.enable();
    MqttSettings s;
    s.enabled = true;
    s.host = "broker.test";
    s.username = "hubbo";
    s.password = "not-a-real-password";
    h.bridge.configure(s);
    h.run(200);
    EXPECT_EQ(h.broker.connections(), 1);
    EXPECT_EQ(h.publishes("availability"), 1);  // just the first online
    s.password = "another-fake-password";
    h.bridge.configure(s);
    h.run(200);
    EXPECT_EQ(h.broker.connections(), 2);
}

TEST(MqttBridge, RenameAndEditsRepublish) {
    Harness h;
    h.enable();
    h.setActive("act_tv");
    h.step();
    int before = h.discoveries();
    h.bridge.setHubName("Den");  // unchanged
    h.bridge.setActivities({{"act_tv", "Watch TV"}, {"act_music", "Listen to Music"}});
    h.step();
    EXPECT_EQ(h.discoveries(), before);

    h.bridge.setHubName("Living Room");
    h.step();
    EXPECT_EQ(h.discoveries(), before + 1);
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, h.discovery()));
    EXPECT_EQ(doc["dev"]["name"], "Living Room");

    // Renaming the active activity renames its option and its state.
    h.bridge.setActivities({{"act_tv", "Television"}, {"act_music", "Listen to Music"}});
    h.step();
    EXPECT_EQ(options(h.discovery()), (std::vector<std::string>{"PowerOff", "Television", "Listen to Music"}));
    EXPECT_EQ(h.retained("activity"), "Television");

    // A new IP (or hostname) changes the card's link and the diag.
    before = h.discoveries();
    h.info.hostname = "den-hub";
    h.run(MqttBridge::kLiveInfoMs + 100);
    EXPECT_EQ(h.discoveries(), before + 1);
    ASSERT_FALSE(deserializeJson(doc, h.discovery()));
    EXPECT_EQ(doc["dev"]["cu"], "http://den-hub.local/");
}
