#include "hubbo/integrations/ble_hid.h"

#include <cstring>
#include <iterator>

#include "hubbo/domain/config.h"
#include "hubbo/domain/time.h"
#include "remote_group.h"

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
    X("KEY_Z", Keyboard, 0x1D)                                   \
    X("FAST_FORWARD", Consumer, 0x00B3)                          \
    X("REWIND", Consumer, 0x00B4)                                \
    X("STOP", Consumer, 0x00B7)                                  \
    X("AC_BACK", Consumer, 0x0224)                               \
    X("AC_SEARCH", Consumer, 0x0221)                             \
    X("MENU_ESCAPE", Consumer, 0x0046)   /* Exit */              \
    X("CHANNEL_UP", Consumer, 0x009C)    /* Channel Increment */ \
    X("CHANNEL_DOWN", Consumer, 0x009D)  /* Channel Decrement */ \
    X("INFO", Consumer, 0x01BD)          /* AL OEM Features (Info) */ \
    X("SLEEP", Consumer, 0x0034)                                 \
    X("KEYPAD_ENTER", Keyboard, 0x58)                            \
    X("BACKSPACE", Keyboard, 0x2A)                               \
    X("ESCAPE", Keyboard, 0x29)                                  \
    X("TAB", Keyboard, 0x2B)                                     \
    X("SPACE", Keyboard, 0x2C)                                   \
    X("MINUS", Keyboard, 0x2D)                                   \
    X("EQUAL", Keyboard, 0x2E)                                   \
    X("LEFT_BRACKET", Keyboard, 0x2F)                            \
    X("RIGHT_BRACKET", Keyboard, 0x30)                           \
    X("BACKSLASH", Keyboard, 0x31)                               \
    X("NON_US_HASH", Keyboard, 0x32)                             \
    X("SEMICOLON", Keyboard, 0x33)                               \
    X("APOSTROPHE", Keyboard, 0x34)                              \
    X("GRAVE", Keyboard, 0x35)                                   \
    X("COMMA", Keyboard, 0x36)                                   \
    X("PERIOD", Keyboard, 0x37)                                  \
    X("SLASH", Keyboard, 0x38)                                   \
    X("CAPS_LOCK", Keyboard, 0x39)                               \
    X("F5", Keyboard, 0x3E)                                      \
    X("F6", Keyboard, 0x3F)                                      \
    X("F7", Keyboard, 0x40)                                      \
    X("F8", Keyboard, 0x41)                                      \
    X("F9", Keyboard, 0x42)                                      \
    X("F10", Keyboard, 0x43)                                     \
    X("F11", Keyboard, 0x44)                                     \
    X("F12", Keyboard, 0x45)                                     \
    X("PRINT_SCREEN", Keyboard, 0x46)                            \
    X("SCROLL_LOCK", Keyboard, 0x47)                             \
    X("PAUSE", Keyboard, 0x48)                                   \
    X("INSERT", Keyboard, 0x49)                                  \
    X("HOME", Keyboard, 0x4A)                                    \
    X("DELETE", Keyboard, 0x4C)                                  \
    X("END", Keyboard, 0x4D)                                     \
    X("KEYBOARD_RIGHT", Keyboard, 0x4F)                          \
    X("KEYBOARD_LEFT", Keyboard, 0x50)                           \
    X("KEYBOARD_DOWN", Keyboard, 0x51)                           \
    X("KEYBOARD_UP", Keyboard, 0x52)                             \
    X("NUM_LOCK", Keyboard, 0x53)                                \
    X("KEYPAD_SLASH", Keyboard, 0x54)                            \
    X("KEYPAD_ASTERISK", Keyboard, 0x55)                         \
    X("KEYPAD_MINUS", Keyboard, 0x56)                            \
    X("KEYPAD_PLUS", Keyboard, 0x57)                             \
    X("KEYPAD_1", Keyboard, 0x59)                                \
    X("KEYPAD_2", Keyboard, 0x5A)                                \
    X("KEYPAD_3", Keyboard, 0x5B)                                \
    X("KEYPAD_4", Keyboard, 0x5C)                                \
    X("KEYPAD_5", Keyboard, 0x5D)                                \
    X("KEYPAD_6", Keyboard, 0x5E)                                \
    X("KEYPAD_7", Keyboard, 0x5F)                                \
    X("KEYPAD_8", Keyboard, 0x60)                                \
    X("KEYPAD_9", Keyboard, 0x61)                                \
    X("KEYPAD_0", Keyboard, 0x62)                                \
    X("KEYPAD_PERIOD", Keyboard, 0x63)                           \
    X("NON_US_BACKSLASH", Keyboard, 0x64)                        \
    X("APPLICATION", Keyboard, 0x65)

