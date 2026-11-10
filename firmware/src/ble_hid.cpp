// Ported from Harmonizer src/bluetooth.cpp (soaresbrun0/Harmonizer, AGPL-3.0).
// NimBLEStream's availableForWrite() hides Print's (library code, not ours).
// GCC reports it at Print.h, so the pragma must cover Print.h's first include.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual="
#include "ble_hid.h"
#include "console.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#pragma GCC diagnostic pop


namespace hubbo::fw {

namespace {

constexpr uint8_t kKeyboardReportId = 1;
constexpr uint8_t kConsumerReportId = 2;

constexpr int kTxPowerDbm = 9;
// Requested once bonded: 15 ms interval (1.25 ms units; Apple's minimum for
// accessories), no peripheral latency, 2 s supervision timeout (10 ms
// units). A slow interval makes a key-up arrive late and read as a hold.
constexpr uint16_t kConnIntervalUnits = 12;
constexpr uint16_t kConnTimeoutUnits = 200;

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
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array, Absolute): keys
    0xC0,              // End Collection
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, kConsumerReportId,
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0               // End Collection
};

NimBLEServer *server = nullptr;
NimBLEHIDDevice *hid = nullptr;
NimBLECharacteristic *keyboardInput = nullptr;
NimBLECharacteristic *consumerInput = nullptr;

// Written by NimBLE callbacks on the host task, read on the main loop.
volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
volatile bool secured = false;
volatile bool bondsChanged = false;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
        // One host at a time; a second connection is refused.
        if (connHandle != BLE_HS_CONN_HANDLE_NONE) {
            pServer->disconnect(connInfo);
            return;
        }
        connHandle = connInfo.getConnHandle();
        secured = false;
        NimBLEDevice::startSecurity(connInfo.getConnHandle());
    }

    void onDisconnect(NimBLEServer *, NimBLEConnInfo &connInfo, int) override {
        if (connInfo.getConnHandle() != connHandle) {
            return;
        }
        connHandle = BLE_HS_CONN_HANDLE_NONE;
        secured = false;
    }

    void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
        // Bonding is only allowed while the pairing window is open, so
        // outside it a new host ends up encrypted but not bonded: drop it.
        if (!connInfo.isEncrypted() || !connInfo.isBonded()) {
            server->disconnect(connInfo);
            return;
        }
        secured = true;
        bondsChanged = true;
        server->updateConnParams(connInfo.getConnHandle(), kConnIntervalUnits, kConnIntervalUnits,
                                 0, kConnTimeoutUnits);
    }
};

void setBondingAllowed(bool allowed) {
    // Just Works (no screen or keypad); bonding only while pairing.
    NimBLEDevice::setSecurityAuth(allowed, false, true);
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
    NimBLEDevice::setPower(kTxPowerDbm);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    setBondingAllowed(false);

    server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCallbacks());

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

    bondCount_ = NimBLEDevice::getNumBonds();
    started_ = true;
    updateAdvertising();
    con.printf("ble: started as \"%s\", %d bonded host(s)\n", name, bondCount_);
    return true;
}

void BleHid::loop(uint32_t nowMs) {
    if (!started_) {
        return;
    }

    if (pairing_ && nowMs - pairingStartedMs_ >= pairingWindowMs_) {
        con.println("ble: pairing window closed");
        stopPairing();
        pairingFailed_ = true;
    }

    if (bondsChanged) {
        bondsChanged = false;
        const uint16_t handle = connHandle;
        if (pairing_ && secured && handle != BLE_HS_CONN_HANDLE_NONE) {
            // Paired: keep only this host (off this task), then close the window.
            const NimBLEAddress keep = server->getPeerInfoByHandle(handle).getIdAddress();
            runBondJob([keep] {
                for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; i--) {
                    const NimBLEAddress address = NimBLEDevice::getBondedAddress(i);
                    if (!(address == keep)) {
                        NimBLEDevice::deleteBond(address);
                    }
                }
            });
            con.printf("ble: paired with %s\n", keep.toString().c_str());
            stopPairing();
        }
        bondCount_ = NimBLEDevice::getNumBonds();
    }

    if (pendingBondJob_ && offload_(pendingBondJob_)) {
        pendingBondJob_ = nullptr;
    }

    // Bonding opens only once the old bonds are gone, so a deletion that
    // runs late can't remove the new host.
    if (pairing_ && !bondingOpen_ && !bondsClearing_.load(std::memory_order_acquire)) {
        bondingOpen_ = true;
        setBondingAllowed(true);
    }

    // Polled rather than event-driven: NimBLE stops advertising on connect.
    updateAdvertising();
}

