#include "commands.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "console.h"
#include "diagnostics.h"
#include "flash_fs.h"
#include "hubbo/api/remote_port.h"
#include "hubbo/domain/pronto.h"
#include "hubbo/integrations/ble_hid.h"
#include "hubbo/integrations/ir.h"
#include "ota.h"

namespace hubbo::fw::commands {
namespace {

const Board *board = nullptr;

// Under this, lwIP and the W5500 can't allocate big sends: frames over about
// 190 B stop going out (seen at 9.9 KB free, with a 3.5 KB largest block).
constexpr size_t kLowInternalHeapBytes = 20 * 1024;

// The test code is NEC address 0x04, command 0x02: an LG TV's volume up.
constexpr char kIrTestCode[] =
    "0000 006D 0022 0002 0156 00AB 0015 0015 0015 0015 0015 0040 0015 0015 0015 0015 0015 0015 0015 0015 0015 0015 "
    "0015 0040 0015 0040 0015 0015 0015 0040 0015 0040 0015 0040 0015 0040 0015 0040 0015 0015 0015 0040 0015 0015 "
    "0015 0015 0015 0015 0015 0015 0015 0015 0015 0015 0015 0040 0015 0015 0015 0040 0015 0040 0015 0040 0015 0040 "
    "0015 0040 0015 0040 0015 05ED 0156 0055 0015 0E47";

void printHelp() {
    con.print(
        "commands (activities and devices are driven through the API):\n"
        "  status                 radio / engine / eth / ble / memory / latency\n"
        "  scan [seconds]         learn a Harmony Hub (press its pair button)\n"
        "  scan stop              stop scanning\n"
        "  radio set <ch> <addr>  use a known Hub endpoint instead of scanning (ch 0-124, hex addr)\n"
        "  radio forget           erase the saved Hub endpoint\n"
        "  ble pair <device_id>   open a pairing window for that Bluetooth device\n"
        "  ble cancel <device_id> close it (its old host, if any, stays)\n"
        "  ble forget <device_id> delete that device's bond\n"
        "  ir ...                 send IR codes (`ir` lists the commands)\n");
    con.println(
        "  raw on|off             print raw radio payloads (distorts latency)\n"
        "  lat [reset]            latency summary, or clear it\n"
        "  mem                    memory and filesystem\n"
        "  tasks                  CPU share per task since the last `tasks` (core, priority)\n"
        "  fs format              ERASE the config filesystem (reboots, formats at boot)\n"
        "  wdt                    hang the loop to test the watchdog (reboots)\n"
        "  reboot");
}

}  // namespace

void begin(const Board &b) {
    static const Board kept = b;
    board = &kept;
}

void printMemory(const char *label) {
    const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    con.printf("%smem: heap free %lu min %lu largest %lu | internal free %lu min %lu | psram free %lu"
               " | loop stack left %lu\n",
               label, static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMinFreeHeap()),
               static_cast<unsigned long>(ESP.getMaxAllocHeap()), static_cast<unsigned long>(internalFree),
               static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned long>(ESP.getFreePsram()),
               static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
    if (internalFree < kLowInternalHeapBytes) {
        con.println("mem: WARNING internal RAM low; large network sends will stall");
    }
}

void printStorage() {
    con.printf("fs: %s, %lu of %lu KB used\n",
               flash_fs::mounted() ? "mounted" : "NOT MOUNTED (storage unavailable; `fs format` to fix)",
               static_cast<unsigned long>(flash_fs::usedBytes() >> 10),
               static_cast<unsigned long>(flash_fs::totalBytes() >> 10));
}

void printStatus() {
    const Board &b = *board;
    const uint32_t nowMs = millis();
    const RadioEndpoint ep = b.radio.endpoint();
    con.printf("status: up %lu s\n", static_cast<unsigned long>(nowMs / 1000));
    if (b.radio.state() == hubbo::api::RadioState::Scanning) {
        con.printf("  radio: scanning (channel %u), power %s\n", b.radio.scanChannel(),
                   hubbo::config::radioPowerName(b.radio.power()));
    } else {
        con.printf("  radio: %s", hubbo::api::radioStateName(b.radio.state()));
        if (ep.isValid()) {
            con.printf(" channel %u address 0x%010llX", ep.channel, static_cast<unsigned long long>(ep.address));
        }
        con.printf(", power %s, %lu payloads, last %lu ms ago, %lu bad checksums, %lu unknown codes\n",
                   hubbo::config::radioPowerName(b.radio.power()), static_cast<unsigned long>(b.radio.payloadCount()),
                   static_cast<unsigned long>(b.radio.payloadCount() ? nowMs - b.radio.lastPayloadMs() : 0),
                   static_cast<unsigned long>(b.input.badChecksumCount()),
                   static_cast<unsigned long>(b.input.unknownCodeCount()));
    }
    const auto &es = b.engine.stats();
    const auto &xs = b.engine.executor().stats();
    con.printf("  engine: %s, slot %s, %lu events, %lu unbound, %lu dispatched, %lu transitions;"
               " sequences %lu ok %lu with errors %lu rejected %lu timeouts\n",
               b.engine.hasActiveActivity() ? b.engine.activeActivityId() : "(off)",
               hubbo::smartSlotName(b.engine.selectedSlotShared()), static_cast<unsigned long>(es.events),
               static_cast<unsigned long>(es.unbound), static_cast<unsigned long>(es.dispatched),
               static_cast<unsigned long>(es.transitions), static_cast<unsigned long>(xs.completed),
               static_cast<unsigned long>(xs.withErrors), static_cast<unsigned long>(xs.rejected),
               static_cast<unsigned long>(xs.timeouts));
    con.printf("  eth: %s %s, http %s (%lu requests), ota %s\n", EthernetLink::stateName(b.ethernet.state()),
               b.ethernet.ipText(), b.http.running() ? "up" : "down",
               static_cast<unsigned long>(b.http.requestCount()), ota::enabled() ? "enabled" : "disabled");
    b.ble.printStatus(nowMs);
    b.printBootState();  // live state: GET /api/v1/status
    con.print("  ");
    printMemory();
    diag::printLatency();
}

bool isIrCommand(std::string_view line) {
    return line == "ir" || line.rfind("ir ", 0) == 0;
}

// Codes go through the transmitter directly, not the engine, converted
// exactly as the IR adapter converts them.
void irCommand(std::string_view args) {
    RmtIrTransmitter &ir = board->ir;
    // The durations must stay put until the send ends (PSRAM: allocated
    // after extmem_enable(0)). Only httpd runs this.
    static std::vector<uint32_t> durations;
    static esp_timer_handle_t holdTimer = nullptr;  // ends `ir hold`
    if (holdTimer == nullptr) {
        esp_timer_create_args_t timer = {};
        timer.callback = [](void *) { board->ir.stop(); };
        timer.name = "ir_hold";
        if (esp_timer_create(&timer, &holdTimer) != ESP_OK) {
            con.println("ir: no timer");
            holdTimer = nullptr;
            return;
        }
    }
    auto skipSpaces = [&] {
        while (!args.empty() && args.front() == ' ') {
            args.remove_prefix(1);
        }
    };
    auto take = [&](std::string_view name) {
        skipSpaces();
        if (args.rfind(name, 0) == 0 && (args.size() == name.size() || args[name.size()] == ' ')) {
            args.remove_prefix(name.size());
            skipSpaces();
            return true;
        }
        return false;
    };
    auto number = [&](unsigned long fallback) {
        unsigned long n = 0;
        size_t digits = 0;
        while (digits < args.size() && args[digits] >= '0' && args[digits] <= '9') {
            n = n * 10 + (args[digits++] - '0');
        }
        args.remove_prefix(digits);
        skipSpaces();
        return digits ? n : fallback;
    };
    auto send = [&](std::string_view pronto, uint16_t frames, bool hold) {
        if (ir.busy()) {  // the durations may still be in use
            con.println("ir: busy");
            return false;
        }
        std::vector<uint16_t> words;
        if (!hubbo::pronto::parse(pronto, &words)) {
            con.println("ir: not a raw Pronto code (uppercase 4-digit hex words, single spaces, 0000 first)");
            return false;
        }
        durations.clear();
        const hubbo::integrations::IrCodeLayout code = hubbo::integrations::appendIrCode(words.data(), &durations);
        const hubbo::integrations::IrSignal signal = hubbo::integrations::irSignal(code, durations, "console", "ir");
        const bool sent = ir.send(signal, frames, hold);
        con.printf("ir: %u frame(s)%s at %lu Hz: %s\n", frames, hold ? " then hold" : "",
                   static_cast<unsigned long>(signal.carrierHz), sent ? "sending" : "refused");
        return sent;
    };
    if (take("test")) {
        send(kIrTestCode, static_cast<uint16_t>(number(1)), false);
    } else if (take("hold")) {
        const uint32_t ms = static_cast<uint32_t>(number(3000));
        if (send(kIrTestCode, 1, true)) {
            esp_timer_start_once(holdTimer, uint64_t{ms} * 1000);
        }
    } else if (take("send")) {
        const uint16_t frames = static_cast<uint16_t>(number(1));
        send(args, frames, false);
    } else if (take("stop")) {
        esp_timer_stop(holdTimer);  // so it can't end a later hold
        ir.stop();
    } else {
        con.printf("ir: %s%s. ir test [frames] | hold [ms] | send <frames> <pronto> | stop\n",
                   ir.ready() ? "ready" : "not ready", ir.busy() ? ", busy" : "");
    }
}

void run(char *line) {
    const Board &b = *board;
    if (isIrCommand(line)) {  // takes the rest of the line (a Pronto code)
        if (!b.http.post([args = std::string(std::string_view(line).substr(2))] { irCommand(args); })) {
            con.println("ir: busy, try again");
        }
        return;
    }
    const uint32_t nowMs = millis();
    char *cmd = strtok(line, " \t");
    char *arg1 = strtok(nullptr, " \t");
    char *arg2 = strtok(nullptr, " \t");
    char *arg3 = strtok(nullptr, " \t");
    if (cmd == nullptr) {
        return;
    }
    auto is = [](const char *a, const char *b) { return a != nullptr && strcmp(a, b) == 0; };
    auto seconds = [&](uint32_t fallbackMs) {
        const uint32_t s = arg1 ? strtoul(arg1, nullptr, 10) : 0;
        return s ? s * 1000 : fallbackMs;
    };

    if (is(cmd, "help") || is(cmd, "?")) {
        printHelp();
    } else if (is(cmd, "status")) {
        printStatus();
    } else if (is(cmd, "scan") && is(arg1, "stop")) {
        b.radio.stopScan();
    } else if (is(cmd, "scan")) {
        if (!b.radio.startScan(nowMs, seconds(hubbo::api::kRemoteScanTimeoutMs))) {
            con.println("radio: can't scan now (not ready or already scanning)");
        }
    } else if (is(cmd, "radio") && is(arg1, "set")) {
        // radio set <channel 0-124, decimal> <40-bit address, hex, 0x optional>
        char *end = nullptr;
        RadioEndpoint endpoint;
        const unsigned long channel = arg2 ? strtoul(arg2, &end, 10) : 0;
        // Parse only; the rule is RadioEndpoint::isValid (the API's) in setEndpoint().
        const bool channelOk = arg2 != nullptr && *end == '\0' && channel <= hubbo::api::kMaxRemoteChannel;
        uint64_t address = 0;
        const bool addressOk = arg3 != nullptr && hubbo::api::parseRemoteAddress(arg3, &address);
        endpoint.channel = static_cast<uint8_t>(channel);
        endpoint.address = address;
        if (!channelOk || !addressOk || !b.radio.setEndpoint(endpoint)) {
            con.println("radio: usage `radio set <channel 0-124> <40-bit hex address>`; invalid endpoint or radio not ready");
        }  // listen() logs the endpoint
    } else if (is(cmd, "radio") && is(arg1, "forget")) {
        b.radio.forgetEndpoint();
    } else if (is(cmd, "ble") && is(arg1, "pair") && arg2 != nullptr) {
        if (!b.ble.startPairing(arg2, nowMs, hubbo::integrations::kBlePairingDefaultWindowMs)) {
            con.println("ble: not started, or a pairing window is already open");
        }
    } else if (is(cmd, "ble") && is(arg1, "cancel") && arg2 != nullptr) {
        b.ble.cancelPairing(arg2);
    } else if (is(cmd, "ble") && is(arg1, "forget") && arg2 != nullptr) {
        b.ble.forget(arg2);
    } else if (is(cmd, "raw")) {
        diag::setRawLog(is(arg1, "on"));
        con.printf("raw %s\n", diag::rawLog() ? "on" : "off");
    } else if (is(cmd, "lat") && is(arg1, "reset")) {
        diag::resetLatency();
        con.println("latency reset");
    } else if (is(cmd, "lat")) {
        diag::printLatency();
    } else if (is(cmd, "tasks")) {
        diag::printTasks();
    } else if (is(cmd, "mem")) {
        printMemory();
        if (!b.http.post(&printStorage)) {  // walks flash: httpd prints it
            con.println("fs: busy, try again");
        }
    } else if (is(cmd, "fs") && is(arg1, "format")) {
        // Never live: a config save may be running on httpd. The request is
        // an NVS write, so httpd records it; the format runs at next boot.
        const bool posted = b.http.post([] {
            if (flash_fs::requestFormatAtBoot()) {
                board->requestReboot("fs format: the format runs at boot");
            } else {
                con.println("fs: couldn't record the format request; not rebooting");
            }
        });
        if (!posted) {
            con.println("fs: busy, try again");
        }
    } else if (is(cmd, "wdt")) {
        con.println("hanging the loop; the task watchdog should reboot in ~5 s");
        b.flushSerial();
        for (;;) {
        }
    } else if (is(cmd, "reboot")) {
        b.requestReboot("console");
    } else {
        con.printf("unknown command: %s (try `help`)\n", cmd);
    }
}

void poll() {
    // `ir send` takes a whole Pronto code: the line lives in PSRAM.
    constexpr size_t kLineBytes = 512;
    static char *line = static_cast<char *>(heap_caps_malloc(kLineBytes, MALLOC_CAP_SPIRAM));
    static size_t length = 0;
    static bool tooLong = false;  // drop the whole line, never run a cut one
    if (line == nullptr) {
        return;
    }
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (tooLong) {
                con.printf("console: line too long (over %u characters); not run\n",
                           static_cast<unsigned>(kLineBytes - 1));
            } else if (length > 0) {
                line[length] = '\0';
                run(line);
            }
            length = 0;
            tooLong = false;
        } else if (length < kLineBytes - 1) {
            line[length++] = static_cast<char>(c);
        } else {
            tooLong = true;
        }
    }
}

}  // namespace hubbo::fw::commands