#define HUBBO_KEY_ENTRY(name, page, usage) {name, HidPage::page, usage},
#define HUBBO_KEY_NAME(name, page, usage) name,

constexpr HidKey kKeys[] = {HUBBO_BLE_HID_KEYS(HUBBO_KEY_ENTRY)};
constexpr const char *kKeyNames[] = {HUBBO_BLE_HID_KEYS(HUBBO_KEY_NAME)};

#undef HUBBO_KEY_ENTRY
#undef HUBBO_KEY_NAME
#undef HUBBO_BLE_HID_KEYS

constexpr auto kKeyCount = static_cast<uint16_t>(std::size(kKeys));
static_assert(kKeyCount <= limits::kMaxEnumOptions, "the key param is one enum");

constexpr ParamSpec kKeyParams[] = {
    ParamSpec::enumeration("key", kKeyNames, kKeyCount),
};
constexpr ParamSpec kUsageParams[] = {
    ParamSpec::integer("usage", limits::kHidConsumerUsageMin, limits::kHidConsumerUsageMax),
};
constexpr ParamSpec kWaitParams[] = {
    ParamSpec::integer("timeout_ms", kBleWaitConnectedMinMs, kBleWaitConnectedMaxMs, /*required=*/false,
                       kBleWaitConnectedDefaultMs)
        .described(nullptr, "ms")
        .shownDefault(),
};

constexpr const char *kKeysGroup = "keys";
constexpr const char *kCustom = "custom";
constexpr uint32_t kPressTimeoutMs = limits::kMaxHoldMs + 1000;
constexpr uint32_t kWaitTimeoutMs = kBleWaitConnectedMaxMs + 1000;

ExecResult sent(bool ok) { return ok ? ExecResult::accepted() : ExecResult::failed(ErrorCode::AdapterFailed); }

// The same command list, in every field a catalog uses (not the label).
bool sameCommands(const std::vector<ProfileCommand> &a, const std::vector<ProfileCommand> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].name != b[i].name || a[i].page != b[i].page || a[i].usage != b[i].usage ||
            a[i].holdMs != b[i].holdMs) {
            return false;
        }
    }
    return true;
}

}  // namespace

// One device's catalog: its profile's commands by name (the `command`
// enum), and the same keys, usages and wait as every Bluetooth device.
struct BleHidAdapter::DeviceCatalog {
    std::vector<ProfileCommand> commands;
    detail::RemoteGroup remote;  // names into commands[i].name
    CommandSpec specs[static_cast<size_t>(BleHidCommand::WaitConnected) + 1];

    explicit DeviceCatalog(const std::vector<ProfileCommand> &profileCommands) : commands(profileCommands) {
        for (const ProfileCommand &c : commands) remote.names.push_back(c.name.c_str());
        static_assert(static_cast<size_t>(BleHidCommand::Press) == 0 && static_cast<size_t>(BleHidCommand::PressUp) == 2,
                      "the remote group leads the catalog");
        remote.fill(specs, this, kPressTimeoutMs);
        const auto spec = [&](BleHidCommand index, const char *id, const ParamSpec *params, uint8_t n,
                              CommandEffect effect, const char *releasedBy, const char *group, uint32_t timeout) {
            specs[static_cast<size_t>(index)] = {id, params, n, effect, releasedBy, group, this, timeout};
        };
        using E = CommandEffect;
        using C = BleHidCommand;
        spec(C::KeyTap, "key_tap", kKeyParams, 1, E::Momentary, nullptr, kKeysGroup, 0);
        spec(C::KeyDown, "key_down", kKeyParams, 1, E::Hold, "key_up", kKeysGroup, 0);
        spec(C::KeyUp, "key_up", kKeyParams, 1, E::Momentary, nullptr, kKeysGroup, 0);
        spec(C::UsageTap, "usage_tap", kUsageParams, 1, E::Momentary, nullptr, kCustom, 0);
        spec(C::UsageDown, "usage_down", kUsageParams, 1, E::Hold, "usage_up", kCustom, 0);
        spec(C::UsageUp, "usage_up", kUsageParams, 1, E::Momentary, nullptr, kCustom, 0);
        spec(C::WaitConnected, "wait_connected", kWaitParams, 1, E::Wait, nullptr, nullptr, kWaitTimeoutMs);
    }
    DeviceCatalog(const DeviceCatalog &) = delete;
    DeviceCatalog &operator=(const DeviceCatalog &) = delete;

