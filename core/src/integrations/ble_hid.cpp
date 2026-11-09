#include "hubbo/integrations/ble_hid.h"

#include <cstring>
#include <iterator>

#include "hubbo/domain/config.h"

namespace hubbo::integrations {

namespace {

// X(name, page, usage). Names follow the HID usage tables (the integration is
// generic HID; the UI offers friendly labels). Never reorder or rename: names
// are config values.
#define HUBBO_BLE_HID_KEYS(X)                                    \
    X("ARROW_UP", Consumer, 0x0042)      /* Menu Up */           \
    X("ARROW_DOWN", Consumer, 0x0043)    /* Menu Down */         \
    X("ARROW_LEFT", Consumer, 0x0044)    /* Menu Left */         \
    X("ARROW_RIGHT", Consumer, 0x0045)   /* Menu Right */        \
    X("SELECT", Consumer, 0x0041)        /* Menu Pick */         \
    X("MENU", Consumer, 0x0040)          /* Menu (tvOS Home) */  \
    X("AC_HOME", Consumer, 0x0223)       /* AC Home (tvOS: Back) */ \
    X("PLAY_PAUSE", Consumer, 0x00CD)                            \
    X("SCAN_NEXT", Consumer, 0x00B5)     /* Scan Next Track */   \
    X("SCAN_PREVIOUS", Consumer, 0x00B6) /* Scan Previous Track */ \
    X("RECORD", Consumer, 0x00B2)                                \
    X("VOLUME_UP", Consumer, 0x00E9)                             \
    X("VOLUME_DOWN", Consumer, 0x00EA)                           \
    X("MUTE", Consumer, 0x00E2)                                  \
    X("KEY_1", Keyboard, 0x1E)                                 \
    X("KEY_2", Keyboard, 0x1F)                                 \
    X("KEY_3", Keyboard, 0x20)                                 \
    X("KEY_4", Keyboard, 0x21)                                 \
    X("KEY_5", Keyboard, 0x22)                                 \
    X("KEY_6", Keyboard, 0x23)                                 \
    X("KEY_7", Keyboard, 0x24)                                 \
    X("KEY_8", Keyboard, 0x25)                                 \
    X("KEY_9", Keyboard, 0x26)                                 \
    X("KEY_0", Keyboard, 0x27)                                 \
    X("ENTER", Keyboard, 0x28)                                   \
    X("F1", Keyboard, 0x3A)                                      \
    X("F2", Keyboard, 0x3B)                                      \
    X("F3", Keyboard, 0x3C)                                      \
    X("F4", Keyboard, 0x3D)                                      \
    X("PAGE_UP", Keyboard, 0x4B)                                 \
    X("PAGE_DOWN", Keyboard, 0x4E)                               \
    X("KEY_A", Keyboard, 0x04)                                   \
    X("KEY_B", Keyboard, 0x05)                                   \
    X("KEY_C", Keyboard, 0x06)                                   \
    X("KEY_D", Keyboard, 0x07)                                   \
    X("KEY_E", Keyboard, 0x08)                                   \
    X("KEY_F", Keyboard, 0x09)                                   \
    X("KEY_G", Keyboard, 0x0A)                                   \
    X("KEY_H", Keyboard, 0x0B)                                   \
    X("KEY_I", Keyboard, 0x0C)                                   \
    X("KEY_J", Keyboard, 0x0D)                                   \
    X("KEY_K", Keyboard, 0x0E)                                   \
    X("KEY_L", Keyboard, 0x0F)                                   \
    X("KEY_M", Keyboard, 0x10)                                   \
    X("KEY_N", Keyboard, 0x11)                                   \
    X("KEY_O", Keyboard, 0x12)                                   \
    X("KEY_P", Keyboard, 0x13)                                   \
    X("KEY_Q", Keyboard, 0x14)                                   \
    X("KEY_R", Keyboard, 0x15)                                   \
    X("KEY_S", Keyboard, 0x16)                                   \
    X("KEY_T", Keyboard, 0x17)                                   \
    X("KEY_U", Keyboard, 0x18)                                   \
    X("KEY_V", Keyboard, 0x19)                                   \
    X("KEY_W", Keyboard, 0x1A)                                   \
    X("KEY_X", Keyboard, 0x1B)                                   \
    X("KEY_Y", Keyboard, 0x1C)                                   \
    X("KEY_Z", Keyboard, 0x1D)

#define HUBBO_KEY_ENTRY(name, page, usage) {name, HidPage::page, usage},
#define HUBBO_KEY_NAME(name, page, usage) name,

constexpr HidKey kKeys[] = {HUBBO_BLE_HID_KEYS(HUBBO_KEY_ENTRY)};
constexpr const char *kKeyNames[] = {HUBBO_BLE_HID_KEYS(HUBBO_KEY_NAME)};

#undef HUBBO_KEY_ENTRY
#undef HUBBO_KEY_NAME
#undef HUBBO_BLE_HID_KEYS

constexpr auto kKeyCount = static_cast<uint16_t>(std::size(kKeys));

constexpr ParamSpec kKeyParams[] = {
    ParamSpec::enumeration("key", kKeyNames, kKeyCount),
};

constexpr CommandSpec kCommands[] = {
    {"key_down", kKeyParams, 1, CommandEffect::Hold, "key_up"},
    {"key_up", kKeyParams, 1, CommandEffect::Momentary, nullptr},
    {"key_tap", kKeyParams, 1, CommandEffect::Momentary, nullptr},
};

ExecResult sent(bool ok) { return ok ? ExecResult::accepted() : ExecResult::failed(ErrorCode::AdapterFailed); }

}  // namespace

size_t bleHidKeyCount() { return kKeyCount; }

const HidKey &bleHidKey(size_t index) { return kKeys[index < kKeyCount ? index : 0]; }

int bleHidKeyIndex(const char *name) {
    if (name == nullptr) return -1;
    for (uint16_t i = 0; i < kKeyCount; i++) {
        if (std::strcmp(kKeys[i].name, name) == 0) return i;
    }
    return -1;
}

CommandCatalog bleHidCatalog() {
    return {kCommands, static_cast<uint16_t>(std::size(kCommands))};
}

const char *BleHidAdapter::integrationType() const { return kBleHidType; }

BleHidAdapter::BleHidAdapter(const char *integrationId, HidTransport &transport)
    : id_(integrationId), transport_(transport) {}

CommandCatalog BleHidAdapter::commandsFor(const DeviceSpec &device) const {
    (void)device;
    return bleHidCatalog();
}

ExecResult BleHidAdapter::execute(const ExecRequest &request) {
    if (request.command == nullptr || request.params == nullptr || request.commandIndex >= std::size(kCommands)) {
        return ExecResult::failed(ErrorCode::Internal);
    }
    const uint16_t option = request.params[0].asOption();
    if (option >= kKeyCount) {
        return ExecResult::failed(ErrorCode::Internal);
    }
    if (forgetIfDisconnected()) {
        return ExecResult::failed(ErrorCode::AdapterUnavailable);
    }
    const HidKey &key = kKeys[option];
    switch (static_cast<BleHidCommand>(request.commandIndex)) {
        case BleHidCommand::KeyDown:
            return press(key);
        case BleHidCommand::KeyUp:
            return release(key);
        case BleHidCommand::KeyTap: {
            const ExecResult down = press(key);
            if (down.status != ExecStatus::Accepted) return down;
            return release(key);
        }
    }
    return ExecResult::failed(ErrorCode::Internal);
}

ExecResult BleHidAdapter::press(const HidKey &key) {
    if (key.page == HidPage::Consumer) {
        if (consumer_ == key.usage) return ExecResult::accepted();
        consumer_ = key.usage;  // one consumer usage at a time: replaces
        return sent(transport_.sendConsumer(consumer_));
    }
    const auto usage = static_cast<uint8_t>(key.usage);
    for (uint8_t k : keys_) {
        if (k == usage) return ExecResult::accepted();
    }
    for (auto &k : keys_) {
        if (k == 0) {
            k = usage;
            return sent(flushKeyboard());
        }
    }
    return ExecResult::failed(ErrorCode::CapacityExceeded);  // 6-key rollover
}

ExecResult BleHidAdapter::release(const HidKey &key) {
    if (key.page == HidPage::Consumer) {
        if (consumer_ != key.usage) return ExecResult::accepted();  // not held (or replaced)
        consumer_ = 0;
        return sent(transport_.sendConsumer(0));
    }
    const auto usage = static_cast<uint8_t>(key.usage);
    for (auto &k : keys_) {
        if (k == usage) {
            k = 0;
            return sent(flushKeyboard());
        }
    }
    return ExecResult::accepted();
}

bool BleHidAdapter::flushKeyboard() {
    uint8_t report[8] = {};
    size_t n = 2;
    for (uint8_t k : keys_) {
        if (k != 0) report[n++] = k;
    }
    return transport_.sendKeyboard(report);
}

void BleHidAdapter::releaseAll(uint32_t nowMs) {
    (void)nowMs;
    const bool anyKeyboard = heldKeyboardKeys() != 0;
    const bool anyConsumer = consumer_ != 0;
    clearHeld();
    if (!transport_.connected()) return;
    if (anyKeyboard) {
        const uint8_t empty[8] = {};
        transport_.sendKeyboard(empty);
    }
    if (anyConsumer) transport_.sendConsumer(0);
}

void BleHidAdapter::poll(uint32_t nowMs, CompletionSink &sink) {
    (void)nowMs;
    (void)sink;
    forgetIfDisconnected();  // so a stale key isn't re-sent after a reconnect
}

bool BleHidAdapter::forgetIfDisconnected() {
    if (transport_.connected()) return false;
    clearHeld();  // whatever the host thought was held went with the link
    return true;
}

void BleHidAdapter::clearHeld() {
    std::memset(keys_, 0, sizeof(keys_));
    consumer_ = 0;
}

size_t BleHidAdapter::heldKeyboardKeys() const {
    size_t n = 0;
    for (uint8_t k : keys_) n += k != 0 ? 1 : 0;
    return n;
}

}  // namespace hubbo::integrations
