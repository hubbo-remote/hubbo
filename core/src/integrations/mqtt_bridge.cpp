#include "hubbo/integrations/mqtt_bridge.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace hubbo::integrations {

namespace {

constexpr const char *kHaStatusTopic = "homeassistant/status";  // HA's birth ("online") and will

std::string serialize(const JsonDocument &doc) {
    std::string out;
    out.reserve(measureJson(doc));
    serializeJson(doc, out);
    return out;
}

}  // namespace

std::vector<MqttActivity> mqttActivities(const ConfigSpec &config) {
    std::vector<MqttActivity> out;
    out.reserve(config.activities.size());
    for (const ActivitySpec &a : config.activities) out.push_back({a.id, a.name});
    return out;
}

std::vector<std::string> activityLabels(const std::vector<MqttActivity> &activities) {
    std::vector<std::string> labels;
    labels.reserve(activities.size());
    const auto taken = [&](const std::string &label) {
        if (label == kPowerOffLabel) return true;
        for (const std::string &l : labels) {
            if (l == label) return true;
        }
        return false;
    };
    for (const MqttActivity &a : activities) {
        std::string label = a.name;
        for (int n = 2; taken(label); n++) label = a.name + " (" + std::to_string(n) + ")";
        labels.push_back(std::move(label));
    }
    return labels;
}

std::string mqttNodeId(const uint8_t mac[6]) {
    char id[20];
    std::snprintf(id, sizeof id, "hubbo_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return id;
}

std::string mqttMacText(const uint8_t mac[6]) {
    char text[18];
    std::snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

// Device-based discovery, with the abbreviations in HA's table
// (homeassistant/components/mqtt/abbreviations.py); full names elsewhere.
std::string mqttDiscoveryPayload(const std::string &nodeId, const MqttDeviceInfo &device,
                                 const std::vector<std::string> &labels) {
    const std::string base = "hubbo/" + nodeId + "/";
    JsonDocument doc;
    JsonObject dev = doc["dev"].to<JsonObject>();
    dev["ids"].add(nodeId);
    dev["name"] = device.name;
    dev["mf"] = "Hubbo";
    dev["mdl"] = device.board;
    dev["sw"] = device.version;
    if (!device.mac.empty()) {
        JsonArray mac = dev["cns"].add<JsonArray>();
        mac.add("mac");
        mac.add(device.mac);
    }
    if (!device.hostname.empty()) dev["cu"] = "http://" + device.hostname + ".local/";
    JsonObject origin = doc["o"].to<JsonObject>();
    origin["name"] = "Hubbo";
    origin["sw"] = device.version;
    doc["avty_t"] = base + "availability";

    JsonObject cmps = doc["cmps"].to<JsonObject>();
    const auto component = [&](const char *key, const char *platform, const char *name) {
        JsonObject c = cmps[key].to<JsonObject>();
        c["p"] = platform;
        c["name"] = name;
        c["uniq_id"] = nodeId + "_" + key;
        return c;
    };
    JsonObject activity = component("activity", "select", "Activity");
    activity["stat_t"] = base + "activity";
    activity["cmd_t"] = base + "cmd/activity";
    JsonArray options = activity["options"].to<JsonArray>();
    options.add(kPowerOffLabel);
    for (const std::string &label : labels) options.add(label);
    activity["ic"] = "mdi:remote-tv";

    JsonObject power = component("power", "switch", "Power");
    power["stat_t"] = base + "power";
    power["cmd_t"] = base + "cmd/power";

    JsonObject restart = component("restart", "button", "Restart");
    restart["cmd_t"] = base + "cmd/restart";
    restart["dev_cla"] = "restart";
    restart["ent_cat"] = "config";

    const std::string diag = base + "diag";
    const auto sensor = [&](const char *key, const char *name, const char *field) {
        JsonObject s = component(key, "sensor", name);
        s["stat_t"] = diag;
        s["val_tpl"] = std::string("{{ value_json.") + field + " }}";
        s["ent_cat"] = "diagnostic";
        return s;
    };
    sensor("version", "Firmware version", "version");
    sensor("board", "Board", "board");
    sensor("ip", "IP address", "ip");
    sensor("mac", "MAC address", "mac");
    sensor("hostname", "Hostname", "hostname");
    // The hub has no wall clock: HA works the boot time out from the uptime.
    JsonObject booted = component("booted", "sensor", "Booted");
    booted["stat_t"] = diag;
    booted["val_tpl"] = "{{ (now() - timedelta(seconds=value_json.uptime_s)).replace(microsecond=0).isoformat() }}";
    booted["dev_cla"] = "timestamp";
    booted["ent_cat"] = "diagnostic";
    return serialize(doc);
}

MqttBridge::MqttBridge(MqttTransport &transport, std::string nodeId, Hooks hooks)
    : transport_(transport), nodeId_(std::move(nodeId)), base_("hubbo/" + nodeId_ + "/"), hooks_(std::move(hooks)) {
    transport_.setListener(this);
}

MqttBridge::~MqttBridge() { transport_.setListener(nullptr); }

std::string MqttBridge::discoveryTopic() const { return "homeassistant/device/" + nodeId_ + "/config"; }

void MqttBridge::configure(MqttSettings settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (settings == settings_) return;
    settings_ = std::move(settings);
    settingsChanged_ = true;
}

void MqttBridge::setActivities(std::vector<MqttActivity> activities) {
    std::vector<std::string> labels = activityLabels(activities);
    std::lock_guard<std::mutex> lock(mutex_);
    if (activities == activities_) return;
    activities_ = std::move(activities);
    labels_ = std::move(labels);
    discoveryChanged_ = true;
}

void MqttBridge::setHubName(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name == hubName_) return;
    hubName_ = std::move(name);
    discoveryChanged_ = true;
}

const char *MqttBridge::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!settings_.enabled) return "disabled";
    if (settings_.host.empty()) return "not_configured";
    switch (link_) {
        case Link::Connected: return "connected";
        case Link::AuthFailed: return "auth_failed";
        case Link::Unreachable: return "unreachable";
        case Link::Connecting: break;
    }
    return "connecting";
}

