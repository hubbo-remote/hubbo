#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/config/service.h"
#include "hubbo/domain/config.h"
#include "hubbo/integrations/mqtt_transport.h"

// Hubbo as a Home Assistant device over MQTT discovery
// (docs/design/mqtt-discovery.md): an Activity select ("PowerOff" and the
// activity labels), a Power switch, a Restart button and diagnostic sensors,
// with availability through a retained last will.
//
// Threading: poll() and every transport callback run on the mqtt task.
// configure(), setActivities() and setHubName() may be called from any task
// (a mutex guards what they hand over; poll() picks it up). Hooks are called
// on the mqtt task; `transition` and `restart` only post work and never
// block. Nothing here runs on the RF/engine task.
namespace hubbo::integrations {

constexpr const char *kPowerOffLabel = "PowerOff";  // the select's "off" option
constexpr size_t kMaxDiscoveryBytes = 3584;          // the discovery message, pinned by a test

struct MqttSettings {
    bool enabled = false;  // the mqtt_main integration's enabled flag
    std::string host;      // "" = not configured
    uint16_t port = 1883;
    std::string username;
    std::string password;  // from the SecretStore (mqtt_password); never logged
    bool operator==(const MqttSettings &o) const {
        return enabled == o.enabled && host == o.host && port == o.port && username == o.username &&
               password == o.password;
    }
};

struct MqttActivity {
    std::string id;
    std::string name;
    bool operator==(const MqttActivity &o) const { return id == o.id && name == o.name; }
};

// The config's activities, in order.
std::vector<MqttActivity> mqttActivities(const ConfigSpec &config);

// The select's labels, in config order: activity names, a repeat getting
// " (2)", " (3)"... and a name equal to kPowerOffLabel too ("PowerOff (2)").
// Same size and order as `activities`.
std::vector<std::string> activityLabels(const std::vector<MqttActivity> &activities);

// "hubbo_" and the 12 lowercase hex digits of the Ethernet MAC: the
// discovery node id and the client id. A rename doesn't change it.
std::string mqttNodeId(const uint8_t mac[6]);
// "aa:bb:cc:dd:ee:ff": the diag sensor's and the device card's MAC.
std::string mqttMacText(const uint8_t mac[6]);

// What the device card and the diagnostic sensors show.
struct MqttDeviceInfo {
    std::string name;      // the hub name
    std::string version;   // firmware version
    std::string board;
    std::string ip;        // "" until known
    std::string mac;       // "aa:bb:cc:dd:ee:ff"
    std::string hostname;  // the .local hostname shown (HubIdentity::shownHostname())
    uint32_t uptimeS = 0;
};

// The retained device-based discovery message for
// homeassistant/device/<nodeId>/config. At most kMaxDiscoveryBytes, even
// with limits::kMaxActivities labels of the longest names (a test pins it).
std::string mqttDiscoveryPayload(const std::string &nodeId, const MqttDeviceInfo &device,
                                 const std::vector<std::string> &labels);

class MqttBridge : private MqttTransport::Listener {
public:
    struct Hooks {
        // A counter that changes whenever the active activity may have
        // (firmware: main.cpp's activityChanges). Read every poll().
        std::function<uint32_t()> activityChanges;
        // The active activity id, "" for none (Engine::activeActivityIdShared()).
        std::function<std::string()> activeActivityId;
        // Version, board, IP, MAC, hostname and uptime; `name` is ignored
        // (setHubName() is the source). Read about once a second.
        std::function<MqttDeviceInfo()> liveInfo;
        // Start an activity by id, or "" to turn everything off
        // (api::runTransition, posted to httpd on the board). Never blocks.
        std::function<void(const std::string &activityId)> transition;
        // Restart the hub as POST /admin/restart does (refused during a
        // firmware update). Never blocks.
        std::function<void()> restart;
    };  // all five are required (none is checked)

    MqttBridge(MqttTransport &transport, std::string nodeId, Hooks hooks);
    ~MqttBridge() override;

    // Any task. New settings take effect at the next poll(): an open
    // connection is closed (offline published first) and opened again.
    // The same settings again do nothing (every save re-applies them).
    void configure(MqttSettings settings);
    // Any task: the config's activities (a config listener). Republishes the
    // discovery and the activity state if the labels changed.
    void setActivities(std::vector<MqttActivity> activities);
    // Any task: the hub name (the HubSettings rename hook). Republishes the
    // discovery.
    void setHubName(std::string name);

    // Any task: an IntegrationState name: "disabled", "not_configured",
    // "connecting", "connected", "auth_failed" or "unreachable".
    const char *state() const;
    // Any task: a short human detail for the status line, only for a
    // problem (unreachable, auth_failed); "" otherwise. No host and no
    // secrets, well under the API's 120-character cap.
    std::string detail() const;

    const std::string &nodeId() const { return nodeId_; }
    // Topics: "hubbo/<node>/<leaf>" and the discovery topic.
    std::string topic(std::string_view leaf) const { return base_ + std::string(leaf); }
    std::string discoveryTopic() const;

    // The mqtt task, every loop (at most every 50 ms): settings, activity
    // changes, the delayed state publish, diag.
    void poll(uint64_t nowMs);

    static constexpr uint32_t kStatesDelayMs = 1000;  // discovery and online first, then states
    static constexpr uint32_t kLiveInfoMs = 1000;
    static constexpr uint32_t kConnectRetryMs = 5000;  // after connect() refused to start

private:
    enum class Link : uint8_t { Connecting, Connected, AuthFailed, Unreachable };

    void onConnected() override;
    void onDropped(MqttDropReason reason) override;
    void onMessage(std::string_view topic, std::string_view payload, bool retained) override;

    void applySettings();
    void startConnect();  // a refused start counts as unreachable; retried after kConnectRetryMs
    void announce();  // discovery, online; states after kStatesDelayMs
    void publishDiscovery();
    void publishStates();
    void publishDiag();
    void command(std::string_view leaf, std::string_view payload);
    // The activity to start for a command ("" = off); none: nothing to do.
    std::optional<std::string> commandTarget(std::string_view leaf, std::string_view payload);
    void refreshLive();

    MqttTransport &transport_;
    const std::string nodeId_;
    const std::string base_;  // "hubbo/<node>/"
    Hooks hooks_;

    mutable std::mutex mutex_;  // everything below up to `poll-only`
    MqttSettings settings_;
    bool settingsChanged_ = false;
    std::vector<MqttActivity> activities_;
    std::vector<std::string> labels_;
    std::string hubName_;
    bool discoveryChanged_ = false;
    Link link_ = Link::Connecting;

    // poll-only (the mqtt task)
    uint64_t nowMs_ = 0;
    std::optional<uint64_t> statesDueMs_;  // the delayed states and diag after announce()
    std::optional<uint64_t> retryConnectMs_;  // connect() refused: try again then
    uint32_t seenChanges_ = 0;
    bool seenAny_ = false;
    std::string activeId_;
    std::string lastActivityId_;  // the most recent non-empty active id (Power ON)
    uint64_t liveDueMs_ = 0;
    MqttDeviceInfo live_;
};

// The config listener that keeps the bridge's activity list current (in a
// ConfigFanOut after the Bluetooth hosts' reconciler).
class MqttActivitySync : public config::ConfigListener {
public:
    explicit MqttActivitySync(MqttBridge &bridge) : bridge_(bridge) {}
    void configSaved(const ConfigSpec &config) override { bridge_.setActivities(mqttActivities(config)); }

private:
    MqttBridge &bridge_;
};

}  // namespace hubbo::integrations