    CommandCatalog catalog() const { return {specs, static_cast<uint16_t>(std::size(specs))}; }
};

size_t bleHidKeyCount() { return kKeyCount; }

const HidKey &bleHidKey(size_t index) { return kKeys[index < kKeyCount ? index : 0]; }

int bleHidKeyIndex(const char *name) {
    if (name == nullptr) return -1;
    for (uint16_t i = 0; i < kKeyCount; i++) {
        if (std::strcmp(kKeys[i].name, name) == 0) return i;
    }
    return -1;
}

const char *BleHidAdapter::integrationType() const { return kBleHidType; }

BleHidAdapter::BleHidAdapter(const char *integrationId, HidTransport &transport)
    : id_(integrationId), transport_(transport) {}

BleHidAdapter::~BleHidAdapter() = default;

CommandCatalog BleHidAdapter::commandsFor(const DeviceSpec &device) const {
    if (!device.profile) return {};
    std::lock_guard<std::mutex> lock(catalogsMutex_);
    // One catalog per distinct command list, shared by every device (and
    // every compile, dry run or not) with that list. A device's commands come
    // from the profile it was added from and never change after, so the set
    // grows only with the profiles ever added, never per edit. Never freed:
    // compiled configs, queued work and held keys point into them.
    for (const auto &c : catalogs_) {
        if (sameCommands(c->commands, device.profile->commands)) return c->catalog();
    }
    catalogs_.push_back(std::make_unique<DeviceCatalog>(device.profile->commands));
    return catalogs_.back()->catalog();
}