std::string MqttBridge::detail() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!settings_.enabled || settings_.host.empty()) return "";
    // Problems only (connected or connecting would just repeat the state, as
    // HA's client leaves it empty). Short and without the host (up to 253
    // characters): the card shows the broker, and the API caps detail at 120.
    switch (link_) {
        case Link::AuthFailed: return "The broker refused the username or password";
        case Link::Unreachable: return "Can't reach the broker";
        case Link::Connected:
        case Link::Connecting: break;
    }
    return "";
}

void MqttBridge::applySettings() {
    bool wanted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settingsChanged_ = false;
        link_ = Link::Connecting;
        wanted = settings_.enabled && !settings_.host.empty();
    }
    // Close any open connection (a clean disconnect sends no will, so say
    // offline first); both are no-ops when idle.
    transport_.publish(topic("availability"), "offline", true);
    transport_.disconnect();
    statesDueMs_.reset();
    if (wanted) {
        startConnect();
    } else {
        retryConnectMs_.reset();
    }
}

void MqttBridge::startConnect() {
    MqttConnectOptions options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        options.host = settings_.host;
        options.port = settings_.port;
        options.username = settings_.username;
        options.password = settings_.password;
    }
    options.clientId = nodeId_;
    options.will = MqttWill{topic("availability"), "offline", true};
    const bool started = transport_.connect(options);
    retryConnectMs_ = started ? std::nullopt : std::optional<uint64_t>(nowMs_ + kConnectRetryMs);
    if (started) return;
    std::lock_guard<std::mutex> lock(mutex_);
    link_ = Link::Unreachable;
}

// Version, IP, hostname... (allocates on the board: only while connected).
void MqttBridge::refreshLive() {
    liveDueMs_ = nowMs_ + kLiveInfoMs;
    live_ = hooks_.liveInfo();
}

