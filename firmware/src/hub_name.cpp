#include "hub_name.h"

#include <mdns.h>

#include "console.h"

namespace hubbo::fw {

namespace {

constexpr size_t kMdnsNameMaxBytes = 63;  // a DNS label; mdns.h's MDNS_NAME_MAX_LEN

// The hostname first (mdns_instance_name_set refuses, ESP_ERR_INVALID_ARG,
// while none is set), then the instance: each sends goodbyes for the old
// records and probes and announces the new ones. mdns_hostname_set waits for
// the mDNS task (core 0, priority 1) to take it. Core keeps the hostname
// within a label; only the instance name may need cutting.
void registerMdns(const hubbo::config::HubIdentity &identity) {
    const std::string instance = identity.name.substr(0, utf8Prefix(identity.name, kMdnsNameMaxBytes));
    const esp_err_t hostResult = mdns_hostname_set(identity.hostname.c_str());
    const esp_err_t instanceResult = mdns_instance_name_set(instance.c_str());
    if (instanceResult != ESP_OK || hostResult != ESP_OK) {
        con.printf("mdns: rename failed (instance %s, hostname %s)\n", esp_err_to_name(instanceResult),
                   esp_err_to_name(hostResult));
        return;
    }
    con.printf("mdns: \"%s\" at %s.local\n", instance.c_str(), identity.hostname.c_str());
}

}  // namespace

size_t utf8Prefix(std::string_view text, size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return text.size();
    }
    size_t length = maxBytes;
    // Back off continuation bytes (10xxxxxx) to the start of the code point
    // that doesn't fit.
    while (length > 0 && (static_cast<uint8_t>(text[length]) & 0xC0) == 0x80) {
        length--;
    }
    return length;
}

void HubName::startMdns(const hubbo::config::HubIdentity &identity, uint16_t httpPort) {
    if (mdnsStarted_) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        con.println("mdns: init failed");
        return;
    }
    mdnsStarted_ = true;
    registerMdns(identity);
    // No instance of its own: it follows the global instance name.
    if (mdns_service_add(nullptr, "_http", "_tcp", httpPort, nullptr, 0) != ESP_OK) {
        con.println("mdns: couldn't add _http._tcp");
    }
}

void HubName::apply(const hubbo::config::HubIdentity &identity) {
    if (mdnsStarted_) {
        registerMdns(identity);
    }
    portENTER_CRITICAL(&lock_);
    bleName_.assign(identity.name);  // core keeps it within kHubNameMaxBytes
    portEXIT_CRITICAL(&lock_);
    blePending_.store(true, std::memory_order_release);
}

bool HubName::takeBleName(HubNameString &out) {
    if (!blePending_.exchange(false, std::memory_order_acquire)) {
        return false;
    }
    portENTER_CRITICAL(&lock_);
    out = bleName_;
    portEXIT_CRITICAL(&lock_);
    return true;
}

}  // namespace hubbo::fw
