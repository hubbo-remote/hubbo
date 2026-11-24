#include "mqtt_transport.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mqtt_client.h>

#include "console.h"

namespace hubbo::fw {

namespace {
constexpr UBaseType_t kQueueDepth = 16;

esp_mqtt_client_handle_t handle(void *client) {
    return static_cast<esp_mqtt_client_handle_t>(client);
}
}  // namespace

struct EspMqttTransport::Message {
    bool retained = false;
    std::string topic;
    std::string payload;
};

EspMqttTransport::EspMqttTransport() : queue_(xQueueCreate(kQueueDepth, sizeof(Message *))) {
    configASSERT(queue_ != nullptr);  // a static object, built at boot
}

EspMqttTransport::~EspMqttTransport() {
    disconnect();
    vQueueDelete(static_cast<QueueHandle_t>(queue_));
}

bool EspMqttTransport::connect(const hubbo::integrations::MqttConnectOptions &options) {
    if (client_ != nullptr || options.host.empty()) {
        return false;
    }
    esp_mqtt_client_config_t config = {};
    config.broker.address.hostname = options.host.c_str();
    config.broker.address.port = options.port;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    config.credentials.client_id = options.clientId.c_str();
    config.credentials.username = options.username.empty() ? nullptr : options.username.c_str();
    config.credentials.authentication.password = options.password.empty() ? nullptr : options.password.c_str();
    config.session.last_will.topic = options.will.topic.c_str();
    config.session.last_will.msg = options.will.payload.c_str();
    config.session.last_will.msg_len = static_cast<int>(options.will.payload.size());
    config.session.last_will.qos = 0;
    config.session.last_will.retain = options.will.retain ? 1 : 0;
    config.session.keepalive = kKeepaliveS;
    config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    config.network.reconnect_timeout_ms = kReconnectMs;
    config.task.priority = kTaskPriority;
    config.task.stack_size = kTaskStackBytes;
    config.buffer.size = kInBufferBytes;
    config.buffer.out_size = kOutBufferBytes;
    config.outbox.limit = kOutboxBytes;
    // esp_mqtt_client_init copies every string.
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (client == nullptr) {
        con.println("mqtt: client init failed");
        return false;
    }
    // A fresh link: nothing from an earlier client is pending.
    up_.store(false);
    authRefused_.store(false);
    seenConnects_ = connects_.load();
    seenDrops_ = drops_.load();
    if (esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
                                       reinterpret_cast<esp_event_handler_t>(&EspMqttTransport::onEvent),
                                       this) != ESP_OK ||
        esp_mqtt_client_start(client) != ESP_OK) {
        con.println("mqtt: client start failed");
        esp_mqtt_client_destroy(client);
        return false;
    }
    client_ = client;
    return true;
}

bool EspMqttTransport::publish(std::string_view topic, std::string_view payload, bool retain) {
    if (!connected_) {
        return false;
    }
    const std::string name(topic);  // esp-mqtt wants a C string
    return esp_mqtt_client_enqueue(handle(client_), name.c_str(), payload.data(), static_cast<int>(payload.size()),
                                   0, retain ? 1 : 0, true) >= 0;
}

bool EspMqttTransport::subscribe(std::string_view filter) {
    if (!connected_) {
        return false;
    }
    const std::string name(filter);
    return esp_mqtt_client_subscribe_single(handle(client_), name.c_str(), 0) >= 0;
}

