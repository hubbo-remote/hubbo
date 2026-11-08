// Hubbo board bring-up firmware (Phase 0.2 / 0.3). TEMPORARY: replaced by
// the real application once the core engine and adapters are wired in.
//
// Logs remote events, radio / Ethernet / BLE status and memory over USB
// serial, and measures latency from radio FIFO read to event emitted. A few
// serial commands drive Hub scan, BLE pairing and test keys; type `help`.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "ble_hid.h"
#include "board.h"
#include "ethernet_link.h"
#include "hubbo/engine/spsc.h"
#include "hubbo/integrations/ble_hid.h"
#include "hubbo/remote/button.h"
#include "hubbo/remote/input.h"
#include "latency.h"
#include "ota.h"
#include "remote_radio.h"

#ifndef HUBBO_BLE_NAME
#define HUBBO_BLE_NAME "Hubbo"
#endif

using hubbo::remote::EventKind;
using hubbo::remote::RemoteEvent;
using namespace hubbo::fw;

namespace {

constexpr uint32_t kStatusIntervalMs = 30000;
constexpr uint32_t kTapMs = 100;  // `ble tap` press length
constexpr uint32_t kNoLatency = UINT32_MAX;

char hostname[24];

// --- Logs -------------------------------------------------------------------
// The hot path only pushes; loop() prints. Keeps serial I/O out of the
// measured path. Full rings drop and count.
struct LoggedEvent {
    RemoteEvent event;
    uint32_t latencyUs;  // read -> emitted; kNoLatency for poll-driven events
};
struct LoggedPayload {
    uint8_t bytes[32];
    uint8_t length;
    uint8_t pipe;
};
hubbo::SpscRing<LoggedEvent, 33> eventLog;
hubbo::SpscRing<LoggedPayload, 17> payloadLog;
uint32_t logDropped = 0;

// --- State ------------------------------------------------------------------

LatencyStats emitLatency;  // radio FIFO read -> RemoteEvent emitted
uint32_t maxLoopGapUs = 0;
uint32_t slowLoops = 0;    // iterations over 5 ms
uint32_t lastLoopUs = 0;

bool rawLog = false;
bool inPayload = false;  // set while RemoteInput handles a radio payload
uint32_t payloadReadUs = 0;

BleHid ble;
EthernetLink ethernet;

struct PendingTap {
    bool active = false;
    const hubbo::integrations::HidKey *key = nullptr;
    uint32_t releaseAtMs = 0;
} pendingTap;

class EventLogger : public hubbo::remote::EventSink {
public:
    void onRemoteEvent(const RemoteEvent &event) override {
        uint32_t latency = kNoLatency;
        if (inPayload) {
            latency = micros() - payloadReadUs;
            // Down/Up only: Tap rides on the same Up and would double-count it.
            if (event.kind == EventKind::Down || event.kind == EventKind::Up) {
                emitLatency.record(latency);
            }
        }
        if (!eventLog.push({event, latency})) {
            logDropped++;
        }
    }
};

EventLogger eventLogger;
hubbo::remote::RemoteInput input(eventLogger);

class InputFeeder : public PayloadSink {
public:
    void onRadioPayload(const uint8_t *bytes, uint8_t length, uint8_t pipe,
                        uint32_t readUs) override {
        if (rawLog) {
            LoggedPayload entry{};
            memcpy(entry.bytes, bytes, length);
            entry.length = length;
            entry.pipe = pipe;
            if (!payloadLog.push(entry)) {
                logDropped++;
            }
        }
        payloadReadUs = readUs;
        inPayload = true;
        input.onPayload(bytes, length, millis());
        inPayload = false;
    }