ExecResult BleHidAdapter::execute(const ExecRequest &request) {
    if (request.command == nullptr || request.command->data == nullptr || request.params == nullptr ||
        request.deviceId == nullptr) {
        return ExecResult::failed(ErrorCode::Internal);
    }
    const auto &catalog = *static_cast<const DeviceCatalog *>(request.command->data);
    const auto command = static_cast<BleHidCommand>(request.commandIndex);
    const std::string_view deviceId = request.deviceId;
    const bool connected = transport_.connected(deviceId);
    if (command == BleHidCommand::WaitConnected) {
        if (connected) return ExecResult::accepted();
        const auto timeout = static_cast<uint32_t>(request.params[0].integer);
        return start(request, true, HidKey{}, request.nowMs + timeout);
    }

    // The key, and for a timed press how long to hold it.
    HidKey key{};
    uint32_t holdMs = 0;
    switch (command) {
        case BleHidCommand::Press:
        case BleHidCommand::PressDown:
        case BleHidCommand::PressUp: {
            const uint16_t option = request.params[0].asOption();
            if (option >= catalog.commands.size()) return ExecResult::failed(ErrorCode::Internal);
            const ProfileCommand &c = catalog.commands[option];
            key = HidKey{c.name.c_str(), c.page, c.usage};
            if (command == BleHidCommand::Press) {
                const bool stepHold = (request.paramsPresent & 0x2) != 0;
                holdMs = stepHold ? static_cast<uint32_t>(request.params[1].integer) : c.holdMs.value_or(0);
            }
            break;
        }
        case BleHidCommand::KeyTap:
        case BleHidCommand::KeyDown:
        case BleHidCommand::KeyUp: {
            const uint16_t option = request.params[0].asOption();
            if (option >= kKeyCount) return ExecResult::failed(ErrorCode::Internal);
            key = kKeys[option];
            break;
        }
        case BleHidCommand::UsageTap:
        case BleHidCommand::UsageDown:
        case BleHidCommand::UsageUp:
            key = HidKey{nullptr, HidPage::Consumer, static_cast<uint16_t>(request.params[0].integer)};
            break;
        case BleHidCommand::WaitConnected:
            break;
    }

    if (!connected) {
        // Whatever its host thought was held went with the link.
        if (Host *gone = find(deviceId)) *gone = Host{};
        return ExecResult::failed(ErrorCode::AdapterUnavailable);
    }
    Host *host = findOrAdd(deviceId);
    if (host == nullptr) return ExecResult::failed(ErrorCode::CapacityExceeded);  // more hosts than bonds
    ExecResult result = ExecResult::failed(ErrorCode::Internal);
    switch (command) {
        case BleHidCommand::PressDown:
        case BleHidCommand::KeyDown:
        case BleHidCommand::UsageDown:
            result = press(*host, key);
            break;
        case BleHidCommand::PressUp:
        case BleHidCommand::KeyUp:
        case BleHidCommand::UsageUp:
            result = release(*host, key);
            break;
        case BleHidCommand::Press:
        case BleHidCommand::KeyTap:
        case BleHidCommand::UsageTap:
            result = press(*host, key);
            if (result.status != ExecStatus::Accepted) break;
            if (holdMs != 0) {
                // A timed press: poll() releases it (and finishes the action).
                result = start(request, false, key, request.nowMs + holdMs);
                if (result.status == ExecStatus::Failed) (void)release(*host, key);
                break;
            }
            result = release(*host, key);
            break;
        case BleHidCommand::WaitConnected:
            break;
    }
    freeIfIdle(*host);
    return result;
}

ExecResult BleHidAdapter::start(const ExecRequest &request, bool waitConnected, const HidKey &key, uint32_t atMs) {
    for (Timer &t : timers_) {
        if (t.inUse()) continue;
        if (!t.deviceId.assign(request.deviceId)) return ExecResult::failed(ErrorCode::Internal);
        t.ticket = request.ticket;
        t.key = key;
        t.waitConnected = waitConnected;
        t.detached = request.answerAtStart && !waitConnected;
        t.atMs = atMs;
        return t.detached ? ExecResult::accepted() : ExecResult::pending();
    }
    return ExecResult::failed(ErrorCode::CapacityExceeded);
}

void BleHidAdapter::finishTimer(Timer &t) {
    if (!t.waitConnected) {
        if (Host *host = find(t.deviceId.view())) {
            (void)release(*host, t.key);
            freeIfIdle(*host);
        }
    }
    t = Timer{};
}

bool BleHidAdapter::timed(const Host &host, HidPage page, uint16_t usage) const {
    for (const Timer &t : timers_) {
        if (t.inUse() && !t.waitConnected && t.key.page == page && t.key.usage == usage && t.deviceId == host.deviceId.view()) {
            return true;
        }
    }
    return false;
}

ExecResult BleHidAdapter::press(Host &host, const HidKey &key) {
    if (key.page == HidPage::Consumer) {
        if (host.consumer == key.usage) return ExecResult::accepted();
        host.consumer = key.usage;  // one consumer usage at a time: replaces
        return sent(transport_.sendConsumer(host.deviceId.view(), host.consumer));
    }
    const auto usage = static_cast<uint8_t>(key.usage);
    for (uint8_t k : host.keys) {
        if (k == usage) return ExecResult::accepted();
    }
    for (auto &k : host.keys) {
        if (k == 0) {
            k = usage;
            return sent(flushKeyboard(host));
        }
    }
    return ExecResult::failed(ErrorCode::CapacityExceeded);  // 6-key rollover
}