void EspMqttTransport::disconnect() {
    if (client_ == nullptr) {
        return;
    }
    if (connected_) {
        for (uint32_t waited = 0; esp_mqtt_client_get_outbox_size(handle(client_)) > 0 && waited < kFlushMs;
             waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    esp_mqtt_client_stop(handle(client_));  // DISCONNECT if connected, then its task ends
    esp_mqtt_client_destroy(handle(client_));
    client_ = nullptr;
    connected_ = false;
    drain();
}

void EspMqttTransport::poll(uint32_t timeoutMs) {
    auto queue = static_cast<QueueHandle_t>(queue_);
    Message *message = nullptr;
    const bool received = xQueueReceive(queue, &message, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    deliverLink();  // first: a message from a new connection follows its onConnected()
    if (received) {
        deliverMessage(message);
        while (xQueueReceive(queue, &message, 0) == pdTRUE) {
            deliverMessage(message);
        }
    }
}

void EspMqttTransport::deliverLink() {
    // Counters first: a field read early is at worst one event behind, and
    // the moved counter brings it up to date at the next poll.
    const uint32_t connects = connects_.load(std::memory_order_acquire);
    const uint32_t drops = drops_.load(std::memory_order_acquire);
    const bool up = up_.load(std::memory_order_acquire);
    const bool authRefused = authRefused_.load(std::memory_order_acquire);
    const bool dropped = drops != seenDrops_;
    const bool connected = connects != seenConnects_;
    seenConnects_ = connects;
    seenDrops_ = drops;
    if (client_ == nullptr || listener_ == nullptr) {
        return;
    }
    // A live link lost, or attempts that failed while down.
    if (dropped && (connected_ || !up)) {
        connected_ = false;
        listener_->onDropped(authRefused ? hubbo::integrations::MqttDropReason::AuthFailed
                                         : hubbo::integrations::MqttDropReason::Unreachable);
    }
    if (connected && up) {
        connected_ = true;
        listener_->onConnected();
    }
}

void EspMqttTransport::deliverMessage(Message *message) {
    if (message != nullptr && connected_ && listener_ != nullptr) {
        listener_->onMessage(message->topic, message->payload, message->retained);
    }
    delete message;
}

// The esp-mqtt task. Records or copies only; never calls the listener.
void EspMqttTransport::onEvent(void *arg, const char *, int32_t id, void *data) {
    auto *self = static_cast<EspMqttTransport *>(arg);
    auto *source = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED:
            self->authRefused_.store(false, std::memory_order_release);
            self->up_.store(true, std::memory_order_release);
            self->connects_.fetch_add(1, std::memory_order_release);
            self->wake();
            return;
        case MQTT_EVENT_DISCONNECTED:
            self->up_.store(false, std::memory_order_release);
            self->drops_.fetch_add(1, std::memory_order_release);
            self->wake();
            return;
        case MQTT_EVENT_ERROR:
            // Only a refused login matters; the drop that follows says the rest.
            if (source->error_handle != nullptr &&
                source->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                (source->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                 source->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED)) {
                self->authRefused_.store(true, std::memory_order_release);
            }
            return;
        case MQTT_EVENT_DATA: {
            // Whole messages only: a fragment (offset > 0 or short) or an
            // oversized one is dropped.
            if (source->current_data_offset != 0 || source->data_len != source->total_data_len ||
                static_cast<size_t>(source->data_len) > kMaxInboundBytes || source->topic_len <= 0 ||
                static_cast<size_t>(source->topic_len) > kMaxTopicBytes) {
                return;
            }
            auto *message = new Message;
            message->retained = source->retain;
            message->topic.assign(source->topic, source->topic_len);
            message->payload.assign(source->data, source->data_len);
            if (xQueueSend(static_cast<QueueHandle_t>(self->queue_), &message, 0) != pdTRUE) {
                delete message;  // poll() is far behind; a command can be lost, the link can't
            }
            return;
        }
        default:
            return;
    }
}

// Best effort: poll() delivers the link at its next pass anyway.
void EspMqttTransport::wake() {
    auto queue = static_cast<QueueHandle_t>(queue_);
    Message *none = nullptr;
    if (uxQueueMessagesWaiting(queue) == 0) {  // a waiting message wakes poll() too
        xQueueSend(queue, &none, 0);
    }
}

void EspMqttTransport::drain() {
    Message *message = nullptr;
    while (xQueueReceive(static_cast<QueueHandle_t>(queue_), &message, 0) == pdTRUE) {
        delete message;
    }
}

}  // namespace hubbo::fw
