#pragma once

// The hub's name on the network and over Bluetooth. Core's HubSettings
// (hubbo/config/hub.h) owns the name (1-32 code points, default "Hubbo") and
// its hostname (hostnameFor: "Living Room" -> living-room); this applies them.
//
//   Bluetooth  the GAP Device Name is the name as typed; the scan response
//              carries it too, shortened at a UTF-8 boundary past 29 bytes
//              (BleHid::setName).
//   mDNS       instance name = the name (cut at 63 bytes), hostname = the slug,
//              _http._tcp on port 80.
//   DHCP, OTA  the boot hostname, until the next boot.
//
// A rename (HubSettings::onChanged, on httpd) is applied live: mDNS at once
// (mdns_hostname_set waits on the mDNS task, so never on the RF loop),
// Bluetooth through a one-slot mailbox the loop takes (BleHid is loop-only,
// and the loop must not wait on HubSettings' mutex). A spinlock guards only
// the copy; the loop checks an atomic flag first.

#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/config/hub.h"
#include "hubbo/domain/ids.h"

namespace hubbo::fw {

// The longest prefix of `text` of at most `maxBytes` bytes that doesn't split
// a UTF-8 sequence.
size_t utf8Prefix(std::string_view text, size_t maxBytes);

using HubNameString = hubbo::FixedString<hubbo::config::kHubNameMaxBytes>;

class HubName {
public:
    // setup(), before Ethernet and the API: the hostname DHCP and OTA keep
    // until the next boot.
    void begin(const hubbo::config::HubIdentity &identity) { bootHostname_ = identity.hostname; }
    const char *bootHostname() const { return bootHostname_.c_str(); }

    // httpd, once the network has an address: starts mDNS. Later calls do
    // nothing.
    void startMdns(const hubbo::config::HubIdentity &identity, uint16_t httpPort);
    // httpd, after a rename commits: mDNS now (if started), Bluetooth on the
    // loop's next takeBleName().
    void apply(const hubbo::config::HubIdentity &identity);

    // Loop: true (and the new name in `out`) once per rename.
    bool takeBleName(HubNameString &out);

private:
    std::string bootHostname_;  // set once in setup()
    bool mdnsStarted_ = false;  // httpd
    // The mailbox to the loop.
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    HubNameString bleName_;
    std::atomic<bool> blePending_{false};
};

}  // namespace hubbo::fw
