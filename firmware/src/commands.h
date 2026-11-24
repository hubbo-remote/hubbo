#pragma once

// The serial console's commands (README "Serial console"): read a line on
// the loop task, run it there. `ir ...` runs on httpd instead, where its
// buffers have one owner. Dev builds also take lines from GET /debug/run
// (dev_console.h).

#include <cstdint>
#include <string_view>

#include "ble_hid.h"
#include "ethernet_link.h"
#include "http_server.h"
#include "hubbo/engine/engine.h"
#include "hubbo/remote/input.h"
#include "ir_transmitter.h"
#include "remote_radio.h"

namespace hubbo::fw::commands {

// What the commands reach; main.cpp owns all of it.
struct Board {
    RemoteRadio &radio;
    hubbo::remote::RemoteInput &input;
    hubbo::Engine &engine;
    BleHid &ble;
    EthernetLink &ethernet;
    HttpServer &http;
    RmtIrTransmitter &ir;
    void (*printBootState)();  // config and credentials as found at boot
    void (*requestReboot)(const char *why);
    void (*flushSerial)();
};

// Boot, before the loop.
void begin(const Board &board);
// Loop task: reads serial and runs each complete line.
void poll();
// Loop task: one command line (modified in place).
void run(char *line);

// Loop task (status every 30 s).
void printStatus();
// Any task.
void printMemory(const char *label = "");
// Walks the filesystem: never on the loop task.
void printStorage();

// `ir ...`, without the "ir": httpd only.
bool isIrCommand(std::string_view line);
void irCommand(std::string_view args);

}  // namespace hubbo::fw::commands
