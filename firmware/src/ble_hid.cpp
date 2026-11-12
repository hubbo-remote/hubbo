// Ported from Harmonizer src/bluetooth.cpp (soaresbrun0/Harmonizer, AGPL-3.0).
// NimBLEStream's availableForWrite() hides Print's (library code, not ours).
// GCC reports it at Print.h, so the pragma must cover Print.h's first include.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual="
#include "ble_hid.h"
#include "console.h"
#include "hubbo/domain/limits.h"
#include "hubbo/engine/spsc.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Preferences.h>
#pragma GCC diagnostic pop

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

// Each host plus one (include/sdkconfig.h).
static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS == hubbo::fw::BleHid::kMaxHosts + 1, "see include/sdkconfig.h");
static_assert(CONFIG_BT_NIMBLE_MAX_BONDS == hubbo::fw::BleHid::kMaxHosts + 1, "see include/sdkconfig.h");

namespace hubbo::fw {

namespace {

// The report map's ranges are core's (what an action can name): any usage
// within them can be sent, and the sends refuse anything above (0 is none
// or released).
using hubbo::limits::kHidConsumerUsageMax;
using hubbo::limits::kHidKeyboardUsageMax;

constexpr uint8_t kKeyboardReportId = 1;
constexpr uint8_t kConsumerReportId = 2;

constexpr int kTxPowerDbm = 9;
// Requested once bonded: 15 ms interval (1.25 ms units; Apple's minimum for
// accessories), no peripheral latency, 2 s supervision timeout (10 ms
// units). A slow interval makes a key-up arrive late and read as a hold.
constexpr uint16_t kConnIntervalUnits = 12;
constexpr uint16_t kConnTimeoutUnits = 200;
constexpr uint32_t kAdvertisingCheckMs = 1000;

// Keyboard (report 1: modifiers, reserved, 6 keys; LED output report) and
// consumer control (report 2: one 16-bit usage). Unchanged from Harmonizer.
const uint8_t kReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, kKeyboardReportId,
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute): modifiers
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant): reserved
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data, Variable, Absolute): LEDs
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Constant): LED padding
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, kHidKeyboardUsageMax,  // Logical Maximum (core's keyboard max)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, kHidKeyboardUsageMax,  // Usage Maximum (core's keyboard max)
    0x81, 0x00,        //   Input (Data, Array, Absolute): keys
    0xC0,              // End Collection
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, kConsumerReportId,
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, kHidConsumerUsageMax & 0xFF, kHidConsumerUsageMax >> 8,  // Logical Maximum (core's consumer max)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, kHidConsumerUsageMax & 0xFF, kHidConsumerUsageMax >> 8,  // Usage Maximum (core's consumer max)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0               // End Collection
};

NimBLEServer *server = nullptr;
NimBLEHIDDevice *hid = nullptr;
NimBLECharacteristic *keyboardInput = nullptr;
NimBLECharacteristic *consumerInput = nullptr;

// Set by NimBLE callbacks on the host task, taken by loop(): any link
// change, and which connections completed (re)authentication since (a bit
// per handle, mod 32; a stray bit only means one extra check).
std::atomic<bool> linksChanged{false};
std::atomic<uint32_t> authenticated{0};

uint32_t handleBit(uint16_t handle) { return 1u << (handle & 31u); }

// Link events for the log: pushed by a GAP listener on the host task,
// printed by loop(). A full ring counts and drops; nothing waits.
struct LinkEvent {
    enum class Kind : uint8_t { Connect, Encryption, Disconnect };
    uint32_t atMs = 0;
    Kind kind = Kind::Connect;
    uint16_t handle = 0;
    int code = 0;  // connect / encryption status, disconnect reason (NimBLE host codes)
    bool encrypted = false;
    bool bonded = false;
    uint8_t addrType = 0;  // identity address (an unresolved one before pairing)
    uint8_t addr[6] = {};
};
hubbo::SpscRing<LinkEvent, 17> linkEvents;  // 16 events
std::atomic<uint32_t> linkEventsDropped{0};

