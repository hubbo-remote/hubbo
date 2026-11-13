#include "fake_mqtt.h"

#include <algorithm>
#include <utility>

namespace hubbo::sim {

void FakeMqttBroker::reset() {
    if (client_ != nullptr) {
        FakeMqttTransport *client = client_;
        client_ = nullptr;
        client->dropped();
    }
    reachable = true;
    refuseAuth = false;
    retained_.clear();
    published_.clear();
    subscriptions_.clear();
    clientId_.clear();
}

bool FakeMqttBroker::matches(std::string_view filter, std::string_view topic) {
    for (;;) {
        const size_t fEnd = filter.find('/');
        const std::string_view level = filter.substr(0, fEnd);
        if (level == "#") return true;
        const size_t tEnd = topic.find('/');
        if (level != "+" && level != topic.substr(0, tEnd)) return false;
        if (fEnd == std::string_view::npos || tEnd == std::string_view::npos) {
            return fEnd == std::string_view::npos && tEnd == std::string_view::npos;
        }
        filter.remove_prefix(fEnd + 1);
        topic.remove_prefix(tEnd + 1);
    }
}

void FakeMqttBroker::store(std::string_view topic, std::string_view payload, bool retain) {
    if (!retain) return;
    if (payload.empty()) {
        retained_.erase(std::string(topic));
    } else {
        retained_[std::string(topic)] = std::string(payload);
    }
}

void FakeMqttBroker::publish(std::string_view topic, std::string_view payload, bool retain) {
    store(topic, payload, retain);
    if (client_ == nullptr) return;
    for (const std::string &filter : subscriptions_) {
        if (matches(filter, topic)) {
            // A live publish reaches subscribers with the retain flag clear.
            client_->deliver(std::string(topic), std::string(payload), false);
            return;
        }
    }
}

void FakeMqttBroker::attach(FakeMqttTransport *client, const integrations::MqttConnectOptions &options) {
    client_ = client;
    clientId_ = options.clientId;
    will_ = options.will;
    connections_++;
}

void FakeMqttBroker::detach(FakeMqttTransport *client, bool clean) {
    if (client_ != client) return;
    client_ = nullptr;
    subscriptions_.clear();  // a clean session: the next one subscribes again
    if (!clean && !will_.topic.empty()) store(will_.topic, will_.payload, will_.retain);
}

void FakeMqttBroker::fromClient(std::string_view topic, std::string_view payload, bool retain) {
    store(topic, payload, retain);
    published_.push_back({std::string(topic), std::string(payload), retain});
    if (published_.size() > kMaxPublished) published_.pop_front();
}

void FakeMqttBroker::subscribe(std::string_view filter) {
    if (std::find(subscriptions_.begin(), subscriptions_.end(), filter) == subscriptions_.end()) {
        subscriptions_.emplace_back(filter);
    }
    for (const auto &[topic, payload] : retained_) {
        if (matches(filter, topic)) client_->deliver(topic, payload, true);
    }
}

// ---- transport ---------------------------------------------------------

FakeMqttTransport::~FakeMqttTransport() {
    if (open_) broker_.detach(this, true);
}

bool FakeMqttTransport::connect(const integrations::MqttConnectOptions &options) {
    connectCalls++;
    if (started_ || options.host.empty() || refuseConnect) return false;
    options_ = options;
    started_ = true;
    reported_.reset();
    return true;  // the attempt is made in poll()
}

bool FakeMqttTransport::publish(std::string_view topic, std::string_view payload, bool retain) {
    if (!open_) return false;
    broker_.fromClient(topic, payload, retain);
    return true;
}

bool FakeMqttTransport::subscribe(std::string_view filter) {
    if (!open_) return false;
    broker_.subscribe(filter);
    return true;
}

void FakeMqttTransport::disconnect() {
    if (open_) broker_.detach(this, true);
    open_ = false;
    started_ = false;
    inbox_.clear();
}

void FakeMqttTransport::deliver(std::string topic, std::string payload, bool retained) {
    inbox_.push_back({std::move(topic), std::move(payload), retained});
}

void FakeMqttTransport::dropped() {
    open_ = false;
    reported_.reset();
    inbox_.clear();
}

void FakeMqttTransport::poll(uint32_t) {
    if (!started_ || listener_ == nullptr) return;
    const bool refused = !broker_.reachable || broker_.refuseAuth;
    if (open_ && refused) {
        broker_.detach(this, false);  // the link dropped: the will fires
        dropped();
    }
    if (!open_) {
        // The platform's auto-reconnect: an attempt every poll, a failure
        // reported once (and again if its reason changes).
        if (refused) {
            const integrations::MqttDropReason reason = broker_.reachable ? integrations::MqttDropReason::AuthFailed
                                                                          : integrations::MqttDropReason::Unreachable;
            if (reported_ != reason) {
                reported_ = reason;
                listener_->onDropped(reason);
            }
            return;
        }
        if (broker_.connected()) return;  // one client at a time
        broker_.attach(this, options_);
        open_ = true;
        reported_.reset();
        listener_->onConnected();
    }
    while (open_ && !inbox_.empty()) {
        FakeMqttBroker::Message m = std::move(inbox_.front());
        inbox_.pop_front();
        listener_->onMessage(m.topic, m.payload, m.retained);
    }
}

}  // namespace hubbo::sim
