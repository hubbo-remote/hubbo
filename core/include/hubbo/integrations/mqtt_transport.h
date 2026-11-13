#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The MQTT client port (docs/design/mqtt-discovery.md). One MQTT 3.1.1
// connection to the user's broker, provided by each platform:
//   firmware  esp-mqtt (EspMqttTransport), its events queued and delivered
//             from the mqtt task's poll();
//   hub-sim   an in-process fake broker (sim/src/fake_mqtt.h).
//
// Threading, as the WebSocket port: one task (firmware: the mqtt task) owns
// the loop
//
//     for (;;) {
//         transport.poll(50);  // may wait for I/O
//         bridge.poll(now);
//     }
//
// Every method is called on that task, and every listener callback is
// delivered on it, from inside poll(). The RF/engine task never calls in.
//
// The transport owns reconnecting: after connect() it keeps the connection
// up (retrying by itself) until disconnect(). Every (re)connection is a
// clean session: subscriptions don't survive it, so the listener subscribes
// again in each onConnected().
namespace hubbo::integrations {

// What the broker publishes for us when the connection drops without a
// disconnect() (the last will).
struct MqttWill {
    std::string topic;
    std::string payload;
    bool retain = true;
};

struct MqttConnectOptions {
    std::string host;  // an IP or a hostname
    uint16_t port = 1883;
    std::string clientId;
    std::string username;  // "" = none
    std::string password;  // "" = none. Never logged.
    MqttWill will;
};

enum class MqttDropReason : uint8_t {
    Unreachable,  // no TCP connection, or it dropped (the transport retries)
    AuthFailed,   // the broker refused the credentials (CONNACK 4 or 5; it retries)
};

class MqttTransport {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        // A connection (or a reconnection) is up: subscribe and publish.
        virtual void onConnected() = 0;
        // The connection is down, or an attempt failed. The transport keeps
        // retrying; not called after disconnect().
        virtual void onDropped(MqttDropReason reason) = 0;
        // One message on a subscribed topic. `retained`: the broker's stored
        // copy, sent on subscribe, not a live publish.
        virtual void onMessage(std::string_view topic, std::string_view payload, bool retained) = 0;
    };

    virtual ~MqttTransport() = default;

    virtual void setListener(Listener *listener) = 0;
    // Start connecting (and keep connected). False if it can't start (empty
    // host, already started): no callback then. A new connect() needs a
    // disconnect() first.
    virtual bool connect(const MqttConnectOptions &options) = 0;
    // Queue one message, QoS 0 (a retained one too). False if not connected or
    // the outbox is full (dropped).
    virtual bool publish(std::string_view topic, std::string_view payload, bool retain) = 0;
    // QoS 0. `filter` may use + and #. False if not connected.
    virtual bool subscribe(std::string_view filter) = 0;
    // Close cleanly (no last will) and stop retrying. Anything already
    // queued, published before it, is sent first. No callback.
    virtual void disconnect() = 0;
    // Every loop: I/O and callbacks. Waits at most `timeoutMs` (0: don't).
    virtual void poll(uint32_t timeoutMs) = 0;
};

}  // namespace hubbo::integrations