void recordLinkEvent(LinkEvent::Kind kind, uint16_t handle, int code, const ble_gap_conn_desc *desc) {
    LinkEvent event;
    event.atMs = millis();
    event.kind = kind;
    event.handle = handle;
    event.code = code;
    if (desc != nullptr) {
        event.encrypted = desc->sec_state.encrypted;
        event.bonded = desc->sec_state.bonded;
        event.addrType = desc->peer_id_addr.type;
        memcpy(event.addr, desc->peer_id_addr.val, sizeof(event.addr));
    }
    if (!linkEvents.push(event)) {
        linkEventsDropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// Alongside NimBLE-Arduino's own handler, which keeps the server callbacks
// (they drive the links); this one sees the connect and encryption statuses
// they don't pass on, and logs disconnects with them in order.
int onGapEvent(ble_gap_event *event, void *) {
    ble_gap_conn_desc desc{};
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT: {
            const bool found = event->connect.status == 0 && ble_gap_conn_find(event->connect.conn_handle, &desc) == 0;
            recordLinkEvent(LinkEvent::Kind::Connect, event->connect.conn_handle, event->connect.status,
                            found ? &desc : nullptr);
            break;
        }
        case BLE_GAP_EVENT_ENC_CHANGE: {
            const bool found = ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0;
            recordLinkEvent(LinkEvent::Kind::Encryption, event->enc_change.conn_handle, event->enc_change.status,
                            found ? &desc : nullptr);
            break;
        }
        case BLE_GAP_EVENT_DISCONNECT:
            recordLinkEvent(LinkEvent::Kind::Disconnect, event->disconnect.conn.conn_handle, event->disconnect.reason,
                            &event->disconnect.conn);
            break;
        default:
            break;
    }
    return 0;
}

ble_gap_event_listener gapListener;

// The host table in NVS: device id -> identity address, next to NimBLE's
// bonds. One blob, rewritten whole when it changes.
constexpr const char *kPrefsNamespace = "hubbo-ble";
constexpr const char *kPrefsHosts = "hosts";
struct HostRecord {
    char deviceId[hubbo::limits::kMaxIdLength + 1];
    uint8_t addrType;
    uint8_t addr[6];
};
using HostRecords = std::array<HostRecord, BleHid::kMaxHosts>;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &connInfo) override {
        // Bonded hosts re-encrypt; new ones pair (bonding only in a window).
        NimBLEDevice::startSecurity(connInfo.getConnHandle());
        linksChanged.store(true);
    }

    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override { linksChanged.store(true); }

    void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
        // Bonding is only allowed while a pairing window is open, so outside
        // it a new host ends up encrypted but not bonded: drop it.
        if (!connInfo.isEncrypted() || !connInfo.isBonded()) {
            server->disconnect(connInfo);
            return;
        }
        server->updateConnParams(connInfo.getConnHandle(), kConnIntervalUnits, kConnIntervalUnits, 0,
                                 kConnTimeoutUnits);
        authenticated.fetch_or(handleBit(connInfo.getConnHandle()));
        linksChanged.store(true);
    }
};

// NimBLE's default deletes the oldest bond when the store is full, which
// would drop a paired host. Refuse instead: the new bond isn't stored, and
// adoptBond() ends the window without a new host.
class DeviceCallbacks : public NimBLEDeviceCallbacks {
    int onStoreStatus(struct ble_store_status_event *event, void *) override {
        return event->event_code == BLE_STORE_EVENT_OVERFLOW ? BLE_HS_ESTORE_CAP : 0;
    }
};

void setBondingAllowed(bool allowed) {
    // Just Works (no screen or keypad); bonding only in a pairing window.
    NimBLEDevice::setSecurityAuth(allowed, false, true);
}

NimBLEAddress addressOf(uint8_t type, const uint8_t addr[6]) {
    ble_addr_t base{};
    base.type = type;
    memcpy(base.val, addr, sizeof(base.val));
    return NimBLEAddress(base);
}

}  // namespace

// --- Lifecycle -------------------------------------------------------------