    void onListeningStopped() override { input.releaseAll(millis()); }
};

InputFeeder feeder;
RemoteRadio radio(feeder);

// Before anything that stops the board acting on the remote (OTA, reboot).
void releaseOutputs() {
    input.releaseAll(millis());
    ble.releaseAll();
}

// --- Keys for `ble tap` -----------------------------------------------------

bool sendKey(const hubbo::integrations::HidKey &key, bool down) {
    if (key.page == hubbo::integrations::HidPage::Consumer) {
        return ble.sendConsumer(down ? key.usage : 0);
    }
    uint8_t report[8] = {};
    if (down) {
        report[2] = static_cast<uint8_t>(key.usage);
    }
    return ble.sendKeyboard(report);
}

// --- Printing -----------------------------------------------------------------

void printLogs() {
    LoggedEvent entry;
    while (eventLog.pop(&entry)) {
        const RemoteEvent &e = entry.event;
        Serial.printf("event %lu %s %s%s", static_cast<unsigned long>(e.atMs),
                      hubbo::remote::buttonName(e.button), hubbo::remote::eventKindName(e.kind),
                      e.timedOut ? " timedOut" : "");
        if (entry.latencyUs != kNoLatency) {
            Serial.printf(" (%lu us)", static_cast<unsigned long>(entry.latencyUs));
        }
        Serial.println();
    }
    LoggedPayload p;
    while (payloadLog.pop(&p)) {
        char line[16 + 3 * sizeof(p.bytes)];
        int n = snprintf(line, sizeof(line), "raw pipe %u:", p.pipe);
        for (uint8_t i = 0; i < p.length; i++) {
            n += snprintf(line + n, sizeof(line) - n, " %02X", p.bytes[i]);
        }
        Serial.println(line);
    }
    if (logDropped != 0) {
        Serial.printf("log overflow: %lu dropped\n", static_cast<unsigned long>(logDropped));
        logDropped = 0;
    }
}

void printLatency() {
    const LatencyStats::Summary s = emitLatency.summarize();
    if (s.count == 0) {
        Serial.println("latency read->event: no samples");
    } else {
        Serial.printf("latency read->event: n=%lu (last %lu) p50=%lu us p95=%lu us p99=%lu us max=%lu us\n",
                      static_cast<unsigned long>(s.count), static_cast<unsigned long>(s.kept),
                      static_cast<unsigned long>(s.p50), static_cast<unsigned long>(s.p95),
                      static_cast<unsigned long>(s.p99), static_cast<unsigned long>(s.max));
    }
    Serial.printf("loop: max gap %lu us, %lu iterations over 5 ms\n",
                  static_cast<unsigned long>(maxLoopGapUs), static_cast<unsigned long>(slowLoops));
}

void printMemory() {
    Serial.printf("mem: heap free %lu min %lu largest %lu | internal free %lu min %lu | psram free %lu\n",
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                  static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned long>(ESP.getFreePsram()));
}

void printStatus() {
    const uint32_t nowMs = millis();
    const RadioEndpoint ep = radio.endpoint();
    Serial.printf("status: up %lu s\n", static_cast<unsigned long>(nowMs / 1000));
    if (radio.state() == RemoteRadio::State::Scanning) {
        Serial.printf("  radio: scanning (channel %u)\n", radio.scanChannel());
    } else {
        Serial.printf("  radio: %s", RemoteRadio::stateName(radio.state()));
        if (ep.isValid()) {
            Serial.printf(" channel %u address 0x%010llX", ep.channel,
                          static_cast<unsigned long long>(ep.address));
        }
        Serial.printf(", %lu payloads, last %lu ms ago, %lu bad checksums, %lu unknown codes\n",
                      static_cast<unsigned long>(radio.payloadCount()),
                      static_cast<unsigned long>(radio.payloadCount() ? nowMs - radio.lastPayloadMs() : 0),
                      static_cast<unsigned long>(input.badChecksumCount()),
                      static_cast<unsigned long>(input.unknownCodeCount()));
    }
    const auto eth = ethernet.state();
    Serial.printf("  eth: %s", EthernetLink::stateName(eth));
    if (eth == EthernetLink::State::Up) {
        Serial.printf(" %s %u Mbps", ethernet.ip().toString().c_str(), ethernet.linkSpeedMbps());
    }
    Serial.printf(", ota %s\n", ota::enabled() ? "enabled" : "disabled");
    char peer[18];
    ble.peerAddress(peer, sizeof(peer));
    Serial.printf("  ble: %s", BleHid::stateName(ble.state()));
    if (peer[0] != '\0') {
        Serial.printf(" peer %s", peer);
    }
    if (ble.state() == BleHid::State::Pairing) {
        Serial.printf(" (%lu s left)", static_cast<unsigned long>(ble.pairingSecondsLeft(nowMs)));
    }
    Serial.printf(", notify failures %lu\n", static_cast<unsigned long>(ble.notifyFailures()));
    Serial.print("  ");
    printMemory();
    Serial.print("  ");
    printLatency();
}

const char *resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_USB: return "usb";
        default: return "other";
    }
}