void BleHid::runBondJob(std::function<void()> job) {
    bondsClearing_.store(true, std::memory_order_release);
    auto done = [this, job = std::move(job)] {
        job();
        bondsChanged = true;  // loop() refreshes the count
        bondsClearing_.store(false, std::memory_order_release);
    };
    if (!offload_) {
        done();  // boot: no other task yet
    } else if (!offload_(done)) {
        pendingBondJob_ = std::move(done);  // queue full: loop() retries
    }
}

// --- Reports ------------------------------------------------------------------

bool BleHid::connected() const {
    return started_ && secured && connHandle != BLE_HS_CONN_HANDLE_NONE;
}

bool BleHid::sendKeyboard(const uint8_t report[8]) {
    if (!connected()) {
        return false;
    }
    if (!keyboardInput->notify(report, 8)) {
        notifyFailures_++;
        return false;
    }
    return true;
}

bool BleHid::sendConsumer(uint16_t usage) {
    if (!connected()) {
        return false;
    }
    const uint8_t report[2] = {static_cast<uint8_t>(usage & 0xFF), static_cast<uint8_t>(usage >> 8)};
    if (!consumerInput->notify(report, sizeof(report))) {
        notifyFailures_++;
        return false;
    }
    return true;
}

void BleHid::releaseAll() {
    static const uint8_t kEmpty[8] = {};
    sendKeyboard(kEmpty);
    sendConsumer(0);
}

// --- Pairing ----------------------------------------------------------------

void BleHid::updateAdvertising() {
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    const bool shouldAdvertise =
        connHandle == BLE_HS_CONN_HANDLE_NONE && (pairing_ || bondCount_ > 0);
    if (shouldAdvertise && !advertising->isAdvertising()) {
        advertising->start();
    } else if (!shouldAdvertise && advertising->isAdvertising()) {
        advertising->stop();
    }
}

bool BleHid::startPairing(uint32_t nowMs, uint32_t windowMs) {
    if (!started_) {
        return false;
    }
    if (bondCount_ > 0 || connHandle != BLE_HS_CONN_HANDLE_NONE) {
        forget();
    }
    pairing_ = true;
    pairingFailed_ = false;
    bondingOpen_ = false;  // loop() opens it once any bond deletion is done
    pairingStartedMs_ = nowMs;
    pairingWindowMs_ = windowMs;
    updateAdvertising();
    con.printf("ble: pairing window open for %lu s\n",
                  static_cast<unsigned long>(windowMs / 1000));
    return true;
}

void BleHid::stopPairing() {
    if (!pairing_) {
        return;
    }
    pairing_ = false;
    bondingOpen_ = false;
    setBondingAllowed(false);
    updateAdvertising();
}

uint32_t BleHid::pairingSecondsLeft(uint32_t nowMs) const {
    if (!pairing_) {
        return 0;
    }
    const uint32_t elapsed = nowMs - pairingStartedMs_;
    return elapsed >= pairingWindowMs_ ? 0 : (pairingWindowMs_ - elapsed + 999) / 1000;
}

void BleHid::forget() {
    if (!started_) {
        return;
    }
    con.println("ble: forgetting bonded hosts");
    stopPairing();
    pairingFailed_ = false;
    releaseAll();
    const uint16_t handle = connHandle;
    if (handle != BLE_HS_CONN_HANDLE_NONE) {
        secured = false;  // no longer trusted: not connected() while the disconnect lands
        server->disconnect(handle);
    }
    runBondJob([] { NimBLEDevice::deleteAllBonds(); });
    bondCount_ = 0;
    updateAdvertising();
}

// --- Status -------------------------------------------------------------------

BleHid::State BleHid::state() const {
    if (!started_) {
        return State::Off;
    }
    if (connected()) {
        return State::Connected;
    }
    if (pairing_) {
        return State::Pairing;
    }
    return bondCount_ > 0 ? State::Waiting : State::NotPaired;
}

const char *BleHid::stateName(State state) {
    switch (state) {
        case State::Off: return "off";
        case State::NotPaired: return "not_paired";
        case State::Pairing: return "pairing";
        case State::Waiting: return "waiting";
        case State::Connected: return "connected";
    }
    return "off";
}

void BleHid::peerAddress(char *out, size_t size) const {
    if (size == 0) {
        return;
    }
    out[0] = '\0';
    if (!started_) {
        return;
    }
    const uint16_t handle = connHandle;
    if (handle != BLE_HS_CONN_HANDLE_NONE) {
        snprintf(out, size, "%s", server->getPeerInfoByHandle(handle).getIdAddress().toString().c_str());
    } else if (bondCount_ > 0) {
        snprintf(out, size, "%s", NimBLEDevice::getBondedAddress(0).toString().c_str());
    }
}

}  // namespace hubbo::fw