bool BleHid::begin(const char *name) {
    if (started_) {
        return true;
    }
    if (!NimBLEDevice::init(name)) {
        con.println("ble: NimBLE init failed");
        return false;
    }
    static DeviceCallbacks deviceCallbacks;
    NimBLEDevice::setDeviceCallbacks(&deviceCallbacks);
    ble_gap_event_listener_register(&gapListener, onGapEvent, nullptr);
    NimBLEDevice::setPower(kTxPowerDbm);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    setBondingAllowed(false);

    server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());
    server->advertiseOnDisconnect(false);  // updateAdvertising() decides

    hid = new NimBLEHIDDevice(server);
    hid->setManufacturer("Hubbo");
    hid->setPnp(0x01, 0x02E5, 0x0001, 0x0100);  // Bluetooth SIG company id: Espressif
    hid->setHidInfo(0x00, 0x01);                // not localized; remote wake
    hid->setReportMap(const_cast<uint8_t *>(kReportMap), sizeof(kReportMap));
    keyboardInput = hid->getInputReport(kKeyboardReportId);
    hid->getOutputReport(kKeyboardReportId);  // LED report, ignored
    consumerInput = hid->getInputReport(kConsumerReportId);
    hid->setBatteryLevel(100);
    server->start();

    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->setAppearance(HID_KEYBOARD);
    advertising->addServiceUUID(hid->getHidService()->getUUID());
    // Scan response first: setName puts the name there only if it's already
    // on, and a longer name doesn't fit in the advertisement itself.
    advertising->enableScanResponse(true);
    advertising->setName(name);

    started_ = true;
    loadHosts();
    updateAdvertising();
    size_t paired = 0;
    for (const Host &host : hosts_) {
        paired += host.paired();
    }
    con.printf("ble: started as \"%s\", %u paired host(s), %d bond(s)\n", name, static_cast<unsigned>(paired),
               NimBLEDevice::getNumBonds());
    return true;
}

// Boot (setup(), before the offload exists): the table, then any bond that
// no device owns (deleted) and any entry whose bond is gone (dropped).
void BleHid::loadHosts() {
    HostRecords records{};
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, true)) {
        if (prefs.getBytesLength(kPrefsHosts) == sizeof(records)) {
            prefs.getBytes(kPrefsHosts, records.data(), sizeof(records));
        }
        prefs.end();
    }
    bool changed = false;
    for (size_t i = 0; i < kMaxHosts; i++) {
        HostRecord &record = records[i];
        record.deviceId[sizeof(record.deviceId) - 1] = '\0';
        if (record.deviceId[0] == '\0') {
            continue;
        }
        if (!NimBLEDevice::isBonded(addressOf(record.addrType, record.addr))) {
            con.printf("ble: %s's bond is gone; not paired\n", record.deviceId);
            changed = true;
            continue;
        }
        hosts_[i].deviceId.assign(record.deviceId);
        hosts_[i].addrType = record.addrType;
        memcpy(hosts_[i].addr, record.addr, sizeof(record.addr));
    }
    for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; i--) {
        const NimBLEAddress bonded = NimBLEDevice::getBondedAddress(i);
        const ble_addr_t *base = bonded.getBase();
        if (findByIdentity(base->type, base->val) == nullptr) {
            con.printf("ble: deleting a bond no device owns (%s)\n", bonded.toString().c_str());
            NimBLEDevice::deleteBond(bonded);
        }
    }
    if (changed) {
        saveHosts();
    }
}

void BleHid::loop(uint32_t nowMs) {
    if (!started_) {
        return;
    }
    if (windowOpen() && nowMs - windowStartedMs_ >= windowMs_) {
        con.printf("ble: pairing window for %s closed with no new host\n", windowDevice_.c_str());
        closeWindow();
    }
    logLinkEvents();
    const bool linkEvent = linksChanged.exchange(false);
    if (linkEvent) {
        refreshLinks();
    }
    while (!pendingJobs_.empty() && offload_(pendingJobs_.front())) {
        pendingJobs_.pop_front();
    }
    // On link events (NimBLE stops advertising on connect), and once a
    // second in case a start failed.
    if (linkEvent || nowMs - advertisingCheckedMs_ >= kAdvertisingCheckMs) {
        advertisingCheckedMs_ = nowMs;
        updateAdvertising();
    }
}