// --- Serial commands ----------------------------------------------------------

void printHelp() {
    Serial.println(
        "commands:\n"
        "  status                 radio / eth / ble / memory / latency\n"
        "  scan [seconds]         learn a Harmony Hub (press its pair button)\n"
        "  scan stop              stop scanning\n"
        "  radio forget           erase the saved Hub endpoint\n"
        "  pair [seconds]         open the BLE pairing window\n"
        "  pair stop              close it\n"
        "  ble forget             delete BLE bonds\n"
        "  ble tap <key>          tap a key from core's HID key table (e.g. SELECT)  \n"
        "  ble release            release all keys\n"
        "  raw on|off             print raw radio payloads (distorts latency)\n"
        "  lat [reset]            latency summary, or clear it\n"
        "  mem                    memory\n"
        "  wdt                    hang the loop to test the watchdog (reboots)\n"
        "  reboot");
}

void runCommand(char *line) {
    const uint32_t nowMs = millis();
    char *cmd = strtok(line, " \t");
    char *arg1 = strtok(nullptr, " \t");
    char *arg2 = strtok(nullptr, " \t");
    if (cmd == nullptr) {
        return;
    }
    auto is = [](const char *a, const char *b) { return a != nullptr && strcmp(a, b) == 0; };

    if (is(cmd, "help") || is(cmd, "?")) {
        printHelp();
    } else if (is(cmd, "status")) {
        printStatus();
    } else if (is(cmd, "scan")) {
        if (is(arg1, "stop")) {
            radio.stopScan();
        } else {
            const uint32_t seconds = arg1 ? strtoul(arg1, nullptr, 10) : 0;
            const uint32_t timeout = seconds ? seconds * 1000 : RemoteRadio::kDefaultScanTimeoutMs;
            if (!radio.startScan(nowMs, timeout)) {
                Serial.println("radio: can't scan now (not ready or already scanning)");
            }
        }
    } else if (is(cmd, "radio") && is(arg1, "forget")) {
        radio.forgetEndpoint();
    } else if (is(cmd, "pair")) {
        if (is(arg1, "stop")) {
            ble.stopPairing();
            Serial.println("ble: pairing window closed");
        } else {
            const uint32_t seconds = arg1 ? strtoul(arg1, nullptr, 10) : 0;
            if (!ble.startPairing(nowMs, seconds ? seconds * 1000 : BleHid::kDefaultPairingWindowMs)) {
                Serial.println("ble: not started");
            }
        }
    } else if (is(cmd, "ble") && is(arg1, "forget")) {
        ble.forget();
    } else if (is(cmd, "ble") && is(arg1, "release")) {
        pendingTap.active = false;
        ble.releaseAll();
    } else if (is(cmd, "ble") && is(arg1, "tap")) {
        const int index = hubbo::integrations::bleHidKeyIndex(arg2);
        if (index < 0) {
            Serial.println("ble: unknown key; names are in core/src/integrations/ble_hid.cpp");
        } else if (pendingTap.active) {
            Serial.println("ble: a tap is already in progress");
        } else {
            const auto &key = hubbo::integrations::bleHidKey(static_cast<size_t>(index));
            if (sendKey(key, true)) {
                pendingTap = {true, &key, nowMs + kTapMs};
            } else {
                Serial.println("ble: not connected");
            }
        }
    } else if (is(cmd, "raw")) {
        rawLog = is(arg1, "on");
        Serial.printf("raw %s\n", rawLog ? "on" : "off");
    } else if (is(cmd, "lat")) {
        if (is(arg1, "reset")) {
            emitLatency.reset();
            maxLoopGapUs = 0;
            slowLoops = 0;
            Serial.println("latency reset");
        } else {
            printLatency();
        }
    } else if (is(cmd, "mem")) {
        printMemory();
    } else if (is(cmd, "wdt")) {
        Serial.println("hanging the loop; the task watchdog should reboot in ~5 s");
        Serial.flush();
        for (;;) {
        }
    } else if (is(cmd, "reboot")) {
        releaseOutputs();
        Serial.println("rebooting");
        Serial.flush();
        ESP.restart();
    } else {
        Serial.printf("unknown command: %s (try `help`)\n", cmd);
    }
}