ExecResult BleHidAdapter::release(Host &host, const HidKey &key) {
    if (key.page == HidPage::Consumer) {
        if (host.consumer != key.usage) return ExecResult::accepted();  // not held (or replaced)
        host.consumer = 0;
        return sent(transport_.sendConsumer(host.deviceId.view(), 0));
    }
    const auto usage = static_cast<uint8_t>(key.usage);
    for (auto &k : host.keys) {
        if (k == usage) {
            k = 0;
            return sent(flushKeyboard(host));
        }
    }
    return ExecResult::accepted();
}

bool BleHidAdapter::flushKeyboard(const Host &host) {
    uint8_t report[8] = {};
    size_t n = 2;
    for (uint8_t k : host.keys) {
        if (k != 0) report[n++] = k;
    }
    return transport_.sendKeyboard(host.deviceId.view(), report);
}

void BleHidAdapter::releaseAll(uint32_t nowMs) {
    (void)nowMs;
    for (Host &host : hosts_) {
        // Keys a timed press holds stay down until its time (or cancel()).
        bool keysReleased = false;
        for (uint8_t &k : host.keys) {
            if (k == 0 || timed(host, HidPage::Keyboard, k)) continue;
            k = 0;
            keysReleased = true;
        }
        const bool consumerReleased = host.consumer != 0 && !timed(host, HidPage::Consumer, host.consumer);
        if (consumerReleased) host.consumer = 0;
        if (transport_.connected(host.deviceId.view())) {  // if not, poll() drops the host
            if (keysReleased) (void)flushKeyboard(host);
            if (consumerReleased) transport_.sendConsumer(host.deviceId.view(), 0);
        }
        freeIfIdle(host);
    }
}

void BleHidAdapter::cancel(Ticket ticket, uint32_t nowMs) {
    (void)nowMs;
    for (Timer &t : timers_) {
        if (t.inUse() && !t.detached && t.ticket == ticket) return finishTimer(t);
    }
}

void BleHidAdapter::poll(uint32_t nowMs, CompletionSink &sink) {
    // A host that dropped loses its held state, so a stale key isn't re-sent
    // after it reconnects.
    for (Host &host : hosts_) {
        if (host.holding() && !transport_.connected(host.deviceId.view())) host = Host{};
    }
    for (Timer &t : timers_) {
        if (!t.inUse()) continue;
        const bool arrived = t.waitConnected && transport_.connected(t.deviceId.view());
        if (!arrived && !timeReached(nowMs, t.atMs)) continue;
        ExecResult result = ExecResult::accepted();
        if (!arrived && t.waitConnected) result.error = ErrorCode::AdapterUnavailable;  // gave up: the sequence goes on
        const Ticket ticket = t.ticket;
        const bool detached = t.detached;
        finishTimer(t);
        if (!detached) sink.complete(ticket, result, nowMs);
    }
}

size_t BleHidAdapter::Host::keyCount() const {
    size_t n = 0;
    for (uint8_t k : keys) n += k != 0 ? 1 : 0;
    return n;
}

bool BleHidAdapter::Host::holding() const { return consumer != 0 || keyCount() != 0; }

void BleHidAdapter::freeIfIdle(Host &host) {
    if (!host.holding()) host = Host{};
}

BleHidAdapter::Host *BleHidAdapter::find(std::string_view deviceId) {
    for (Host &host : hosts_) {
        if (!host.deviceId.empty() && host.deviceId == deviceId) return &host;
    }
    return nullptr;
}

const BleHidAdapter::Host *BleHidAdapter::find(std::string_view deviceId) const {
    return const_cast<BleHidAdapter *>(this)->find(deviceId);
}

BleHidAdapter::Host *BleHidAdapter::findOrAdd(std::string_view deviceId) {
    if (Host *found = find(deviceId)) return found;
    for (Host &host : hosts_) {
        if (host.deviceId.empty()) {
            if (!host.deviceId.assign(deviceId)) return nullptr;
            return &host;
        }
    }
    return nullptr;
}

size_t BleHidAdapter::heldKeyboardKeys(std::string_view deviceId) const {
    const Host *host = find(deviceId);
    return host != nullptr ? host->keyCount() : 0;
}

uint16_t BleHidAdapter::heldConsumer(std::string_view deviceId) const {
    const Host *host = find(deviceId);
    return host != nullptr ? host->consumer : 0;
}

}  // namespace hubbo::integrations