// One line per link event, naming the device when the address is a host's.
void BleHid::logLinkEvents() {
    if (linkEventsDropped.load(std::memory_order_relaxed) != 0) {
        con.printf("ble: %lu link event(s) not logged\n",
                   static_cast<unsigned long>(linkEventsDropped.exchange(0, std::memory_order_relaxed)));
    }
    LinkEvent event;
    while (linkEvents.pop(&event)) {
        const Host *host = findByIdentity(event.addrType, event.addr);
        const char *who = host != nullptr ? host->deviceId.c_str() : "(no device)";
        const unsigned long at = event.atMs;
        char address[18];  // as NimBLEAddress::toString(), without the heap
        snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x", event.addr[5], event.addr[4],
                 event.addr[3], event.addr[2], event.addr[1], event.addr[0]);
        switch (event.kind) {
            case LinkEvent::Kind::Connect:
                if (event.code == 0) {
                    con.printf("ble: %lu %s %s connected (handle %u)\n", at, who, address, event.handle);
                } else {
                    con.printf("ble: %lu connect failed (status 0x%x)\n", at, event.code);
                }
                break;
            case LinkEvent::Kind::Encryption:
                con.printf("ble: %lu %s %s encryption %s (status 0x%x, encrypted %d, bonded %d)\n", at, who, address,
                           event.code == 0 ? "ok" : "failed", event.code, event.encrypted, event.bonded);
                break;
            case LinkEvent::Kind::Disconnect:
                con.printf("ble: %lu %s %s disconnected (reason 0x%x)\n", at, who, address, event.code);
                break;
        }
    }
}

// On a link event: adopt or drop new bonds, then each host's link from the
// stack.
void BleHid::refreshLinks() {
    const uint32_t justAuthenticated = authenticated.exchange(0);
    for (const uint16_t handle : server->getPeerDevices()) {
        const NimBLEConnInfo info = server->getPeerInfoByHandle(handle);
        if (!info.isEncrypted() || !info.isBonded()) {
            continue;  // still pairing, or dropped by onAuthenticationComplete
        }
        const NimBLEAddress identity = info.getIdAddress();
        const ble_addr_t *base = identity.getBase();
        if (const Host *host = findByIdentity(base->type, base->val)) {
            // A paired host (re)connected. If it's the window's device's own
            // host authenticating, that re-pair is done: nothing to delete.
            // A host bound to another device just stays with it.
            if (host->deviceId == windowDevice_.view() && (justAuthenticated & handleBit(handle))) {
                con.printf("ble: %s paired with its host again\n", windowDevice_.c_str());
                closeWindow();
            }
            continue;
        }
        if (windowOpen()) {
            adoptBond(handle, base->type, base->val);
        } else {
            con.printf("ble: dropping a host no device owns (%s)\n", identity.toString().c_str());
            server->disconnect(handle);
            deleteBond(base->type, base->val);
        }
    }
    for (Host &host : hosts_) {
        host.linked = host.paired() && connHandleOf(host);
    }
}

// The first new bond in the window: the window's device's host. Only now is
// that device's old bond (if another host) deleted.
void BleHid::adoptBond(uint16_t handle, uint8_t type, const uint8_t addr[6]) {
    Host *slot = find(windowDevice_.view());
    if (slot == nullptr) {
        for (Host &host : hosts_) {
            if (!host.paired()) {
                slot = &host;
                break;
            }
        }
    }
    // A refused store write (full) leaves the link bonded in name only.
    if (slot == nullptr || !NimBLEDevice::isBonded(addressOf(type, addr))) {
        con.printf("ble: pairing %s failed: %s\n", windowDevice_.c_str(),
                   slot == nullptr ? "no free host slot" : "the bond store is full");
        server->disconnect(handle);
        deleteBond(type, addr);
        closeWindow();
        return;
    }
    if (slot->paired()) {
        disconnect(*slot);
        deleteBond(slot->addrType, slot->addr);
    }
    slot->deviceId = windowDevice_;
    slot->addrType = type;
    memcpy(slot->addr, addr, sizeof(slot->addr));
    slot->linked = true;
    con.printf("ble: %s paired with %s\n", slot->deviceId.c_str(), addressOf(type, addr).toString().c_str());
    saveHosts();
    closeWindow();
}

void BleHid::closeWindow() {
    windowDevice_.clear();
    setBondingAllowed(false);
    updateAdvertising();
}

// --- NVS (bonds and the host table): always off this task -------------------

void BleHid::deleteBond(uint8_t type, const uint8_t addr[6]) {
    const NimBLEAddress address = addressOf(type, addr);
    runNvsJob([address] { NimBLEDevice::deleteBond(address); });
}

