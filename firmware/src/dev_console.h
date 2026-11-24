#pragma once

// The dev console (README "Dev console over HTTP"): GET /debug/* on the
// `_ota` env (HUBBO_DEV_CONSOLE), for a PoE board with no serial attached.
// Read-only by design: it inspects and sends test IR, nothing that changes
// config or state. Without HUBBO_DEV_CONSOLE
// every call here is an empty inline, so main.cpp has one seam and no #ifdefs.

#include "engine_runtime.h"
#include "firmware_releases.h"
#include "firmware_update.h"
#include "http_server.h"
#include "ir_transmitter.h"

namespace hubbo::fw::dev_console {

// What the console reaches; main.cpp owns all of it.
struct Board {
    HttpServer &http;
    EngineRuntime &runtime;
    RmtIrTransmitter &ir;
    FirmwareReleases &releases;
    FirmwareUpdate &update;
};

#ifdef HUBBO_DEV_CONSOLE
// Once httpd runs: registers the /debug/* endpoints.
void begin(const Board &board);
// Loop task, every pass: runs a command queued by /debug/run.
void poll();
#else
inline void begin(const Board &) {}
inline void poll() {}
#endif

}  // namespace hubbo::fw::dev_console