void MqttBridge::poll(uint64_t nowMs) {
    nowMs_ = nowMs;
    bool settingsChanged = false;
    bool discoveryChanged = false;
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settingsChanged = settingsChanged_;
        discoveryChanged = discoveryChanged_;
        discoveryChanged_ = false;
        connected = link_ == Link::Connected;
    }
    if (settingsChanged) {
        applySettings();
        return;  // the rest follows the connection
    }
    if (retryConnectMs_ && nowMs_ >= *retryConnectMs_) startConnect();

    // The active activity: re-read whenever the counter moves.
    const uint32_t changes = hooks_.activityChanges();
    bool activityChanged = false;
    if (!seenAny_ || changes != seenChanges_) {
        seenAny_ = true;
        seenChanges_ = changes;
        std::string id = hooks_.activeActivityId();
        if (!id.empty()) lastActivityId_ = id;
        activityChanged = id != activeId_;
        activeId_ = std::move(id);
    }

    if (!connected) return;
    bool liveChanged = false;
    if (nowMs_ >= liveDueMs_) {
        const MqttDeviceInfo before = live_;
        refreshLive();
        liveChanged = live_.ip != before.ip || live_.hostname != before.hostname || live_.version != before.version;
    }
    if (discoveryChanged || liveChanged) publishDiscovery();  // the name, options or the card's link
    if (statesDueMs_ && nowMs_ >= *statesDueMs_) {
        statesDueMs_.reset();
        publishStates();
        publishDiag();
    } else if (!statesDueMs_) {
        if (activityChanged || discoveryChanged) publishStates();  // a renamed activity changes its label too
        if (liveChanged) publishDiag();
    }
}

void MqttBridge::onConnected() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        link_ = Link::Connected;
    }
    transport_.subscribe(topic("cmd/+"));
    transport_.subscribe(kHaStatusTopic);
    refreshLive();  // the announce carries it
    announce();
}

void MqttBridge::onDropped(MqttDropReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    link_ = reason == MqttDropReason::AuthFailed ? Link::AuthFailed : Link::Unreachable;
}

void MqttBridge::announce() {
    publishDiscovery();
    transport_.publish(topic("availability"), "online", true);
    statesDueMs_ = nowMs_ + kStatesDelayMs;  // HA subscribes to them once it has the discovery
}

void MqttBridge::publishDiscovery() {
    MqttDeviceInfo device = live_;
    std::vector<std::string> labels;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        device.name = hubName_;
        labels = labels_;
    }
    transport_.publish(discoveryTopic(), mqttDiscoveryPayload(nodeId_, device, labels), true);
}

void MqttBridge::publishStates() {
    std::string label = kPowerOffLabel;  // none (or one the table doesn't have yet)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int i = indexOfId(activities_, activeId_);
        if (i >= 0) label = labels_[static_cast<size_t>(i)];
    }
    transport_.publish(topic("activity"), label, true);
    transport_.publish(topic("power"), activeId_.empty() ? "OFF" : "ON", true);
}

void MqttBridge::publishDiag() {
    JsonDocument doc;
    doc["version"] = live_.version;
    doc["board"] = live_.board;
    doc["ip"] = live_.ip;
    doc["mac"] = live_.mac;
    doc["hostname"] = live_.hostname;
    doc["uptime_s"] = live_.uptimeS;
    transport_.publish(topic("diag"), serialize(doc), false);
}

void MqttBridge::onMessage(std::string_view topicName, std::string_view payload, bool retained) {
    if (retained) return;  // a stored command (or birth) isn't a request now
    if (topicName == kHaStatusTopic) {
        if (payload == "online") announce();  // HA restarted: it forgot the device
        return;
    }
    constexpr std::string_view kCmd = "cmd/";
    if (topicName.substr(0, base_.size()) != base_ || topicName.substr(base_.size(), kCmd.size()) != kCmd) return;
    command(topicName.substr(base_.size() + kCmd.size()), payload);
}

void MqttBridge::command(std::string_view leaf, std::string_view payload) {
    if (leaf == "restart") {
        if (payload == "PRESS") hooks_.restart();
        return;
    }
    if (std::optional<std::string> target = commandTarget(leaf, payload)) {
        hooks_.transition(*target);
    } else {
        publishStates();  // unknown, or nothing to do: HA shows what's true again
    }
}

std::optional<std::string> MqttBridge::commandTarget(std::string_view leaf, std::string_view payload) {
    if (leaf == "activity") {
        if (payload == kPowerOffLabel) return std::string();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find(labels_.begin(), labels_.end(), payload);
        if (it != labels_.end()) return activities_[static_cast<size_t>(it - labels_.begin())].id;
    } else if (leaf == "power") {
        if (payload == "OFF" && !activeId_.empty()) return std::string();
        if (payload == "ON" && activeId_.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!lastActivityId_.empty() && indexOfId(activities_, lastActivityId_) >= 0) return lastActivityId_;
        }
    }
    return std::nullopt;
}

}  // namespace hubbo::integrations