void BleHid::saveHosts() {
    whitelistDirty_ = true;
    HostRecords records{};
    for (size_t i = 0; i < kMaxHosts; i++) {
        const Host &host = hosts_[i];
        memcpy(records[i].deviceId, host.deviceId.c_str(), host.deviceId.size() + 1);
        records[i].addrType = host.addrType;
        memcpy(records[i].addr, host.addr, sizeof(records[i].addr));
    }
    runNvsJob([records] {
        Preferences prefs;
        if (!prefs.begin(kPrefsNamespace, false) || prefs.putBytes(kPrefsHosts, records.data(), sizeof(records)) == 0) {
            con.println("ble: couldn't save the paired hosts");
        }
        prefs.end();
    });
}

// In order: a table save queued after a bond deletion runs after it.
void BleHid::runNvsJob(std::function<void()> job) {
    if (!offload_) {
        job();  // boot: no other task yet
    } else if (!pendingJobs_.empty() || !offload_(job)) {
        pendingJobs_.push_back(std::move(job));  // loop() retries
    }
}

// --- Reports ------------------------------------------------------------------

BleHid::Host *BleHid::find(std::string_view deviceId) {
    for (Host &host : hosts_) {
        if (host.paired() && host.deviceId == deviceId) {
            return &host;
        }
    }
    return nullptr;
}

const BleHid::Host *BleHid::find(std::string_view deviceId) const {
    return const_cast<BleHid *>(this)->find(deviceId);
}

BleHid::Host *BleHid::findByIdentity(uint8_t type, const uint8_t addr[6]) {
    for (Host &host : hosts_) {
        if (host.paired() && host.addrType == type && memcmp(host.addr, addr, sizeof(host.addr)) == 0) {
            return &host;
        }
    }
    return nullptr;
}

// The stack's live, encrypted connection to this host's identity (it
// matches a host using a private address once resolved).
bool BleHid::connHandleOf(const Host &host, uint16_t *handle) const {
    ble_gap_conn_desc desc{};
    if (ble_gap_conn_find_by_addr(addressOf(host.addrType, host.addr).getBase(), &desc) != 0 ||
        !desc.sec_state.encrypted) {
        return false;
    }
    if (handle != nullptr) {
        *handle = desc.conn_handle;
    }
    return true;
}

void BleHid::disconnect(const Host &host) {
    uint16_t handle = 0;
    if (connHandleOf(host, &handle)) {
        server->disconnect(handle);
    }
}

bool BleHid::connected(std::string_view deviceId) const {
    const Host *host = find(deviceId);
    return started_ && host != nullptr && host->linked;
}

bool BleHid::notify(std::string_view deviceId, NimBLECharacteristic *input, const uint8_t *report, size_t length) {
    const Host *host = find(deviceId);
    uint16_t handle = 0;
    if (!started_ || host == nullptr || !connHandleOf(*host, &handle)) {
        return false;
    }
    if (!input->notify(report, length, handle)) {
        notifyFailures_++;
        return false;
    }
    return true;
}

bool BleHid::sendKeyboard(std::string_view deviceId, const uint8_t report[8]) {
    for (size_t i = 2; i < 8; i++) {
        if (report[i] > kHidKeyboardUsageMax) {
            return false;  // outside the report map: the host would ignore it
        }
    }
    return notify(deviceId, keyboardInput, report, 8);
}

bool BleHid::sendConsumer(std::string_view deviceId, uint16_t usage) {
    if (usage > kHidConsumerUsageMax) {
        return false;
    }
    const uint8_t report[2] = {static_cast<uint8_t>(usage & 0xFF), static_cast<uint8_t>(usage >> 8)};
    return notify(deviceId, consumerInput, report, sizeof(report));
}

// --- Pairing ----------------------------------------------------------------

void BleHid::updateAdvertising() {
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    bool hostMissing = false;
    for (const Host &host : hosts_) {
        hostMissing = hostMissing || (host.paired() && !host.linked);
    }
    const bool filtered = !windowOpen();
    const bool shouldAdvertise = started_ && server->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS &&
                                 (!filtered || hostMissing);
    // The whitelist and filter can't change while advertising uses them.
    if (advertising->isAdvertising() && (!shouldAdvertise || whitelistDirty_ || filtered != advertisingFiltered_)) {
        advertising->stop();
    }
    if (!shouldAdvertise || advertising->isAdvertising()) {
        return;
    }
    if (whitelistDirty_) {
        while (NimBLEDevice::getWhiteListCount() > 0) {
            NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));
        }
        for (const Host &host : hosts_) {
            if (host.paired()) {
                NimBLEDevice::whiteListAdd(addressOf(host.addrType, host.addr));
            }
        }
        whitelistDirty_ = false;
    }
    advertising->setScanFilter(false, filtered);  // connect: paired hosts only, unless pairing
    advertisingFiltered_ = filtered;
    advertising->start();
}