void pollSerial() {
    static char line[64];
    static size_t length = 0;
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (length > 0) {
                line[length] = '\0';
                runCommand(line);
                length = 0;
            }
        } else if (length < sizeof(line) - 1) {
            line[length++] = static_cast<char>(c);
        }
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    // Give USB CDC up to 1 s to attach so the boot log isn't lost; on PoE
    // (no USB) this times out.
    while (!Serial && millis() < 1000) {
    }
    snprintf(hostname, sizeof(hostname), "hubbo-%012llX",
             static_cast<unsigned long long>(ESP.getEfuseMac()));

    Serial.printf("\nHubbo bring-up on %s (%s)\n", board::kName, hostname);
    Serial.printf("reset reason: %s\n", resetReasonName(esp_reset_reason()));
    Serial.printf("chip %s rev %u, %u cores, %lu MHz, flash %lu MB, psram %lu KB\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                  static_cast<unsigned long>(ESP.getCpuFreqMHz()),
                  static_cast<unsigned long>(ESP.getFlashChipSize() >> 20),
                  static_cast<unsigned long>(ESP.getPsramSize() >> 10));
    Serial.printf("sdk %s, %u buttons known\n", ESP.getSdkVersion(),
                  static_cast<unsigned>(hubbo::remote::kButtonCount));

    if (!radio.begin()) {
        Serial.println("radio: failed; check wiring and the 10 uF capacitor. Continuing without it.");
    }
    if (!ble.begin(HUBBO_BLE_NAME)) {
        Serial.println("ble: failed. Continuing without it.");
    }
    ethernet.begin(hostname);
    Serial.printf("ota: %s\n", ota::enabled() ? "compiled in, starts when Ethernet is up"
                                              : "disabled (no HUBBO_OTA_PASSWORD at build)");

    printMemory();
    enableLoopWDT();
    Serial.println("ready; type `help`");
    lastLoopUs = micros();
}

void loop() {
    const uint32_t nowUs = micros();
    const uint32_t gap = nowUs - lastLoopUs;
    lastLoopUs = nowUs;
    if (gap > maxLoopGapUs) {
        maxLoopGapUs = gap;
    }
    if (gap > 5000) {
        slowLoops++;
    }

    // Hot path first: drain the radio, then fire due Hold / release timeouts.
    radio.loop(millis());
    const uint32_t nowMs = millis();
    input.poll(nowMs);

    if (pendingTap.active && static_cast<int32_t>(nowMs - pendingTap.releaseAtMs) >= 0) {
        pendingTap.active = false;
        sendKey(*pendingTap.key, false);
    }

    ble.loop(nowMs);
    ethernet.loop(nowMs);
    if (ethernet.up()) {
        ota::begin(hostname, &releaseOutputs);  // no-op if disabled or already started
    }
    ota::loop();

    printLogs();
    pollSerial();

    static uint32_t lastStatusMs = 0;
    if (nowMs - lastStatusMs >= kStatusIntervalMs) {
        lastStatusMs = nowMs;
        printStatus();
    }
}
