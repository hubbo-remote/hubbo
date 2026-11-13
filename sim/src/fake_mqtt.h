#pragma once

// A fake MQTT broker and the in-process transport that talks to it, so the
// MQTT bridge, the contract and e2e run without a real broker. Host-only.
// One client at a time (the hub). Retained messages, + and # filters, the
// last will, and down and refuse-auth modes. Messages are delivered on the
// transport's next poll(), as the firmware's queue delivers them.

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "host_guard.h"
#include "hubbo/integrations/mqtt_transport.h"

namespace hubbo::sim {

class FakeMqttTransport;

class FakeMqttBroker {
public:
    static constexpr size_t kMaxPublished = 100;

    struct Message {
        std::string topic;
        std::string payload;
        bool retained = false;
    };

    bool reachable = true;    // false: the client's link drops (its will fires) and attempts fail
    bool refuseAuth = false;  // true: every connect is refused (bad credentials); an open link drops

    // Empty, reachable, accepting; the client (if any) is dropped.
    void reset();

    // From outside (a test, POST /sim/mqtt/publish): store it if retained
    // (an empty retained payload clears the topic) and deliver it to the
    // client's matching subscriptions.
    void publish(std::string_view topic, std::string_view payload, bool retain);

    const std::map<std::string, std::string> &retained() const { return retained_; }
    // What the client published, the last kMaxPublished, oldest first.
    const std::deque<Message> &published() const { return published_; }
    bool connected() const { return client_ != nullptr; }
    const std::string &clientId() const { return clientId_; }
    const std::vector<std::string> &subscriptions() const { return subscriptions_; }
    int connections() const { return connections_; }

    // MQTT topic filter matching: '+' one level, '#' the rest (last only).
    static bool matches(std::string_view filter, std::string_view topic);

private:
    friend class FakeMqttTransport;
    void attach(FakeMqttTransport *client, const integrations::MqttConnectOptions &options);
    void detach(FakeMqttTransport *client, bool clean);  // !clean: the will fires
    void fromClient(std::string_view topic, std::string_view payload, bool retain);
    void subscribe(std::string_view filter);
    void store(std::string_view topic, std::string_view payload, bool retain);

    std::map<std::string, std::string> retained_;
    std::deque<Message> published_;
    FakeMqttTransport *client_ = nullptr;
    std::string clientId_;
    integrations::MqttWill will_;
    std::vector<std::string> subscriptions_;
    int connections_ = 0;
};

class FakeMqttTransport : public integrations::MqttTransport {
public:
    explicit FakeMqttTransport(FakeMqttBroker &broker) : broker_(broker) {}
    ~FakeMqttTransport() override;

    void setListener(Listener *listener) override { listener_ = listener; }
    bool connect(const integrations::MqttConnectOptions &options) override;
    bool publish(std::string_view topic, std::string_view payload, bool retain) override;
    bool subscribe(std::string_view filter) override;
    void disconnect() override;
    void poll(uint32_t timeoutMs) override;

    const integrations::MqttConnectOptions &lastOptions() const { return options_; }
    bool refuseConnect = false;  // connect() fails to start (as esp-mqtt's init can)
    int connectCalls = 0;

private:
    friend class FakeMqttBroker;
    void deliver(std::string topic, std::string payload, bool retained);  // queued for poll()
    void dropped();  // the broker let go of us

    FakeMqttBroker &broker_;
    Listener *listener_ = nullptr;
    integrations::MqttConnectOptions options_;
    bool started_ = false;
    bool open_ = false;
    std::optional<integrations::MqttDropReason> reported_;  // the failure last reported
    std::deque<FakeMqttBroker::Message> inbox_;
};

}  // namespace hubbo::sim
