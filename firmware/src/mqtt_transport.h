#pragma once

// MqttTransport (core/include/hubbo/integrations/mqtt_transport.h) on
// ESP-IDF's esp-mqtt: plain MQTT 3.1.1 over TCP, QoS 0, clean sessions.
//
// esp-mqtt runs its own task (unpinned, internal-RAM stack) and reconnects
// by itself. Its event handler only records the link in atomics and queues
// inbound messages; poll(), on the mqtt task, delivers both, so every
// callback runs there. disconnect() lets the outbox drain (at most
// kFlushMs), then stops the client, which sends DISCONNECT (no last will).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/integrations/mqtt_transport.h"

namespace hubbo::fw {

class EspMqttTransport : public hubbo::integrations::MqttTransport {
public:
    static constexpr unsigned kTaskPriority = 4;     // under httpd (5); can't preempt the RF loop (16)
    static constexpr int kTaskStackBytes = 4096;     // TCP only, no TLS (esp-mqtt's default: 6144)
    static constexpr int kInBufferBytes = 512;       // commands and HA's birth are a few bytes
    static constexpr int kOutBufferBytes = 5120;     // one discovery message (escaped names: ~4.4 KB) and its header
    static constexpr uint64_t kOutboxBytes = 8192;   // a discovery and a few states (PSRAM)
    static constexpr int kKeepaliveS = 15;
    static constexpr int kReconnectMs = 5000;
    static constexpr size_t kMaxInboundBytes = 256;  // payload
    static constexpr size_t kMaxTopicBytes = 128;
    static constexpr uint32_t kFlushMs = 1000;       // disconnect(): the outbox's last messages

    EspMqttTransport();
    ~EspMqttTransport() override;
    EspMqttTransport(const EspMqttTransport &) = delete;
    EspMqttTransport &operator=(const EspMqttTransport &) = delete;

    void setListener(Listener *listener) override { listener_ = listener; }
    bool connect(const hubbo::integrations::MqttConnectOptions &options) override;
    bool publish(std::string_view topic, std::string_view payload, bool retain) override;
    bool subscribe(std::string_view filter) override;
    void disconnect() override;
    void poll(uint32_t timeoutMs) override;

private:
    struct Message;
    static void onEvent(void *self, const char *base, int32_t id, void *data);
    void deliverLink();
    void deliverMessage(Message *message);  // and frees it
    void wake();
    void drain();  // frees queued messages without delivering them

    Listener *listener_ = nullptr;
    void *client_ = nullptr;  // esp_mqtt_client_handle_t
    void *queue_ = nullptr;   // QueueHandle_t of Message * (nullptr: a wake-up)
    // The link as the esp-mqtt task saw it, never depending on queue space.
    // Counters, so a connect and a drop between two polls are both seen.
    std::atomic<uint32_t> connects_{0};
    std::atomic<uint32_t> drops_{0};
    std::atomic<bool> up_{false};
    std::atomic<bool> authRefused_{false};  // since the last connect: a CONNACK 4/5
    uint32_t seenConnects_ = 0;  // poll()'s: what was delivered
    uint32_t seenDrops_ = 0;
    bool connected_ = false;  // as delivered: between onConnected() and onDropped()
};

}  // namespace hubbo::fw