bool BleHid::startPairing(std::string_view deviceId, uint32_t nowMs, uint32_t windowMs) {
    if (!started_ || windowOpen() || deviceId.empty() || !windowDevice_.assign(deviceId)) {
        return false;
    }
    windowStartedMs_ = nowMs;
    windowMs_ = windowMs;
    setBondingAllowed(true);
    updateAdvertising();
    con.printf("ble: pairing window open for %s, %lu s\n", windowDevice_.c_str(),
               static_cast<unsigned long>(windowMs / 1000));
    return true;
}

void BleHid::cancelPairing(std::string_view deviceId) {
    if (windowOpen() && windowDevice_ == deviceId) {
        con.printf("ble: pairing window for %s cancelled\n", windowDevice_.c_str());
        closeWindow();
    }
}

void BleHid::forget(std::string_view deviceId) {
    if (!started_) {
        return;
    }
    cancelPairing(deviceId);
    Host *host = find(deviceId);
    if (host == nullptr) {
        return;
    }
    forgetHost(*host);
    saveHosts();
    updateAdvertising();
}

void BleHid::forgetHost(Host &host) {
    con.printf("ble: forgetting %s's host\n", host.deviceId.c_str());
    disconnect(host);
    deleteBond(host.addrType, host.addr);
    host = Host{};
}

void BleHid::retainOnly(const hubbo::integrations::BleDeviceIds &devices) {
    if (!started_) {
        return;
    }
    if (windowOpen() && !devices.contains(windowDevice_.view())) {
        cancelPairing(windowDevice_.view());
    }
    bool changed = false;
    for (Host &host : hosts_) {
        if (host.paired() && !devices.contains(host.deviceId.view())) {
            forgetHost(host);
            changed = true;
        }
    }
    if (changed) {
        saveHosts();
        updateAdvertising();
    }
}

// --- Status -------------------------------------------------------------------

hubbo::integrations::BleHostStatus BleHid::status(std::string_view deviceId, uint32_t nowMs) const {
    hubbo::integrations::BleHostStatus status;
    const Host *host = find(deviceId);
    status.bonded = host != nullptr;
    status.connected = started_ && host != nullptr && host->linked;
    status.pairing = windowOpen() && windowDevice_ == deviceId;
    if (status.pairing) {
        const uint32_t elapsed = nowMs - windowStartedMs_;
        status.windowRemainingMs = elapsed >= windowMs_ ? 0 : windowMs_ - elapsed;
    }
    return status;
}

void BleHid::snapshot(Snapshot &out, uint32_t nowMs) const {
    out = Snapshot{};
    out.radioReady = started_;
    const auto add = [&](const hubbo::IdString &deviceId) {
        if (deviceId.empty()) {
            return;
        }
        for (size_t i = 0; i < out.count; i++) {
            if (out.entries[i].deviceId == deviceId.view()) {
                return;
            }
        }
        Snapshot::Entry &entry = out.entries[out.count++];
        entry.deviceId = deviceId;
        entry.status = status(deviceId.view(), nowMs);
    };
    for (const Host &host : hosts_) {
        add(host.deviceId);
    }
    add(windowDevice_);
}

void BleHid::printStatus(uint32_t nowMs) const {
    if (!started_) {
        con.println("  ble: off");
        return;
    }
    con.printf("  ble: %d bond(s), %u connection(s), notify failures %lu\n", NimBLEDevice::getNumBonds(),
               static_cast<unsigned>(server->getConnectedCount()), static_cast<unsigned long>(notifyFailures_));
    for (const Host &host : hosts_) {
        if (host.paired()) {
            con.printf("    %s: %s, %s\n", host.deviceId.c_str(), addressOf(host.addrType, host.addr).toString().c_str(),
                       host.linked ? "connected" : "not connected");
        }
    }
    if (windowOpen()) {
        con.printf("    pairing window for %s, %lu s left\n", windowDevice_.c_str(),
                   static_cast<unsigned long>(
                       hubbo::integrations::bleDeviceState(status(windowDevice_.view(), nowMs)).windowS));
    }
}

}  // namespace hubbo::fw
